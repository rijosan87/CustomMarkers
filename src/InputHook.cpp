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
#include <chrono>
#include <Windows.h>
#include "HoverImageRenderer.h"
#include "SettingsMenu.h"

namespace {
    using ProcessInputQueueFn = void(*)(RE::BSTEventSource<RE::InputEvent*>*, RE::InputEvent**);
    static REL::Relocation<ProcessInputQueueFn> g_originalProcessInputQueue;

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

    struct ControllerInspectInputState {
        bool openTapPending{ false };
        std::int32_t openTapButton{ -1 };
        std::chrono::steady_clock::time_point openTappedAt{};
        bool openLatch{ false };
        std::int32_t openLatchButton{ -1 };
        bool addMarkerLatch{ false };
        bool closeLatch{ false };
        bool modeLatch{ false };
        bool pathLatch{ false };
    };

    static ControllerInspectInputState s_controllerInspect{};
    static bool s_controllerMapActivateLatch = false;
    static bool s_controllerContextOpenLatch = false;
    static bool s_controllerContextActivateLatch = false;
    static bool s_controllerContextCloseLatch = false;
    static bool s_controllerNoteActivateLatch = false;
    static bool s_controllerSettingsCloseLatch = false;
    constexpr std::uint32_t kControllerRB = 275;
    constexpr std::uint32_t kControllerB = 277;
    constexpr auto kControllerInspectDoubleTap = std::chrono::milliseconds(300);
    constexpr float kControllerInspectLongPressSeconds = 0.5f;
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
                    bool controllerInspectHandled = false;

                    if (device == RE::INPUT_DEVICE::kGamepad) {
                        auto* mapRenderer = MapMarkerRenderer::GetSingleton();
                        const bool mapModalOpen = MenuUtils::IsMapModalOpen();
                        const bool contextBlocked = mapOverlayBlocked || dialogOpen || settingsOpen ||
                            wantText || mapModalOpen || (isMapOpen && IsMapFading());

                        if (code == 276) {
                            if (s_controllerNoteActivateLatch) {
                                controllerInspectHandled = true;
                                consume = true;
                                if (btn->IsUp()) s_controllerNoteActivateLatch = false;
                            } else if (btn->IsDown() && isMapOpen && dialogOpen &&
                                       !mapOverlayBlocked && !mapModalOpen && !IsMapFading()) {
                                mapRenderer->QueueControllerNoteActivation();
                                s_controllerNoteActivateLatch = true;
                                controllerInspectHandled = true;
                                consume = true;
                            }
                        }

                        if (code == kControllerRB) {
                            if (s_controllerContextOpenLatch) {
                                controllerInspectHandled = true;
                                consume = true;
                                if (btn->IsUp()) s_controllerContextOpenLatch = false;
                            } else if (isMapOpen) {
                                // A map RB press cannot complete an Inspect double press.
                                controllerInspectHandled = true;
                                if (s_controllerInspect.openTapButton == static_cast<std::int32_t>(code)) {
                                    s_controllerInspect.openTapPending = false;
                                    s_controllerInspect.openTapButton = -1;
                                }
                                if (btn->IsDown() && !contextBlocked && !s_controllerInspect.openLatch) {
                                    s_controllerContextOpenLatch = true;
                                    consume = true;
                                    if (mapRenderer->IsControllerContextActiveOrOpening() ||
                                        ImGuiRenderer::IsAnyPopupOpen() || ImGui::GetIO().WantCaptureMouse ||
                                        mapRenderer->CursorOverPanel()) {
                                        // List, Legend, or another popup owns this cursor position.
                                    } else if (HoverImageRenderer::GetHoveredFormID() != 0) {
                                        HoverImageRenderer::OpenContextMenu();
                                        mapRenderer->MarkControllerContextOpening();
                                    } else {
                                        const std::uint32_t worldPinId = mapRenderer->GetHoveredMarkerId();
                                        const std::uint32_t localPinId = LocalMapRenderer::GetSingleton()->GetHoveredMarkerId();
                                        const std::uint32_t customPinId = worldPinId != 0 ? worldPinId : localPinId;
                                        if (customPinId != 0) {
                                            mapRenderer->OpenCustomContextMenu(customPinId);
                                            mapRenderer->MarkControllerContextOpening();
                                        } else if (HoverImageRenderer::IsLocalMarkerHovered()) {
                                            HoverImageRenderer::OpenLocalContextMenu();
                                            mapRenderer->MarkControllerContextOpening();
                                        } else {
                                            mapRenderer->OpenTerrainMenu();
                                            mapRenderer->MarkControllerContextOpening();
                                        }
                                    }
                                }
                            }
                        }

                        if (code == 276) {
                            if (s_controllerContextActivateLatch) {
                                controllerInspectHandled = true;
                                consume = true;
                                if (btn->IsUp()) s_controllerContextActivateLatch = false;
                            } else if (!s_controllerNoteActivateLatch && !s_controllerMapActivateLatch &&
                                       !s_controllerInspect.addMarkerLatch &&
                                       btn->IsDown() && isMapOpen && !contextBlocked &&
                                       mapRenderer->QueueControllerContextAction()) {
                                s_controllerContextActivateLatch = true;
                                controllerInspectHandled = true;
                                consume = true;
                            }
                        }

                        if (code == kControllerB) {
                            if (s_controllerContextCloseLatch) {
                                controllerInspectHandled = true;
                                consume = true;
                                if (btn->IsUp()) s_controllerContextCloseLatch = false;
                            } else if (!s_controllerSettingsCloseLatch && !s_controllerInspect.closeLatch &&
                                       btn->IsDown() && isMapOpen && !contextBlocked &&
                                       mapRenderer->QueueControllerContextClose()) {
                                s_controllerContextCloseLatch = true;
                                controllerInspectHandled = true;
                                consume = true;
                            }
                        }

                        if (code == kControllerB && !controllerInspectHandled) {
                            if (s_controllerSettingsCloseLatch) {
                                controllerInspectHandled = true;
                                consume = true;
                                if (btn->IsUp()) s_controllerSettingsCloseLatch = false;
                            } else if (btn->IsDown() && SettingsMenu::IsVisible()) {
                                SettingsMenu::Hide("controller_back");
                                s_controllerSettingsCloseLatch = true;
                                controllerInspectHandled = true;
                                consume = true;
                            }
                        }

                        const auto now = std::chrono::steady_clock::now();
                        const auto openButton = settings->controllerInspectOpenButton;
                        const auto openGesture = settings->controllerInspectOpenGesture;

                        if (s_controllerInspect.openTapPending &&
                            (openGesture != 1 || openButton != s_controllerInspect.openTapButton)) {
                            s_controllerInspect.openTapPending = false;
                            s_controllerInspect.openTapButton = -1;
                        }

                        if (s_controllerInspect.openLatch &&
                            code == static_cast<std::uint32_t>(s_controllerInspect.openLatchButton)) {
                            controllerInspectHandled = true;
                            consume = true;
                            if (btn->IsUp()) {
                                s_controllerInspect.openLatch = false;
                                s_controllerInspect.openLatchButton = -1;
                            }
                        } else if (!controllerInspectHandled && openButton >= 266 && openButton <= 281 &&
                                   code == static_cast<std::uint32_t>(openButton) &&
                                   !MinimapRenderer::IsControllerInspectActive()) {
                            bool openNow = false;
                            const char* gestureName = "unknown";

                            if (openGesture == 0 && btn->IsDown()) {
                                openNow = MinimapRenderer::CanOpenControllerInspect();
                                gestureName = "single_press";
                            } else if (openGesture == 1 && btn->IsDown()) {
                                const auto sinceFirstTap = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - s_controllerInspect.openTappedAt);
                                if (s_controllerInspect.openTapPending &&
                                    s_controllerInspect.openTapButton == openButton &&
                                    sinceFirstTap <= kControllerInspectDoubleTap &&
                                    MinimapRenderer::CanOpenControllerInspect()) {
                                    openNow = true;
                                    gestureName = "double_press";
                                    if (verbose) {
                                        logger::info("[ControllerInspect] double_press intervalMs={} openingPressConsumed=true button={}",
                                                     sinceFirstTap.count(), openButton);
                                    }
                                } else {
                                    s_controllerInspect.openTapPending = true;
                                    s_controllerInspect.openTapButton = openButton;
                                    s_controllerInspect.openTappedAt = now;
                                    if (verbose) {
                                        logger::info("[ControllerInspect] open_press_pending gesture=double thresholdMs={} firstPressConsumed=false button={}",
                                                     kControllerInspectDoubleTap.count(), openButton);
                                    }
                                }
                            } else if (openGesture == 2 &&
                                       btn->HeldDuration() >= kControllerInspectLongPressSeconds) {
                                openNow = MinimapRenderer::CanOpenControllerInspect();
                                gestureName = "long_press";
                            }

                            if (openNow) {
                                MinimapRenderer::OpenControllerInspect();
                                s_controllerInspect.openTapPending = false;
                                s_controllerInspect.openTapButton = -1;
                                s_controllerInspect.openLatch = !btn->IsUp();
                                s_controllerInspect.openLatchButton = btn->IsUp() ? -1 : openButton;
                                controllerInspectHandled = true;
                                consume = true;
                                if (verbose) {
                                    logger::info("[ControllerInspect] open gesture={} button={} heldSeconds={:.3f}",
                                                 gestureName, openButton, btn->HeldDuration());
                                }
                            }
                        }

                        const bool controllerInspectActive = MinimapRenderer::IsControllerInspectActive();
                        const bool inspectVisible = controllerInspectActive || MinimapRenderer::IsInspectShown();

                        if (!controllerInspectHandled && code == kControllerB) {
                            if (s_controllerInspect.closeLatch) {
                                controllerInspectHandled = true;
                                consume = true;
                                if (btn->IsUp()) s_controllerInspect.closeLatch = false;
                            } else if (inspectVisible) {
                                controllerInspectHandled = true;
                                consume = true;
                                if (btn->IsDown()) {
                                    if (controllerInspectActive) {
                                        MinimapRenderer::CloseControllerInspect();
                                        s_controllerInspect.openTapPending = false;
                                        s_controllerInspect.openTapButton = -1;
                                    }
                                    s_controllerInspect.closeLatch = true;
                                    if (verbose) {
                                        logger::info("[ControllerInspect] close source=B controllerActive={}",
                                                     controllerInspectActive);
                                    }
                                }
                            }
                        } else if (!controllerInspectHandled && settings->controllerInspectAddMarkerButton >= 266 &&
                                   code == static_cast<std::uint32_t>(settings->controllerInspectAddMarkerButton)) {
                            if (s_controllerInspect.addMarkerLatch) {
                                controllerInspectHandled = true;
                                consume = true;
                                if (btn->IsUp()) s_controllerInspect.addMarkerLatch = false;
                            } else if (inspectVisible) {
                                controllerInspectHandled = true;
                                consume = true;
                                if (btn->IsDown()) {
                                    MinimapRenderer::RequestControllerQuickMark();
                                    s_controllerInspect.addMarkerLatch = true;
                                    if (verbose) {
                                        logger::info("[ControllerInspect] action=add_marker queued=true button={}",
                                                     settings->controllerInspectAddMarkerButton);
                                    }
                                }
                            }
                        } else if (!controllerInspectHandled && settings->controllerInspectSwitchViewButton >= 266 &&
                                   code == static_cast<std::uint32_t>(settings->controllerInspectSwitchViewButton)) {
                            if (s_controllerInspect.modeLatch) {
                                controllerInspectHandled = true;
                                consume = true;
                                if (btn->IsUp()) s_controllerInspect.modeLatch = false;
                            } else if (inspectVisible) {
                                controllerInspectHandled = true;
                                consume = true;
                                if (btn->IsDown()) {
                                    const bool interior = MinimapRenderer::IsInteriorInspectShown();
                                    if (interior) MinimapRenderer::RequestControllerModeToggle();
                                    s_controllerInspect.modeLatch = true;
                                    if (verbose) {
                                        logger::info("[ControllerInspect] action=switch_view queued={} button={}",
                                                     interior, settings->controllerInspectSwitchViewButton);
                                    }
                                }
                            }
                        } else if (!controllerInspectHandled && settings->controllerInspectPathTrackingButton >= 266 &&
                                   code == static_cast<std::uint32_t>(settings->controllerInspectPathTrackingButton)) {
                            if (s_controllerInspect.pathLatch) {
                                controllerInspectHandled = true;
                                consume = true;
                                if (btn->IsUp()) s_controllerInspect.pathLatch = false;
                            } else if (inspectVisible) {
                                controllerInspectHandled = true;
                                consume = true;
                                if (btn->IsDown()) {
                                    const bool canToggle = MinimapRenderer::IsInteriorInspectShown() &&
                                                           settings->enablePathTracking;
                                    if (canToggle) PathTracker::GetSingleton()->ToggleTracking();
                                    s_controllerInspect.pathLatch = true;
                                    if (verbose) {
                                        logger::info("[ControllerInspect] action=path_tracking toggled={} button={}",
                                                     canToggle, settings->controllerInspectPathTrackingButton);
                                    }
                                }
                            }
                        }
                    }

                    if (device == RE::INPUT_DEVICE::kGamepad && code == 276) {
                        if (s_controllerMapActivateLatch) {
                            consume = true;
                            if (btn->IsUp()) s_controllerMapActivateLatch = false;
                        } else if (btn->IsDown() && !consume && isMapOpen &&
                                   !mapOverlayBlocked && !dialogOpen && !settingsOpen &&
                                   !ImGuiRenderer::IsAnyPopupOpen() &&
                                   !(RE::UI::GetSingleton() &&
                                     RE::UI::GetSingleton()->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME)) &&
                                   MapMarkerRenderer::GetSingleton()->TryHandleControllerMapActivation()) {
                            s_controllerMapActivateLatch = true;
                            consume = true;
                        }
                    }

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
                    } else if (!controllerInspectHandled && !s_inspectPathToggleLatch.active && btn->IsDown() &&
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
                    } else if (!controllerInspectHandled && !s_inspectToggleLatch.active && btn->IsDown() &&
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
                    } else if (!controllerInspectHandled && !s_glanceToggleLatch.active && btn->IsDown() &&
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
                    } else if (!controllerInspectHandled && !s_lootBeamToggleLatch.active && btn->IsDown() &&
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

                        if (idCode == 1 && isMapOpen && !mapOverlayBlocked && !MenuUtils::IsMapModalOpen()) {
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

                        if (isGameplayHosted) {
                            consume = true;
                        }
                    } else if (device == RE::INPUT_DEVICE::kGamepad) {
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
        REL::RelocationID hookLocation(67315, 68617);
        const std::uintptr_t hookAddress = hookLocation.address() + REL::Relocate(0x7B, 0x7B, 0x81);

        g_originalProcessInputQueue = SKSE::GetTrampoline().write_call<5>(
            hookAddress, Hook_ProcessInputQueue);

        logger::info("InputHook installed | hookAddr={:X}", hookAddress);
    }
}
