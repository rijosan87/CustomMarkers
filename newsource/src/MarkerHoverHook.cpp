#include "MarkerHoverHook.h"
#include "MapMarkerRenderer.h"
#include "MarkerManager.h"
#include "InputHook.h"
#include "PlaceMarkerHook.h"
#include "HoverImageRenderer.h"
#include "MapLookRemapHook.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <Windows.h>
#include <memory>
#include "ImGui/Renderer.h"
#include <imgui.h>

// ============================================================================
// MarkerHoverHook — Ctrl-suppress vanilla map marker hover/snap
// ----------------------------------------------------------------------------
// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):
//   Working: while Ctrl is held on the world map, the engine itself is made
//            to drive AS into the Hide state by rewriting the arg to vanilla's
//            FxDelegate::Invoke("SetSelectedMarker", iVar9) call inside
//            MapMenu::UpdateHoveredMarker. Two hooks run:
//              1. Entry detour on MapMenu::UpdateHoveredMarker (RVA 0x986360).
//                 After vanilla returns, pin MapMenu+0x30558 to -1 whenever
//                 Ctrl is held OR the engine cursor is inside our ImGui list
//                 panel rect. The Ctrl case enables Ctrl-suppress; the panel
//                 case stops a vanilla marker under the panel from stealing
//                 the click from MapMenu::OnMouseDown (RVA 0x987610), so list
//                 row clicks always reach our placement hook and then
//                 TryHandlePanelClick.
//              2. Entry detour on FxDelegate::Invoke wrapper FUN_140fbe900
//                 (RVA 0xFBE900). Filter by methodName pointer-equal to the
//                 .rdata literal "SetSelectedMarker" at RVA 0x906F78. When
//                 Ctrl is held, rewrite args[0] to {type=kNumber, value=-1}
//                 in place before forwarding. AS sees -1, runs MarkerRollOut
//                 and gotoAndPlay("Hide"), the Hide tween reaches its
//                 terminal _visible=false frame, and both the marker zoom-up
//                 and the MarkerDescriptionHolder tooltip card go away.
//                 Other invokes (RefreshMarkers, ClickCallback, SetDateString,
//                 invokes from other menus) all share FUN_140fbe900 but pass
//                 a different methodName pointer, so the filter is exact.
//              3. Pre-detour on MapMenu::OnMouseDown (RVA 0x987610). Before
//                 vanilla reads MapMenu+0x30558 to choose fast-travel vs
//                 placement, force the slot to -1 if the cursor is over our
//                 list panel rect. Closes the race the UpdateHoveredMarker
//                 post-hook leaves open: vanilla can write a real index in the
//                 same frame OnMouseDown fires, and the post-hook only pins
//                 -1 after the dispatcher has already read. With this hook,
//                 the placement hook always wins list-panel clicks.
//   Why post-hook FxDelegate::Invoke("SetSelectedMarker", -1) was dropped:
//     V1 sent that from native after vanilla finished. AS would Hide, but
//     vanilla's next-frame Show restarted the tween before _visible=false
//     ever fired. By rewriting vanilla's own arg, the Show never happens in
//     the first place, so the Hide path reaches its terminal frame.
//   DUAL-BUILD (SE 1.5.97 + AE 1.6.1170). All hook targets now resolve via
//   REL::RelocationID(seID, aeID) from findings/findings-SE-Hook-IDs.md:
//   UpdateHoveredMarker 52224/53111, OnMouseDown 52230/53117,
//   FxDelegate::Invoke 80520/82640, "SetSelectedMarker" data 270681/216427.
//   The old AE raw RVAs (0x986360, 0xFBE900, methodName 0x906F78) are the AE
//   resolutions of those IDs. The AE-only ID 53111 was the SE load-crash:
//   on SE it pointed at a save-load/VM fn, so the hook fired with the SkyrimVM
//   global as a_menu and SampleCardState crashed in GetMember.
//   See findings/findings-World-Map-Hover-Animation-And-Card-Anchor.md,
//   findings/findings-World-Map-Description-Card-Suppress.md, and
//   findings/findings-World-Map-Description-Card-Suppress-V2.md.
// ============================================================================
//
// Hook target: MapMenu::UpdateHoveredMarker, raw RVA 0x986360.
// findings/findings-World-Map-Marker-Snap-HitTest.md:
//   - This function runs every MapMenu update frame.
//   - It projects every vanilla marker to screen space, picks the nearest one
//     inside the snap radius, and writes its index (or -1) to MapMenu+0x30558.
//   - The click dispatcher (MapMenu::OnMouseDown, SE 52230 / AE 53117) reads that
//     field: >-1 -> ClickSelectedMarker (fast-travel), -1 -> PlaceMarker
//     (terrain ray -> SetDestination SE 39458 / AE 40535 -> our custom-marker hook).
//
// Strategy: always let vanilla run so it can keep projecting marker screen
// positions (Scaleform draws the marker clips from those). After vanilla
// returns, if Ctrl is held, (a) overwrite +0x30558 to -1 so the click
// dispatcher takes the terrain path, and (b) push Invoke("SetSelectedMarker",
// -1) so the AS side clears its hover visual (zoom-up + tooltip).
//
// Earlier attempt (skipping vanilla entirely) caused vanilla marker clips to
// stop updating while Ctrl was held, so dragging the map made them drift away
// from their world positions and snap back on release.
namespace {
    constexpr std::ptrdiff_t kHoveredMarkerOffset = 0x30558;
    // The literal "SetSelectedMarker" sits at VA 0x141906F78, i.e. module base
    // + 0x1906F78. The image base for SkyrimSE.exe is 0x140000000, NOT
    // 0x141000000. Mixing those up is what V2's first wire-up did wrong.
    // Dual-build (SE 1.5.97 / AE 1.6.1170). RelocationID is (seID, aeID).
    // SE IDs from findings/findings-SE-Hook-IDs.md.
    constexpr REL::RelocationID kSetSelectedMarkerStringID{ 270681, 216427 };
    constexpr REL::RelocationID kFxDelegateInvokeID{ 80520, 82640 };
    constexpr REL::RelocationID kOnMouseDownID{ 52230, 53117 };

    std::uint64_t                              g_trampolineAddr = 0;
    std::unique_ptr<PLH::x64Detour>            g_detour;

    std::uint64_t                              g_fxInvokeTrampoline = 0;
    std::unique_ptr<PLH::x64Detour>            g_fxInvokeDetour;

    std::uint64_t                              g_onMouseDownTrampoline = 0;
    std::unique_ptr<PLH::x64Detour>            g_onMouseDownDetour;

    // Resolved at install time. Used by the FxDelegate::Invoke detour to
    // identify the "SetSelectedMarker" call site by pointer-equality on the
    // methodName arg.
    const char* g_setSelectedMarkerLiteral = nullptr;

    using UpdateHoveredMarkerFn = void(*)(RE::MapMenu*);
    using FxDelegateInvokeFn    = void(*)(void* /*movie*/,
                                          const char* /*methodName*/,
                                          void* /*args*/,
                                          std::uint64_t /*reserved*/);
    using OnMouseDownFn         = void(*)(RE::MapMenu*);

    bool CtrlHeld() {
        return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    }

    // Layout verified in findings/findings-World-Map-Description-Card-Suppress-V2.md.
    // FxResponseArgs<N>: vtable@+0x00, GFxValue[0]@+0x08, GFxValue[1]@+0x20, ...
    // GFxValue: value@+0x00, type@+0x08 (low byte = kind, kNumber=3),
    //           numberValue@+0x10.
    struct GFxValueRaw {
        void*         value;
        std::uint32_t type;
        std::uint32_t pad;
        double        numberValue;
    };
    static_assert(sizeof(GFxValueRaw) == 0x18);

    using GetValuesFn = std::uint32_t(*)(void* a_this, GFxValueRaw** a_values);

    // Tracks last suppress state so we only log on transitions, not every frame.
    static bool s_lastSuppressState = false;

    void HookedFxDelegateInvoke(void* a_movie, const char* a_methodName,
                                void* a_args, std::uint64_t a_reserved) {
        const bool isOurMethod = (a_methodName == g_setSelectedMarkerLiteral);

        if (isOurMethod && a_args) {
            // CursorOverPanel: with the list panel's background alpha turned
            // down (or fully transparent), a vanilla marker underneath still
            // fires this SetSelectedMarker invoke and pops its hover/selection
            // card through the panel. UpdateHoveredMarker's post-hook below
            // already pins the snapped index to -1 for the same reason; mirror
            // that condition here so the description card is suppressed too.
            // IsTilting: during a middle-mouse tilt drag the camera sweeps
            // markers past the (pinned) cursor and each one fires a legit
            // hover, so the card flickers on and off for the whole drag.
            // Nothing is being pointed at while tilting, so suppress outright.
            const bool suppress = CtrlHeld() || MapLookRemapHook::IsTilting()
                               || InputHook::IsTextInputActive()
                               || MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen()
                               || MapMarkerRenderer::GetSingleton()->IsCustomPinHovered()
                               || MapMarkerRenderer::GetSingleton()->CursorOverPanel()
                               || (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse);

            // if (suppress != s_lastSuppressState) {
            //     s_lastSuppressState = suppress;
            //     logger::debug("HookedFxDelegateInvoke: suppress state -> {}", suppress ? "ON" : "OFF");
            // }

            if (suppress) {
                auto** vtbl = *reinterpret_cast<void***>(a_args);
                auto getValues = reinterpret_cast<GetValuesFn>(vtbl[1]);

                GFxValueRaw* values = nullptr;
                std::uint32_t count = getValues(a_args, &values);

                if (values) {
                    // logger::debug("HookedFxDelegateInvoke: suppressing SetSelectedMarker | count={}", count);
                    for (std::uint32_t i = 0; i < count; ++i) {
                        values[i].value       = nullptr;
                        values[i].type        = 3; // kNumber
                        values[i].numberValue = -1.0;
                    }
                } else {
                    // logger::warn("HookedFxDelegateInvoke: suppress triggered but getValues returned null | args={}", a_args);
                }
            }
        }
        
        reinterpret_cast<FxDelegateInvokeFn>(g_fxInvokeTrampoline)(
            a_movie, a_methodName, a_args, a_reserved);
    }

    // Pre-detour for MapMenu::OnMouseDown (RVA 0x987610). The function reads
    // MapMenu+0x30558 to decide between fast-travel (>= 0) and placement (-1).
    // Vanilla writes that slot from UpdateHoveredMarker on the same frame, so
    // a post-hook on UpdateHoveredMarker loses the race when a real vanilla
    // marker sits under our list panel rect: vanilla writes a real index,
    // OnMouseDown reads it before our post-hook can pin -1, click goes to
    // fast-travel, our placement hook never runs. Force -1 here, just before
    // the dispatcher reads, to close the race.
    void HookedOnMouseDown(RE::MapMenu* a_menu) {
        // When the vanilla fast-travel prompt (or any other modal MessageBox)
        // is open over the map, its Yes/No/Place Marker buttons live in the
        // MessageBox menu but the click still reaches MapMenu::OnMouseDown.
        // Let vanilla handle it cleanly so "Place Marker" does its real job
        // and our placement path doesn't run on top.
        if (auto* ui = RE::UI::GetSingleton(); ui &&
            ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME)) {
            reinterpret_cast<OnMouseDownFn>(g_onMouseDownTrampoline)(a_menu);
            return;
        }

        // While the note dialog or any ImGui popup (like the right-click context menu) 
        // is open, swallow all map clicks so the user doesn't accidentally fast-travel 
        // or place a marker when clicking outside the menu to close it.
        // While the note dialog or any ImGui popup (like the right-click context menu) 
        // is open, swallow all map clicks so the user doesn't accidentally fast-travel 
        // or place a marker when clicking outside the menu to close it.
        if (MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen() ||
            ImGuiRenderer::IsAnyPopupOpen() ||
            (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse)) {
            return;
        }

        // A click on a hover panel's "Remove Marker" button deletes that
        // marker instead of placing a new one. The renderer hit-tests the
        // button each frame and exposes the target id here. Must run BEFORE
        // the IsCustomPinHovered check, because the remove panel only appears
        // while a pin is hovered, so IsCustomPinHovered is true in this case.
        if (auto removeId = MapMarkerRenderer::GetSingleton()->GetRemoveButtonTarget();
            removeId != 0) {
            MarkerManager::GetSingleton()->RemoveMarker(removeId);
            return;
        }

        // Set/Clear destination button on the hover panel. Same routing pattern
        // as the remove button. Single-destination rule is enforced by
        // MarkerManager::SetDestination itself.
        if (auto destId = MapMarkerRenderer::GetSingleton()->GetDestButtonTarget();
            destId != 0) {
            // Real markers only: the synthesized vanilla destination has no
            // hover panel and must never be toggled through this path.
            auto markers = MarkerManager::GetSingleton()->GetUserMarkers();
            bool isCurrentlyDest = false;
            for (const auto& m : markers) {
                if (m.id == destId) { isCurrentlyDest = m.isDestination; break; }
            }
            MarkerManager::GetSingleton()->SetDestination(destId, !isCurrentlyDest);
            return;
        }

        // "Edit Note" button on the hover panel. Reopens the note dialog seeded
        // with this marker's note/category/destination; confirm updates it in
        // place. Same routing pattern as remove/destination.
        if (auto editId = MapMarkerRenderer::GetSingleton()->GetEditButtonTarget();
            editId != 0) {
            MapMarkerRenderer::GetSingleton()->BeginEditMarker(editId);
            return;
        }

    // If the cursor is hovering one of our custom pins (but not the remove
        // button), swallow the click. Without this, clicking the pin falls
        // through to the trampoline and the engine fires the placement hook a
        // second time, opening the note dialog on top of the existing marker.
        if (MapMarkerRenderer::GetSingleton()->IsCustomPinHovered()) return;

        // In interior cells, vanilla markers still snap and write their index to
        // MapMenu+0x30558. If a vanilla marker is snapped (index >= 0), forward
        // to the trampoline so vanilla's fast-travel dispatch runs. It will fail
        // in interiors and surface the "You cannot fast travel from here" message,
        // matching vanilla behavior. The placement hook only fires on the
        // terrain-ray path (index = -1), so this won't open our note dialog.
        if (a_menu) {
            auto base = reinterpret_cast<std::uintptr_t>(a_menu);
            auto hoveredIdx = *reinterpret_cast<std::int32_t*>(base + kHoveredMarkerOffset);
            if (hoveredIdx >= 0) {
                // If it's a valid vanilla marker but it's UNDISCOVERED, swallow the click
                // to prevent the vanilla "Place Marker" prompt from appearing. We already
                // have our own custom destination system via the right-click menu (which
                // is intentionally disabled on undiscovered markers to avoid spoilers).
                if (HoverImageRenderer::IsHoveredUndiscovered()) {
                    return;
                }

                // Vanilla may walk the terrain-ray path during dispatch and
                // call into PlaceMarkerHook. Suppress that one call so our
                // note dialog doesn't open behind the fast-travel message.
                PlaceMarkerHook::SuppressNextPlacement();
                reinterpret_cast<OnMouseDownFn>(g_onMouseDownTrampoline)(a_menu);
                return;
            }
        }

        // Clicks inside the custom marker list panel either pan the map to
        // the clicked row or just get swallowed on the panel chrome. Either
        // way, do not let the engine process the click.
        if (MapMarkerRenderer::GetSingleton()->TryHandlePanelClick()) return;

        if (a_menu && MapMarkerRenderer::GetSingleton()->CursorOverPanel()) {
            auto base = reinterpret_cast<std::uintptr_t>(a_menu);
            *reinterpret_cast<std::int32_t*>(base + kHoveredMarkerOffset) = -1;
        }
        reinterpret_cast<OnMouseDownFn>(g_onMouseDownTrampoline)(a_menu);
    }

    void HookedUpdateHoveredMarker(RE::MapMenu* a_menu) {
        reinterpret_cast<UpdateHoveredMarkerFn>(g_trampolineAddr)(a_menu);
        if (!a_menu) return;
        // Pin the snapped-marker index to -1 when Ctrl is held OR the cursor
        // is over our list panel. The second case keeps vanilla markers under
        // the panel rect from stealing OnMouseDown clicks from the placement
        // hook, which is how list-row clicks reach TryHandlePanelClick.
        if (CtrlHeld() || MapLookRemapHook::IsTilting()
                       || MapMarkerRenderer::GetSingleton()->CursorOverPanel()
                       || InputHook::IsTextInputActive()
                       || MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen()
                       || MapMarkerRenderer::GetSingleton()->IsCustomPinHovered()
                       || (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse)) {
            auto base = reinterpret_cast<std::uintptr_t>(a_menu);
            *reinterpret_cast<std::int32_t*>(base + kHoveredMarkerOffset) = -1;
        }
        // Sample the vanilla description-card's stage rect into a cached
        // screen rect for the ImGui-side HoverImageRenderer to consume. Must
        // run here (main thread, post-original) so the holder reflects the
        // post-RefreshMarkers position written this frame.
        HoverImageRenderer::SampleCardState(a_menu);
    }
}

void MarkerHoverHook::Install() {
    // UpdateHoveredMarker: SE 52224 / AE 53111 (findings-SE-Hook-IDs.md row 1).
    REL::Relocation<std::uintptr_t> target{ REL::RelocationID(52224, 53111) };
    const std::uint64_t targetAddr = target.address();
    const std::uint64_t hookAddr   = reinterpret_cast<std::uint64_t>(&HookedUpdateHoveredMarker);

    g_detour = std::make_unique<PLH::x64Detour>(targetAddr, hookAddr, &g_trampolineAddr);

    if (!g_detour->hook()) {
        logger::error("MarkerHoverHook: PolyHook x64Detour::hook() returned false");
        g_detour.reset();
        return;
    }

    logger::info("MarkerHoverHook installed | target={:X} | trampoline={:X}",
                 targetAddr, g_trampolineAddr);

    // Resolve the runtime address of the ".rdata" literal "SetSelectedMarker"
    // so the FxDelegate::Invoke detour can pointer-compare against it.
    REL::Relocation<const char*> stringLit{ kSetSelectedMarkerStringID };
    g_setSelectedMarkerLiteral = stringLit.get();

    REL::Relocation<std::uintptr_t> fxInvokeTarget{ kFxDelegateInvokeID };
    const std::uint64_t fxTargetAddr = fxInvokeTarget.address();
    const std::uint64_t fxHookAddr   = reinterpret_cast<std::uint64_t>(&HookedFxDelegateInvoke);

    g_fxInvokeDetour = std::make_unique<PLH::x64Detour>(
        fxTargetAddr, fxHookAddr, &g_fxInvokeTrampoline);

    if (!g_fxInvokeDetour->hook()) {
        logger::error("MarkerHoverHook: FxDelegate::Invoke detour failed");
        g_fxInvokeDetour.reset();
        return;
    }

    logger::info("FxDelegate::Invoke hook installed | target={:X} | trampoline={:X} | literalVA={}",
                 fxTargetAddr, g_fxInvokeTrampoline,
                 static_cast<const void*>(g_setSelectedMarkerLiteral));

    REL::Relocation<std::uintptr_t> onMouseDownTarget{ kOnMouseDownID };
    const std::uint64_t mdTargetAddr = onMouseDownTarget.address();
    const std::uint64_t mdHookAddr   = reinterpret_cast<std::uint64_t>(&HookedOnMouseDown);

    g_onMouseDownDetour = std::make_unique<PLH::x64Detour>(
        mdTargetAddr, mdHookAddr, &g_onMouseDownTrampoline);

    if (!g_onMouseDownDetour->hook()) {
        logger::error("MarkerHoverHook: OnMouseDown detour failed");
        g_onMouseDownDetour.reset();
        return;
    }

    logger::info("OnMouseDown hook installed | target={:X} | trampoline={:X}",
                 mdTargetAddr, g_onMouseDownTrampoline);
}
