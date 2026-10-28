#include "LocalMapClickHook.h"
#include "MarkerManager.h"
#include "MapMarkerRenderer.h"
#include "LocalMapRenderer.h"
#include "Settings.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <imgui.h>

#include <Windows.h>
#include <cmath>
#include <memory>

// ============================================================================
// LocalMapClickHook — left-click on local map → drop custom marker at world pos
// ----------------------------------------------------------------------------
// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):
//   Working: ImGui cursor unprojected through the local-map NiCamera and
//            intersected with a horizontal plane to resolve world position.
//            Local-map placement opens the shared note dialog; blank
//            confirmation uses the same "Custom Marker N" fallback as world map.
//            Works in BOTH interiors and exteriors since 2026-07-17 (was
//            interior only): exterior placements store the player's worldspace
//            FormID so the pin also shows on the world map and nav overlays.
//            Clicks on vanilla local-map markers are detected via
//            LocalMapMenu+0x303F8 (hovered-marker index, -1 = none) and passed
//            through to the engine so they don't open our note dialog.
//            See findings/findings-localmap-vanilla-marker-detect.md.
//   Broken/limited: this is experimental local-map territory; see
//            findings/findings-localmap-click-re.md for the camera model.
//            Local-map markers and the local-map hooks generally are not
//            considered v1 features.
//   Placement is RIGHT-CLICK ONLY (confirmed in-game). The raycast+place core
//            is TryPlaceAtCursor(), exposed as LocalMapClickHook::PlaceAtCursor()
//            and driven by the local-map right-click "Add Marker Here" menu (in
//            MapMarkerRenderer::DrawTerrainMenu, called from the isLocalMap block
//            of Draw()). Left-click no longer places; this handler still routes
//            left-click for the hover/remove/destination buttons, then calls the
//            original for vanilla camera repositioning. Mirrors the world map.
//   Runtime mapping: input handler SE 52088 / AE 52978; WindowPointToRay
//            SE 69263 / AE 70630. SE 1.5.97 placement was confirmed in-game
//            after correcting the ray ID. Hovered-marker field offset 0x303F8
//            is raw.
//   Ctrl override: holding Ctrl treats the +0x303F8 hovered index as -1 so a
//            near-door click still places. MapHoverHook suppresses the
//            matching vanilla hover-zoom + description card under Ctrl.
//            See findings/findings-LocalMap-Hover-Suppress.md.
//
//   LMU compat (Phase 1, unconditional): on every left-click exit path, we
//   clear a_event->userEvent so Local Map Upgrade's vtable-patched
//   ProcessButton skips its own PlayerSet marker placement in its post-call
//   block. Phase 2 will gate this on GetModuleHandleW(L"LocalMapUpgrade.dll").
// ============================================================================
//
// Hook target: LocalMapMenu::InputHandler::ProcessButton, Address Library ID 52978
// (RVA 0x97d4a0, Skyrim AE 1.6.1170). Vtable slot 0 of the local-map input handler,
// fired per button event while the local map is open.
//
// The vanilla handler only repositions the camera on REFR-based marker clicks. It
// has no concept of an arbitrary world position. So we read ImGui cursor, unproject
// through the local map's NiCamera, and intersect with a horizontal plane.
namespace {
    std::uint64_t                              g_trampolineAddr = 0;
    std::unique_ptr<PLH::x64Detour>            g_detour;

    using ScreenToWorldRay_t =
        bool(*)(RE::NiCamera*, int, int, RE::NiPoint3*, RE::NiPoint3*, float, float);

    bool RayAtZ(const RE::NiPoint3& a, const RE::NiPoint3& b, float planeZ,
                RE::NiPoint3& out) {
        float dz = b.z - a.z;
        if (std::abs(dz) < 1e-6f) return false;
        float t = (planeZ - a.z) / dz;
        out.x   = a.x + t * (b.x - a.x);
        out.y   = a.y + t * (b.y - a.y);
        out.z   = planeZ;
        return true;
    }

    // Unproject the current MenuCursor through the local-map NiCamera, intersect
    // the player's ground plane, and open the note dialog at the hit. Returns
    // true if a marker placement was started. Shared by the left-click path and
    // the right-click "Add Marker Here" menu item (LocalMapClickHook::PlaceAtCursor).
    // Interior cells only, matching the original left-click behavior.
    bool TryPlaceAtCursor() {
        const bool verbose = Settings::GetSingleton()->inputVerboseLog;
        if (verbose) {
            logger::info("[InputVerbose] local_place begin runtime={}",
                         REL::Module::get().version().string("."));
        }

        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            if (verbose) logger::info("[InputVerbose] local_place failed reason=no_ui");
            return false;
        }
        auto mapMenu = ui->GetMenu<RE::MapMenu>();
        if (!mapMenu) {
            if (verbose) logger::info("[InputVerbose] local_place failed reason=no_map_menu");
            return false;
        }

        const auto base         = reinterpret_cast<std::uintptr_t>(mapMenu.get());
        const auto localMapBase = base + 0x60;
        auto* cam = *reinterpret_cast<RE::NiCamera**>(localMapBase + 0x302F8);
        if (!cam) {
            if (verbose) logger::info("[InputVerbose] local_place failed reason=no_camera");
            return false;
        }

        // Cursor must come from the same source LocalMapRenderer projects against.
        auto* menuCursor = RE::MenuCursor::GetSingleton();
        if (!menuCursor || menuCursor->screenWidthX <= 0.f || menuCursor->screenWidthY <= 0.f) {
            if (verbose) logger::info("[InputVerbose] local_place failed reason=invalid_cursor");
            return false;
        }

        float vpL = *reinterpret_cast<const float*>(localMapBase + 0x30);
        float vpT = *reinterpret_cast<const float*>(localMapBase + 0x34);
        float vpR = *reinterpret_cast<const float*>(localMapBase + 0x38);
        float vpB = *reinterpret_cast<const float*>(localMapBase + 0x3c);

        float vpWidth = vpR - vpL;
        float vpHeight = vpB - vpT;
        if (vpWidth <= 0.f || vpHeight <= 0.f) {
            if (verbose) {
                logger::info("[InputVerbose] local_place failed reason=invalid_viewport width={} height={}",
                             vpWidth, vpHeight);
            }
            return false;
        }

        float localX = menuCursor->cursorPosX - vpL;
        float localY = menuCursor->cursorPosY - vpT;

        // CommonLib's NiCamera::WindowPointToRay mapping. Do not reuse the AE
        // ID on SE; 70630 resolves to a different SE function.
        REL::Relocation<ScreenToWorldRay_t> screenToWorldRay{ REL::RelocationID(69263, 70630) };

        RE::NiPoint3 rayA{}, rayDir{};
        bool rayOk = screenToWorldRay(cam,
                                      static_cast<int>(localX),
                                      static_cast<int>(localY),
                                      &rayA, &rayDir,
                                      vpWidth, vpHeight);
        if (verbose) {
            logger::info(
                "[InputVerbose] local_place ray ok={} cursor=({:.1f},{:.1f}) local=({:.1f},{:.1f}) viewport=({:.1f},{:.1f}) origin=({:.1f},{:.1f},{:.1f}) dir=({:.4f},{:.4f},{:.4f})",
                rayOk, menuCursor->cursorPosX, menuCursor->cursorPosY, localX, localY,
                vpWidth, vpHeight, rayA.x, rayA.y, rayA.z, rayDir.x, rayDir.y, rayDir.z);
        }
        if (!rayOk) return false;

        RE::NiPoint3 rayB = rayA + rayDir * 10000.f;

        // Intersect the player's ground plane, not the camera plane (see the
        // long note on the original left-click path for why rayA.z is wrong).
        RE::FormID   cellID       = 0;
        RE::FormID   worldSpaceID = 0;
        float        groundZ      = rayA.z;
        bool         haveCell     = false;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* cell = player->GetParentCell()) {
                cellID   = cell->GetFormID();
                haveCell = true;
                // Exterior local map placement (enabled 2026-07-17, was interior
                // only): store the worldspace like quick-mark/world-map markers
                // so the pin shows on the world map and nav overlays too.
                // worldSpaceID stays 0 for interiors (the local-marker sentinel).
                if (!cell->IsInteriorCell()) {
                    if (auto* ws = player->GetWorldspace()) {
                        worldSpaceID = ws->GetFormID();
                    }
                }
            }
            groundZ = player->GetPositionZ();
        }

        if (!haveCell) {
            if (verbose) logger::info("[InputVerbose] local_place failed reason=no_player_cell");
            return false;
        }

        RE::NiPoint3 hitPos{};
        if (!RayAtZ(rayA, rayB, groundZ, hitPos)) {
            if (verbose) logger::info("[InputVerbose] local_place failed reason=ray_parallel groundZ={}", groundZ);
            return false;
        }

        if (verbose) {
            logger::info("[InputVerbose] local_place dialog pos=({:.1f},{:.1f},{:.1f}) world={:08X} cell={:08X}",
                         hitPos.x, hitPos.y, hitPos.z, worldSpaceID, cellID);
        }
        MapMarkerRenderer::GetSingleton()->OpenNoteDialog(hitPos, worldSpaceID, cellID);
        return true;
    }

    bool HookedProcessButton(std::uintptr_t a_handler, RE::ButtonEvent* a_event) {
        using func_t = bool(*)(std::uintptr_t, RE::ButtonEvent*);
        auto callOriginal = [&]() -> bool {
            return REL::Relocation<func_t>(g_trampolineAddr)(a_handler, a_event);
        };

        if (!a_handler || !a_event) return callOriginal();

        // Only act on left-mouse press. idCode 0 = left button on kMouse device.
        const bool isLeftMouseDown =
            a_event->GetDevice() == RE::INPUT_DEVICE::kMouse &&
            a_event->GetIDCode() == 0 &&
            a_event->IsDown();

        if (!isLeftMouseDown) return callOriginal();

        // Phase 1 LMU compat (unconditional). Local Map Upgrade vtable-patches
        // the same ProcessButton and, after our detour returns, re-reads
        // a_event->userEvent. If it's still "click", LMU drops its own
        // PlayerSet marker on top of whatever we just did. Neutralize the
        // userEvent on every exit path below so LMU's post-call block skips
        // placement. Safe when LMU isn't installed: vanilla has already read
        // userEvent by the time control returns to LMU/nobody.
        struct NeutralizeOnExit {
            RE::ButtonEvent* ev;
            ~NeutralizeOnExit() {
                if (ev) ev->userEvent = RE::BSFixedString("");
            }
        } _neutralize{ a_event };

        // The shared note dialog draws over both world and local map. While it
        // is open, local-map clicks belong to that dialog and must not create
        // another pending marker behind it.
        if (MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen()) {
            return true;
        }

        if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse) {
            return true;
        }

        // Reach the LocalMapMenu's NiCamera via the live MapMenu in the UI.
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return callOriginal();
        auto mapMenu = ui->GetMenu<RE::MapMenu>();
        if (!mapMenu) return callOriginal();

        const auto base         = reinterpret_cast<std::uintptr_t>(mapMenu.get());
        const auto localMapBase = base + 0x60;
        auto* cam = *reinterpret_cast<RE::NiCamera**>(localMapBase + 0x302F8);
        if (!cam) return callOriginal();

        // Handle remove-button clicks: delete the marker instead of placing one.
        // Must run BEFORE the IsCustomPinHovered check, because the remove panel
        // only appears while a pin is hovered, so IsCustomPinHovered is true here.
        // Must also run BEFORE the vanilla hovered-marker check, because the
        // engine's snap field can be >= 0 while the cursor is on our pin/panel.
        if (auto removeId = LocalMapRenderer::GetSingleton()->GetRemoveButtonTarget();
            removeId != 0) {
            MarkerManager::GetSingleton()->RemoveMarker(removeId);
            return true;
        }

        // Set/Clear destination button click. Same routing pattern as remove.
        if (auto destId = LocalMapRenderer::GetSingleton()->GetDestButtonTarget();
            destId != 0) {
            // Real markers only: the synthesized vanilla destination has no
            // hover panel and must never be toggled through this path.
            auto markers = MarkerManager::GetSingleton()->GetUserMarkers();
            bool isCurrentlyDest = false;
            for (const auto& m : markers) {
                if (m.id == destId) { isCurrentlyDest = m.isDestination; break; }
            }
            MarkerManager::GetSingleton()->SetDestination(destId, !isCurrentlyDest);
            return true;
        }

        // "Edit Note" button on the local-map hover panel.
        if (auto editId = LocalMapRenderer::GetSingleton()->GetEditButtonTarget();
            editId != 0) {
            MapMarkerRenderer::GetSingleton()->BeginEditMarker(editId);
            return true;
        }

        // If the cursor is hovering one of our custom pins (but not the remove
        // button), swallow the click. Without this, clicking the pin falls
        // through to the placement path and opens the note dialog on top of the
        // existing marker.
        if (LocalMapRenderer::GetSingleton()->IsCustomPinHovered()) {
            return true;
        }

        // Same for the exit-door beacon (info-only pin shown when an exterior
        // destination is set and we're inside an interior). No interactions,
        // but the click must not fall through to placement.
        if (LocalMapRenderer::GetSingleton()->IsBeaconHovered()) {
            return true;
        }

        // If the cursor is hovering a vanilla local-map marker, let vanilla
        // handle the click (blink/toggle) instead of opening our note dialog.
        // LocalMapMenu+0x303F8 is the hovered-marker index, written by
        // RefreshMarkers (RVA 0x97d690); -1 means no vanilla marker is snapped.
        // See findings/findings-localmap-vanilla-marker-detect.md.
        // Ctrl forces placement even if the engine remembered a hovered marker
        // this frame. MapHoverHook already suppresses the vanilla hover-zoom
        // and description card under Ctrl, so the snap field is the only thing
        // that would still route a near-door click to vanilla. Treat it as -1.
        // See findings/findings-LocalMap-Hover-Suppress.md (integration item 3).
        const bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const auto hoveredMarkerIndex =
            *reinterpret_cast<const std::int32_t*>(localMapBase + 0x303F8);
        if (hoveredMarkerIndex != -1 && !ctrlHeld) {
            return callOriginal();
        }

        // Left-click no longer places markers. Placement is right-click only,
        // via the "Add Marker Here" menu (MapMarkerRenderer::DrawTerrainMenu ->
        // LocalMapClickHook::PlaceAtCursor), matching the world map. The hover/
        // remove/destination button checks above still run on left-click; only
        // the terrain-placement call is gone. TryPlaceAtCursor() / PlaceAtCursor()
        // remain, now driven solely by the right-click menu.

        // Always call original so vanilla camera repositioning still works.
        return callOriginal();
    }
}

bool LocalMapClickHook::PlaceAtCursor() {
    return TryPlaceAtCursor();
}

void LocalMapClickHook::Install() {
    // LocalMapMenu__InputHandler::ProcessButton: SE 52088 / AE 52978
    // (findings-SE-Hook-IDs.md row 11).
    REL::Relocation<std::uintptr_t> target{ REL::RelocationID(52088, 52978) };
    const std::uint64_t targetAddr = target.address();
    const std::uint64_t hookAddr   = reinterpret_cast<std::uint64_t>(&HookedProcessButton);

    g_detour = std::make_unique<PLH::x64Detour>(targetAddr, hookAddr, &g_trampolineAddr);

    if (!g_detour->hook()) {
        logger::error("LocalMapClickHook: PolyHook x64Detour::hook() returned false");
        g_detour.reset();
        return;
    }

    logger::info("LocalMapClickHook installed | target={:X} | trampoline={:X}",
                 targetAddr, g_trampolineAddr);
}
