#include "MapLookRemapHook.h"
#include "ImGui/Renderer.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <memory>

// ============================================================================
// MapLookRemapHook — move world-map tilt/look from right-mouse to middle-mouse
// ----------------------------------------------------------------------------
// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):
//   Working (Phase 1): the world-map camera tilt/orbit (vanilla "MapLookMode",
//            bound to right-mouse) is driven by middle-mouse instead. Right-
//            mouse no longer triggers any look/recenter, so it is free for our
//            ImGui context menu everywhere on the map.
//   How: hook MapLookHandler::CanProcess (ID 53061) and ProcessButton
//        (ID 53062). Vanilla identifies the look button by the event's
//        userEvent STRING (QUserEvent() == "MapLookMode"), NOT by idCode. The
//        control map binds "MapLookMode" to right-mouse; middle-mouse arrives
//        with an empty userEvent. So for a mouse BUTTON event we rewrite the
//        userEvent (BSFixedString at IDEvent+0x18) for the duration of the
//        original call, then restore it:
//          - middle (idCode 2) -> userEvent "MapLookMode": vanilla's own
//            look-mode entry/exit AND recenter-on-press run unchanged, just
//            triggered by middle now. We reuse engine code, no synthesized
//            events. We also rewrite idCode 2->1 in case any path also reads it.
//          - right (idCode 1): return false / skip the original so the look
//            handler never sees it. Right is inert for tilt.
//        Mouse-move "Cursor" events (the actual drag deltas) are NOT touched,
//        so once look mode is active the tilt drag works exactly as vanilla.
//   Why string-rewrite instead of reimplementing the handler: the finding's
//        alternative writes MapLookHandler+0x18 by hand and calls the recenter
//        helper directly. Rewriting the userEvent lets vanilla run its real
//        code path, keeping Phase 1 behavior identical with less surface area.
//   NOTE: rewriting only idCode does NOT work — vanilla gates on the userEvent
//        string, so the middle event is rejected by CanProcess and never
//        reaches ProcessButton. The string rewrite is the load-bearing part.
//   Phase 2 (working): middle-hold tilts in place, no recenter jump. In
//        HookedProcessButton's middle-mouse path we DON'T run vanilla (which
//        fires the recenter helper FUN_140fba4a0 on press). Instead we set the
//        look-mode flag MapLookHandler::unk18 ourselves (1 on press, 0 on
//        release) so look mode engages without the recenter. CanProcess still
//        delegates to vanilla via RunAsLook so the button and the "Cursor"
//        drag events route correctly. Tilt math is delta-based (MapCamera_-
//        AddRotate, ID 33020) and decoupled, so it tilts around the current
//        view. Note: vanilla's recenter helper also hid the OS cursor; we skip
//        it, but the map already hides the cursor (plugin.cpp), so no visible
//        change. See findings/findings-Map-Tilt-Button-Remap.md section 7.5.
//   Cursor pin (Phase 2): the tilt drag moves the mouse, so the MenuCursor
//        would drift to the screen edge and the engine would edge-pan the map.
//        PinCursorIfTilting() (called per-frame from the Present hook in
//        ImGui/Renderer.cpp) freezes MenuCursor cursorPosX/Y at the press point
//        while look mode is active. Tilt deltas come from raw mouse movement
//        (ProcessMouseMove), not the absolute cursor, so tilt still works while
//        the cursor stays put and never edge-pans.
//   Diagnostics: InputVerboseLog snapshots the real Windows pointer/window
//        state on middle-tilt press/release. MenuCursor coordinates cannot show
//        a hidden OS pointer escaping onto another monitor.
//   AE 1.6.1170. Handlers resolve via Address Library IDs 53061 / 53062.
//   See findings/findings-worldmap-camera.md section 7.
// ============================================================================

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

    // True only for a mouse button event we want to treat as the look button.
    // a_idCode is filled with the event's current idCode for the caller to test.
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

    // Present a middle-mouse event to vanilla as the look button: swap its
    // userEvent string (and idCode) to what vanilla expects, run the original,
    // then restore so no other handler sees a mutated event. Returns vanilla's
    // result. T is CanProcessFn or ProcessButtonFn; E is InputEvent* or
    // ButtonEvent*.
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
                // Phase 2: tilt in place, no recenter jump.
                // Vanilla's ProcessButton fires the recenter helper
                // (FUN_140fba4a0) on press. Instead of running it, we set the
                // look-mode flag (MapLookHandler+0x18) ourselves so look mode
                // engages without the recenter. The tilt drag still works
                // because ProcessMouseMove just reads this flag and the tilt
                // math is delta-based and decoupled from the recenter.
                // Press = first frame of a non-zero value (heldDuration == 0).
                // Release = value == 0. See findings-Map-Tilt-Button-Remap 7.5.
                if (a_event->Value() == 0.0f) {
                    a_this->unk18 = 0;  // release: leave look mode
                    g_tilting = false;
                    ImGuiRenderer::LogInputEnvironment("map_tilt_release");
                } else if (a_event->HeldDuration() == 0.0f) {
                    a_this->unk18 = 1;  // press: enter look mode, no recenter
                    // Capture the press point so PinCursorIfTilting can freeze
                    // the cursor there for the whole drag. This stops the cursor
                    // from reaching the screen edge and triggering edge-pan.
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
    // Freeze the MenuCursor at the press point. The tilt deltas come from raw
    // mouse movement (ProcessMouseMove), not from this absolute position, so
    // the tilt keeps working while the cursor stays put and never edge-pans.
    if (auto* cursor = RE::MenuCursor::GetSingleton()) {
        cursor->cursorPosX = g_pinX.load();
        cursor->cursorPosY = g_pinY.load();
    }
}

void MapLookRemapHook::Install() {
    // MapLookHandler: SE 52169/52170 / AE 53061/53062 (findings-SE-Hook-IDs.md rows 7-8).
    InstallDetour(REL::RelocationID(52169, 53061), reinterpret_cast<void*>(&HookedCanProcess),
                  g_canProcessTramp, g_canProcessDetour,
                  "MapLookHandler::CanProcess");
    InstallDetour(REL::RelocationID(52170, 53062), reinterpret_cast<void*>(&HookedProcessButton),
                  g_processButtonTramp, g_processButtonDetour,
                  "MapLookHandler::ProcessButton");
}
