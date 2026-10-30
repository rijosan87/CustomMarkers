// SKSE Menu Framework Mod Control Panel integration.

#include "SettingsMenu.h"

#include <filesystem>
#pragma warning(push)
#pragma warning(disable: 4099 5054)
#include "SKSEMenuFramework.h"
#pragma warning(pop)

#include "BackgroundManager.h"
#include "InputHandler.h"
#include "LocalizationManager.h"
#include "Settings.h"
#include "GlanceBounds.h"

#include <algorithm>
#include <string>
#include <vector>

namespace SettingsMenuFramework {
    enum class Tab : std::uint8_t {
        General,
        Hotkeys,
        Appearance,
        Map,
        PathTracking,
        RadarInspect,
        Beacons,
        Count
    };

    static bool g_registered = false;
    static bool g_tabInitialized = false;
    static Tab  g_activeTab = Tab::General;

    static void SectionLabel(const char* text) {
        ImGuiMCP::Text("%s", text);
    }

    struct FlowItem { std::string label; bool* value; };
    static void CheckboxFlow(const FlowItem* items, int count) {
        auto* style = ImGuiMCP::GetStyle();
        const float spacing = style ? style->ItemSpacing.x : 8.0f;
        const float innerSpacing = style ? style->ItemInnerSpacing.x : 4.0f;
        const float right = ImGuiMCP::GetCursorScreenPos().x + ImGuiMCP::GetContentRegionAvail().x;
        for (int i = 0; i < count; ++i) {
            ImGuiMCP::Checkbox(items[i].label.c_str(), items[i].value);
            if (i + 1 < count) {
                const float glyph = ImGuiMCP::GetFrameHeight() + innerSpacing;
                const float nextW = glyph + ImGuiMCP::CalcTextSize(items[i + 1].label.c_str(), nullptr, true).x;
                if (ImGuiMCP::GetItemRectMax().x + spacing + nextW < right) {
                    ImGuiMCP::SameLine();
                }
            }
        }
    }

    static bool ColorEdit3(const char* label,
                           std::int32_t& r, std::int32_t& g, std::int32_t& b) {
        float c[3] = { r / 255.0f, g / 255.0f, b / 255.0f };
        const ImGuiMCP::ImGuiColorEditFlags flags =
            ImGuiMCP::ImGuiColorEditFlags_DisplayHex | ImGuiMCP::ImGuiColorEditFlags_NoInputs;
        if (ImGuiMCP::ColorEdit3(label, c, flags)) {
            r = static_cast<std::int32_t>(c[0] * 255.0f + 0.5f);
            g = static_cast<std::int32_t>(c[1] * 255.0f + 0.5f);
            b = static_cast<std::int32_t>(c[2] * 255.0f + 0.5f);
            return true;
        }
        return false;
    }

    static bool ColorEdit4(const char* label,
                           std::int32_t& r, std::int32_t& g,
                           std::int32_t& b, std::int32_t& a) {
        float c[4] = { r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };
        const ImGuiMCP::ImGuiColorEditFlags flags =
            ImGuiMCP::ImGuiColorEditFlags_AlphaBar | ImGuiMCP::ImGuiColorEditFlags_DisplayHex |
            ImGuiMCP::ImGuiColorEditFlags_NoInputs;
        if (ImGuiMCP::ColorEdit4(label, c, flags)) {
            r = static_cast<std::int32_t>(c[0] * 255.0f + 0.5f);
            g = static_cast<std::int32_t>(c[1] * 255.0f + 0.5f);
            b = static_cast<std::int32_t>(c[2] * 255.0f + 0.5f);
            a = static_cast<std::int32_t>(c[3] * 255.0f + 0.5f);
            return true;
        }
        return false;
    }

    struct KeyOption {
        int code;
        std::string name;
    };

    static const std::vector<KeyOption>& GetKeyOptions(bool allowUnbound) {
        static std::vector<KeyOption> withUnbound;
        static std::vector<KeyOption> withoutUnbound;
        static bool initialized = false;
        if (!initialized) {
            withUnbound.push_back({ -1, LocalizationManager::Get("Settings.Unbound") });
            for (int code = 1; code <= 255; ++code) {
                if (InputHandler::IsModifierScancode(static_cast<std::uint32_t>(code))) {
                    continue;
                }
                if (!InputHandler::IsValidKeyCode(code)) {
                    continue;
                }
                withUnbound.push_back({ code, InputHandler::DIKToKeyName(code) });
            }
            for (int code = 256; code <= 263; ++code) {
                withUnbound.push_back({ code, InputHandler::DIKToKeyName(code) });
            }
            for (int code = 266; code <= 281; ++code) {
                withUnbound.push_back({ code, InputHandler::DIKToKeyName(code) });
            }

            for (size_t i = 1; i < withUnbound.size(); ++i) {
                withoutUnbound.push_back(withUnbound[i]);
            }
            initialized = true;
        }
        return allowUnbound ? withUnbound : withoutUnbound;
    }

    static void DrawKeyCombo(const char* label, std::int32_t& key, bool allowUnbound) {
        const auto& options = GetKeyOptions(allowUnbound);
        int current = -1;
        for (int i = 0; i < static_cast<int>(options.size()); ++i) {
            if (options[i].code == key) {
                current = i;
                break;
            }
        }
        if (current == -1) {
            current = 0;
        }
        auto getter = [](void* data, int idx) -> const char* {
            const auto* opts = static_cast<const std::vector<KeyOption>*>(data);
            if (idx >= 0 && idx < static_cast<int>(opts->size())) {
                return (*opts)[idx].name.c_str();
            }
            return "";
        };
        if (ImGuiMCP::Combo(label, &current, getter, const_cast<void*>(static_cast<const void*>(&options)), static_cast<int>(options.size()), 15)) {
            key = options[current].code;
        }
    }

    static void DrawModifierCombo(const char* label, std::int32_t& modifier) {
        static const char* modifierNames[] = {
            "None",
            "Alt",
            "Ctrl",
            "Shift",
            "LAlt",
            "RAlt",
            "LCtrl",
            "RCtrl",
            "LShift",
            "RShift"
        };
        int current = (modifier >= 0 && modifier <= 9) ? modifier : 0;
        if (ImGuiMCP::Combo(label, &current, modifierNames, 10)) {
            modifier = current;
        }
    }

    static void RenderHotkeyRow(const char* label, const char* id,
                                std::int32_t& key, std::int32_t& mod,
                                bool allowUnbound = true) {
        ImGuiMCP::Text("%s:", label);
        std::string keyLabel = std::string("Key##") + id;
        std::string modLabel = std::string("Modifier##") + id;
        DrawKeyCombo(keyLabel.c_str(), key, allowUnbound);
        DrawModifierCombo(modLabel.c_str(), mod);
        ImGuiMCP::Spacing();
    }

    static void RenderControllerButtonRow(Settings* settings, const char* label,
                                          const char* id, std::int32_t& field) {
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
        int current = (field >= 266 && field <= 281) ? field - 265 : 0;
        ImGuiMCP::Text("%s:", label);
        ImGuiMCP::SetNextItemWidth(180.0f);
        const std::string comboId = std::string("##controller_") + id;
        if (ImGuiMCP::Combo(comboId.c_str(), &current, names, 17)) {
            field = current == 0 ? -1 : current + 265;
            settings->Save();
        }
        ImGuiMCP::Spacing();
    }

    static void RenderControllerInspectSettings(Settings* settings) {
        SectionLabel(LocalizationManager::Get("Settings.InspectHold").c_str());
        RenderControllerButtonRow(settings, LocalizationManager::Get("Settings.ControllerOpenInspect").c_str(), "inspect_open",
                                  settings->controllerInspectOpenButton);
        {
            const std::string single = LocalizationManager::Get("Settings.ControllerSinglePress");
            const std::string doublePress = LocalizationManager::Get("Settings.ControllerDoublePress");
            const std::string longPress = LocalizationManager::Get("Settings.ControllerLongPress");
            const char* gestures[] = { single.c_str(), doublePress.c_str(), longPress.c_str() };
            int current = std::clamp(settings->controllerInspectOpenGesture, 0, 2);
            ImGuiMCP::Text("%s:", LocalizationManager::Get("Settings.ControllerOpenGesture").c_str());
            ImGuiMCP::SetNextItemWidth(180.0f);
            if (ImGuiMCP::Combo("##controller_inspect_gesture", &current, gestures, 3)) {
                settings->controllerInspectOpenGesture = current;
                settings->Save();
            }
            ImGuiMCP::Spacing();
        }
        RenderControllerButtonRow(settings, LocalizationManager::Get("Settings.ControllerAddMarker").c_str(), "inspect_add_marker",
                                  settings->controllerInspectAddMarkerButton);
        RenderControllerButtonRow(settings, LocalizationManager::Get("Settings.ControllerSwitchView").c_str(), "inspect_switch_view",
                                  settings->controllerInspectSwitchViewButton);
        RenderControllerButtonRow(settings, LocalizationManager::Get("Settings.PathTrackingKey").c_str(), "inspect_path_tracking",
                                  settings->controllerInspectPathTrackingButton);
        ImGuiMCP::TextDisabled("%s", LocalizationManager::Get("Settings.ControllerBackClose").c_str());
    }

    static void DrawActivationCombo(const char* label, const char* id, std::int32_t& activation) {
        const std::string holdStr = LocalizationManager::Get("Settings.ActivationHold");
        const std::string toggleStr = LocalizationManager::Get("Settings.ActivationToggle");
        const char* modes[] = {
            holdStr == "Settings.ActivationHold" ? "Hold" : holdStr.c_str(),
            toggleStr == "Settings.ActivationToggle" ? "Toggle" : toggleStr.c_str()
        };
        int current = (activation >= 0 && activation <= 1) ? activation : 0;
        std::string comboLabel = std::string("##activation_") + id;
        ImGuiMCP::Text("%s:", label);
        if (ImGuiMCP::Combo(comboLabel.c_str(), &current, modes, 2)) {
            activation = current;
            Settings::GetSingleton()->Save();
        }
        ImGuiMCP::Spacing();
    }

    static void RenderBackgroundPickerCombo(const char* label, std::int32_t& currentIndex,
                                            BackgroundManager::Type type) {
        const int count = BackgroundManager::GetBackgroundCount(type);
        std::vector<std::string> names;
        names.push_back(LocalizationManager::Get("Settings.BgNone"));
        for (int i = 1; i <= count; ++i) {
            names.push_back(BackgroundManager::GetBackgroundName(type, i));
        }

        int current = (currentIndex >= 0 && currentIndex <= count) ? currentIndex : 0;
        auto getter = [](void* data, int idx) -> const char* {
            const auto* list = static_cast<const std::vector<std::string>*>(data);
            if (idx >= 0 && idx < static_cast<int>(list->size())) {
                return (*list)[idx].c_str();
            }
            return "";
        };

        if (ImGuiMCP::Combo(label, &current, getter, &names, static_cast<int>(names.size()))) {
            currentIndex = current;
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

        constexpr int kPerRow = 4;
        auto* style = ImGuiMCP::GetStyle();
        const float spacing = style ? style->ItemSpacing.x : 8.0f;
        const float avail   = ImGuiMCP::GetContentRegionAvail().x;
        const float tileW   = (avail - spacing * (kPerRow - 1)) / kPerRow;

        int col = 0;
        for (const auto& t : tiles) {
            if (col != 0) ImGuiMCP::SameLine();
            const bool active = (g_activeTab == t.tab);
            std::string btnText = active ? ("> " + t.label + " <") : t.label;
            if (ImGuiMCP::Button(btnText.c_str(), ImGuiMCP::ImVec2(tileW, 0.0f))) {
                g_activeTab = t.tab;
                settings->menuActiveTab = static_cast<std::int32_t>(t.tab);
                settings->SavePositions();
            }
            if (++col == kPerRow) col = 0;
        }
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
        ImGuiMCP::Spacing();
        ImGuiMCP::Separator();

        SectionLabel(LocalizationManager::Get("Settings.GlanceRadarHeader").c_str());
        {
            const FlowItem items[] = {
                { LocalizationManager::Get("Settings.ShowIndoors") + "##glance_gen",  &settings->navGlanceInteriorVisible },
                { LocalizationManager::Get("Settings.ShowOutdoors") + "##glance_gen", &settings->navGlanceExteriorVisible },
            };
            CheckboxFlow(items, 2);
        }
        ImGuiMCP::Spacing();
        ImGuiMCP::Separator();

        SectionLabel(LocalizationManager::Get("Settings.InspectModeHeader").c_str());
        {
            const FlowItem items[] = {
                { LocalizationManager::Get("Settings.ShowIndoors") + "##inspect_gen",  &settings->navInspectInteriorVisible },
                { LocalizationManager::Get("Settings.ShowOutdoors") + "##inspect_gen", &settings->navInspectExteriorVisible },
            };
            CheckboxFlow(items, 2);
        }
        ImGuiMCP::Spacing();
        ImGuiMCP::Separator();

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
        ImGuiMCP::Spacing();
        ImGuiMCP::Separator();

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
        RenderHotkeyRow(LocalizationManager::Get("Settings.HudSettings").c_str(), "settings",
                        settings->menuSettingsKey, settings->menuSettingsModifier, true);
        RenderHotkeyRow(LocalizationManager::Get("Settings.MarkerListPanel").c_str(), "listpanel",
                        settings->listPanelKey, settings->listPanelModifier, true);
        RenderHotkeyRow(LocalizationManager::Get("Settings.PathTrackingKey").c_str(), "pathtrack",
                        settings->pathTrackKey, settings->pathTrackModifier, true);

        ImGuiMCP::Spacing();
        ImGuiMCP::Separator();
        SectionLabel(LocalizationManager::Get("Settings.SectionNavigationOverlay").c_str());
        RenderHotkeyRow(LocalizationManager::Get("Settings.InspectHold").c_str(), "inspect",
                        settings->navInspectKey, settings->navInspectModifier, true);
        const std::string inspectActivation = LocalizationManager::Get("Settings.InspectActivation");
        DrawActivationCombo(inspectActivation == "Settings.InspectActivation" ? "Activation" : inspectActivation.c_str(),
                            "inspect", settings->navInspectActivation);
        RenderHotkeyRow(LocalizationManager::Get("Settings.InspectModeToggle").c_str(), "inspectmode",
                        settings->navInspectModeToggleKey, settings->navInspectModeToggleModifier, true);
        RenderHotkeyRow(LocalizationManager::Get("Settings.QuickMark").c_str(), "quickmark",
                        settings->navQuickMarkKey, settings->navQuickMarkModifier, true);
        RenderHotkeyRow(LocalizationManager::Get("Settings.MiniRadarToggleKey").c_str(), "glancetoggle",
                        settings->navGlanceToggleKey, settings->navGlanceToggleModifier, true);
        RenderHotkeyRow(LocalizationManager::Get("Settings.LootBeamToggleKey").c_str(), "lootbeamtoggle",
                        settings->lootBeamToggleKey, settings->lootBeamToggleModifier, true);

        ImGuiMCP::Spacing();
        ImGuiMCP::Separator();
        SectionLabel(LocalizationManager::Get("Settings.SectionLookPin").c_str());
        RenderHotkeyRow(LocalizationManager::Get("Settings.LookPinKey").c_str(), "lookpin",
                        settings->lookPinKey, settings->lookPinModifier, true);

        ImGuiMCP::Spacing();
        ImGuiMCP::Separator();
        if (ImGuiMCP::CollapsingHeader(LocalizationManager::Get("Settings.Controller").c_str(), ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderControllerInspectSettings(settings);
        }
    }

    static void PanelAppearance(Settings* settings) {
        if (!settings) return;

        SectionLabel(LocalizationManager::Get("Settings.SectionGlobalStyles").c_str());
        auto flowNext = [](const std::string& label) {
            const auto* style = ImGuiMCP::GetStyle();
            const float spacing = style ? style->ItemSpacing.x : 8.0f;
            const float innerSpacing = style ? style->ItemInnerSpacing.x : 4.0f;
            const float right = ImGuiMCP::GetCursorScreenPos().x + ImGuiMCP::GetContentRegionAvail().x;
            const float nextW = ImGuiMCP::GetFrameHeight() + innerSpacing +
                                ImGuiMCP::CalcTextSize(label.c_str(), nullptr, true).x;
            if (ImGuiMCP::GetItemRectMax().x + spacing + nextW < right) {
                ImGuiMCP::SameLine();
            }
        };
        ColorEdit3(LocalizationManager::Get("Settings.TitleColor").c_str(), settings->titleColorR, settings->titleColorG,
                   settings->titleColorB);
        flowNext(LocalizationManager::Get("Settings.BodyColor"));
        ColorEdit3(LocalizationManager::Get("Settings.BodyColor").c_str(), settings->bodyColorR, settings->bodyColorG,
                   settings->bodyColorB);
        flowNext(LocalizationManager::Get("Settings.Background"));
        ColorEdit4(LocalizationManager::Get("Settings.Background").c_str(), settings->bgColorR, settings->bgColorG,
                   settings->bgColorB, settings->bgColorA);
        flowNext(LocalizationManager::Get("Settings.Border"));
        ColorEdit4(LocalizationManager::Get("Settings.Border").c_str(), settings->borderColorR, settings->borderColorG,
                   settings->borderColorB, settings->borderColorA);
        if (ImGuiMCP::SliderInt(LocalizationManager::Get("Settings.InterfaceScale").c_str(), &settings->interfaceScale, 50, 200, "%d%%")) {
        }
        if (ImGuiMCP::IsItemDeactivatedAfterEdit()) {
            settings->Save();
        }
        ImGuiMCP::SliderFloat(LocalizationManager::Get("Settings.TitleScale").c_str(), &settings->titleScale, 0.5f, 1.35f, "%.2f");
        ImGuiMCP::SliderFloat(LocalizationManager::Get("Settings.BodyScale").c_str(),  &settings->bodyScale,  0.5f, 1.35f, "%.2f");
        ImGuiMCP::SliderFloat(LocalizationManager::Get("Settings.NotesScale").c_str(), &settings->noteTextScale, 0.5f, 3.0f, "%.2f");

        ImGuiMCP::Spacing();
        ImGuiMCP::Separator();
        SectionLabel(LocalizationManager::Get("Settings.SectionIconDesign").c_str());
        ColorEdit4(LocalizationManager::Get("Settings.PinTint").c_str(), settings->pinTintR, settings->pinTintG,
                   settings->pinTintB, settings->pinTintA);
        flowNext(LocalizationManager::Get("Settings.Icons3D"));
        ImGuiMCP::Checkbox(LocalizationManager::Get("Settings.Icons3D").c_str(), &settings->iconFx3D);
        const std::string iconShadowLabel = LocalizationManager::Get("Settings.BgShadow");
        flowNext(iconShadowLabel);
        ImGuiMCP::Checkbox((iconShadowLabel + "##IconShadow").c_str(), &settings->iconShadow);

        ImGuiMCP::Spacing();
        ImGuiMCP::Separator();
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

    static void PanelStatCard(Settings* settings) {
        ImGuiMCP::Checkbox(LocalizationManager::Get("Settings.ShowMeta").c_str(), &settings->statCardShowDetails);
        ImGuiMCP::Indent();
        ImGuiMCP::BeginDisabled(!settings->statCardShowDetails);
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
        ImGuiMCP::Combo(LocalizationManager::Get("Settings.DistanceUnits").c_str(), &settings->statCardDistanceUnits, distanceUnits, 2);
        ImGuiMCP::EndDisabled();
        ImGuiMCP::Unindent();
        ImGuiMCP::Spacing();
        SectionLabel(LocalizationManager::Get("Settings.SectionVanillaCardPlacement").c_str());
        const std::string anchorTop    = LocalizationManager::Get("Settings.AnchorTop");
        const std::string anchorBottom = LocalizationManager::Get("Settings.AnchorBottom");
        const std::string anchorLeft   = LocalizationManager::Get("Settings.AnchorLeft");
        const std::string anchorRight  = LocalizationManager::Get("Settings.AnchorRight");
        const char* anchors[] = { anchorTop.c_str(), anchorBottom.c_str(), anchorLeft.c_str(), anchorRight.c_str() };
        ImGuiMCP::Combo(LocalizationManager::Get("Settings.Anchor").c_str(), &settings->statCardAnchor, anchors, 4);
        ImGuiMCP::SliderInt(LocalizationManager::Get("Settings.OffsetX").c_str(), &settings->statCardOffsetX, -300, 300);
        ImGuiMCP::SliderInt(LocalizationManager::Get("Settings.OffsetY").c_str(), &settings->statCardOffsetY, -300, 300);

        ImGuiMCP::Spacing();
        ImGuiMCP::Separator();
        SectionLabel(LocalizationManager::Get("Settings.SectionCustomCardPlacement").c_str());
        ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.OffsetX") + "##hover").c_str(), &settings->markerHoverOffsetX, -300, 300);
        ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.OffsetY") + "##hover").c_str(), &settings->markerHoverOffsetY, -300, 300);
    }

    static void PanelMarkers(Settings* settings) {
        SectionLabel(LocalizationManager::Get("Settings.SectionFavoriteBadge").c_str());
        ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.Size") + "##fav").c_str(), &settings->favBadgeSize, 1.0f, 12.0f, "%.1f");
        ColorEdit4((LocalizationManager::Get("Settings.Color") + "##fav").c_str(), settings->favBadgeColorR, settings->favBadgeColorG,
                   settings->favBadgeColorB, settings->favBadgeColorA);
        ImGuiMCP::Spacing();

        ImGuiMCP::Separator();
        SectionLabel(LocalizationManager::Get("Settings.SectionNoteBadge").c_str());
        ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.Size") + "##note").c_str(), &settings->noteBadgeSize, 1.0f, 12.0f, "%.1f");
        ColorEdit4((LocalizationManager::Get("Settings.Color") + "##note").c_str(), settings->noteBadgeColorR, settings->noteBadgeColorG,
                   settings->noteBadgeColorB, settings->noteBadgeColorA);
        ImGuiMCP::Spacing();

        ImGuiMCP::Separator();
        SectionLabel(LocalizationManager::Get("Settings.SectionClearedBadge").c_str());
        ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.Size") + "##cleared").c_str(), &settings->clearedBadgeSize, 1.0f, 12.0f, "%.1f");
        ColorEdit4((LocalizationManager::Get("Settings.Color") + "##cleared").c_str(), settings->clearedBadgeColorR, settings->clearedBadgeColorG,
                   settings->clearedBadgeColorB, settings->clearedBadgeColorA);
    }

    static void PanelMarkerImages(Settings* settings) {
        ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.ImageMinWidth") + "##markerimg").c_str(), &settings->markerImageMinWidth, 0, 800, "%d px");
        ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.Opacity") + "##markerimg").c_str(), &settings->markerImageAlpha,
                              0.0f, 1.0f, "%.2f");
    }

    static void PanelMap(Settings* settings) {
        if (ImGuiMCP::CollapsingHeader(LocalizationManager::Get("Settings.ListPanelHeader").c_str())) {
            ImGuiMCP::Indent();
            ImGuiMCP::SliderInt(LocalizationManager::Get("Settings.Width").c_str(), &settings->listPanelWidth, 200, 600);
            ImGuiMCP::SliderInt(LocalizationManager::Get("Settings.MinHeightPct").c_str(), &settings->listPanelMinHeightPct, 0, 95, "%d%%");
            ImGuiMCP::SliderInt(LocalizationManager::Get("Settings.MaxHeightPct").c_str(), &settings->listPanelMaxHeightPct, 30, 95, "%d%%");
            ImGuiMCP::Checkbox(LocalizationManager::Get("Settings.ShowOverview").c_str(), &settings->listShowOverview);
            if (settings->listShowOverview) {
                ImGuiMCP::SameLine();
                ImGuiMCP::Checkbox(LocalizationManager::Get("Settings.ShowCompanions").c_str(), &settings->listShowCompanions);
            }
            ImGuiMCP::Checkbox(LocalizationManager::Get("Settings.ShowActiveQuests").c_str(), &settings->listShowActiveQuests);

            ImGuiMCP::Spacing();
            ImGuiMCP::Checkbox(LocalizationManager::Get("Settings.CustomBg").c_str(),
                               &settings->listPanelUseCustomBg);
            if (settings->listPanelUseCustomBg) {
                ImGuiMCP::SameLine();
                RenderBackgroundPickerCombo("##listPanelBgPicker", settings->listPanelBgIndex,
                                            BackgroundManager::Type::ListPanel);
            }
            ImGuiMCP::Unindent();
        }

        if (ImGuiMCP::CollapsingHeader(LocalizationManager::Get("Settings.StatCardHeader").c_str())) {
            ImGuiMCP::Indent();
            PanelStatCard(settings);
            ImGuiMCP::Unindent();
        }

        if (ImGuiMCP::CollapsingHeader(LocalizationManager::Get("Settings.CustomPinsHeader").c_str())) {
            ImGuiMCP::Indent();
            ImGuiMCP::SliderFloat(LocalizationManager::Get("Settings.IconScale").c_str(), &settings->markerIconScale, 0.5f, 2.0f, "%.2f");
            ImGuiMCP::Unindent();
        }

        if (ImGuiMCP::CollapsingHeader(LocalizationManager::Get("Settings.BadgesHeader").c_str())) {
            ImGuiMCP::Indent();
            PanelMarkers(settings);
            ImGuiMCP::Unindent();
        }

        if (ImGuiMCP::CollapsingHeader(LocalizationManager::Get("Settings.MarkerImagesHeader").c_str())) {
            ImGuiMCP::Indent();
            PanelMarkerImages(settings);
            ImGuiMCP::Unindent();
        }
    }

    static void PanelPathTracking(Settings* settings) {
        SectionLabel(LocalizationManager::Get("Settings.SectionTrail").c_str());
        ColorEdit4(LocalizationManager::Get("Settings.Color").c_str(), settings->pathTrailColorR, settings->pathTrailColorG,
                   settings->pathTrailColorB, settings->pathTrailColorA);
        ImGuiMCP::SliderFloat(LocalizationManager::Get("Settings.Thickness").c_str(), &settings->pathTrailThickness, 1.0f, 8.0f, "%.1f");
        {
            const FlowItem items[] = {
                { LocalizationManager::Get("Settings.FadeOlderSegments"), &settings->pathTrailFadeByAge },
                { LocalizationManager::Get("Settings.SmartSpacing"),      &settings->pathTrailSmartSpacing },
            };
            CheckboxFlow(items, 2);
        }
        ImGuiMCP::Spacing();

        ImGuiMCP::Separator();
        SectionLabel(LocalizationManager::Get("Settings.SectionCurrentPositionPulse").c_str());
        ImGuiMCP::Checkbox(LocalizationManager::Get("Settings.EnablePulse").c_str(), &settings->pathPulseEnabled);
        ColorEdit3(LocalizationManager::Get("Settings.PulseColor").c_str(), settings->pathPulseColorR, settings->pathPulseColorG,
                   settings->pathPulseColorB);
    }

    static void PanelRadarInspect(Settings* settings) {
        if (ImGuiMCP::CollapsingHeader(LocalizationManager::Get("Settings.SectionGeneral").c_str(), ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGuiMCP::Indent();
            ImGuiMCP::Checkbox(LocalizationManager::Get("Settings.HideInBeastForm").c_str(),
                               &settings->hideInBeastForm);
            ImGuiMCP::Checkbox(LocalizationManager::Get("Settings.EntitiesRequireLOS").c_str(), &settings->navEntityRequireLOS);
            ImGuiMCP::Spacing();
            {
                float rangeMeters = settings->navMarkerRange / Settings::kUnitsPerMeter;
                if (ImGuiMCP::SliderFloat(LocalizationManager::Get("Settings.ShowMarkersWithin").c_str(), &rangeMeters, 100.0f, 5000.0f, "%.0f m")) {
                    settings->navMarkerRange = rangeMeters * Settings::kUnitsPerMeter;
                }
            }
            ImGuiMCP::SliderInt(LocalizationManager::Get("Settings.MaxQuestMarkers").c_str(), &settings->navMaxQuestMarkers, 0, 10);
            {
                const std::string unlimitedLabel = LocalizationManager::Get("Settings.Unlimited");
                ImGuiMCP::SliderInt(LocalizationManager::Get("Settings.MaxCustomMarkers").c_str(), &settings->navMaxCustomMarkers, 0, 30,
                                    settings->navMaxCustomMarkers == 0 ? unlimitedLabel.c_str() : "%d");
            }
            ImGuiMCP::Unindent();
        }
        ImGuiMCP::Spacing();

        if (ImGuiMCP::CollapsingHeader(LocalizationManager::Get("Settings.EntityIconStyle").c_str())) {
            ImGuiMCP::Indent();
            ColorEdit4(LocalizationManager::Get("Settings.Background").c_str(),
                       settings->navEntityIconBgR, settings->navEntityIconBgG,
                       settings->navEntityIconBgB, settings->navEntityIconBgA);
            ColorEdit4(LocalizationManager::Get("Settings.Border").c_str(),
                       settings->navEntityIconBorderR, settings->navEntityIconBorderG,
                       settings->navEntityIconBorderB, settings->navEntityIconBorderA);
            ImGuiMCP::SliderFloat(LocalizationManager::Get("Settings.Padding").c_str(),
                                  &settings->navEntityIconPadding, 0.0f, 8.0f, "%.1f px");
            ImGuiMCP::Checkbox(LocalizationManager::Get("Settings.Icons3D").c_str(), &settings->navEntityIcon3D);
            ImGuiMCP::Checkbox(LocalizationManager::Get("Settings.BgShadow").c_str(), &settings->navEntityIconShadow);
            ImGuiMCP::Unindent();
        }

        if (ImGuiMCP::CollapsingHeader(LocalizationManager::Get("Settings.ActorsHeader").c_str())) {
            ImGuiMCP::Indent();
            {
                const FlowItem items[] = {
                    { LocalizationManager::Get("Settings.ShowAllies"),      &settings->navActorShowAlly },
                    { LocalizationManager::Get("Settings.ShowHostiles"),    &settings->navActorShowHostile },
                    { LocalizationManager::Get("Settings.ShowNeutralNPCs"), &settings->navActorShowNeutral },
                    { LocalizationManager::Get("Settings.ShowWildlife"),    &settings->navActorShowAnimal },
                };
                CheckboxFlow(items, 4);
            }
            ImGuiMCP::SliderFloat(LocalizationManager::Get("Settings.DetectionRange").c_str(), &settings->navActorProximityUnits, 200.0f, 4000.0f, "%.0f");
            ImGuiMCP::Unindent();
        }

        if (ImGuiMCP::CollapsingHeader(LocalizationManager::Get("Settings.GlanceRadarHeader").c_str())) {
            ImGuiMCP::Indent();
            ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.Size") + "##glance").c_str(), &settings->navGlanceSize, 100, 500);
            {
                const std::string circle = LocalizationManager::Get("Settings.Circle");
                const std::string square = LocalizationManager::Get("Settings.Square");
                const char* shapes[] = { circle.c_str(), square.c_str() };
                ImGuiMCP::Combo(LocalizationManager::Get("Settings.GlanceShape").c_str(),
                                &settings->navGlanceShape, shapes, 2);
            }
            {
                const std::string fixedNorth = LocalizationManager::Get("Settings.GlanceFixedNorth");
                const std::string followCamera = LocalizationManager::Get("Settings.GlanceFollowCamera");
                const char* orientations[] = { fixedNorth.c_str(), followCamera.c_str() };
                ImGuiMCP::Combo(LocalizationManager::Get("Settings.GlanceOrientation").c_str(),
                                &settings->navGlanceOrientation, orientations, 2);
            }
            ImGuiMCP::Checkbox(LocalizationManager::Get("Settings.GlanceShowCardinals").c_str(), &settings->navGlanceShowCardinals);
            ImGuiMCP::Checkbox(LocalizationManager::Get("Settings.GlanceShowLocation").c_str(), &settings->navGlanceShowLocation);
            ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.PinSize") + "##glance").c_str(), &settings->navGlancePinSize, 6, 24);
            ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.ActorDotSize") + "##glance").c_str(), &settings->navGlanceActorDotSize, 2, 16);
            ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.EntityIconSize") + "##glance").c_str(), &settings->navGlanceEntityIconSize, 12, 48);
            ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.ZoomMap") + "##glance").c_str(), &settings->navGlanceRangeUnits, 400.0f, 3000.0f, "%.0f");
            ImGuiMCP::Spacing();

            {
                auto* io = ImGuiMCP::GetIO();
                const ImGuiMCP::ImVec2 disp = io ? io->DisplaySize : ImGuiMCP::ImVec2{ 1920.0f, 1080.0f };
                const int drawnMaxX = GlanceBounds::maxPosX.load();
                const int drawnMaxY = GlanceBounds::maxPosY.load();
                const int maxX = drawnMaxX >= 0 ? drawnMaxX : std::max(0, static_cast<int>(disp.x) - settings->navGlanceSize);
                const int maxY = drawnMaxY >= 0 ? drawnMaxY : std::max(0, static_cast<int>(disp.y) - settings->navGlanceSize);
                ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.PositionX") + "##glance").c_str(), &settings->navGlancePosX, 0, maxX);
                ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.PositionY") + "##glance").c_str(), &settings->navGlancePosY, 0, maxY);
                if (ImGuiMCP::Button((LocalizationManager::Get("Settings.ResetPosition") + "##glance").c_str())) {
                    settings->navGlancePosX = maxX - 22;
                    settings->navGlancePosY = 22;
                }
            }
            ImGuiMCP::Spacing();

            if (ImGuiMCP::TreeNode((LocalizationManager::Get("Settings.EntitiesTree") + "##glance").c_str())) {
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
                ImGuiMCP::TreePop();
            }
            ImGuiMCP::Unindent();
        }

        if (ImGuiMCP::CollapsingHeader(LocalizationManager::Get("Settings.InspectModeHeader").c_str())) {
            ImGuiMCP::Indent();
            ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.SizePercent") + "##inspect").c_str(), &settings->navInspectSizePct, 50, 100);
            ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.PinSize") + "##inspect").c_str(), &settings->navInspectPinSize, 12, 48);
            ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.ActorDotSize") + "##inspect").c_str(), &settings->navInspectActorDotSize, 3, 24);
            ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.EntityIconSize") + "##inspect").c_str(), &settings->navInspectEntityIconSize, 10, 80);
            ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.ZoomMapRadarMode") + "##inspect").c_str(), &settings->navInspectRadarRangeUnits, 1000.0f, 8000.0f, "%.0f");
            ImGuiMCP::Spacing();

            if (ImGuiMCP::TreeNode((LocalizationManager::Get("Settings.EntitiesTree") + "##inspect").c_str())) {
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
                ImGuiMCP::TreePop();
            }
            ImGuiMCP::Unindent();
        }
    }

    static void PanelBeacons(Settings* settings) {
        if (ImGuiMCP::CollapsingHeader(LocalizationManager::Get("Settings.WorldBeaconsHeader").c_str(), ImGuiMCP::ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGuiMCP::Indent();
            ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.PinSize") + "##beacon").c_str(), &settings->navBeaconPinSize, 12, 48);
            ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.PillarHeight") + "##beacon").c_str(), &settings->navBeaconPillarHeight,
                                  50.0f, 2000.0f, "%.0f");
            ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.NearHeight") + "##beacon").c_str(), &settings->navBeaconNearScale,
                                  0.05f, 1.0f, "%.2f");
            {
                float fadeMeters = settings->navBeaconFadeNearUnits / Settings::kUnitsPerMeter;
                if (ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.FadeNear") + "##beacon").c_str(), &fadeMeters, 1.0f, 30.0f, "%.0f m")) {
                    settings->navBeaconFadeNearUnits = fadeMeters * Settings::kUnitsPerMeter;
                }
            }
            ImGuiMCP::Checkbox((LocalizationManager::Get("Settings.ClearOnArrival") + "##beacon").c_str(), &settings->navBeaconClearOnArrival);
            ImGuiMCP::Spacing();

            ImGuiMCP::Checkbox(LocalizationManager::Get("Settings.OffScreenArrow").c_str(), &settings->navBeaconEdgeIndicator);
            ImGuiMCP::Spacing();

            ImGuiMCP::Unindent();
        }

        if (ImGuiMCP::CollapsingHeader(LocalizationManager::Get("Settings.LootBeamsHeader").c_str())) {
            ImGuiMCP::Indent();
            {
                const std::string modern = LocalizationManager::Get("Settings.LootBeamModern");
                const std::string wisp = LocalizationManager::Get("Settings.LootBeamWisp");
                const char* designs[] = { modern.c_str(), wisp.c_str() };
                ImGuiMCP::Combo(LocalizationManager::Get("Settings.LootBeamDesign").c_str(),
                                &settings->lootBeamDesign, designs, 2);
            }
            {
                const bool modern = settings->lootBeamDesign == 0;
                ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.LootBeamBottomWidth") + "##lootbeam").c_str(),
                    modern ? &settings->lootBeamModernBottomWidth : &settings->lootBeamWispBottomWidth,
                    0.0f, modern ? 36.0f : 20.0f, "%.2f px", ImGuiMCP::ImGuiSliderFlags_AlwaysClamp);
                ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.LootBeamTopWidth") + "##lootbeam").c_str(),
                    modern ? &settings->lootBeamModernTopWidth : &settings->lootBeamWispTopWidth,
                    0.0f, modern ? 36.0f : 20.0f, "%.2f px", ImGuiMCP::ImGuiSliderFlags_AlwaysClamp);
                ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.LootBeamGroundGlow") + "##lootbeam").c_str(),
                    modern ? &settings->lootBeamModernGroundGlow : &settings->lootBeamWispGroundGlow,
                    0.0f, 1.0f, "%.2f", ImGuiMCP::ImGuiSliderFlags_AlwaysClamp);
            }
            ImGuiMCP::Spacing();
            {
                const FlowItem items[] = {
                    { LocalizationManager::Get("Settings.HideInCombat") + "##lootbeam", &settings->lootBeamHideInCombat },
                    { LocalizationManager::Get("Settings.HideAfterLooting") + "##lootbeam", &settings->lootBeamHideAfterLooting },
                    { LocalizationManager::Get("Settings.HideAfterInspecting") + "##lootbeam", &settings->lootBeamHideAfterInspecting },
                    { LocalizationManager::Get("Settings.DiscoverOnSight") + "##lootbeam", &settings->lootBeamRequireLOS },
                    { LocalizationManager::Get("Settings.HideBehindWalls") + "##lootbeam", &settings->lootBeamOcclusion },
                    { LocalizationManager::Get("Settings.HideOwnedLoot") + "##lootbeam", &settings->lootBeamHideOwned },
                    { LocalizationManager::Get("Settings.TrackDroppedWeapons") + "##lootbeam", &settings->lootBeamTrackDroppedWeapons },
                };
                CheckboxFlow(items, 7);
            }
            ImGuiMCP::Spacing();

            {
                float rangeMeters = settings->lootBeamRangeUnits / Settings::kUnitsPerMeter;
                if (ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.Range") + "##lootbeam").c_str(), &rangeMeters, 3.0f, 85.0f, "%.0f m")) {
                    settings->lootBeamRangeUnits = rangeMeters * Settings::kUnitsPerMeter;
                }
            }
            {
                float bandMeters = settings->lootBeamZBandUnits / Settings::kUnitsPerMeter;
                if (ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.FloorBand") + "##lootbeam").c_str(), &bandMeters, 1.0f, 28.0f, "%.0f m")) {
                    settings->lootBeamZBandUnits = bandMeters * Settings::kUnitsPerMeter;
                }
            }
            ImGuiMCP::Spacing();

            ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.BeamLength") + "##lootbeam").c_str(), &settings->lootBeamLengthUnits,
                                  20.0f, 600.0f, "%.0f");
            {
                float brightnessPct = settings->lootBeamBrightness * 100.0f;
                if (ImGuiMCP::SliderFloat((LocalizationManager::Get("Settings.Brightness") + "##lootbeam").c_str(), &brightnessPct, 10.0f, 100.0f, "%.0f%%")) {
                    settings->lootBeamBrightness = brightnessPct / 100.0f;
                }
            }
            ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.IconSize") + "##lootbeam").c_str(), &settings->lootBeamIconSize, 10, 48);
            ImGuiMCP::Spacing();

            ImGuiMCP::SliderInt((LocalizationManager::Get("Settings.MinGoldValue") + "##lootbeam").c_str(),
                                &settings->lootBeamMinGoldValue, 0, 5000,
                                settings->lootBeamMinGoldValue == 0 ? "Off" : "%d g");
            ImGuiMCP::Spacing();

            if (ImGuiMCP::TreeNode((LocalizationManager::Get("Settings.LootTypesTree") + "##lootbeam").c_str())) {
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
                    CheckboxFlow(items, static_cast<int>(sizeof(items) / sizeof(items[0])));
                }
                ImGuiMCP::TreePop();
            }
            ImGuiMCP::Unindent();
        }
    }

    static void DrawActivePanel(Settings* settings) {
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
    }

    static void __stdcall RenderFrameworkSettings() {
        auto* settings = Settings::GetSingleton();
        if (!settings) {
            return;
        }

        if (!g_tabInitialized) {
            const auto value = settings->menuActiveTab;
            g_activeTab = value >= 0 && value < static_cast<std::int32_t>(Tab::Count)
                ? static_cast<Tab>(value)
                : Tab::General;
            g_tabInitialized = true;
        }

        DrawTabGrid(settings);
        ImGuiMCP::Separator();

        if (ImGuiMCP::BeginChild("CMMSettingsContent", ImGuiMCP::ImVec2(0.0f, -45.0f))) {
            DrawActivePanel(settings);
        }
        ImGuiMCP::EndChild();

        ImGuiMCP::Separator();
        if (ImGuiMCP::Button(
                LocalizationManager::Get("Settings.SaveSettingsButton").c_str(),
                ImGuiMCP::ImVec2(-1.0f, 0.0f))) {
            settings->Save();
        }
    }
}

void SettingsMenu::RegisterWithMenuFramework() {
    if (SettingsMenuFramework::g_registered) {
        return;
    }

    auto module = GetMenuFrameworkModule();
    if (!module || !GetProcAddress(module, "AddSectionItem")) {
        return;
    }

    SKSEMenuFramework::SetSection("Custom Markers");
    SKSEMenuFramework::AddSectionItem(
        "HUD Settings", &SettingsMenuFramework::RenderFrameworkSettings);
    SettingsMenuFramework::g_registered = true;
    logger::info("SettingsMenu: registered with SKSE Menu Framework");
}
