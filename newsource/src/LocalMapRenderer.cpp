// src/LocalMapRenderer.cpp
#include "LocalMapRenderer.h"
#include "MapDrawHelpers.h"
#include "MarkerManager.h"
#include "MapMarkerRenderer.h"  // IsMapFading()
#include "TextureManager.h"
#include "PathTracker.h"
#include "InputHandler.h"   // DIKToKeyName for the localized tracking legend
#include "Settings.h"
#include "FontManager.h"
#include "MenuUtils.h"
#include "UISound.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <imgui.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

// ============================================================================
// LocalMapRenderer — draws custom markers on the local map (interior + exterior)
// ----------------------------------------------------------------------------
// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):
//   Working: pins project through the local-map NiCamera and stay locked to
//            terrain through pan/zoom (after the viewport-rect fix below).
//            Path projection is skipped while path tracking or local-map trail
//            visibility is disabled; stored trail data remains intact.
//            Off-screen pins are culled in NDC (matches vanilla RefreshMarkers
//            at RVA 0x97d690) and a scissor clip rect on the viewport bounds
//            prevents any bleed onto the menu border when panned to an edge.
//            See findings/findings-localmap-marker-bleed.md.
//            When an exterior destination is set and the player is inside
//            an interior, an exit-door beacon (info-only, no buttons) is
//            projected at the load-door position that the compass chevron
//            points to. Picks the exit door the same way CompassHook does.
//            Rendering: no pin — a pulsing ring highlights the vanilla door
//            marker when on-screen; off-screen, the beacon survives the NDC
//            cull clamped to the viewport edge and draws a chevron pointing
//            at the door ("Exit" tag). Hover shows "Exit to <destination>".
//            See findings/findings-CompassExteriorDestinationFromInterior.md.
//            The vanilla destination is drawn RING-ONLY here (isRingOnly on
//            the id == kVanillaDestId entry), never as a pin: it is a
//            synthesized entry from MarkerManager, not a marker the player
//            placed, and drawing it as a pin put a stray marker on the local map
//            that the user could hover, rename and remove. The ring earns its
//            place because a door set as destination from THIS map's own
//            right-click menu has a vanilla door icon sitting right here, and
//            the halo belongs under it. Confirmed in-game 2026-08-25 on the
//            Whiterun exterior local map (Bannered Mare, Temple of Kynareth).
//            isRingOnly gates the entry out of the hover keep-rect, hover pick,
//            hover anim and pin-draw loops, so it can never set _activeMarkerId
//            — which is what every click and context-menu consumer reads.
//            The world map suppresses this same ring for a door destination
//            (VanillaDestination::fromLocalDoor): a door has no world-map icon,
//            so the ring pulsed over bare terrain there. A vanilla world MARKER
//            set as destination leaves that flag false and keeps its world-map
//            ring, which does sit under an icon.
//            Custom-pin hover panel is INFO-ONLY (name + "Custom Marker"
//            subtitle), no action rows — mirrors the world-map change in
//            MapMarkerRenderer. Right-click on a custom pin opens the shared
//            "CustomContextMenu" popup (owned by MapMarkerRenderer, which owns
//            MarkerManager/BeginEditMarker either way); InputHook's right-click
//            handler reads GetHoveredMarkerId() to route it. GetRemoveButtonTarget/
//            GetDestButtonTarget/GetEditButtonTarget always return 0 now (no
//            more panel rows to hit-test) — left in place, LocalMapClickHook
//            still reads them harmlessly.
//            Entering a custom pin plays vanilla's UIMapRolloverFlyout cue once
//            per hover transition, including direct movement between pins.
//            A blocking SKSE Menu Framework window temporarily hides all local-
//            map pins, trails, destination rings, and hover surfaces.
//   Broken/limited: local-map features as a whole are not a v1 target.
//   Dual-build: the RefreshMarkers hook target is version-gated as
//   REL::RelocationID(52090, 52980). The raw LocalMapMenu FIELD offsets quoted
//   throughout the comments below are AE 1.6.1170 figures; they are read via the
//   shared MapMenu offset helpers, but any NEW field offset added here must be
//   checked against findings/findings-SE-MapMenu-Offsets.md before use.
// ============================================================================
//
// References (all addresses for AE 1.6.1170):
//   findings-localmap-click-re.md §3   — LocalMapCamera layout
//   findings-localmap-click-re.md §9   — single-NiCamera confirmation
//   findings-localmap-click-re.md §10  — viewport-rect fix (the real bug)
//
// LocalMapMenu  = MapMenu + 0x60
// LocalMapCamera = LocalMapMenu + 0x302A0
// NiCamera*      = LocalMapCamera + 0x58       (= LocalMapMenu + 0x302F8)
// mode byte      = LocalMapMenu + 0x303FC      (1 = local map mode active)
// viewport rect  = LocalMapMenu + 0x30..0x3c   (left/top/right/bottom in menu space)
//
// What "panning jitter" actually was: the engine projects into a small
// viewport rect inside the menu, not the full screen. NiCamera::WorldPtToScreenPt3
// returns [0,1] inside that viewport. We were stretching those coords to
// ImGui::GetIO().DisplaySize, which moves them faster than the visible terrain.
// Fixed in Draw() by mapping the NDC through the viewport rect before scaling.
// Hook timing is not the issue; any frame point after DefaultState::Update has
// run produces a consistent matrix.

namespace {
    constexpr std::uintptr_t kLocalMapBaseOffset      = 0x60;
    constexpr std::uintptr_t kLocalMapCameraOffset    = 0x302A0;
    constexpr std::uintptr_t kNiCameraSubOffset       = 0x58;       // within LocalMapCamera
    constexpr std::uintptr_t kModeByteOffset          = 0x303FC;    // within LocalMapMenu
    // Hook target: FUN_14097d690 ("RefreshMarkers") entry. This is the same
    // instant the engine projects its own vanilla markers, so our cache is
    // built against an NDC space that matches the viewport rect we read in
    // Draw(). Any post-DefaultState::Update hook would work; this one is
    // cheap, runs once per frame, and is outside the render graph so it has
    // no allocator restrictions.
    // LocalMapMenu::RefreshMarkers, SE 52090 (0x8dd090) / AE 52980 (0x97d690).
    // Pair from findings/findings-LocalMap-Hover-Identity.md section 208. Bare
    // REL::ID(52980) was AE-only and resolves to SkyrimScript__SavePatcher::Func0
    // on SE, i.e. it would have detoured save-patcher code on that runtime.
    constexpr REL::RelocationID kRefreshMarkersID{ 52090, 52980 };

    // Cached projected markers. Written by the RefreshMarkers pre-hook,
    // read by ImGui Draw. Snapshotted under a mutex to keep the read short.
    struct CachedMarker {
        std::uint32_t id;
        float         nx, ny;   // normalized [0,1] screen coords (Y already flipped)
        std::string   note;
        bool          isDestination;
        bool          isExitDoorBeacon;  // synthetic entry for exterior dest from interior
        // Ring only, no pin: a vanilla DOOR set as destination from this map's
        // own right-click menu. Vanilla already draws the door icon; we add the
        // pulsing halo under it and nothing else. Never a hover or menu target.
        bool          isRingOnly;
        std::uint32_t iconType;
    };

    // Same logic as CompassHook's FindExitDoorForWorldspace, duplicated here
    // because the renderer needs to project the door position. Returns the
    // first load door in the cell whose linked door's worldspace matches
    // (or is parent-chained to) the target; falls back to the first door
    // reaching any exterior worldspace if no exact match. See
    // findings/findings-CompassExteriorDestinationFromInterior.md.
    bool WorldMatchesParent(RE::TESWorldSpace* current, RE::FormID markerWorldID) {
        for (auto* w = current; w; w = w->parentWorld) {
            if (w->GetFormID() == markerWorldID) return true;
        }
        return false;
    }

    RE::TESObjectREFR* FindExitDoorForWorldspaceLM(RE::TESObjectCELL* cell,
                                                   RE::FormID targetWorldspaceID) {
        if (!cell) return nullptr;
        RE::TESObjectREFR* foundDoor    = nullptr;
        RE::TESObjectREFR* fallbackDoor = nullptr;
        cell->ForEachReference([&](RE::TESObjectREFR* refPtr) {
            if (!refPtr) return RE::BSContainer::ForEachResult::kContinue;
            auto& ref = *refPtr;
            auto* extraTeleport = ref.extraList.GetByType<RE::ExtraTeleport>();
            if (extraTeleport && extraTeleport->teleportData) {
                auto linkedDoorRef = extraTeleport->teleportData->linkedDoor.get();
                if (linkedDoorRef) {
                    RE::TESWorldSpace* destWorld = linkedDoorRef->GetWorldspace();
                    if (destWorld) {
                        if (WorldMatchesParent(destWorld, targetWorldspaceID)) {
                            foundDoor = &ref;
                            return RE::BSContainer::ForEachResult::kStop;
                        }
                        if (!fallbackDoor) fallbackDoor = &ref;
                    }
                }
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });
        return foundDoor ? foundDoor : fallbackDoor;
    }

    std::mutex                 g_cacheMutex;
    std::vector<CachedMarker>  g_cache;

    // Exit-door beacon cache. FindExitDoorForWorldspaceLM walks every reference
    // in the cell, which is too expensive to run every frame (it cost ~30 fps).
    // We resolve the door once and reuse it until the cell or the destination
    // marker changes. The position is copied out by value, so we never hold a
    // raw TESObjectREFR* across frames — if the door is gone on re-resolve, the
    // key just changes and we re-scan (or find nothing). No dangling pointer.
    struct BeaconCache {
        RE::FormID  cellID         = 0;
        std::uint32_t destMarkerId = 0;  // 0 = none resolved
        bool        hasDoor        = false;
        RE::NiPoint3 doorPos{};
        std::string note;
    };
    BeaconCache g_beaconCache;

    std::uint64_t                              g_lmrTrampolineAddr = 0;
    std::unique_ptr<PLH::x64Detour>            g_lmrDetour;

    // Returns the LocalMapMenu base if the local map is currently active and
    // its camera is populated. Null otherwise — caller should skip work.
    std::uintptr_t GetActiveLocalMapBase() {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) return 0;
        auto mapMenu = ui->GetMenu<RE::MapMenu>();
        if (!mapMenu) return 0;
        const auto lmBase = reinterpret_cast<std::uintptr_t>(mapMenu.get()) + kLocalMapBaseOffset;
        if (*reinterpret_cast<const std::uint8_t*>(lmBase + kModeByteOffset) != 1) return 0;
        return lmBase;
    }

    // Project every current-cell marker into NDC coords and park the result
    // in the cache. NDC is [0,1] inside the local-map viewport rect — Draw()
    // maps that rect to screen space.
    void RefreshProjections() {
        const auto lmBase = GetActiveLocalMapBase();
        if (!lmBase) {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            g_cache.clear();
            return;
        }

        auto* niCam = *reinterpret_cast<RE::NiCamera**>(
            lmBase + kLocalMapCameraOffset + kNiCameraSubOffset);
        if (!niCam) {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            g_cache.clear();
            return;
        }

        RE::FormID currentCell = 0;
        RE::FormID currentWorld = 0;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* cell = player->GetParentCell()) {
                currentCell = cell->GetFormID();
                if (!cell->IsInteriorCell()) {
                    if (auto* ws = player->GetWorldspace()) {
                        currentWorld = ws->GetFormID();
                    }
                }
            }
        }

        auto& camData  = niCam->GetRuntimeData();
        auto& camData2 = niCam->GetRuntimeData2();

        std::vector<CachedMarker> next;
        auto markers = MarkerManager::GetSingleton()->GetMarkers();
        next.reserve(markers.size());

        for (const auto& m : markers) {
            // Interior: exact-cell match (worldSpaceID 0 sentinel). Exterior:
            // worldspace match — exterior grid cells roll over every 4096 units,
            // a cellID filter would drop pins one cell away. Off-view pins are
            // culled by the projection/scissor anyway.
            const bool sameScope = (currentWorld != 0)
                ? (m.worldSpaceID == currentWorld)
                : (m.worldSpaceID == 0 && m.cellID == currentCell);
            if (!sameScope) continue;
            // The vanilla destination is a synthesized entry, not a marker the
            // player placed. Drawing it here put a pin on the local map that the
            // user never created, hoverable and right-clickable with Rename and
            // Remove Marker on it. Its local-map representation is the exit-door
            // beacon below. This one skip also removes it from hover and the
            // context menu, which both take their target from this list.
            // The vanilla destination is a synthesized entry, not a marker the
            // player placed. Drawing it as a normal pin put a marker on the map
            // the user never created, hoverable and right-clickable with Rename
            // and Remove Marker on it. It still earns a RING: a door set as
            // destination from this map's own menu has a vanilla icon sitting
            // right here, and the halo belongs under it. Ring-only, so it stays
            // out of every hover, pin and context-menu path below.
            const bool ringOnly = (m.id == kVanillaDestId);
            float sx, sy, sz;
            bool inFront = RE::NiCamera::WorldPtToScreenPt3(
                camData.worldToCam, camData2.port, m.position, sx, sy, sz, 1e-5f);
            if (!inFront || !MapDraw::ProjectionDepthValid(sz)) continue;
            next.push_back({ m.id, sx, 1.0f - sy, ResolveMarkerName(m), m.isDestination, false, ringOnly, m.iconType });
        }

        // Exit-door beacon: if we're inside an interior (player has no
        // worldspace) and an exterior destination marker exists, project a
        // synthetic pin at the load door in the current cell that leads to
        // the destination's worldspace. Informational only, no interaction.
        //
        // The door resolution (FindExitDoorForWorldspaceLM) walks every ref in
        // the cell, so it only runs when the cell or destination marker changes
        // (see g_beaconCache). Projection of the cached door position still runs
        // every frame so the pin tracks the camera as you pan/zoom.
        if (auto* player = RE::PlayerCharacter::GetSingleton(); player && !player->GetWorldspace()) {
            // Find the current exterior destination marker (if any).
            const MapMarker* destMarker = nullptr;
            for (const auto& m : markers) {
                if (m.isDestination && m.worldSpaceID != 0) { destMarker = &m; break; }
            }

            // Re-resolve the door only when cell or destination marker changed.
            const bool keyChanged =
                g_beaconCache.cellID != currentCell ||
                g_beaconCache.destMarkerId != (destMarker ? destMarker->id : 0u);
            if (keyChanged) {
                g_beaconCache = {};
                g_beaconCache.cellID       = currentCell;
                g_beaconCache.destMarkerId = destMarker ? destMarker->id : 0u;
                if (destMarker) {
                    if (auto* exitDoor = FindExitDoorForWorldspaceLM(
                            player->GetParentCell(), destMarker->worldSpaceID)) {
                        const auto& loc = exitDoor->data.location;
                        g_beaconCache.hasDoor = true;
                        g_beaconCache.doorPos = RE::NiPoint3{ loc.x, loc.y, loc.z };
                        // The destination note is already the clean display
                        // name (MarkerManager synthesizes it without a prefix).
                        g_beaconCache.note = LocalizationManager::Format("Navigation.ExitTo",
                                                                        destMarker->note);
                    }
                }
            }

            // Project the cached door position every frame.
            if (g_beaconCache.hasDoor) {
                float sx, sy, sz;
                bool inFront = RE::NiCamera::WorldPtToScreenPt3(
                    camData.worldToCam, camData2.port,
                    g_beaconCache.doorPos, sx, sy, sz, 1e-5f);
                if (inFront && MapDraw::ProjectionDepthValid(sz)) {
                    next.push_back({ 0u, sx, 1.0f - sy, g_beaconCache.note, true, true, false, 0u });
                }
            }
        }

        std::lock_guard<std::mutex> lock(g_cacheMutex);
        g_cache = std::move(next);
    }

    using RefreshMarkers_t = void(*)(void*);
    void HookedRefreshMarkers(void* a_menu) {
        RefreshProjections();
        auto orig = reinterpret_cast<RefreshMarkers_t>(g_lmrTrampolineAddr);
        orig(a_menu);
    }
}

void LocalMapRenderer::Install() {
    REL::Relocation<std::uintptr_t> target{ kRefreshMarkersID };
    const std::uint64_t targetAddr = target.address();
    const std::uint64_t hookAddr   = reinterpret_cast<std::uint64_t>(&HookedRefreshMarkers);

    g_lmrDetour = std::make_unique<PLH::x64Detour>(targetAddr, hookAddr, &g_lmrTrampolineAddr);

    if (!g_lmrDetour->hook()) {
        logger::error("LocalMapRenderer: RefreshMarkers hook failed");
        g_lmrDetour.reset();
        return;
    }
    logger::info("LocalMapRenderer hook installed | target={:X} | trampoline={:X}",
                 targetAddr, g_lmrTrampolineAddr);
}

void LocalMapRenderer::Draw() {
    auto* ui = RE::UI::GetSingleton();
    if (!ui || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
        _activeMarkerId = 0;
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
        _editButtonTarget.store(0);
        _beaconHovered.store(false);
        return;
    }
    if (MenuUtils::ShouldBlockMapOverlay()) {
        _activeMarkerId = 0;
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
        _editButtonTarget.store(0);
        _beaconHovered.store(false);
        return;
    }

    auto mapMenu = ui->GetMenu<RE::MapMenu>();
    if (!mapMenu) return;

    // Never paint over the black fade (FaderMenu up during open/close
    // transitions), so interior local-map markers don't pop on the black
    // screen. No effect during normal interactive map use. See plugin.cpp.
    if (IsMapFading()) {
        _activeMarkerId = 0;
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
        _editButtonTarget.store(0);
        _beaconHovered.store(false);
        return;
    }

    const auto lmBase = reinterpret_cast<std::uintptr_t>(mapMenu.get()) + kLocalMapBaseOffset;
    if (*reinterpret_cast<const std::uint8_t*>(lmBase + kModeByteOffset) != 1) {
        _activeMarkerId = 0;
        _removeButtonTarget.store(0);
        _destButtonTarget.store(0);
        _editButtonTarget.store(0);
        _beaconHovered.store(false);
        return;
    }

    ImDrawList* dl       = ImGui::GetBackgroundDrawList();
    ImVec2      dispSize = ImGui::GetIO().DisplaySize;

    ImVec2 mouse{ -1.0f, -1.0f };
    float cursorW = 1.0f;
    float cursorH = 1.0f;
    if (auto* cursor = RE::MenuCursor::GetSingleton()) {
        cursorW = cursor->screenWidthX;
        cursorH = cursor->screenWidthY;
        if (cursorW > 0.0f && cursorH > 0.0f)
            mouse = ImVec2((cursor->cursorPosX / cursorW) * dispSize.x,
                           (cursor->cursorPosY / cursorH) * dispSize.y);
    }
    if (cursorW <= 0.0f) cursorW = 1.0f;
    if (cursorH <= 0.0f) cursorH = 1.0f;

    float vpL = *reinterpret_cast<const float*>(lmBase + 0x30);
    float vpT = *reinterpret_cast<const float*>(lmBase + 0x34);
    float vpR = *reinterpret_cast<const float*>(lmBase + 0x38);
    float vpB = *reinterpret_cast<const float*>(lmBase + 0x3c);

    struct Projected {
        std::uint32_t id;
        float         px, py;
        std::string   note;
        bool          isDestination;
        bool          isExitDoorBeacon;
        bool          isRingOnly;
        std::uint32_t iconType;
        bool          offscreen;       // beacon only: door projects outside the viewport
        float         dirX, dirY;      // beacon only: unit direction toward the real door pos
    };
    std::vector<Projected> visible;
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        visible.reserve(g_cache.size());
        for (const auto& c : g_cache) {
            // Vanilla local-map markers are culled in RefreshMarkers (RVA
            // 0x97d690) when their projected NDC falls outside [0,1]; the SWF
            // mask hides anything that leaks. Our pins are drawn via ImGui
            // straight to the backbuffer, so we must do the same cull here or
            // the markers bleed onto the menu border when the camera pans to
            // an edge. See findings/findings-localmap-marker-bleed.md.
            // Exception: the exit-door beacon survives the cull clamped to the
            // edge, so Draw() can show an edge chevron pointing at the door.
            const bool out = (c.nx < 0.0f || c.nx > 1.0f || c.ny < 0.0f || c.ny > 1.0f);
            if (out && !c.isExitDoorBeacon) continue;
            const float cnx = std::clamp(c.nx, 0.0f, 1.0f);
            const float cny = std::clamp(c.ny, 0.0f, 1.0f);
            float menuX = vpL + cnx * (vpR - vpL);
            float menuY = vpT + cny * (vpB - vpT);
            float px = (menuX / cursorW) * dispSize.x;
            float py = (menuY / cursorH) * dispSize.y;
            float dirX = 0.0f, dirY = 0.0f;
            if (out) {
                // Direction from the clamped edge point toward the real door.
                float rMenuX = vpL + c.nx * (vpR - vpL);
                float rMenuY = vpT + c.ny * (vpB - vpT);
                float rx = (rMenuX / cursorW) * dispSize.x - px;
                float ry = (rMenuY / cursorH) * dispSize.y - py;
                float len = std::sqrt(rx * rx + ry * ry);
                if (len > 1e-3f) { dirX = rx / len; dirY = ry / len; }
            }
            visible.push_back({ c.id, px, py, c.note, c.isDestination, c.isExitDoorBeacon, c.isRingOnly, c.iconType, out, dirX, dirY });
        }

    }

    auto* niCam = *reinterpret_cast<RE::NiCamera**>(lmBase + kLocalMapCameraOffset + kNiCameraSubOffset);
    std::vector<std::vector<ImVec2>> visibleSegments;
    // Real world Z per visible trail point, parallel to visibleSegments. Feeds
    // DrawPathTrail's elevation banding. Built in the SAME loop and pushed under
    // the SAME conditions as the ImVec2s below — points that fail projection are
    // dropped, so a separately-built Z array would silently desync and colour
    // the trail by the wrong points.
    std::vector<std::vector<float>> visibleSegmentZ;
    float playerTrailZ = 0.0f;
    // World pos of the newest trail point. Dedup stops the trail growing on
    // already-mapped ground, so the head can sit far from the player; the
    // draw block only pulses it when the player is actually near it.
    bool headNearPlayer = false;
    if (niCam && Settings::GetSingleton()->enablePathTracking &&
        Settings::GetSingleton()->pathTrailVisible) {
        auto& camData  = niCam->GetRuntimeData();
        auto& camData2 = niCam->GetRuntimeData2();
        auto segments = PathTracker::GetSingleton()->GetSegments();
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            // Elevation bands are relative to where the player is RIGHT NOW, so
            // this is read live every frame, not cached with the trail.
            playerTrailZ = player->GetPosition().z;
            if (!segments.empty() && !segments.back().empty()) {
                const auto& head = segments.back().back();
                const auto  ppos = player->GetPosition();
                const float hx = ppos.x - head.x, hy = ppos.y - head.y, hz = ppos.z - head.z;
                headNearPlayer = (hx * hx + hy * hy + hz * hz) < (250.0f * 250.0f);
            }
        }
        visibleSegments.reserve(segments.size());
        visibleSegmentZ.reserve(segments.size());
        for (const auto& seg : segments) {
            std::vector<ImVec2> vseg;
            std::vector<float>  vsegZ;
            vseg.reserve(seg.size());
            vsegZ.reserve(seg.size());
            for (const auto& pt : seg) {
                float sx, sy, sz;
                bool inFront = RE::NiCamera::WorldPtToScreenPt3(
                    camData.worldToCam, camData2.port, pt, sx, sy, sz, 1e-5f);
                if (inFront && MapDraw::ProjectionDepthValid(sz)) {
                    float menuX = vpL + sx * (vpR - vpL);
                    float menuY = vpT + (1.0f - sy) * (vpB - vpT);
                    float px = (menuX / cursorW) * dispSize.x;
                    float py = (menuY / cursorH) * dispSize.y;
                    vseg.push_back(ImVec2(px, py));
                    vsegZ.push_back(pt.z);   // same branch: keeps the arrays aligned
                }
            }
            if (!vseg.empty()) {
                visibleSegments.push_back(std::move(vseg));
                visibleSegmentZ.push_back(std::move(vsegZ));
            }
        }

        // Revisit glow removed 2026-07-17 (read as noise); hits now drive the
        // retracing arrow tint in MinimapRenderer instead (this file has no
        // player arrow of its own — vanilla draws the local-map player icon).
    }

    // Belt-and-braces: a scissor clip matching the local-map viewport ensures
    // any leftover bleed (e.g. a hover panel whose pin sits just inside the
    // edge) is GPU-clipped at the viewport boundary, matching Scaleform's
    // built-in mask. Push before pin/panel draws, pop before cursor.
    ImVec2 clipMin((vpL / cursorW) * dispSize.x, (vpT / cursorH) * dispSize.y);
    ImVec2 clipMax((vpR / cursorW) * dispSize.x, (vpB / cursorH) * dispSize.y);
    dl->PushClipRect(clipMin, clipMax, true);

    // Resolve the active (panel-showing) marker. Skip exit-door beacons here;
    // they're handled below with a simpler note-only panel and don't share
    // the _activeMarkerId state (their id is 0, used as a sentinel).
    // HUD-settings scales; must match the draw block below or hit rects drift.
    // Heading font (INI HeadingFontSize) folds into the title scale: same TTF,
    // so base-font measurement times the size ratio stays exact.
    auto* scaleSet = Settings::GetSingleton();
    ImFont* titleFont = FontManager::GetSingleton()->GetHeadingFont();
    if (!titleFont) titleFont = ImGui::GetFont();
    const float fontRatio  = titleFont->FontSize / ImGui::GetFontSize();
    const float titleScale = std::clamp(scaleSet->titleScale, 0.5f, 3.0f) * fontRatio;
    const float bodyScale  = std::clamp(scaleSet->bodyScale,  0.5f, 3.0f);

    // Skip hover detection entirely while the Note dialog is open: it already
    // sits over the map and swallows right-click, but without this guard
    // hovering a custom pin behind it still pops the hover panel underneath.
    // Pins themselves keep drawing below.
    const Projected* active = nullptr;
    if (!MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen() &&
        !MapMarkerRenderer::GetSingleton()->IsCustomMenuActive()) {
        if (_activeMarkerId != 0) {
            for (const auto& v : visible) {
                if (v.isExitDoorBeacon || v.isRingOnly) continue;
                if (v.id != _activeMarkerId) continue;
                // Info-only panel (no action rows), so the keep-rect is sized
                // from the header alone.
                auto pl   = MapDraw::ComputeInfoPanel(v.px, v.py, v.note.c_str(), dispSize, titleScale, bodyScale,
                                                      static_cast<float>(scaleSet->markerHoverOffsetX),
                                                      static_cast<float>(scaleSet->markerHoverOffsetY));
                auto keep = MapDraw::Combine(MapDraw::PinBox(v.px, v.py), pl.panel, 8.0f);
                if (MapDraw::PointInRect(mouse, keep)) active = &v;
                break;
            }
        }
        if (!active) {
            for (const auto& v : visible) {
                if (v.isExitDoorBeacon || v.isRingOnly) continue;
                if (MapDraw::PointInRect(mouse, MapDraw::PinBox(v.px, v.py))) { active = &v; break; }
            }
        }
    }
    const std::uint32_t previousMarkerId = _activeMarkerId;
    _activeMarkerId = active ? active->id : 0;
    if (_activeMarkerId != 0 && _activeMarkerId != previousMarkerId) {
        UISound::PlayMapMarkerHover();
    }

    // Beacon hover: independent of _activeMarkerId. Just the pin under the cursor.
    const Projected* activeBeacon = nullptr;
    for (const auto& v : visible) {
        if (!v.isExitDoorBeacon) continue;
        if (v.offscreen) continue;
        if (MapDraw::PointInRect(mouse, MapDraw::PinBox(v.px, v.py))) { activeBeacon = &v; break; }
    }

    const float timeSec = std::chrono::duration<float>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    // Shared trail renderer keeps LocalMap, Glance, and Inspect on the same
    // breadcrumb/diamond/anchor/pulse design and settings. Visibility here
    // is local-map-only (pathTrailVisible); Glance/Inspect gate themselves.
    if (Settings::GetSingleton()->enablePathTracking) {
        if (Settings::GetSingleton()->pathTrailVisible) {
            MapDraw::DrawPathTrail(dl, visibleSegments, headNearPlayer,
                                   PathTracker::GetSingleton()->IsTracking(),
                                   ImVec2(0.0f, 0.0f), 0.0f, 0.0f, false,
                                   nullptr, 0, 1.0f,
                                   &visibleSegmentZ, playerTrailZ);
        }

        // Draw Tracking Legend. Interior only: exteriors also have a local-map
        // view, but the tracker pauses outside (PathTracker::Update exits on
        // exterior cells), so an "ON" legend there would claim recording that
        // isn't happening.
        {
            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* cell   = player ? player->GetParentCell() : nullptr;
            if (cell && cell->IsInteriorCell()) {
                bool isTracking = PathTracker::GetSingleton()->IsTracking();
                // Key name comes from the live bind, so a rebind is reflected without
                // a translator having to guess which key the player uses.
                const char* trackKeyName =
                    InputHandler::DIKToKeyName(Settings::GetSingleton()->pathTrackKey);
                const std::string legendStr = LocalizationManager::Format(
                    isTracking ? "PathTracking.LegendOn" : "PathTracking.LegendPaused", trackKeyName);
                const char* legendText = legendStr.c_str();
                ImU32 legendColor = isTracking ? IM_COL32(100, 255, 100, 255) : IM_COL32(200, 200, 200, 255);
                float legendX = (vpL / cursorW) * dispSize.x + 10.0f;
                float legendY = (vpT / cursorH) * dispSize.y + 10.0f;
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(legendX, legendY), legendColor, legendText);
            }
        }
    }

    for (const auto& v : visible) {
        // Beacon draws its own dot+ring further down; skip so the two don't stack.
        if (v.isDestination && !v.isExitDoorBeacon) MapDraw::DrawDestinationRing(dl, v.px, v.py, timeSec);
    }

    // Hover tint eases in/out per marker id (same scheme as MapMarkerRenderer).
    // Beacon pins have no hover animation - they aren't part of _activeMarkerId
    // state, so they always draw at hoverT=0.
    {
        const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
        for (const auto& v : visible) {
            if (v.isExitDoorBeacon || v.isRingOnly) continue;
            float& anim = _hoverAnim[v.id];
            const float target = (v.id == _activeMarkerId) ? 1.0f : 0.0f;
            const float rate = (target > anim) ? (dt / 0.26f) : (dt / 0.30f);
            anim = std::clamp(anim + (target - anim > 0.0f ? rate : -rate), 0.0f, 1.0f);
        }
        for (auto it = _hoverAnim.begin(); it != _hoverAnim.end(); ) {
            if (it->second <= 0.0f && it->first != _activeMarkerId) it = _hoverAnim.erase(it);
            else ++it;
        }
    }
    for (const auto& v : visible) {
        if (v.isExitDoorBeacon) continue;  // ring/chevron below, no pin
        if (v.isRingOnly) continue;        // ring already drawn; vanilla owns the icon
        // Destination markers (ours or a vanilla marker set as destination) draw
        // destination.png regardless of their category/waypoint icon, so every
        // destination reads the same across radar, Inspect, and this local map.
        // Force iconType 0 so DrawPin renders the PNG, not a waypoint glyph.
        const std::uint32_t drawIconType = v.isDestination ? 0u : v.iconType;
        auto* pinSrv = v.isDestination ? TextureManager::GetIconByName("destination")
                                       : TextureManager::GetCategoryIcon(v.iconType);
        MapDraw::DrawPin(dl, v.px, v.py, _hoverAnim[v.id], drawIconType, pinSrv,
                         Settings::GetSingleton()->markerIconScale,
                         Settings::GetSingleton()->PinTintCol());
    }

    // Exit-door beacon. On-screen: pulsing ring around the vanilla door marker
    // (vanilla owns the door icon, we only highlight it). Off-screen: chevron
    // clamped inside the viewport pointing toward the door.
    for (const auto& v : visible) {
        if (!v.isExitDoorBeacon) continue;
        if (!v.offscreen) {
            // Solid dot anchor + two static outer rings.
            dl->AddCircleFilled(ImVec2(v.px, v.py), 8.0f, MapTheme::kBeaconBorder);
            dl->AddCircle(ImVec2(v.px, v.py), 12.0f, MapTheme::kBeaconBorder, 0, 1.5f);
            dl->AddCircle(ImVec2(v.px, v.py), 16.0f, MapTheme::kBeaconBorder, 0, 1.5f);
        } else {
            // Inset the chevron so it sits fully inside the clip rect.
            const float inset = 18.0f;
            float cx = std::clamp(v.px, clipMin.x + inset, clipMax.x - inset);
            float cy = std::clamp(v.py, clipMin.y + inset, clipMax.y - inset);
            const float ax = v.dirX, ay = v.dirY;      // toward door
            const float bx = -ay,   by = ax;           // perpendicular
            ImVec2 tip(cx + ax * 10.0f, cy + ay * 10.0f);
            ImVec2 l(cx - ax * 6.0f + bx * 8.0f, cy - ay * 6.0f + by * 8.0f);
            ImVec2 r(cx - ax * 6.0f - bx * 8.0f, cy - ay * 6.0f - by * 8.0f);
            dl->AddTriangleFilled(tip, l, r, MapTheme::kBeaconBorder);
            dl->AddTriangle(tip, l, r, IM_COL32(0, 0, 0, 200), 1.5f);
            // Label sits opposite the pointing direction so it stays in view.
            const std::string exitStr = LocalizationManager::Get("Navigation.Exit");
            ImVec2 ts = ImGui::CalcTextSize(exitStr.c_str());
            ImVec2 tp(cx - ax * 14.0f - ts.x * 0.5f, cy - ay * 14.0f - ts.y * 0.5f);
            tp.x = std::clamp(tp.x, clipMin.x + 2.0f, clipMax.x - ts.x - 2.0f);
            tp.y = std::clamp(tp.y, clipMin.y + 2.0f, clipMax.y - ts.y - 2.0f);
            dl->AddText(tp, MapTheme::kBeaconText, exitStr.c_str());
        }
    }

    // Info-only hover panel: name + "Custom Marker" subtitle, no action rows.
    // Actions (Set/Clear Destination, Rename, Remove) moved to the right-click
    // menu (MapMarkerRenderer::DrawCustomContextMenu, opened via
    // OpenCustomContextMenu below); mirrors the world-map change in
    // MapMarkerRenderer::Draw. Button-target getters below always store 0 now
    // (no more panel rows to hit-test) — left in place since
    // LocalMapClickHook still reads them harmlessly.
    if (active) {
        auto pl = MapDraw::ComputeInfoPanel(active->px, active->py, active->note.c_str(), dispSize, titleScale, bodyScale,
                                            static_cast<float>(scaleSet->markerHoverOffsetX),
                                            static_cast<float>(scaleSet->markerHoverOffsetY));

        // Bg from the global appearance setting, matching the world-map hover
        // panel and the vanilla stat card (see MapMarkerRenderer).
        MapDraw::DrawPanelCard(dl, pl.panel.mn, pl.panel.mx, scaleSet->BgCol(), scaleSet->BorderCol());

        // Header
        const float fs    = ImGui::GetFontSize();
        const float hPadX = MapDraw::kHeaderPadX;
        const float hPadY = MapDraw::kHeaderPadY;
        MapDraw::DrawTextShadowed(dl, titleFont, fs * titleScale,
                                  ImVec2(pl.panel.mn.x + hPadX, pl.panel.mn.y + hPadY),
                                  scaleSet->TitleCol(), active->note.c_str(), nullptr,
                                  pl.noteWrapW);
        dl->AddText(ImGui::GetFont(), fs * bodyScale * 0.85f,
                    ImVec2(pl.panel.mn.x + hPadX, pl.panel.mn.y + hPadY + pl.nameH + 2.0f),
                    MapDraw::FadeAlpha(scaleSet->BodyCol(), MapDraw::kLabelAlpha),
                    LocalizationManager::Get("Labels.CustomMarker").c_str());
    }
    _removeButtonTarget.store(0);
    _destButtonTarget.store(0);
    _editButtonTarget.store(0);
    _beaconHovered.store(activeBeacon != nullptr);

    // Beacon hover panel: note-only, no buttons, no swallow logic. Purely info.
    // Full destination label is fine now: the ring no longer covers the vanilla
    // door tooltip the way the old pin did.
    if (activeBeacon) {
        const char* beaconLabel = activeBeacon->note.c_str();
        const float pad     = 8.0f;
        ImVec2 noteSize = ImGui::CalcTextSize(beaconLabel);
        float panelW    = noteSize.x + pad * 2.0f;
        float panelH    = noteSize.y + pad * 2.0f;
        float ox = activeBeacon->px + 14.0f;
        float oy = activeBeacon->py - panelH - 8.0f;
        ox = std::min(ox, dispSize.x - panelW - 4.0f);
        oy = std::max(oy, 4.0f);
        ImVec2 mn(ox, oy);
        ImVec2 mx(ox + panelW, oy + panelH);
        dl->AddRectFilled(mn, mx, MapTheme::kBeaconBg, MapTheme::kPanelRounding);
        dl->AddRect(mn, mx, MapTheme::kBeaconBorder, MapTheme::kPanelRounding, 0, MapTheme::kPanelBorderW);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(mn.x + pad, mn.y + pad),
                    MapTheme::kBeaconText, beaconLabel);
    }

    dl->PopClipRect();

    // Requested, not drawn: HoverImageRenderer (the local-map door menu and the
    // stat card) appends after this function returns, and drawing here put the
    // cursor underneath it. Drawn once at the end of the frame by
    // MapDraw::DrawRequestedCursor. Also lands on the foreground list now rather
    // than this clipped background one, so the cursor is no longer scissored to
    // the map viewport.
    if (mouse.x >= 0.0f && mouse.y >= 0.0f)
        MapDraw::RequestCursor(mouse.x, mouse.y);
}
