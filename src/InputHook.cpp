// Hooks game input dispatching to capture key and mouse events.

#include "InputHook.h"
#include "InputHandler.h"
#include "MapMarkerRenderer.h"
#include "LocalMapRenderer.h"
#include "MinimapRenderer.h"
#include "LookPinRenderer.h"
#include "Settings.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <imgui.h>
#include "HoverImageRenderer.h"
#include "SettingsMenu.h"

// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):

//   MapMarkerRenderer/LocalMapRenderer STATUS blocks for the hover-panel side

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

    static void Hook_ProcessInputQueue(RE::BSTEventSource<RE::InputEvent*>* a_source,
                                       RE::InputEvent** a_events) {
        if (!ImGui::GetCurrentContext() || !a_events || !*a_events) {
            g_originalProcessInputQueue(a_source, a_events);
            return;
        }
        const bool wantText        = ImGui::GetIO().WantTextInput;
        const bool dialogOpen      = MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen();
        const bool settingsOpen    = SettingsMenu::IsVisible();
        const bool isGameplayHosted = SettingsMenu::IsGameplayHosted();
        const bool blockKeys       = wantText || dialogOpen || settingsOpen;
        auto* settings = Settings::GetSingleton();
        RE::InputEvent* prev = nullptr;
        RE::InputEvent* curr = *a_events;
        while (curr) {
            bool consume = false;
            if (curr->eventType == RE::INPUT_EVENT_TYPE::kChar) {
                if (wantText) {
                    auto* charEvent = static_cast<RE::CharEvent*>(curr);
                    const unsigned int ch = static_cast<unsigned int>(charEvent->keyCode);
                    ImGui::GetIO().AddInputCharacter(ch);
                }
                if (blockKeys || isGameplayHosted) {
                    consume = true;
                }
            } else if (curr->eventType == RE::INPUT_EVENT_TYPE::kButton) {
                auto* btn = curr->AsButtonEvent();
                if (btn) {
                    const auto device = btn->GetDevice();
                    const auto idCode = btn->GetIDCode();
                    const auto code   = KeycodeFromScancode(idCode, device);
                    if (s_hotkeyLatch.active && s_hotkeyLatch.device == device && s_hotkeyLatch.idCode == idCode) {
                        consume = true;
                        if (btn->IsUp()) {
                            s_hotkeyLatch.active = false;
                        }
                    }
                    if (device == RE::INPUT_DEVICE::kKeyboard) {
                        ImGuiKey imKey = ScancodeToImGuiKey(idCode);
                        if (imKey != ImGuiKey_None) {
                            ImGui::GetIO().AddKeyEvent(imKey, btn->IsDown() || btn->IsPressed());
                        }
                        if (g_hkCaptureCb) {
                            consume = true;
                            if (btn->IsDown() && idCode != 0x01 && idCode != 0x0F &&
                                !InputHandler::IsModifierScancode(idCode)) {
                                auto cb = std::move(g_hkCaptureCb);
                                g_hkCaptureCb = nullptr;
                                cb(idCode);
                            }
                        } else if (!s_hotkeyLatch.active) {
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
                        if (blockKeys) {
                            consume = true;
                        }
                        {
                            static bool swallowSettingsClose = false;
                            if (idCode == 0x01 || idCode == 0x0F) {
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
                        {
                            static bool swallowLookPinEsc = false;
                            if (idCode == 0x01) {
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
                        const bool isMapOpen = RE::UI::GetSingleton() && RE::UI::GetSingleton()->IsMenuOpen(RE::MapMenu::MENU_NAME);
                        if (idCode == 1 && isMapOpen) {
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
                        if (idCode == 8 || idCode == 9) {
                            const bool blockMouse = (isMapOpen &&
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
        SKSE::AllocTrampoline(14);
        g_originalProcessInputQueue = SKSE::GetTrampoline().write_call<5>(
            hookAddress, Hook_ProcessInputQueue);
        logger::info("InputHook installed | hookAddr={:X}", hookAddress);
    }
}
