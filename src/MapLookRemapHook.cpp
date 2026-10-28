#include "MapLookRemapHook.h"
#include "ImGui/Renderer.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <memory>

// Remaps map camera rotation/tilt to middle-mouse drag.

namespace {
    // mouse idCodes in the Skyrim input queue
    constexpr std::uint32_t kRightMouse  = 1;
    constexpr std::uint32_t kMiddleMouse = 2;

    // Set true while middle-mouse tilt (look mode) is active. The per-frame
    // PinCursorIfTilting() reads this to freeze the cursor at the press point.
    std::atomic<bool>  g_tilting{ false };
    std::atomic<float> g_pinX{ 0.0f };
    std::atomic<float> g_pinY{ 0.0f };

    using CanProcessFn    = bool(*)(RE::MapLookHandler*, RE::InputEvent*);
    using ProcessButtonFn = bool(*)(RE::MapLookHandler*, RE::ButtonEvent*);

    std::uint64_t                              g_canProcessTramp = 0;
    std::unique_ptr<PLH::x64Detour>            g_canProcessDetour;

    std::uint64_t                              g_processButtonTramp = 0;
    std::unique_ptr<PLH::x64Detour>            g_processButtonDetour;

    bool IsMouseButton(RE::InputEvent* a_event, std::uint32_t& a_idCode) {
        if (!a_event) return false;
        if (a_event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) return false;
        if (a_event->GetDevice() != RE::INPUT_DEVICE::kMouse) return false;
        auto* id = a_event->AsIDEvent();
        if (!id) return false;
        a_idCode = id->GetIDCode();
        return true;
    }

    // The userEvent string vanilla compares against to identify the look button.
    const RE::BSFixedString kMapLookMode{ "MapLookMode" };

    template <class Fn, class Ev>
    bool RunAsLook(std::uint64_t a_tramp, RE::MapLookHandler* a_this, Ev* a_event) {
        auto* id = a_event->AsIDEvent();
        const RE::BSFixedString savedUserEvent = id->userEvent;
        const std::uint32_t     savedIdCode    = id->idCode;
        id->userEvent = kMapLookMode;
        id->idCode    = kRightMouse;
        const bool result = reinterpret_cast<Fn>(a_tramp)(a_this, a_event);
        id->userEvent = savedUserEvent;
        id->idCode    = savedIdCode;
        return result;
    }

    bool HookedCanProcess(RE::MapLookHandler* a_this, RE::InputEvent* a_event) {
        std::uint32_t idCode = 0;
        if (IsMouseButton(a_event, idCode)) {
            if (idCode == kRightMouse) {
                // Right-mouse must never enter look mode. It belongs to our
                // context menu now.
                return false;
            }
            if (idCode == kMiddleMouse) {
                return RunAsLook<CanProcessFn>(g_canProcessTramp, a_this, a_event);
            }
        }
        // Mouse-move "Cursor" events, keyboard, gamepad: untouched.
        return reinterpret_cast<CanProcessFn>(g_canProcessTramp)(a_this, a_event);
    }

    bool HookedProcessButton(RE::MapLookHandler* a_this, RE::ButtonEvent* a_event) {
        std::uint32_t idCode = 0;
        if (IsMouseButton(a_event, idCode)) {
            if (idCode == kRightMouse) {
                return false;
            }
            if (idCode == kMiddleMouse) {
                if (a_event->Value() == 0.0f) {
                    a_this->unk18 = 0;  // release: leave look mode
                    g_tilting = false;
                    ImGuiRenderer::LogInputEnvironment("map_tilt_release");
                } else if (a_event->HeldDuration() == 0.0f) {
                    a_this->unk18 = 1;  // press: enter look mode, no recenter
                    if (auto* cursor = RE::MenuCursor::GetSingleton()) {
                        g_pinX = cursor->cursorPosX;
                        g_pinY = cursor->cursorPosY;
                    }
                    g_tilting = true;
                    ImGuiRenderer::LogInputEnvironment("map_tilt_press");
                }
                return true;
            }
        }
        return reinterpret_cast<ProcessButtonFn>(g_processButtonTramp)(a_this, a_event);
    }

    bool InstallDetour(REL::RelocationID a_id, void* a_hook, std::uint64_t& a_tramp,
                       std::unique_ptr<PLH::x64Detour>& a_detour,
                       const char* a_name) {
        REL::Relocation<std::uintptr_t> target{ a_id };
        a_detour = std::make_unique<PLH::x64Detour>(
            target.address(), reinterpret_cast<std::uint64_t>(a_hook), &a_tramp);
        if (!a_detour->hook()) {
            logger::error("MapLookRemapHook: {} detour failed", a_name);
            a_detour.reset();
            return false;
        }
        logger::info("MapLookRemapHook: {} installed | target={:X} | trampoline={:X}",
                     a_name, target.address(), a_tramp);
        return true;
    }
}

bool MapLookRemapHook::IsTilting() {
    return g_tilting.load();
}

void MapLookRemapHook::PinCursorIfTilting() {
    if (!g_tilting) return;
    if (auto* cursor = RE::MenuCursor::GetSingleton()) {
        cursor->cursorPosX = g_pinX.load();
        cursor->cursorPosY = g_pinY.load();
    }
}

void MapLookRemapHook::Install() {
    InstallDetour(REL::RelocationID(52169, 53061), reinterpret_cast<void*>(&HookedCanProcess),
                  g_canProcessTramp, g_canProcessDetour,
                  "MapLookHandler::CanProcess");
    InstallDetour(REL::RelocationID(52170, 53062), reinterpret_cast<void*>(&HookedProcessButton),
                  g_processButtonTramp, g_processButtonDetour,
                  "MapLookHandler::ProcessButton");
}
