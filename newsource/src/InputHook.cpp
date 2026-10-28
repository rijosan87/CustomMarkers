#include "InputHook.h"
#include "InputHandler.h"   // IsModifierScancode for the rebind capture
#include "MapMarkerRenderer.h"
#include "LocalMapRenderer.h"
#include "MinimapRenderer.h"
#include "LootBeamRenderer.h"
#include "LookPinRenderer.h"
#include "MapLookRemapHook.h"
#include "MenuUtils.h"
#include "Settings.h"
#include "PathTracker.h"
#include "ImGui/Renderer.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <imgui.h>
#include <atomic>
#include <Windows.h>
#include "HoverImageRenderer.h"
#include "SettingsMenu.h"

// ============================================================================
// InputHook — keyboard input capture for ImGui text fields on the map
// ----------------------------------------------------------------------------
// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):
//   Working: feeds kChar events to ImGui AddInputCharacter and nav keys
//            (Backspace, Delete, Enter, arrows, Home, End) via AddKeyEvent.
//            Blocks keyboard IsDown() events from reaching the engine while
//            ImGui WantTextInput is true, preventing accidental map actions.
//            Mouse wheel ticks (kMouse idCode 8 = up, 9 = down, from
//            BSWin32MouseDevice::Process at RVA 0xCDE920) are forwarded to
//            ImGui via AddMouseWheelEvent and consumed while the note dialog
//            is open, ImGui WantCaptureMouse is true, or CursorOverPanel() is
//            true, so list panel scrolling and the category combo work and
//            the map camera does not zoom under the panel/dialog. See
//            findings/findings-map-scroll-wheel-route.md and
//            findings/spec-findings-ListPanel-Scroll-Collapse.md.
//   Text is fed only through kChar. Renderer.cpp's WndProc hook does not feed
//   characters; duplicate events from upstream are still possible.
//   [Logging] InputVerboseLog=1 records all arriving characters/buttons,
//   batch/frame/thread IDs and final consumption decisions, even without text
//   focus. Renderer.cpp records ImGui processing/focus and note diagnostics
//   record buffer edits/wrapping. Events removed upstream cannot be observed.
//   Wheel diagnostics also record MapMenu/tilt state, the real Windows pointer,
//   foreground/focus/window-under-pointer ownership, client/clip/monitor bounds,
//   and ThirdPersonState zoom offsets before/after Skyrim dispatches the batch.
//   Expanded diagnostics await Visual Studio build and in-game verification.
//   Hook target: BSInputDeviceManager::ProcessEvents (ID 67315 SE / 68617 AE).
//   Right-click routing (idCode 1) dispatches to one of three popups by
//   what's under the cursor: vanilla marker -> HoverImageRenderer's
//   "VanillaContextMenu"; our own custom pin (world OR local map) ->
//   MapMarkerRenderer's "CustomContextMenu" (Set/Clear Destination, Rename,
//   Remove); empty terrain -> MapMarkerRenderer's "TerrainContextMenu". Custom
//   pins used to be a no-op here (hover panel only, no menu); see
//   MapMarkerRenderer/LocalMapRenderer STATUS blocks for the hover-panel side
//   of this change (info-only now, no action rows).
//   While a blocking SKSE Menu Framework window is open over MapMenu, map
//   right-click routing and wheel forwarding are disabled. Hidden CMM surfaces
//   cannot retain clickable behavior behind the framework.
//   HUD Settings guard (2026-07-16): while SettingsMenu::IsVisible(), Tab/Esc
//   (DX 0x0F/0x01) close the settings window and are swallowed through release
//   so the map stays open; right-click is swallowed with no menu (same rule as
//   the note dialog), so no marker placement while settings are open.
//   Inspect mode-toggle swallow (2026-07-17): while the Inspect overlay is shown
//   (MinimapRenderer::IsInspectShown), the toggle key is consumed through release
//   so vanilla sheathe-weapon (default R) never fires during a mode toggle.
//   Quick-mark swallow (2026-07-17): same pattern, separate static latch, for
//   navQuickMarkKey (default F) so vanilla toggle-POV never fires while
//   Inspect places a marker. MinimapRenderer polls independently via
//   GetAsyncKeyState to trigger the placement; this hook only blocks the engine.
//   Inspect path-tracking toggle (2026-09-10): while the Inspect overlay is shown
//   in an interior cell (MinimapRenderer::IsInteriorInspectShown) and global
//   enablePathTracking is true, pathTrackKey (default T) toggles
//   PathTracker::ToggleTracking and is consumed through release so vanilla
//   Wait menu never fires.
//   Inspect toggle mode (2026-09-15): when navInspectActivation is 1 (Toggle),
//   navInspectKey triggers MinimapRenderer::ToggleInspect and is swallowed through
//   release. Esc cancels Inspect in toggle mode and is swallowed through release.
//   Mini Radar and Loot Beam visibility toggles (2026-09-15): their unbound-by-
//   default key + modifier bindings flip session-only renderer gates and consume
//   the matched input through release on keyboard, mouse, and gamepad.
//   Hotkey rework: settings now store DIK, so the three swallow blocks compare
//   btn->GetIDCode() directly. The MapVirtualKeyA(VK->scancode) conversions they
//   used are gone; the DIK->VK direction lives at the poll sites instead. Look &
//   Pin tests its STORED modifier via ModifierMatches rather than hardcoded Ctrl,
//   so the swallow and plugin.cpp's dispatch agree on when it fires.
//   Press-to-capture: while g_hkCaptureCb is armed (SettingsMenu's Hotkeys tab),
//   the keyboard and mouse branches swallow EVERY button and hand the first
//   non-modifier one to the callback. Recording must happen here, not in
//   plugin.cpp's sink: this hook unlinks the consumed event, so the sink would
//   are reserved (swallowed, capture stays armed); wheel ticks are skipped.
//   Settings hotkey dispatch is owned here directly with a press-through-release
//   latch across both MapMenu and gameplay hosts. In gameplay host, all gameplay
//   buttons, mouse-look, and thumbsticks are consumed to prevent character actions.
// ============================================================================

namespace {
    using ProcessInputQueueFn = void(*)(RE::BSTEventSource<RE::InputEvent*>*, RE::InputEvent**);
    static REL::Relocation<ProcessInputQueueFn> g_originalProcessInputQueue;

    // scancode is a DXScanCode (DirectInput set 1), not a VK. Values below are
    // those codes: 0x48 Up, 0x50 Down, 0x4B Left, 0x4D Right, 0x0E Backspace,
    // 0xD3 Delete, 0xC7 Home, 0xCF End, 0x1C Enter, 0x01 Escape.
    static ImGuiKey ScancodeToImGuiKey(std::uint32_t scancode) {
        switch (scancode) {
            case 0x48: return ImGuiKey_UpArrow;
            case 0x50: return ImGuiKey_DownArrow;
            case 0x4B: return ImGuiKey_LeftArrow;
            case 0x4D: return ImGuiKey_RightArrow;
            case 0x0E: return ImGuiKey_Backspace;
            case 0xD3: return ImGuiKey_Delete;
            case 0xC7: return ImGuiKey_Home;
            case 0xCF: return ImGuiKey_End;
            case 0x1C: return ImGuiKey_Enter;
            case 0x01: return ImGuiKey_Escape;
            default:   return ImGuiKey_None;
        }
    }

    struct SettingsHotkeyLatch {
        bool active{ false };
        RE::INPUT_DEVICE device{};
        std::uint32_t idCode{ 0 };
    };
    static SettingsHotkeyLatch s_hotkeyLatch{};

    struct InspectPathToggleLatch {
        bool active{ false };
        RE::INPUT_DEVICE device{};
        std::uint32_t idCode{ 0 };
    };
    static InspectPathToggleLatch s_inspectPathToggleLatch{};

    struct InspectToggleLatch {
        bool active{ false };
        RE::INPUT_DEVICE device{};
        std::uint32_t idCode{ 0 };
    };
    static InspectToggleLatch s_inspectToggleLatch{};

    struct VisibilityToggleLatch {
        bool active{ false };
        RE::INPUT_DEVICE device{};
        std::uint32_t idCode{ 0 };
    };
    static VisibilityToggleLatch s_glanceToggleLatch{};
    static VisibilityToggleLatch s_lootBeamToggleLatch{};
    static std::atomic<std::uint64_t> s_inputVerboseSequence{ 0 };
    static std::atomic<std::uint64_t> s_inputVerboseBatch{ 0 };

    struct CameraZoomSnapshot {
        bool present{ false };
        bool active{ false };
        float target{ 0.0f };
        float current{ 0.0f };
        float saved{ 0.0f };
        float pitch{ 0.0f };
    };

    static CameraZoomSnapshot ReadCameraZoom() {
        CameraZoomSnapshot result{};
        auto* camera = RE::PlayerCamera::GetSingleton();
        if (!camera) return result;

        auto* base = camera->cameraStates[RE::CameraStates::kThirdPerson].get();
        if (!base) return result;

        const auto* state = static_cast<RE::ThirdPersonState*>(base);
        result.present = true;
        result.active = camera->currentState.get() == base;
        result.target = state->targetZoomOffset;
        result.current = state->currentZoomOffset;
        result.saved = state->savedZoomOffset;
        result.pitch = state->pitchZoomOffset;
        return result;
    }

    static void Hook_ProcessInputQueue(RE::BSTEventSource<RE::InputEvent*>* a_source,
                                       RE::InputEvent** a_events) {
        const bool verbose = Settings::GetSingleton()->inputVerboseLog;
        const auto batch = verbose ? ++s_inputVerboseBatch : 0;
        if (!ImGui::GetCurrentContext() || !a_events || !*a_events) {
            // Empty queues are summarized by the render heartbeat; logging
            // every empty dispatch would obscure the useful event records.
            if (verbose && a_events && *a_events) {
                logger::info("[InputVerbose] bypass batch={} thread={} reason=no_imgui_context eventsPresent=true",
                             batch, GetCurrentThreadId());
            }
            g_originalProcessInputQueue(a_source, a_events);
            return;
        }

        const bool wantText        = ImGui::GetIO().WantTextInput;
        const bool dialogOpen      = MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen();
        const bool settingsOpen    = SettingsMenu::IsVisible();
        const bool isGameplayHosted = SettingsMenu::IsGameplayHosted();
        const bool mapOverlayBlocked = MenuUtils::ShouldBlockMapOverlay();
        const bool isMapOpen = RE::UI::GetSingleton() &&
            RE::UI::GetSingleton()->IsMenuOpen(RE::MapMenu::MENU_NAME);
        const bool blockKeys       = wantText || dialogOpen || settingsOpen;

        std::uint32_t wheelUpTicks = 0;
        std::uint32_t wheelDownTicks = 0;
        std::uint32_t wheelConsumedTicks = 0;
        bool sawWheelDownEvent = false;
        CameraZoomSnapshot wheelBefore{};

        auto* settings = Settings::GetSingleton();

        RE::InputEvent* prev = nullptr;
        RE::InputEvent* curr = *a_events;

        while (curr) {
            bool consume = false;

            if (curr->eventType == RE::INPUT_EVENT_TYPE::kChar) {
                auto* charEvent = static_cast<RE::CharEvent*>(curr);
                const unsigned int ch = static_cast<unsigned int>(charEvent->keyCode);
                if (wantText) {
                    ImGui::GetIO().AddInputCharacter(ch);
                }
                // Only consume text characters if the note dialog / settings is capturing input.
                // Doing this unconditionally outside these states would break the Skyrim console.
                if (blockKeys || isGameplayHosted) {
                    consume = true;
                }
                if (verbose) {
                    logger::info(
                        "[InputVerbose] kChar seq={} batch={} frame={} thread={} codepoint=U+{:04X} wantText={} dialog={} settings={} gameplay={} forwarded={} consumed={}",
                        ++s_inputVerboseSequence, batch, ImGui::GetFrameCount(), GetCurrentThreadId(), ch, wantText, dialogOpen, settingsOpen,
                        isGameplayHosted, wantText, consume);
                }
            } else if (curr->eventType == RE::INPUT_EVENT_TYPE::kButton) {
                auto* btn = curr->AsButtonEvent();
                if (btn) {
                    const auto device = btn->GetDevice();
                    const auto idCode = btn->GetIDCode();
                    const auto code   = KeycodeFromScancode(idCode, device);

                    // Check release latch for the settings hotkey first
                    if (s_hotkeyLatch.active && s_hotkeyLatch.device == device && s_hotkeyLatch.idCode == idCode) {
                        consume = true;
                        if (btn->IsUp()) {
                            s_hotkeyLatch.active = false;
                        }
                    }

                    if (s_inspectPathToggleLatch.active &&
                        s_inspectPathToggleLatch.device == device &&
                        s_inspectPathToggleLatch.idCode == idCode) {
                        consume = true;
                        if (btn->IsUp()) {
                            s_inspectPathToggleLatch.active = false;
                        }
                    } else if (!s_inspectPathToggleLatch.active && btn->IsDown() &&
                               settings->enablePathTracking &&
                               MinimapRenderer::IsInteriorInspectShown() &&
                               HotkeyMatches(code, settings->pathTrackKey,
                                             settings->pathTrackModifier)) {
                        PathTracker::GetSingleton()->ToggleTracking();
                        logger::debug("Inspect path tracking hotkey: toggled path tracking");
                        s_inspectPathToggleLatch.active = true;
                        s_inspectPathToggleLatch.device = device;
                        s_inspectPathToggleLatch.idCode = idCode;
                        consume = true;
                    }

                    if (s_inspectToggleLatch.active &&
                        s_inspectToggleLatch.device == device &&
                        s_inspectToggleLatch.idCode == idCode) {
                        consume = true;
                        if (btn->IsUp()) {
                            s_inspectToggleLatch.active = false;
                        }
                    } else if (!s_inspectToggleLatch.active && btn->IsDown() &&
                               settings->navInspectActivation == 1 &&
                               HotkeyMatches(code, settings->navInspectKey,
                                             settings->navInspectModifier)) {
                        MinimapRenderer::ToggleInspect();
                        logger::debug("Inspect toggle hotkey: toggled inspect overlay");
                        s_inspectToggleLatch.active = true;
                        s_inspectToggleLatch.device = device;
                        s_inspectToggleLatch.idCode = idCode;
                        consume = true;
                    }

                    if (s_glanceToggleLatch.active &&
                        s_glanceToggleLatch.device == device &&
                        s_glanceToggleLatch.idCode == idCode) {
                        consume = true;
                        if (btn->IsUp()) {
                            s_glanceToggleLatch.active = false;
                        }
                    } else if (!s_glanceToggleLatch.active && btn->IsDown() &&
                               HotkeyMatches(code, settings->navGlanceToggleKey,
                                             settings->navGlanceToggleModifier)) {
                        MinimapRenderer::ToggleGlanceVisible();
                        logger::debug("Mini Radar toggle hotkey: visible={}",
                                      MinimapRenderer::IsGlanceVisible());
                        s_glanceToggleLatch.active = true;
                        s_glanceToggleLatch.device = device;
                        s_glanceToggleLatch.idCode = idCode;
                        consume = true;
                    }

                    if (s_lootBeamToggleLatch.active &&
                        s_lootBeamToggleLatch.device == device &&
                        s_lootBeamToggleLatch.idCode == idCode) {
                        consume = true;
                        if (btn->IsUp()) {
                            s_lootBeamToggleLatch.active = false;
                        }
                    } else if (!s_lootBeamToggleLatch.active && btn->IsDown() &&
                               HotkeyMatches(code, settings->lootBeamToggleKey,
                                             settings->lootBeamToggleModifier)) {
                        LootBeamRenderer::ToggleVisible();
                        logger::debug("Loot Beam toggle hotkey: visible={}",
                                      LootBeamRenderer::IsVisible());
                        s_lootBeamToggleLatch.active = true;
                        s_lootBeamToggleLatch.device = device;
                        s_lootBeamToggleLatch.idCode = idCode;
                        consume = true;
                    }

                    if (device == RE::INPUT_DEVICE::kKeyboard) {
                        // ALWAYS feed nav keys to ImGui so we don't miss KeyUp events
                        // and get stuck keys (which causes instant auto-submit).
                        ImGuiKey imKey = ScancodeToImGuiKey(idCode);
                        if (imKey != ImGuiKey_None) {
                            ImGui::GetIO().AddKeyEvent(imKey, btn->IsDown() || btn->IsPressed());
                        }

                        // CAPTURE (highest priority): while PanelHotkeys waits for a
                        // rebind key, this hook both RECORDS the key and swallows it.
                        if (g_hkCaptureCb) {
                            consume = true;  // swallow every key while armed
                            if (btn->IsDown() && idCode != 0x01 && idCode != 0x0F &&
                                !InputHandler::IsModifierScancode(idCode)) {
                                auto cb = std::move(g_hkCaptureCb);
                                g_hkCaptureCb = nullptr;
                                cb(idCode);
                            }
                        } else if (!s_hotkeyLatch.active) {
                            // Settings hotkey toggle (keyboard)
                            if (HotkeyMatches(code, settings->menuSettingsKey, settings->menuSettingsModifier)) {
                                if (btn->IsDown()) {
                                    if (SettingsMenu::Toggle()) {
                                        s_hotkeyLatch.active = true;
                                        s_hotkeyLatch.device = device;
                                        s_hotkeyLatch.idCode = idCode;
                                        consume = true;
                                    }
                                }
                            }
                        }

                        // Block all keyboard button events if blockKeys is true
                        if (blockKeys) {
                            consume = true;
                        }

                        // HUD Settings window open: Tab/Esc must close the settings
                        // window first, not the map or system menu. Swallow through release.
                        {
                            static bool swallowSettingsClose = false;
                            if (idCode == 0x01 || idCode == 0x0F) { // Esc, Tab (DX scancodes)
                                if (btn->IsDown() && SettingsMenu::IsVisible()) {
                                    SettingsMenu::Hide("esc_or_tab");
                                    swallowSettingsClose = true;
                                }
                                if (swallowSettingsClose) {
                                    consume = true;
                                }
                                if (btn->IsUp()) {
                                    swallowSettingsClose = false;
                                }
                            }
                        }

                        // Look & Pin active: Esc cancels the mode instead of opening
                        // the system menu.
                        {
                            static bool swallowLookPinEsc = false;
                            if (idCode == 0x01) {  // Esc (DX scancode)
                                if (btn->IsDown() && LookPinRenderer::IsActive()) {
                                    LookPinRenderer::ForceExit();
                                    swallowLookPinEsc = true;
                                }
                                if (swallowLookPinEsc) {
                                    consume = true;
                                }
                                if (btn->IsUp()) {
                                    swallowLookPinEsc = false;
                                }
                            }
                        }

                        // Inspect active in toggle mode: Esc cancels Inspect instead of
                        // opening the system menu.
                        {
                            static bool swallowInspectEsc = false;
                            if (idCode == 0x01) {  // Esc (DX scancode)
                                if (btn->IsDown() && settings->navInspectActivation == 1 &&
                                    MinimapRenderer::IsInspectToggleActive()) {
                                    MinimapRenderer::ResetInspectToggle();
                                    swallowInspectEsc = true;
                                }
                                if (swallowInspectEsc) {
                                    consume = true;
                                }
                                if (btn->IsUp()) {
                                    swallowInspectEsc = false;
                                }
                            }
                        }

                        // Inspect view-mode toggle (default R)
                        {
                            static bool swallowToggle = false;
                            const std::int32_t toggleKey =
                                settings->navInspectModeToggleKey;
                            if (toggleKey >= 1 &&
                                idCode == static_cast<std::uint32_t>(toggleKey)) {
                                if (btn->IsDown() && MinimapRenderer::IsInspectShown()) {
                                    swallowToggle = true;
                                }
                                if (swallowToggle) {
                                    consume = true;
                                }
                                if (btn->IsUp()) {
                                    swallowToggle = false;
                                }
                            }
                        }

                        // Quick-mark key (default F)
                        {
                            static bool swallowQuickMark = false;
                            const std::int32_t markKey =
                                settings->navQuickMarkKey;
                            if (markKey >= 1 &&
                                idCode == static_cast<std::uint32_t>(markKey)) {
                                if (btn->IsDown() && MinimapRenderer::IsInspectShown()) {
                                    swallowQuickMark = true;
                                }
                                if (swallowQuickMark) {
                                    consume = true;
                                }
                                if (btn->IsUp()) {
                                    swallowQuickMark = false;
                                }
                            }
                        }

                        // Look & Pin toggle (default Ctrl+N)
                        {
                            static bool swallowLookPin = false;
                            const std::int32_t pinKey = settings->lookPinKey;
                            const bool modHeld =
                                ModifierMatches(settings->lookPinModifier);
                            if (pinKey >= 1 &&
                                idCode == static_cast<std::uint32_t>(pinKey)) {
                                if (btn->IsDown() && (modHeld || LookPinRenderer::IsActive())) {
                                    swallowLookPin = true;
                                }
                                if (swallowLookPin) {
                                    consume = true;
                                }
                                if (btn->IsUp()) {
                                    swallowLookPin = false;
                                }
                            }
                        }
                    } else if (device == RE::INPUT_DEVICE::kMouse) {
                        // CAPTURE, mouse half
                        if (g_hkCaptureCb) {
                            if (idCode != 8 && idCode != 9) {
                                consume = true;
                                if (btn->IsDown()) {
                                    auto cb = std::move(g_hkCaptureCb);
                                    g_hkCaptureCb = nullptr;
                                    cb(idCode + 256);
                                }
                            }
                        } else if (!s_hotkeyLatch.active && idCode != 8 && idCode != 9) {
                            // Settings hotkey toggle (mouse button)
                            if (HotkeyMatches(code, settings->menuSettingsKey, settings->menuSettingsModifier)) {
                                if (btn->IsDown()) {
                                    if (SettingsMenu::Toggle()) {
                                        s_hotkeyLatch.active = true;
                                        s_hotkeyLatch.device = device;
                                        s_hotkeyLatch.idCode = idCode;
                                        consume = true;
                                    }
                                }
                            }
                        }

                        // Right-click on map (idCode == 1)
                        if (idCode == 1 && isMapOpen && !mapOverlayBlocked) {
                            static bool swallowRightClick = false;

                            if (btn->IsDown()) {
                                const bool overVanilla = (HoverImageRenderer::GetHoveredFormID() != 0);
                                const std::uint32_t worldPinId = MapMarkerRenderer::GetSingleton()->GetHoveredMarkerId();
                                const std::uint32_t localPinId = LocalMapRenderer::GetSingleton()->GetHoveredMarkerId();
                                const std::uint32_t customPinId = worldPinId != 0 ? worldPinId : localPinId;
                                const bool overPanel = ImGui::GetIO().WantCaptureMouse ||
                                                       MapMarkerRenderer::GetSingleton()->CursorOverPanel();

                                if (dialogOpen || SettingsMenu::IsVisible()) {
                                    swallowRightClick = true;
                                } else if (overPanel) {
                                    swallowRightClick = true;
                                } else if (overVanilla) {
                                    swallowRightClick = true;
                                    HoverImageRenderer::OpenContextMenu();
                                } else if (customPinId != 0) {
                                    swallowRightClick = true;
                                    MapMarkerRenderer::GetSingleton()->OpenCustomContextMenu(customPinId);
                                } else if (HoverImageRenderer::IsLocalMarkerHovered()) {
                                    swallowRightClick = true;
                                    HoverImageRenderer::OpenLocalContextMenu();
                                } else {
                                    swallowRightClick = true;
                                    MapMarkerRenderer::GetSingleton()->OpenTerrainMenu();
                                }
                            }

                            if (swallowRightClick) {
                                consume = true;
                            }

                            if (btn->IsUp()) {
                                swallowRightClick = false;
                            }
                        }

                        // Wheel ticks 8 & 9: route to ImGui during map dialog/panels or gameplay-hosted settings
                        if (idCode == 8 || idCode == 9) {
                            const bool blockMouse = (isMapOpen && !mapOverlayBlocked &&
                                (dialogOpen
                                 || ImGui::GetIO().WantCaptureMouse
                                 || MapMarkerRenderer::GetSingleton()->CursorOverPanel()))
                                || isGameplayHosted;
                            if (blockMouse) {
                                if (btn->IsDown()) {
                                    const float delta = (idCode == 8) ? 1.0f : -1.0f;
                                    ImGui::GetIO().AddMouseWheelEvent(0.0f, delta);
                                }
                                consume = true;
                            }
                        }

                        // In gameplay host, consume all mouse buttons so clicks don't fire gameplay actions
                        if (isGameplayHosted) {
                            consume = true;
                        }
                    } else if (device == RE::INPUT_DEVICE::kGamepad) {
                        // In gameplay host, consume gamepad buttons
                        if (isGameplayHosted) {
                            consume = true;
                        }
                    }
                }
            } else if (curr->eventType == RE::INPUT_EVENT_TYPE::kMouseMove) {
                if (isGameplayHosted) {
                    consume = true;
                }
            } else if (curr->eventType == RE::INPUT_EVENT_TYPE::kThumbstick) {
                if (isGameplayHosted) {
                    consume = true;
                }
            }

            if (verbose && curr->eventType == RE::INPUT_EVENT_TYPE::kButton) {
                if (auto* btn = curr->AsButtonEvent()) {
                    const auto navKey = btn->GetDevice() == RE::INPUT_DEVICE::kKeyboard
                        ? ScancodeToImGuiKey(btn->GetIDCode()) : ImGuiKey_None;
                    logger::info(
                        "[InputVerbose] button seq={} batch={} frame={} thread={} device={} code=0x{:X} down={} pressed={} up={} value={} held={} navKey={} wantText={} dialog={} settings={} gameplay={} mapOpen={} tilting={} rebindNow={} settingsLatch={} pathLatch={} consumed={}",
                        ++s_inputVerboseSequence, batch, ImGui::GetFrameCount(), GetCurrentThreadId(),
                        static_cast<int>(btn->GetDevice()), btn->GetIDCode(), btn->IsDown(), btn->IsPressed(), btn->IsUp(),
                        btn->Value(), btn->HeldDuration(), static_cast<int>(navKey), wantText, dialogOpen,
                        settingsOpen, isGameplayHosted, isMapOpen, MapLookRemapHook::IsTilting(),
                        static_cast<bool>(g_hkCaptureCb), s_hotkeyLatch.active,
                        s_inspectPathToggleLatch.active, consume);

                    if (btn->GetDevice() == RE::INPUT_DEVICE::kMouse &&
                        (btn->GetIDCode() == 8 || btn->GetIDCode() == 9) && btn->IsDown()) {
                        if (!sawWheelDownEvent) {
                            wheelBefore = ReadCameraZoom();
                            sawWheelDownEvent = true;
                        }
                        if (btn->GetIDCode() == 8) ++wheelUpTicks;
                        else ++wheelDownTicks;
                        if (consume) ++wheelConsumedTicks;
                        ImGuiRenderer::LogInputEnvironment("directinput_wheel");
                    }
                }
            }

            if (consume) {
                if (prev) prev->next = curr->next;
                else       *a_events = curr->next;
                curr = curr->next;
            } else {
                prev = curr;
                curr = curr->next;
            }
        }

        g_originalProcessInputQueue(a_source, a_events);

        if (verbose && sawWheelDownEvent) {
            const auto wheelAfter = ReadCameraZoom();
            logger::info(
                "[InputVerbose] wheel_dispatch batch={} thread={} upTicks={} downTicks={} consumedTicks={} mapOpen={} tilting={} beforePresent={} beforeActive={} beforeTarget={} beforeCurrent={} beforeSaved={} beforePitch={} afterPresent={} afterActive={} afterTarget={} afterCurrent={} afterSaved={} afterPitch={}",
                batch, GetCurrentThreadId(), wheelUpTicks, wheelDownTicks,
                wheelConsumedTicks, isMapOpen, MapLookRemapHook::IsTilting(),
                wheelBefore.present, wheelBefore.active, wheelBefore.target,
                wheelBefore.current, wheelBefore.saved, wheelBefore.pitch,
                wheelAfter.present, wheelAfter.active, wheelAfter.target,
                wheelAfter.current, wheelAfter.saved, wheelAfter.pitch);
        }
    }
}

namespace InputHook {
    bool IsTextInputActive() {
        if (!ImGui::GetCurrentContext()) return false;
        return ImGui::GetIO().WantTextInput;
    }

    void ClearSettingsHotkeyLatch() {
        s_hotkeyLatch.active = false;
    }

    void Install() {
        // Note: BSInputDeviceManager::ProcessEvents
        // SE ID: 67315 | AE ID: 68617
        // Ensure you don't use 67315 for the AE relocation. It will resolve to the wrong function 
        // and cause the input hook to fail silently without crashing.
        REL::RelocationID hookLocation(67315, 68617);
        const std::uintptr_t hookAddress = hookLocation.address() + REL::Relocate(0x7B, 0x7B, 0x81);

        g_originalProcessInputQueue = SKSE::GetTrampoline().write_call<5>(
            hookAddress, Hook_ProcessInputQueue);

        logger::info("InputHook installed | hookAddr={:X}", hookAddress);
    }
}
