// In-game ImGui settings configuration window.
#include "SettingsMenu.h"
#include "Settings.h"
#include "MapDrawHelpers.h"  // MapDraw::ColorEdit4Hex (also pulls in MapTheme.h)
#include "MapTheme.h"
#include "MapMarkerRenderer.h"  // IsMapFading()
#include "InputHandler.h"       // bind-row key names + the capture globals
#include "LocalizationManager.h"
#include "BackgroundManager.h"
#include "MenuUtils.h"
#include "PartySheetState.h"
#include "InputHook.h"
#include "GlanceBounds.h"
#include <imgui.h>
#include <SKSE/SKSE.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>

namespace SettingsMenu {
    static std::atomic<bool> g_isVisible{ false };
    static std::atomic<Host> g_host{ Host::None };
    static float g_toastTimer = 0.0f;
    static bool g_wasDraggingMenu = false; // edge-detect drag-release for auto-persist
    static bool g_wasVisible = false; // edge-detect open to reset the active tab

    bool IsVisible() { return g_isVisible.load(); }
    bool IsGameplayHosted() { return g_isVisible.load() && g_host.load() == Host::Gameplay; }

    static bool MapOpen(RE::UI* ui) {
        return ui && ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
    }

    void Hide(const char* reason) {
        if (!g_isVisible.load() && g_host.load() == Host::None) {
            return;
        }
        logger::debug("SettingsMenu closed (reason: {})", reason ? reason : "unknown");
        g_isVisible.store(false);
        g_host.store(Host::None);
        g_wasVisible = false;
        g_hkCaptureCb = nullptr;
        InputHook::ClearSettingsHotkeyLatch();
        if (ImGui::GetCurrentContext()) {
            ImGui::GetIO().ClearInputKeys();
        }
    }

    bool Show() {
        if (g_isVisible.load()) {
            return true;
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            logger::debug("SettingsMenu open rejected: UI singleton missing");
            return false;
        }
        if (IsMapFading()) {
            logger::debug("SettingsMenu open rejected: map fading");
            return false;
        }

        const Host host = MapOpen(ui) ? Host::Map : Host::Gameplay;

        if (host == Host::Map) {
            // Block opening while SkyUI's Find Location finder is up
            if (auto mapMenu = ui->GetMenu<RE::MapMenu>()) {
                auto& root = mapMenu->GetRuntimeData2()->mapMovie;
                if (root.IsObject() || root.IsDisplayObject()) {
                    RE::GFxValue lf, shown;
                    if (root.GetMember("_locationFinder", &lf) &&
                        lf.GetMember("_bShown", &shown) &&
                        shown.GetBool()) {
                        logger::debug("SettingsMenu open rejected: SkyUI location finder active");
                        return false;
                    }
                }
            }
            if (ui->IsMenuOpen(RE::JournalMenu::MENU_NAME) ||
                ui->IsMenuOpen(RE::Console::MENU_NAME) ||
                ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME)) {
                logger::debug("SettingsMenu open rejected: modal menu open over map");
                return false;
            }
        } else if (host == Host::Gameplay) {
            if (MenuUtils::ShouldBlockOverlay()) {
                logger::debug("SettingsMenu open rejected: overlay blocked by active menu");
                return false;
            }
            if (PartySheetState::IsFullscreenPanelOpen()) {
                logger::debug("SettingsMenu open rejected: Party Sheet fullscreen panel open");
                return false;
            }
        }

        g_host.store(host);
        g_isVisible.store(true);
        logger::debug("SettingsMenu opened (host: {})", host == Host::Map ? "Map" : "Gameplay");
        return true;
    }

    bool Toggle() {
        if (g_isVisible.load()) {
            Hide("toggle");
            return true;
        }
        return Show();
    }

    static char g_toastMessage[256] = "";

    enum class Tab { General, Hotkeys, Appearance, Map, PathTracking, RadarInspect, Beacons, Count };
    static Tab g_activeTab = Tab::General;

    // Gold sub-section label inside a panel. Matches the note dialog / filter
    // panel title color so the whole family reads as one UI.
    static void SectionLabel(const char* text) {
        ImGui::TextColored(MapTheme::kDlgTitleText(), "%s", text);
    }

    struct FlowItem { std::string label; bool* value; };
    static void CheckboxFlow(const FlowItem* items, int count) {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float right   = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (int i = 0; i < count; ++i) {
            ImGui::Checkbox(items[i].label.c_str(), items[i].value);
            if (i + 1 < count) {
                const float glyph = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
                const float nextW = glyph + ImGui::CalcTextSize(items[i + 1].label.c_str(), nullptr, true).x;
                if (ImGui::GetItemRectMax().x + spacing + nextW < right) {
                    ImGui::SameLine();
                }
            }
        }
    }

    static void DrawTabGrid(Settings* settings) {
        struct TileDef { Tab tab; std::string label; };
        const std::string genLabel = LocalizationManager::Get("Settings.TabGeneral");
        const TileDef tiles[] = {
            { Tab::General,      genLabel == "Settings.TabGeneral" ? "General" : genLabel },
            { Tab::Hotkeys,      LocalizationManager::Get("Settings.TabHotkeys")      },
            { Tab::Appearance,   LocalizationManager::Get("Settings.TabAppearance")   },
            { Tab::Map,          LocalizationManager::Get("Settings.TabMap")          },
            { Tab::PathTracking, LocalizationManager::Get("Settings.TabPathTracking") },
            { Tab::RadarInspect, LocalizationManager::Get("Settings.TabRadarInspect") },
            { Tab::Beacons,      LocalizationManager::Get("Settings.TabBeacons")      },
        };
        constexpr int kPerRow = 3;

        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float avail   = ImGui::GetContentRegionAvail().x;
        const float tileW   = (avail - spacing * (kPerRow - 1)) / kPerRow;

        int col = 0;
        for (const auto& t : tiles) {
            if (col != 0) ImGui::SameLine();
            const bool active = (g_activeTab == t.tab);
            if (active) {
                // Selected tile: hold the active/pressed gold so it stays lit.
                ImGui::PushStyleColor(ImGuiCol_Button,        MapTheme::kDlgBtnActive());
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, MapTheme::kDlgBtnActive());
            }
            if (ImGui::Button(t.label.c_str(), ImVec2(tileW, 0.0f))) {
                g_activeTab = t.tab;
                // Auto-persist the tile choice (like window pos) so it survives a
                // reopen without needing the Save button.
                settings->menuActiveTab = static_cast<std::int32_t>(t.tab);
                settings->SavePositions();
            }
            if (active) ImGui::PopStyleColor(2);
            if (++col == kPerRow) col = 0;
        }
    }

    // ---- Per-tab content panels ----

    // Which bind row is capturing, by row id. Only one binds at a time.
    static std::string g_bindingId;

    static void RenderBindRow(Settings* settings, const char* label, const char* id,
                              std::int32_t* keyField, std::int32_t* modField,
                              bool allowUnbound = true) {
        if (!g_bindingId.empty() && !g_hkCaptureCb) {
            g_bindingId.clear();
        }

        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s:", label);
        ImGui::SameLine(190.0f);

        const std::string btnId = std::string("##bind_") + id;

        if (g_bindingId == id) {
            ImGui::TextColored(MapTheme::kDlgTitleText(), "%s",
                               LocalizationManager::Get("Settings.PressAnyKey").c_str());
            ImGui::SameLine();
            if (ImGui::Button((LocalizationManager::Get("Settings.Cancel") + btnId).c_str())) {
                g_bindingId.clear();
                g_hkCaptureCb = nullptr;
            }
            return;
        }

        std::string lbl = (allowUnbound && *keyField < 1)
            ? LocalizationManager::Get("Settings.Unbound")
            : InputHandler::FormatHotkeyWithModifier(*keyField, *modField);
        lbl += btnId;

        if (ImGui::Button(lbl.c_str())) {
            g_bindingId = id;
            g_hkCaptureArmedAt = std::chrono::steady_clock::now();
            g_hkCaptureCb = [settings, keyField, modField, idCopy = std::string(id)](std::uint32_t code) {
                *keyField = static_cast<std::int32_t>(code);
                *modField = static_cast<std::int32_t>(InputHandler::GetHeldModifier());
                settings->Save();
                if (g_bindingId == idCopy) g_bindingId.clear();
            };
        }
        // Right-click clears just the modifier (back to a plain key), not the bind.
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            *modField = 0;
            settings->Save();
        }
        if (allowUnbound && *keyField >= 1) {
            ImGui::SameLine();
            if (ImGui::SmallButton((LocalizationManager::Get("Settings.Unbind") + btnId).c_str())) {
                *keyField = -1;
                *modField = 0;
                settings->Save();
            }
        }
    }

    static void RenderControllerButtonRow(Settings* settings, const char* label,
                                          const char* id, std::int32_t* field) {
        const std::string unbound = LocalizationManager::Get("Settings.Unbound");
        const char* names[] = {
            unbound == "Settings.Unbound" ? "Unbound" : unbound.c_str(),
            InputHandler::DIKToKeyName(266), InputHandler::DIKToKeyName(267),
            InputHandler::DIKToKeyName(268), InputHandler::DIKToKeyName(269),
            InputHandler::DIKToKeyName(270), InputHandler::DIKToKeyName(271),
            InputHandler::DIKToKeyName(272), InputHandler::DIKToKeyName(273),
            InputHandler::DIKToKeyName(274), InputHandler::DIKToKeyName(275),
            InputHandler::DIKToKeyName(276), InputHandler::DIKToKeyName(277),
            InputHandler::DIKToKeyName(278), InputHandler::DIKToKeyName(279),
            InputHandler::DIKToKeyName(280), InputHandler::DIKToKeyName(281)
        };
        int current = (*field >= 266 && *field <= 281) ? *field - 265 : 0;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s:", label);
        ImGui::SameLine(190.0f);
        ImGui::SetNextItemWidth(140.0f);
        const std::string comboId = std::string("##controller_") + id;
        if (ImGui::Combo(comboId.c_str(), &current, names, IM_ARRAYSIZE(names))) {
            *field = current == 0 ? -1 : current + 265;
            settings->Save();
        }
    }

    static void RenderControllerInspectSettings(Settings* settings) {
        SectionLabel(LocalizationManager::Get("Settings.InspectHold").c_str());
        RenderControllerButtonRow(settings, LocalizationManager::Get("Settings.ControllerOpenInspect").c_str(), "inspect_open",
                                  &settings->controllerInspectOpenButton);
        {
            const std::string single = LocalizationManager::Get("Settings.ControllerSinglePress");
            const std::string doublePress = LocalizationManager::Get("Settings.ControllerDoublePress");
            const std::string longPress = LocalizationManager::Get("Settings.ControllerLongPress");
            const char* gestures[] = { single.c_str(), doublePress.c_str(), longPress.c_str() };
            int current = std::clamp(settings->controllerInspectOpenGesture, 0, 2);
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s:", LocalizationManager::Get("Settings.ControllerOpenGesture").c_str());
            ImGui::SameLine(190.0f);
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::Combo("##controller_inspect_gesture", &current, gestures, IM_ARRAYSIZE(gestures))) {
                settings->controllerInspectOpenGesture = current;
                settings->Save();
            }
        }
        RenderControllerButtonRow(settings, LocalizationManager::Get("Settings.ControllerAddMarker").c_str(), "inspect_add_marker",
                                  &settings->controllerInspectAddMarkerButton);
        RenderControllerButtonRow(settings, LocalizationManager::Get("Settings.ControllerSwitchView").c_str(), "inspect_switch_view",
                                  &settings->controllerInspectSwitchViewButton);
        RenderControllerButtonRow(settings, LocalizationManager::Get("Settings.PathTrackingKey").c_str(), "inspect_path_tracking",
                                  &settings->controllerInspectPathTrackingButton);
        ImGui::TextDisabled("%s", LocalizationManager::Get("Settings.ControllerBackClose").c_str());
    }

    static void PanelGeneral(Settings* settings) {
        if (!settings) return;

        SectionLabel(LocalizationManager::Get("Settings.TabBeacons").c_str());
        {
            const FlowItem items[] = {
                { LocalizationManager::Get("Settings.ShowLootBeams"), &settings->lootBeamVisible },
                { LocalizationManager::Get("Settings.ShowBeacons"),   &settings->navBeaconVisible },
                { LocalizationManager::Get("Settings.EnableLookPin"), &settings->lookPinEnabled },
            };
            CheckboxFlow(items, 3);
        }
        ImGui::Spacing();
        ImGui::Separator();

        SectionLabel(LocalizationManager::Get("Settings.GlanceRadarHeader").c_str());
        {
            const FlowItem items[] = {
                { LocalizationManager::Get("Settings.ShowIndoors") + "##glance_gen",  &settings->navGlanceInteriorVisible },
                { LocalizationManager::Get("Settings.ShowOutdoors") + "##glance_gen", &settings->navGlanceExteriorVisible },
            };
            CheckboxFlow(items, 2);
        }
        ImGui::Spacing();
        ImGui::Separator();

        SectionLabel(LocalizationManager::Get("Settings.InspectModeHeader").c_str());
        {
            const FlowItem items[] = {
                { LocalizationManager::Get("Settings.ShowIndoors") + "##inspect_gen",  &settings->navInspectInteriorVisible },
                { LocalizationManager::Get("Settings.ShowOutdoors") + "##inspect_gen", &settings->navInspectExteriorVisible },
            };
            CheckboxFlow(items, 2);
        }
        ImGui::Spacing();
        ImGui::Separator();

        SectionLabel(LocalizationManager::Get("Settings.TabPathTracking").c_str());
        {
            const FlowItem items[] = {
                { LocalizationManager::Get("Settings.EnablePathTracking"),             &settings->enablePathTracking },
                { LocalizationManager::Get("Settings.ShowOnLocalMap") + "##gen",       &settings->pathTrailVisible },
                { LocalizationManager::Get("Settings.ShowOnGlanceRadar") + "##gen",    &settings->navGlanceShowTrail },
                { LocalizationManager::Get("Settings.ShowOnInspectOverlay") + "##gen", &settings->navInspectShowTrail },
            };
            CheckboxFlow(items, 4);
        }
        ImGui::Spacing();
        ImGui::Separator();

        SectionLabel(LocalizationManager::Get("Settings.TabMap").c_str());
        {
            const FlowItem items[] = {
                { LocalizationManager::Get("Settings.EnableBadges"),     &settings->enableBadges },
                { LocalizationManager::Get("Settings.ShowLegend"),       &settings->showFilterPanel },
                { LocalizationManager::Get("Settings.ShowMarkerImages"), &settings->markerImageEnabled },
            };
            CheckboxFlow(items, 3);
        }
    }

    static void PanelHotkeys(Settings* settings) {
        if (!settings) return;

        SectionLabel(LocalizationManager::Get("Settings.SectionMap").c_str());
        // SMF remains available if the standalone settings hotkey is unbound.
        RenderBindRow(settings, LocalizationManager::Get("Settings.HudSettings").c_str(), "settings",
                      &settings->menuSettingsKey, &settings->menuSettingsModifier, true);
        RenderBindRow(settings, LocalizationManager::Get("Settings.MarkerListPanel").c_str(), "listpanel",
                      &settings->listPanelKey, &settings->listPanelModifier);
        RenderBindRow(settings, LocalizationManager::Get("Settings.PathTrackingKey").c_str(), "pathtrack",
                      &settings->pathTrackKey, &settings->pathTrackModifier);

        ImGui::Spacing();
        SectionLabel(LocalizationManager::Get("Settings.SectionNavigationOverlay").c_str());
        RenderBindRow(settings, LocalizationManager::Get("Settings.InspectHold").c_str(), "inspect",
                      &settings->navInspectKey, &settings->navInspectModifier);
        {
            const std::string holdStr = LocalizationManager::Get("Settings.ActivationHold");
            const std::string toggleStr = LocalizationManager::Get("Settings.ActivationToggle");
            const char* modes[] = {
                holdStr == "Settings.ActivationHold" ? "Hold" : holdStr.c_str(),
                toggleStr == "Settings.ActivationToggle" ? "Toggle" : toggleStr.c_str()
            };
            const std::string modeLabel = LocalizationManager::Get("Settings.InspectActivation");
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s:", modeLabel == "Settings.InspectActivation" ? "Activation" : modeLabel.c_str());
            ImGui::SameLine(190.0f);
            ImGui::SetNextItemWidth(140.0f);
            if (ImGui::Combo("##inspect_activation", &settings->navInspectActivation, modes, IM_ARRAYSIZE(modes))) {
                settings->Save();
            }
        }
        RenderBindRow(settings, LocalizationManager::Get("Settings.InspectModeToggle").c_str(), "inspectmode",
                      &settings->navInspectModeToggleKey, &settings->navInspectModeToggleModifier);
        RenderBindRow(settings, LocalizationManager::Get("Settings.QuickMark").c_str(), "quickmark",
                      &settings->navQuickMarkKey, &settings->navQuickMarkModifier);
        RenderBindRow(settings, LocalizationManager::Get("Settings.MiniRadarToggleKey").c_str(), "glancetoggle",
                      &settings->navGlanceToggleKey, &settings->navGlanceToggleModifier);
        RenderBindRow(settings, LocalizationManager::Get("Settings.LootBeamToggleKey").c_str(), "lootbeamtoggle",
                      &settings->lootBeamToggleKey, &settings->lootBeamToggleModifier);

        ImGui::Spacing();
        SectionLabel(LocalizationManager::Get("Settings.SectionLookPin").c_str());
        RenderBindRow(settings, LocalizationManager::Get("Settings.LookPinKey").c_str(), "lookpin",
                      &settings->lookPinKey, &settings->lookPinModifier);

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::CollapsingHeader(LocalizationManager::Get("Settings.Controller").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderControllerInspectSettings(settings);
        }
    }

    static void PanelAppearance(Settings* settings) {
        SectionLabel(LocalizationManager::Get("Settings.SectionGlobalStyles").c_str());
        // Color edits use NoInputs, so each control is a swatch and label.
        // Keep the next one on this line only when its full label fits.
        auto flowNext = [](const std::string& label) {
            const auto& style = ImGui::GetStyle();
            const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            const float nextW = ImGui::GetFrameHeight() + style.ItemInnerSpacing.x +
                                ImGui::CalcTextSize(label.c_str()).x;
            if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + nextW < right) {
                ImGui::SameLine();
            }
        };
        MapDraw::ColorEdit3Hex(LocalizationManager::Get("Settings.TitleColor").c_str(), settings->titleColorR, settings->titleColorG,
                               settings->titleColorB);
        flowNext(LocalizationManager::Get("Settings.BodyColor"));
        MapDraw::ColorEdit3Hex(LocalizationManager::Get("Settings.BodyColor").c_str(), settings->bodyColorR, settings->bodyColorG,
                               settings->bodyColorB);
        flowNext(LocalizationManager::Get("Settings.Background"));
        // One background for every map-drawn panel (list panel, Map Filters).
        MapDraw::ColorEdit4Hex(LocalizationManager::Get("Settings.Background").c_str(), settings->bgColorR, settings->bgColorG,
                               settings->bgColorB, settings->bgColorA);
        flowNext(LocalizationManager::Get("Settings.Border"));
        MapDraw::ColorEdit4Hex(LocalizationManager::Get("Settings.Border").c_str(), settings->borderColorR, settings->borderColorG,
                               settings->borderColorB, settings->borderColorA);
        static int s_pendingInterfaceScale = -1;
        if (s_pendingInterfaceScale < 0) {
            s_pendingInterfaceScale = settings->interfaceScale;
        }
        if (ImGui::SliderInt(LocalizationManager::Get("Settings.InterfaceScale").c_str(), &s_pendingInterfaceScale, 50, 200, "%d%%")) {
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            settings->interfaceScale = s_pendingInterfaceScale;
            settings->Save();
        } else if (!ImGui::IsItemActive()) {
            s_pendingInterfaceScale = settings->interfaceScale;
        }

        ImGui::SliderFloat(LocalizationManager::Get("Settings.TitleScale").c_str(), &settings->titleScale, 0.5f, 1.35f, "%.2f");
        ImGui::SliderFloat(LocalizationManager::Get("Settings.BodyScale").c_str(),  &settings->bodyScale,  0.5f, 1.35f, "%.2f");
        // Notes are free text and can run long, so they get their own scale,
        // independent of Body Scale. Read live by HoverImageRenderer::Draw.
        ImGui::SliderFloat(LocalizationManager::Get("Settings.NotesScale").c_str(), &settings->noteTextScale, 0.5f, 3.0f, "%.2f");

        ImGui::Spacing();
        ImGui::Separator();
        SectionLabel(LocalizationManager::Get("Settings.SectionIconDesign").c_str());
        MapDraw::ColorEdit4Hex(LocalizationManager::Get("Settings.PinTint").c_str(), settings->pinTintR, settings->pinTintG,
                               settings->pinTintB, settings->pinTintA);
        flowNext(LocalizationManager::Get("Settings.Icons3D"));
        ImGui::Checkbox(LocalizationManager::Get("Settings.Icons3D").c_str(), &settings->iconFx3D);
        const std::string iconShadowLabel = LocalizationManager::Get("Settings.BgShadow");
        flowNext(iconShadowLabel);
        ImGui::Checkbox((iconShadowLabel + "##IconShadow").c_str(), &settings->iconShadow);

        ImGui::Spacing();
        ImGui::Separator();
        SectionLabel(LocalizationManager::Get("Settings.SectionBackgroundDesign").c_str());
        const FlowItem backgroundItems[] = {
            { LocalizationManager::Get("Settings.BgShadow"),    &settings->bgShadow },
            { LocalizationManager::Get("Settings.BgGradient"),  &settings->bgGradient },
            { LocalizationManager::Get("Settings.BgVignette"),  &settings->bgVignette },
            { LocalizationManager::Get("Settings.BgHighlight"), &settings->bgHighlight },
            { LocalizationManager::Get("Settings.BgBorder"),    &settings->bgBorder },
        };
        CheckboxFlow(backgroundItems, 5);
    }

    static void PanelStatCard(Settings* settings);
    static void PanelMarkers(Settings* settings);
    static void PanelMarkerImages(Settings* settings);

    static void RenderBackgroundPicker(const char* popupId, std::int32_t& currentIndex,
                                       BackgroundManager::Type type) {
        const int count = BackgroundManager::GetBackgroundCount(type);

        std::string label = (count > 0 && currentIndex >= 1 && currentIndex <= count)
                                ? BackgroundManager::GetBackgroundName(type, currentIndex)
                                : LocalizationManager::Get("Settings.BgNone");

        if (ImGui::Button((label + "##btn" + popupId).c_str())) {
            ImGui::OpenPopup(popupId);
        }

        if (ImGui::BeginPopup(popupId)) {
            ImGui::TextUnformatted(LocalizationManager::Get("Settings.SelectBackground").c_str());
            ImGui::Separator();

            auto entries = BackgroundManager::GetBackgroundEntries(type);
            if (entries.empty()) {
                ImGui::TextDisabled("%s", LocalizationManager::Get("Settings.BgNoneFound").c_str());
            } else {
                const ImGuiWindowFlags childFlags = (static_cast<int>(entries.size()) > 8)
                                                        ? ImGuiWindowFlags_AlwaysVerticalScrollbar
                                                        : 0;
                ImGui::BeginChild("BgGrid", ImVec2(430.0f, 350.0f), false, childFlags);
                constexpr int kCols = 4;
                const auto&   style = ImGui::GetStyle();
                const float   availWidth   = ImGui::GetContentRegionAvail().x;
                const float   totalSpacing = static_cast<float>(kCols - 1) * style.ItemSpacing.x;
                const float   buttonWidth  = std::floor((availWidth - totalSpacing) / static_cast<float>(kCols));
                const float   kW           = std::max(1.0f, buttonWidth - (style.FramePadding.x * 2.0f));
                const float   kH           = std::floor(kW * (160.0f / 90.0f));

                for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
                    if (i % kCols != 0) ImGui::SameLine();
                    ImGui::PushID(i);
                    if (ImGui::ImageButton("##bg", reinterpret_cast<ImTextureID>(entries[i].texture),
                                           ImVec2(kW, kH))) {
                        currentIndex = i + 1;
                        ImGui::CloseCurrentPopup();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", entries[i].filename.c_str());
                    }
                    ImGui::PopID();
                }
                // EndChild is unconditional: BeginChild pushes state even when
                // it returns false, so guarding it leaks the child window.
                ImGui::EndChild();
            }
            ImGui::EndPopup();
        }
    }

    static void PanelMap(Settings* settings) {
        if (ImGui::CollapsingHeader(LocalizationManager::Get("Settings.ListPanelHeader").c_str())) {
            ImGui::Indent();
            ImGui::SliderInt(LocalizationManager::Get("Settings.Width").c_str(), &settings->listPanelWidth, 200, 600);
            ImGui::SliderInt(LocalizationManager::Get("Settings.MinHeightPct").c_str(), &settings->listPanelMinHeightPct, 0, 95, "%d%%");
            ImGui::SliderInt(LocalizationManager::Get("Settings.MaxHeightPct").c_str(), &settings->listPanelMaxHeightPct, 30, 95, "%d%%");
            // Overview-tab sub-block toggles. All three live on the Overview tab; off
            // also skips that block's per-frame Refresh(), not just its draw.
            ImGui::Checkbox(LocalizationManager::Get("Settings.ShowOverview").c_str(), &settings->listShowOverview);
            if (settings->listShowOverview) {
                ImGui::SameLine();
                ImGui::Checkbox(LocalizationManager::Get("Settings.ShowCompanions").c_str(), &settings->listShowCompanions);
            }
            ImGui::Checkbox(LocalizationManager::Get("Settings.ShowActiveQuests").c_str(), &settings->listShowActiveQuests);

            ImGui::Spacing();
            ImGui::Checkbox(LocalizationManager::Get("Settings.CustomBg").c_str(),
                            &settings->listPanelUseCustomBg);
            if (settings->listPanelUseCustomBg) {
                ImGui::SameLine();
                RenderBackgroundPicker("ListPanelBgPicker", settings->listPanelBgIndex,
                                       BackgroundManager::Type::ListPanel);
            }
            ImGui::Unindent();
        }

        if (ImGui::CollapsingHeader(LocalizationManager::Get("Settings.StatCardHeader").c_str())) {
            ImGui::Indent();
            PanelStatCard(settings);
            ImGui::Unindent();
        }

        if (ImGui::CollapsingHeader(LocalizationManager::Get("Settings.CustomPinsHeader").c_str())) {
            ImGui::Indent();
            ImGui::SliderFloat(LocalizationManager::Get("Settings.IconScale").c_str(), &settings->markerIconScale, 0.5f, 2.0f, "%.2f");
            ImGui::Unindent();
        }

        if (ImGui::CollapsingHeader(LocalizationManager::Get("Settings.BadgesHeader").c_str())) {
            ImGui::Indent();
            PanelMarkers(settings);
            ImGui::Unindent();
        }

        if (ImGui::CollapsingHeader(LocalizationManager::Get("Settings.MarkerImagesHeader").c_str())) {
            ImGui::Indent();
            PanelMarkerImages(settings);
            ImGui::Unindent();
        }
    }

    static void PanelStatCard(Settings* settings) {
        ImGui::Checkbox(LocalizationManager::Get("Settings.ShowMeta").c_str(), &settings->statCardShowDetails);
        ImGui::Indent();
        ImGui::BeginDisabled(!settings->statCardShowDetails);
        const FlowItem rowItems[] = {
            { LocalizationManager::Get("Settings.StatCardRowType"),       &settings->statCardShowType },
            { LocalizationManager::Get("Settings.StatCardRowHold"),       &settings->statCardShowHold },
            { LocalizationManager::Get("Settings.StatCardRowDistance"),   &settings->statCardShowDistance },
            { LocalizationManager::Get("Settings.StatCardRowBounty"),     &settings->statCardShowBounty },
            { LocalizationManager::Get("Settings.StatCardRowCleared"),    &settings->statCardShowCleared },
            { LocalizationManager::Get("Settings.StatCardRowBoss"),       &settings->statCardShowBoss },
            { LocalizationManager::Get("Settings.StatCardRowQuests"),     &settings->statCardShowQuests },
            { LocalizationManager::Get("Settings.StatCardRowServices"),   &settings->statCardShowServices },
            { LocalizationManager::Get("Settings.StatCardRowPlayerHome"), &settings->statCardShowPlayerHome },
            { LocalizationManager::Get("Settings.StatCardRowSpouse"),     &settings->statCardShowSpouse },
            { LocalizationManager::Get("Settings.StatCardRowCompanions"), &settings->statCardShowCompanions },
        };
        CheckboxFlow(rowItems, 11);
        const std::string unitMetric   = LocalizationManager::Get("Settings.DistanceUnitsMetric");
        const std::string unitImperial = LocalizationManager::Get("Settings.DistanceUnitsImperial");
        const char* distanceUnits[] = { unitMetric.c_str(), unitImperial.c_str() };
        ImGui::Combo(LocalizationManager::Get("Settings.DistanceUnits").c_str(), &settings->statCardDistanceUnits, distanceUnits, IM_ARRAYSIZE(distanceUnits));
        ImGui::EndDisabled();
        ImGui::Unindent();
        ImGui::Spacing();
        SectionLabel(LocalizationManager::Get("Settings.SectionVanillaCardPlacement").c_str());
        // Direction the card sits from the hovered VANILLA marker. Offsets nudge
        // on top. Read live by HoverImageRenderer::Draw.
        const std::string anchorTop    = LocalizationManager::Get("Settings.AnchorTop");
        const std::string anchorBottom = LocalizationManager::Get("Settings.AnchorBottom");
        const std::string anchorLeft   = LocalizationManager::Get("Settings.AnchorLeft");
        const std::string anchorRight  = LocalizationManager::Get("Settings.AnchorRight");
        const char* anchors[] = { anchorTop.c_str(), anchorBottom.c_str(), anchorLeft.c_str(), anchorRight.c_str() };
        ImGui::Combo(LocalizationManager::Get("Settings.Anchor").c_str(), &settings->statCardAnchor, anchors, IM_ARRAYSIZE(anchors));
        ImGui::SliderInt(LocalizationManager::Get("Settings.OffsetX").c_str(), &settings->statCardOffsetX, -300, 300);
        ImGui::SliderInt(LocalizationManager::Get("Settings.OffsetY").c_str(), &settings->statCardOffsetY, -300, 300);

        ImGui::Spacing();
        ImGui::Separator();
        SectionLabel(LocalizationManager::Get("Settings.SectionCustomCardPlacement").c_str());
        ImGui::SliderInt((LocalizationManager::Get("Settings.OffsetX") + "##hover").c_str(), &settings->markerHoverOffsetX, -300, 300);
        ImGui::SliderInt((LocalizationManager::Get("Settings.OffsetY") + "##hover").c_str(), &settings->markerHoverOffsetY, -300, 300);

    }

    static void PanelPathTracking(Settings* settings) {
        SectionLabel(LocalizationManager::Get("Settings.SectionTrail").c_str());
        // RGBA: alpha lives in the color itself (picker's alpha bar) rather
        // than a separate slider.
        MapDraw::ColorEdit4Hex(LocalizationManager::Get("Settings.Color").c_str(), settings->pathTrailColorR, settings->pathTrailColorG,
                               settings->pathTrailColorB, settings->pathTrailColorA);
        ImGui::SliderFloat(LocalizationManager::Get("Settings.Thickness").c_str(), &settings->pathTrailThickness, 1.0f, 8.0f, "%.1f");
        {
            const FlowItem items[] = {
                { LocalizationManager::Get("Settings.FadeOlderSegments"), &settings->pathTrailFadeByAge },
                { LocalizationManager::Get("Settings.SmartSpacing"),      &settings->pathTrailSmartSpacing },
            };
            CheckboxFlow(items, 2);
        }
        ImGui::Spacing();

        ImGui::Separator();
        SectionLabel(LocalizationManager::Get("Settings.SectionCurrentPositionPulse").c_str());
        ImGui::Checkbox(LocalizationManager::Get("Settings.EnablePulse").c_str(), &settings->pathPulseEnabled);
        MapDraw::ColorEdit3Hex(LocalizationManager::Get("Settings.PulseColor").c_str(), settings->pathPulseColorR, settings->pathPulseColorG,
                               settings->pathPulseColorB);
    }

    static void PanelRadarInspect(Settings* settings) {
        if (ImGui::CollapsingHeader(LocalizationManager::Get("Settings.SectionGeneral").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent();
            ImGui::Checkbox(LocalizationManager::Get("Settings.HideInBeastForm").c_str(),
                            &settings->hideInBeastForm);
            ImGui::Checkbox(LocalizationManager::Get("Settings.EntitiesRequireLOS").c_str(), &settings->navEntityRequireLOS);
            ImGui::Spacing();
            {
                float rangeMeters = settings->navMarkerRange / Settings::kUnitsPerMeter;
                // Label reads with its value: "Show Markers Within  1000 m".
                if (ImGui::SliderFloat(LocalizationManager::Get("Settings.ShowMarkersWithin").c_str(), &rangeMeters, 100.0f, 5000.0f, "%.0f m")) {
                    settings->navMarkerRange = rangeMeters * Settings::kUnitsPerMeter;
                }
            }
            // Count cap on active quest markers (both views). 0 = none. Visible
            // and nearby targets get the slots first; this is not a distance gate.
            ImGui::SliderInt(LocalizationManager::Get("Settings.MaxQuestMarkers").c_str(), &settings->navMaxQuestMarkers, 0, 10);
            {
                const std::string unlimitedLabel = LocalizationManager::Get("Settings.Unlimited");
                ImGui::SliderInt(LocalizationManager::Get("Settings.MaxCustomMarkers").c_str(), &settings->navMaxCustomMarkers, 0, 30,
                                 settings->navMaxCustomMarkers == 0 ? unlimitedLabel.c_str() : "%d");
            }
            ImGui::Unindent();
        }
        ImGui::Spacing();

        if (ImGui::CollapsingHeader(LocalizationManager::Get("Settings.EntityIconStyle").c_str())) {
            ImGui::Indent();
            MapDraw::ColorEdit4Hex(LocalizationManager::Get("Settings.Background").c_str(),
                                   settings->navEntityIconBgR, settings->navEntityIconBgG,
                                   settings->navEntityIconBgB, settings->navEntityIconBgA);
            MapDraw::ColorEdit4Hex(LocalizationManager::Get("Settings.Border").c_str(),
                                   settings->navEntityIconBorderR, settings->navEntityIconBorderG,
                                   settings->navEntityIconBorderB, settings->navEntityIconBorderA);
            ImGui::SliderFloat(LocalizationManager::Get("Settings.Padding").c_str(),
                               &settings->navEntityIconPadding, 0.0f, 8.0f, "%.1f px");
            ImGui::Checkbox(LocalizationManager::Get("Settings.Icons3D").c_str(), &settings->navEntityIcon3D);
            ImGui::Checkbox(LocalizationManager::Get("Settings.BgShadow").c_str(), &settings->navEntityIconShadow);
            ImGui::Unindent();
        }

        if (ImGui::CollapsingHeader(LocalizationManager::Get("Settings.ActorsHeader").c_str())) {
            ImGui::Indent();
            {
                const FlowItem items[] = {
                    { LocalizationManager::Get("Settings.ShowAllies"),      &settings->navActorShowAlly },
                    { LocalizationManager::Get("Settings.ShowHostiles"),    &settings->navActorShowHostile },
                    { LocalizationManager::Get("Settings.ShowNeutralNPCs"), &settings->navActorShowNeutral },
                    { LocalizationManager::Get("Settings.ShowWildlife"),    &settings->navActorShowAnimal },
                };
                CheckboxFlow(items, 4);
            }
            // Live-track range: actor dots move in real time inside this radius,
            // freeze + fade outside it.
            ImGui::SliderFloat(LocalizationManager::Get("Settings.DetectionRange").c_str(), &settings->navActorProximityUnits, 200.0f, 4000.0f, "%.0f");
            ImGui::Unindent();
        }

        if (ImGui::CollapsingHeader(LocalizationManager::Get("Settings.GlanceRadarHeader").c_str())) {
            ImGui::Indent();
            ImGui::SliderInt((LocalizationManager::Get("Settings.Size") + "##glance").c_str(), &settings->navGlanceSize, 100, 500);
            {
                const std::string circle = LocalizationManager::Get("Settings.Circle");
                const std::string square = LocalizationManager::Get("Settings.Square");
                const char* shapes[] = { circle.c_str(), square.c_str() };
                ImGui::Combo(LocalizationManager::Get("Settings.GlanceShape").c_str(),
                             &settings->navGlanceShape, shapes, IM_ARRAYSIZE(shapes));
            }
            {
                const std::string fixedNorth = LocalizationManager::Get("Settings.GlanceFixedNorth");
                const std::string followCamera = LocalizationManager::Get("Settings.GlanceFollowCamera");
                const char* orientations[] = { fixedNorth.c_str(), followCamera.c_str() };
                ImGui::Combo(LocalizationManager::Get("Settings.GlanceOrientation").c_str(),
                             &settings->navGlanceOrientation, orientations, IM_ARRAYSIZE(orientations));
            }
            ImGui::Checkbox(LocalizationManager::Get("Settings.GlanceShowCardinals").c_str(), &settings->navGlanceShowCardinals);
            ImGui::Checkbox(LocalizationManager::Get("Settings.GlanceShowLocation").c_str(), &settings->navGlanceShowLocation);
            ImGui::SliderInt((LocalizationManager::Get("Settings.PinSize") + "##glance").c_str(), &settings->navGlancePinSize, 6, 24);
            ImGui::SliderInt((LocalizationManager::Get("Settings.ActorDotSize") + "##glance").c_str(), &settings->navGlanceActorDotSize, 2, 16);
            ImGui::SliderInt((LocalizationManager::Get("Settings.EntityIconSize") + "##glance").c_str(), &settings->navGlanceEntityIconSize, 12, 24);
            // World units from disc center to rim. Smaller = zoomed in; also
            // tightens the snapshot frustum pad so the backdrop sharpens.
            ImGui::SliderFloat((LocalizationManager::Get("Settings.ZoomMap") + "##glance").c_str(), &settings->navGlanceRangeUnits, 400.0f, 3000.0f, "%.0f");
            ImGui::Spacing();

            {
                const ImVec2 disp = ImGui::GetIO().DisplaySize;
                const int drawnMaxX = GlanceBounds::maxPosX.load();
                const int drawnMaxY = GlanceBounds::maxPosY.load();
                const int maxX = drawnMaxX >= 0 ? drawnMaxX : std::max(0, static_cast<int>(disp.x) - settings->navGlanceSize);
                const int maxY = drawnMaxY >= 0 ? drawnMaxY : std::max(0, static_cast<int>(disp.y) - settings->navGlanceSize);
                ImGui::SliderInt((LocalizationManager::Get("Settings.PositionX") + "##glance").c_str(), &settings->navGlancePosX, 0, maxX);
                ImGui::SliderInt((LocalizationManager::Get("Settings.PositionY") + "##glance").c_str(), &settings->navGlancePosY, 0, maxY);
                if (ImGui::Button((LocalizationManager::Get("Settings.ResetPosition") + "##glance").c_str())) {
                    settings->navGlancePosX = maxX - 22;
                    settings->navGlancePosY = 22;
                }
            }
            ImGui::Spacing();

            if (ImGui::TreeNode((LocalizationManager::Get("Settings.EntitiesTree") + "##glance").c_str())) {
                SectionLabel(LocalizationManager::Get("Settings.Containers").c_str());
                {
                    const FlowItem items[] = {
                        { LocalizationManager::Get("Settings.Chests") + "##glance",      &settings->navGlanceEntContChest },
                        { LocalizationManager::Get("Settings.Barrels") + "##glance",     &settings->navGlanceEntContBarrel },
                        { LocalizationManager::Get("Settings.Sacks") + "##glance",       &settings->navGlanceEntContSack },
                        { LocalizationManager::Get("Settings.Urns") + "##glance",        &settings->navGlanceEntContUrn },
                        { LocalizationManager::Get("Settings.Coffins") + "##glance",     &settings->navGlanceEntContCoffin },
                        { LocalizationManager::Get("Settings.Strongboxes") + "##glance", &settings->navGlanceEntContStrongbox },
                    };
                    CheckboxFlow(items, 6);
                }
                SectionLabel(LocalizationManager::Get("Settings.LooseLoot").c_str());
                {
                    const FlowItem items[] = {
                        { LocalizationManager::Get("Settings.Weapons") + "##glance",   &settings->navGlanceEntLootWeapon },
                        { LocalizationManager::Get("Settings.Ammo") + "##glance",      &settings->navGlanceEntLootAmmo },
                        { LocalizationManager::Get("Settings.Armor") + "##glance",     &settings->navGlanceEntLootArmor },
                        { LocalizationManager::Get("Settings.Gold") + "##glance",      &settings->navGlanceEntLootGold },
                        { LocalizationManager::Get("Settings.Keys") + "##glance",      &settings->navGlanceEntLootKey },
                        { LocalizationManager::Get("Settings.Potions") + "##glance",   &settings->navGlanceEntLootPotion },
                        { LocalizationManager::Get("Settings.Food") + "##glance",      &settings->navGlanceEntLootFood },
                        { LocalizationManager::Get("Settings.Drinks") + "##glance",    &settings->navGlanceEntLootDrink },
                        { LocalizationManager::Get("Settings.Books") + "##glance",     &settings->navGlanceEntLootBook },
                        { LocalizationManager::Get("Settings.SoulGems") + "##glance",  &settings->navGlanceEntLootSoulGem },
                        { LocalizationManager::Get("Settings.Scrolls") + "##glance",   &settings->navGlanceEntLootScroll },
                    };
                    CheckboxFlow(items, 11);
                }
                SectionLabel(LocalizationManager::Get("Settings.Corpses").c_str());
                {
                    const FlowItem items[] = {
                        { LocalizationManager::Get("Settings.Humanoids") + "##glance", &settings->navGlanceEntCorpseHumanoid },
                        { LocalizationManager::Get("Settings.Creatures") + "##glance", &settings->navGlanceEntCorpseCreature },
                    };
                    CheckboxFlow(items, 2);
                }
                SectionLabel(LocalizationManager::Get("Settings.Doors").c_str());
                {
                    const FlowItem items[] = {
                        { LocalizationManager::Get("Settings.LoadDoors") + "##glance",    &settings->navGlanceEntDoorLoad },
                        { LocalizationManager::Get("Settings.RegularDoors") + "##glance", &settings->navGlanceEntDoorRegular },
                    };
                    CheckboxFlow(items, 2);
                }
                SectionLabel(LocalizationManager::Get("Settings.Ingredients").c_str());
                {
                    const FlowItem items[] = {
                        { LocalizationManager::Get("Settings.LooseIngredients") + "##glance", &settings->navGlanceEntIngrLoose },
                        { LocalizationManager::Get("Settings.Plants") + "##glance",            &settings->navGlanceEntIngrPlant },
                    };
                    CheckboxFlow(items, 2);
                }
                ImGui::TreePop();
            }
            ImGui::Unindent();
        }

        if (ImGui::CollapsingHeader(LocalizationManager::Get("Settings.InspectModeHeader").c_str())) {
            ImGui::Indent();
            ImGui::SliderInt((LocalizationManager::Get("Settings.SizePercent") + "##inspect").c_str(), &settings->navInspectSizePct, 50, 100);
            ImGui::SliderInt((LocalizationManager::Get("Settings.PinSize") + "##inspect").c_str(), &settings->navInspectPinSize, 12, 48);
            ImGui::SliderInt((LocalizationManager::Get("Settings.ActorDotSize") + "##inspect").c_str(), &settings->navInspectActorDotSize, 3, 24);
            ImGui::SliderInt((LocalizationManager::Get("Settings.EntityIconSize") + "##inspect").c_str(), &settings->navInspectEntityIconSize, 10, 40);
            // Radar-mode frustum width, world units. Only affects Inspect's
            // player-centred Radar framing, not the full-cell Cell mode.
            ImGui::SliderFloat((LocalizationManager::Get("Settings.ZoomMapRadarMode") + "##inspect").c_str(), &settings->navInspectRadarRangeUnits, 1000.0f, 8000.0f, "%.0f");
            ImGui::Spacing();

            if (ImGui::TreeNode((LocalizationManager::Get("Settings.EntitiesTree") + "##inspect").c_str())) {
                SectionLabel(LocalizationManager::Get("Settings.Containers").c_str());
                {
                    const FlowItem items[] = {
                        { LocalizationManager::Get("Settings.Chests") + "##inspect",      &settings->navInspectEntContChest },
                        { LocalizationManager::Get("Settings.Barrels") + "##inspect",     &settings->navInspectEntContBarrel },
                        { LocalizationManager::Get("Settings.Sacks") + "##inspect",       &settings->navInspectEntContSack },
                        { LocalizationManager::Get("Settings.Urns") + "##inspect",        &settings->navInspectEntContUrn },
                        { LocalizationManager::Get("Settings.Coffins") + "##inspect",     &settings->navInspectEntContCoffin },
                        { LocalizationManager::Get("Settings.Strongboxes") + "##inspect", &settings->navInspectEntContStrongbox },
                    };
                    CheckboxFlow(items, 6);
                }
                SectionLabel(LocalizationManager::Get("Settings.LooseLoot").c_str());
                {
                    const FlowItem items[] = {
                        { LocalizationManager::Get("Settings.Weapons") + "##inspect",   &settings->navInspectEntLootWeapon },
                        { LocalizationManager::Get("Settings.Ammo") + "##inspect",      &settings->navInspectEntLootAmmo },
                        { LocalizationManager::Get("Settings.Armor") + "##inspect",     &settings->navInspectEntLootArmor },
                        { LocalizationManager::Get("Settings.Gold") + "##inspect",      &settings->navInspectEntLootGold },
                        { LocalizationManager::Get("Settings.Keys") + "##inspect",      &settings->navInspectEntLootKey },
                        { LocalizationManager::Get("Settings.Potions") + "##inspect",   &settings->navInspectEntLootPotion },
                        { LocalizationManager::Get("Settings.Food") + "##inspect",      &settings->navInspectEntLootFood },
                        { LocalizationManager::Get("Settings.Drinks") + "##inspect",    &settings->navInspectEntLootDrink },
                        { LocalizationManager::Get("Settings.Books") + "##inspect",     &settings->navInspectEntLootBook },
                        { LocalizationManager::Get("Settings.SoulGems") + "##inspect",  &settings->navInspectEntLootSoulGem },
                        { LocalizationManager::Get("Settings.Scrolls") + "##inspect",   &settings->navInspectEntLootScroll },
                    };
                    CheckboxFlow(items, 11);
                }
                SectionLabel(LocalizationManager::Get("Settings.Corpses").c_str());
                {
                    const FlowItem items[] = {
                        { LocalizationManager::Get("Settings.Humanoids") + "##inspect", &settings->navInspectEntCorpseHumanoid },
                        { LocalizationManager::Get("Settings.Creatures") + "##inspect", &settings->navInspectEntCorpseCreature },
                    };
                    CheckboxFlow(items, 2);
                }
                SectionLabel(LocalizationManager::Get("Settings.Doors").c_str());
                {
                    const FlowItem items[] = {
                        { LocalizationManager::Get("Settings.LoadDoors") + "##inspect",    &settings->navInspectEntDoorLoad },
                        { LocalizationManager::Get("Settings.RegularDoors") + "##inspect", &settings->navInspectEntDoorRegular },
                    };
                    CheckboxFlow(items, 2);
                }
                SectionLabel(LocalizationManager::Get("Settings.Ingredients").c_str());
                {
                    const FlowItem items[] = {
                        { LocalizationManager::Get("Settings.LooseIngredients") + "##inspect", &settings->navInspectEntIngrLoose },
                        { LocalizationManager::Get("Settings.Plants") + "##inspect",            &settings->navInspectEntIngrPlant },
                    };
                    CheckboxFlow(items, 2);
                }
                ImGui::TreePop();
            }
            ImGui::Unindent();
        }
    }

    static void PanelBeacons(Settings* settings) {
        if (ImGui::CollapsingHeader(LocalizationManager::Get("Settings.WorldBeaconsHeader").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Indent();
            ImGui::SliderInt((LocalizationManager::Get("Settings.PinSize") + "##beacon").c_str(), &settings->navBeaconPinSize, 12, 48);
            // World units, not pixels: the beam is projected, so a taller pillar
            // stays visible from further away.
            ImGui::SliderFloat((LocalizationManager::Get("Settings.PillarHeight") + "##beacon").c_str(), &settings->navBeaconPillarHeight,
                               50.0f, 2000.0f, "%.0f");
            ImGui::SliderFloat((LocalizationManager::Get("Settings.NearHeight") + "##beacon").c_str(), &settings->navBeaconNearScale,
                               0.05f, 1.0f, "%.2f");
            {
                float fadeMeters = settings->navBeaconFadeNearUnits / Settings::kUnitsPerMeter;
                if (ImGui::SliderFloat((LocalizationManager::Get("Settings.FadeNear") + "##beacon").c_str(), &fadeMeters, 1.0f, 30.0f, "%.0f m")) {
                    settings->navBeaconFadeNearUnits = fadeMeters * Settings::kUnitsPerMeter;
                }
            }
            ImGui::Checkbox((LocalizationManager::Get("Settings.ClearOnArrival") + "##beacon").c_str(), &settings->navBeaconClearOnArrival);
            ImGui::Spacing();

            ImGui::Checkbox(LocalizationManager::Get("Settings.OffScreenArrow").c_str(), &settings->navBeaconEdgeIndicator);
            ImGui::Spacing();

            ImGui::Unindent();
        }

        if (ImGui::CollapsingHeader(LocalizationManager::Get("Settings.LootBeamsHeader").c_str())) {
            ImGui::Indent();
            {
                const std::string modern = LocalizationManager::Get("Settings.LootBeamModern");
                const std::string wisp = LocalizationManager::Get("Settings.LootBeamWisp");
                const char* designs[] = { modern.c_str(), wisp.c_str() };
                ImGui::Combo(LocalizationManager::Get("Settings.LootBeamDesign").c_str(),
                             &settings->lootBeamDesign, designs, IM_ARRAYSIZE(designs));
            }
            {
                const bool modern = settings->lootBeamDesign == 0;
                ImGui::SliderFloat((LocalizationManager::Get("Settings.LootBeamBottomWidth") + "##lootbeam").c_str(),
                    modern ? &settings->lootBeamModernBottomWidth : &settings->lootBeamWispBottomWidth,
                    0.0f, modern ? 36.0f : 20.0f, "%.2f px", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat((LocalizationManager::Get("Settings.LootBeamTopWidth") + "##lootbeam").c_str(),
                    modern ? &settings->lootBeamModernTopWidth : &settings->lootBeamWispTopWidth,
                    0.0f, modern ? 36.0f : 20.0f, "%.2f px", ImGuiSliderFlags_AlwaysClamp);
                ImGui::SliderFloat((LocalizationManager::Get("Settings.LootBeamGroundGlow") + "##lootbeam").c_str(),
                    modern ? &settings->lootBeamModernGroundGlow : &settings->lootBeamWispGroundGlow,
                    0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            }
            ImGui::Spacing();
            {
                const FlowItem items[] = {
                    // Default on to keep fights readable.
                    { LocalizationManager::Get("Settings.HideInCombat") + "##lootbeam", &settings->lootBeamHideInCombat },
                    { LocalizationManager::Get("Settings.HideAfterLooting") + "##lootbeam", &settings->lootBeamHideAfterLooting },
                    { LocalizationManager::Get("Settings.HideAfterInspecting") + "##lootbeam", &settings->lootBeamHideAfterInspecting },
                    // First reveal only; discovered beams can still show through walls.
                    { LocalizationManager::Get("Settings.DiscoverOnSight") + "##lootbeam", &settings->lootBeamRequireLOS },
                    // Live geometry occlusion dims and culls blocked beams.
                    { LocalizationManager::Get("Settings.HideBehindWalls") + "##lootbeam", &settings->lootBeamOcclusion },
                    // Use the engine theft test so legally accessible loot keeps its beam.
                    { LocalizationManager::Get("Settings.HideOwnedLoot") + "##lootbeam", &settings->lootBeamHideOwned },
                    // Disarm shouts and drop scripts lose weapons mid-fight, in grass.
                    { LocalizationManager::Get("Settings.TrackDroppedWeapons") + "##lootbeam", &settings->lootBeamTrackDroppedWeapons },
                };
                CheckboxFlow(items, 7);
            }
            ImGui::Spacing();

            {
                float rangeMeters = settings->lootBeamRangeUnits / Settings::kUnitsPerMeter;
                if (ImGui::SliderFloat((LocalizationManager::Get("Settings.Range") + "##lootbeam").c_str(), &rangeMeters, 3.0f, 85.0f, "%.0f m")) {
                    settings->lootBeamRangeUnits = rangeMeters * Settings::kUnitsPerMeter;
                }
            }
            {
                float bandMeters = settings->lootBeamZBandUnits / Settings::kUnitsPerMeter;
                if (ImGui::SliderFloat((LocalizationManager::Get("Settings.FloorBand") + "##lootbeam").c_str(), &bandMeters, 1.0f, 28.0f, "%.0f m")) {
                    settings->lootBeamZBandUnits = bandMeters * Settings::kUnitsPerMeter;
                }
            }
            ImGui::Spacing();

            // World units like the pillar height, not pixels: the beam is
            // projected, so it shrinks with distance on its own.
            ImGui::SliderFloat((LocalizationManager::Get("Settings.BeamLength") + "##lootbeam").c_str(), &settings->lootBeamLengthUnits,
                               20.0f, 600.0f, "%.0f");
            {
                float brightnessPct = settings->lootBeamBrightness * 100.0f;
                if (ImGui::SliderFloat((LocalizationManager::Get("Settings.Brightness") + "##lootbeam").c_str(), &brightnessPct, 10.0f, 100.0f, "%.0f%%")) {
                    settings->lootBeamBrightness = brightnessPct / 100.0f;
                }
            }
            ImGui::SliderInt((LocalizationManager::Get("Settings.IconSize") + "##lootbeam").c_str(), &settings->lootBeamIconSize, 10, 48);
            ImGui::Spacing();

            // Min Gold Value threshold slider (0 = Off)
            ImGui::SliderInt((LocalizationManager::Get("Settings.MinGoldValue") + "##lootbeam").c_str(),
                             &settings->lootBeamMinGoldValue, 0, 5000,
                             settings->lootBeamMinGoldValue == 0 ? "Off" : "%d g");
            ImGui::Spacing();

            if (ImGui::TreeNode((LocalizationManager::Get("Settings.LootTypesTree") + "##lootbeam").c_str())) {
                SectionLabel(LocalizationManager::Get("Settings.Corpses").c_str());
                {
                    const FlowItem items[] = {
                        { LocalizationManager::Get("Settings.Humanoids") + "##lootbeam", &settings->lootBeamShowCorpseHumanoid },
                        { LocalizationManager::Get("Settings.Creatures") + "##lootbeam", &settings->lootBeamShowCorpseCreature },
                    };
                    CheckboxFlow(items, 2);
                }
                SectionLabel(LocalizationManager::Get("Settings.Containers").c_str());
                {
                    const FlowItem items[] = {
                        { LocalizationManager::Get("Settings.Chests") + "##lootbeam",      &settings->lootBeamShowContChest },
                        { LocalizationManager::Get("Settings.Strongboxes") + "##lootbeam", &settings->lootBeamShowContStrongbox },
                        { LocalizationManager::Get("Settings.Satchels") + "##lootbeam",   &settings->lootBeamShowContSatchel },
                    };
                    CheckboxFlow(items, 3);
                }
                SectionLabel(LocalizationManager::Get("Settings.LooseLoot").c_str());
                {
                    const FlowItem items[] = {
                        { LocalizationManager::Get("Settings.Weapons") + "##lootbeam",  &settings->lootBeamShowLootWeapon },
                        { LocalizationManager::Get("Settings.Armor") + "##lootbeam",    &settings->lootBeamShowLootArmor },
                        { LocalizationManager::Get("Settings.Gold") + "##lootbeam",     &settings->lootBeamShowLootGold },
                        { LocalizationManager::Get("Settings.CoinPurses") + "##lootbeam", &settings->lootBeamShowLootCoinPurse },
                        { LocalizationManager::Get("Settings.Potions") + "##lootbeam",  &settings->lootBeamShowLootPotion },
                        { LocalizationManager::Get("Settings.SoulGems") + "##lootbeam", &settings->lootBeamShowLootSoulGem },
                        { LocalizationManager::Get("Settings.Scrolls") + "##lootbeam",  &settings->lootBeamShowLootScroll },
                        { LocalizationManager::Get("Settings.Books") + "##lootbeam",    &settings->lootBeamShowLootBook },
                    };
                    CheckboxFlow(items, IM_ARRAYSIZE(items));
                }
                ImGui::TreePop();
            }
            ImGui::Unindent();
        }
    }

    static void PanelMarkers(Settings* settings) {
        SectionLabel(LocalizationManager::Get("Settings.SectionFavoriteBadge").c_str());
        ImGui::SliderFloat((LocalizationManager::Get("Settings.Size") + "##fav").c_str(), &settings->favBadgeSize, 1.0f, 12.0f, "%.1f");
        MapDraw::ColorEdit4Hex((LocalizationManager::Get("Settings.Color") + "##fav").c_str(), settings->favBadgeColorR, settings->favBadgeColorG,
                               settings->favBadgeColorB, settings->favBadgeColorA);
        ImGui::Spacing();

        ImGui::Separator();
        SectionLabel(LocalizationManager::Get("Settings.SectionNoteBadge").c_str());
        ImGui::SliderFloat((LocalizationManager::Get("Settings.Size") + "##note").c_str(), &settings->noteBadgeSize, 1.0f, 12.0f, "%.1f");
        MapDraw::ColorEdit4Hex((LocalizationManager::Get("Settings.Color") + "##note").c_str(), settings->noteBadgeColorR, settings->noteBadgeColorG,
                               settings->noteBadgeColorB, settings->noteBadgeColorA);
        ImGui::Spacing();

        ImGui::Separator();
        SectionLabel(LocalizationManager::Get("Settings.SectionClearedBadge").c_str());
        ImGui::SliderFloat((LocalizationManager::Get("Settings.Size") + "##cleared").c_str(), &settings->clearedBadgeSize, 1.0f, 12.0f, "%.1f");
        MapDraw::ColorEdit4Hex((LocalizationManager::Get("Settings.Color") + "##cleared").c_str(), settings->clearedBadgeColorR, settings->clearedBadgeColorG,
                               settings->clearedBadgeColorB, settings->clearedBadgeColorA);
    }

    static void PanelMarkerImages(Settings* settings) {
        ImGui::SliderInt((LocalizationManager::Get("Settings.ImageMinWidth") + "##markerimg").c_str(), &settings->markerImageMinWidth, 0, 800, "%d px");
        ImGui::SliderFloat((LocalizationManager::Get("Settings.Opacity") + "##markerimg").c_str(), &settings->markerImageAlpha,
                           0.0f, 1.0f, "%.2f");
    }

    static void ShowToast(const char* message) {
        strncpy_s(g_toastMessage, message, sizeof(g_toastMessage));
        g_toastMessage[sizeof(g_toastMessage) - 1] = '\0';
        g_toastTimer = 3.0f;
    }

    void Draw() {
        if (!g_isVisible.load()) { g_wasVisible = false; return; }

        auto* ui = RE::UI::GetSingleton();
        const Host host = g_host.load();

        const char* closeReason = nullptr;
        if (!ui) {
            closeReason = "ui_missing";
        } else if (IsMapFading()) {
            closeReason = "map_fading";
        } else if (host == Host::Map) {
            if (!MapOpen(ui)) {
                closeReason = "map_closed";
            } else if (ui->IsMenuOpen(RE::JournalMenu::MENU_NAME) ||
                       ui->IsMenuOpen(RE::Console::MENU_NAME) ||
                       ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME)) {
                closeReason = "map_modal_opened";
            }
        } else if (host == Host::Gameplay) {
            if (MenuUtils::ShouldBlockOverlay()) {
                closeReason = "blocking_overlay";
            } else if (PartySheetState::IsFullscreenPanelOpen()) {
                closeReason = "partysheet_panel";
            }
        }

        if (closeReason) {
            Hide(closeReason);
            return;
        }

        auto* settings = Settings::GetSingleton();

        if (!g_wasVisible) {
            const int t = settings->menuActiveTab;
            g_activeTab = (t >= 0 && t < static_cast<int>(Tab::Count))
                              ? static_cast<Tab>(t) : Tab::General;
        }
        g_wasVisible = true;

        // Set initial window position and size from settings
        ImGui::SetNextWindowPos(ImVec2(static_cast<float>(settings->menuPosX), static_cast<float>(settings->menuPosY)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(settings->menuWidth), static_cast<float>(settings->menuHeight)), ImGuiCond_FirstUseEver);

        ImGui::SetNextWindowBgAlpha(1.0f);

        ImGui::PushStyleColor(ImGuiCol_WindowBg,          MapTheme::kDlgBg());
        ImGui::PushStyleColor(ImGuiCol_TitleBg,           MapTheme::kDlgHeaderBg());
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive,     MapTheme::kDlgHeaderActive());
        ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed,  MapTheme::kDlgHeaderBg());
        ImGui::PushStyleColor(ImGuiCol_Border,            MapTheme::kDlgBorder());
        ImGui::PushStyleColor(ImGuiCol_Text,              MapTheme::kDlgText());
        ImGui::PushStyleColor(ImGuiCol_TextDisabled,      MapTheme::kMutedTextV());
        ImGui::PushStyleColor(ImGuiCol_PopupBg,           MapTheme::kDlgBg());     // combo dropdown
        ImGui::PushStyleColor(ImGuiCol_FrameBg,           MapTheme::kDlgFrameBg());
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,    MapTheme::kDlgHeaderHover());
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive,     MapTheme::kDlgHeaderActive());
        ImGui::PushStyleColor(ImGuiCol_CheckMark,         MapTheme::kDlgCheckMark());
        ImGui::PushStyleColor(ImGuiCol_SliderGrab,        MapTheme::kDlgGrab());
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,  MapTheme::kDlgGrabActive());
        ImGui::PushStyleColor(ImGuiCol_Header,            MapTheme::kDlgHeaderBg());
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered,     MapTheme::kDlgHeaderHover());
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,      MapTheme::kDlgHeaderActive());
        ImGui::PushStyleColor(ImGuiCol_Button,            MapTheme::kDlgBtnBg());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,     MapTheme::kDlgBtnHover());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,      MapTheme::kDlgBtnActive());
        ImGui::PushStyleColor(ImGuiCol_Separator,         MapTheme::kDlgBorder());
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg,       MapTheme::kDlgScrollBg());
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,     MapTheme::kDlgScrollGrab());
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, MapTheme::kDlgBtnHover());
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,  MapTheme::kDlgGrabActive());
        const int kSettingsStyleColors = 25;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   MapTheme::kPanelRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,    MapTheme::kBtnRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding,     MapTheme::kBtnRounding);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,  1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(12.0f, 10.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(8.0f,  8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,     ImVec2(8.0f,  5.0f));
        const int kSettingsStyleVars = 8;

        bool keepOpen = true;
        if (ImGui::Begin(LocalizationManager::Get("Settings.WindowTitle").c_str(), &keepOpen,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                         ImGuiWindowFlags_NoScrollWithMouse)) {
            DrawTabGrid(settings);
            ImGui::Separator();

            // Scrollable Content Area
            // Leave space at the bottom for the fixed footer (approx 60px)
            if (ImGui::BeginChild("SettingsContent", ImVec2(0, -60), false)) {

                // Borderless frames inside the scroll region only; the footer
                // Save button sits after EndChild, so it keeps its border.
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

                switch (g_activeTab) {
                    case Tab::General:      PanelGeneral(settings);      break;
                    case Tab::Hotkeys:      PanelHotkeys(settings);      break;
                    case Tab::Appearance:   PanelAppearance(settings);   break;
                    case Tab::Map:          PanelMap(settings);          break;
                    case Tab::PathTracking: PanelPathTracking(settings); break;
                    case Tab::RadarInspect: PanelRadarInspect(settings); break;
                    case Tab::Beacons:      PanelBeacons(settings);      break;
                    default: break;
                }

                ImGui::PopStyleVar();  // FrameBorderSize
                ImGui::EndChild();
            }

            // Fixed Footer
            ImGui::Separator();

            if (ImGui::Button(LocalizationManager::Get("Settings.SaveSettingsButton").c_str(), ImVec2(-1, 0))) {
                settings->Save();
                ShowToast(LocalizationManager::Get("Settings.ToastSaved").c_str());
            }

            ImGui::TextColored(MapTheme::kMutedTextV(), "%s",
                               LocalizationManager::Get("Settings.FooterHint").c_str());

            // Toast Notification Overlay
            if (g_toastTimer > 0.0f) {
                g_toastTimer -= ImGui::GetIO().DeltaTime;

                ImVec2 windowPos = ImGui::GetWindowPos();
                ImVec2 windowSize = ImGui::GetWindowSize();

                // Draw a simple overlay for the toast
                ImGui::SetNextWindowPos(ImVec2(windowPos.x + windowSize.x / 2.0f, windowPos.y + windowSize.y - 100.0f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowBgAlpha(0.8f);
                ImGuiWindowFlags toastFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;

                if (ImGui::Begin("ToastOverlay", nullptr, toastFlags)) {
                    ImGui::Text("%s", g_toastMessage);
                }
                ImGui::End();
            }

            // Persist window state
            ImVec2 pos = ImGui::GetWindowPos();
            ImVec2 size = ImGui::GetWindowSize();
            settings->menuPosX = static_cast<std::int32_t>(pos.x);
            settings->menuPosY = static_cast<std::int32_t>(pos.y);
            settings->menuWidth = static_cast<std::int32_t>(size.x);
            settings->menuHeight = static_cast<std::int32_t>(size.y);

            const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            if (g_wasDraggingMenu && !down) {
                settings->SavePositions();
            }
            g_wasDraggingMenu = down && ImGui::IsWindowFocused();
        }
        ImGui::End();

        if (!keepOpen) {
            Hide("imgui_close_btn");
        }

        ImGui::PopStyleVar(kSettingsStyleVars);
        ImGui::PopStyleColor(kSettingsStyleColors);
    }
}
