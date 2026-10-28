#pragma once
#include <RE/Skyrim.h>
#include <mutex>
#include <vector>
#include <chrono>
#include <atomic>
#include <unordered_map>
#include <d3d11.h>
#include <imgui.h>  // ImVec2 for the Inspect crop rect
#include "PathTracker.h"
#include "MarkerManager.h"
#include "EntityMarkerSource.h"
#include "ActorMarkerSource.h"

// STATUS: Active production navigation overlay. CONFIRMED IN-GAME 2026-07-16 (AE).
// Inspect activation (2026-09-15): supports Hold (0) and Toggle (1) modes via navInspectActivation.
// Mini Radar visibility hotkey (2026-09-15): toggle-only, session state defaults
// visible, and the hidden gate suppresses Glance draw and snapshot refresh work.
// Confirmed in-game 2026-09-15: the configured key toggles the radar off and on.
// TERMINOLOGY: "Glance radar" / "Glance mode" is also referred to as "Mini Radar"
// (or "radar") by users for clearer intuition. The code, INI settings, and internal
// symbols intentionally keep "Glance" by default (DrawGlance, navGlance*). Do not
// perform renaming refactors when referred to as mini radar.
// CRITICAL FIX 2026-07-21 (CONFIRMED IN-GAME): the two "render-gate globals" that used to be
// forced/restored around the 16335 snapshot render were MISRESOLVED ids (414746 = a
// BSShaderAccumulator descriptor block, 390865 = a float constant table; SE twin is
// UNWIND_INFO_*). Forcing them wrote stray bytes into live engine state every snapshot
// (~2s while Glance is on), which stopped EXTERIOR world geometry from drawing -- buildings
// vanished while terrain/trees stayed, and snapped back instantly on map close because the
// map's real pipeline rebuilds that state. Both overlays could also render blank. The forcing
// is REMOVED and is NOT needed: 16335 renders correctly without it. Do not restore it from
// findings-MiniMapMenu-RenderPipeline.md section 2 (annotated with a correction there).
// Perf instrumentation stripped same day after sign-off: only "snapshot render took Xms"
// remains (2.4-5.3ms outdoors, typically 3.5-4.0ms). Actor-scan/ghost-merge logs are gone;
// their measured numbers live as comments at both sites.
// Space-transition settling + fade (2026-08-15, CONFIRMED IN-GAME via log): entering a
// cell/worldspace used to leave the radar visible but IMAGELESS until the player physically
// moved (measured 8.5s standing still in an interior). Cause: the first post-transition
// snapshot renders before the cell has finished streaming, and a blank render is UNDETECTABLE
// -- g_render is void, and snap.srv is the engine's persistent kLOCAL_MAP target, so it is
// always non-null and carries no success info. That empty snapshot still publishes with
// valid=true and the current space/pos, so the space check passed and the stationary skip
// (distSq < 16^2) pinned it indefinitely. Fix is two parts, both SCHEDULING ONLY -- the render
// path itself is untouched:
//   1. Settling window: for kSpaceSettleMs (5s) after a transition, SnapshotIsStale suspends
//      the stationary skip and refreshes every kSpaceSettleRetrySeconds (0.5s) regardless of
//      movement, so the half-streamed snapshot is replaced as the cell finishes loading.
//      Costs ~7 extra renders (~3-6ms each) per transition, then stops on its own.
//   2. Fade: _glanceFade slams to 0 on the transition and is HELD there (_spaceFadeHold) until
//      a same-space snapshot with a texture lands, so the radar fades in with content instead
//      of popping into an empty disc. Gated on snapshot arrival, not a timer, because the
//      engine gap varies with streaming. kSpaceFadeMaxHoldMs (5s, matches the settling window)
//      is a stuck-state escape hatch.
// The transition-detect block runs BEFORE the staleness checks in Draw(): they read
// _spaceEnteredAt, so updating it later would leave them on the previous space's timestamp.
// Glance<->Inspect crossfade (2026-07-17, CONFIRMED IN-GAME): 150ms linear ramp
// (_glanceFade/_inspectFade), smoothstep at draw time, both surfaces draw while
// their alpha > 0. FadeCol wraps every color; shared MapDraw::DrawPathTrail /
// DrawPin took alphaMul params (default 1.0, other callers unaffected).
// Backdrop/inspect source is now 1920x1080 (was 960x540): RenderTargetUpscaler
// resizes kLOCAL_MAP + kLOCAL_MAP_SWAP at kDataLoaded, guarded per-snapshot via
// EnsureApplied(). See RenderTargetUpscaler.cpp / findings-LocalMap-RenderTarget-Resolution.md.
// Glance mode (HUD radar) works, and now draws a cropped kLOCAL_MAP snapshot as its backdrop
// (option 1: SHARED snapshot crop, no second render). DrawGlance grabs _snapshot, projects the
// player world pos to UV center, sizes the crop from the Glance world radius (kGlanceRadius/
// kGlanceScale) via a local UV-per-world measure, clamps UV to [0,1], and blits with
// AddImageRounded(rounding=kGlanceRadius) for the circular mask. Per-frame player tracking.
// KNOWN: backdrop pixels carry the snapshot's north rotation while the trail/marker glyphs are
// north-up, so the map picture is aligned only in zero-north cells and blurs at high zoom.
// Snapshot refresh triggers, in Draw() order. Note the asymmetry: the inspectEdge path
// requests a refresh UNCONDITIONALLY, every other path goes through SnapshotIsStale.
//   - inspectEdge (Inspect key pressed): forced re-render, no staleness test. This is the
//     manual escape hatch -- if the radar is ever showing a bad or blank image, tapping
//     Inspect forces a fresh snapshot no matter what the staleness logic believes. Keep it
//     unconditional; that is the point.
//   - inspectHeld / glanceVisibleHere: staleness-gated (space change, move threshold,
//     settling window, periodic timer).
// The backdrop no longer WAITS for Inspect -- the glance-stale path keeps it live whether or
// not Inspect is ever opened -- but Inspect remains the way to force one on demand.
// Inspect view modes (2026-07-17, in-game confirm PENDING): the Inspect overlay
// has two framings, toggled by navInspectModeToggleKey (default R) while the overlay
// is held. Cell mode cover-fits the whole cell to the overlay's aspect (crops the
// long axis, *1.05 margin, centered on cell center); Radar mode frames an
// InspectRadarRangeUnits-wide rect at the overlay's aspect centered on the PLAYER
// (tracks as you walk; no cell clamp). Both blit the full texture edge to edge into
// the overlay rect (frustum aspect == overlay aspect, no letterbox, no un-stretch).
// One function, ComputeInspectFrustum, is the single switch point that turns
// (mode, cellExtents, cellCenter, playerPos, overlayAspect) into the frustum rect;
// RenderOneSnapshot calls it. Runtime mode seeds from InspectDefaultMode each
// session; toggling flips it and forces an immediate snapshot re-render (does NOT
// write the INI). Radar refresh: SnapshotIsStale's <16-unit stationary skip still
// refreshes on movement, so the player-centred crop tracks correctly.
// Framing is per-consumer (2026-07-17 fix): inspect-framed snapshots don't contain
// the whole cell, and Glance's UV-clamped backdrop crop skews/drifts on them. So
// RenderOneSnapshot uses ComputeInspectFrustum only while _inspectShown; Glance-driven
// refreshes frame full-cell anisotropic (the 2026-07-16 framing), and the Inspect
// falling edge forces a "glance reframe" refresh so the radar never sits on an
// inspect-framed snapshot. Glance is hidden while Inspect is held, so no conflict.
// Full-cell alone still skewed near cell edges: the player-centred Glance crop could
// extend past the cell boundary and the [0,1] UV clamp in DrawGlance shortened one
// axis. Fixed by padding the Glance-branch frustum by the Glance world radius
// (kGlanceRadius/kGlanceScale) per side, so the crop always stays inside the texture.
// AND the Glance frustum must be ISOTROPIC (16:9, texture aspect): the snapshot bakes
// the cell's north rotation, AddImageRounded can't rotate, and rotated content through
// anisotropic per-axis pixel scales renders sheared ("squished radar"). Glance framing
// = smallest 16:9 rect covering cell+pad; Inspect stays anisotropic full quality.
// Inspect mode (hold N) live-renders the local map ON DEMAND:
// Frustum mechanics (findings-LocalMap-Frustum-Framing.md): the frustum setter 16332 takes
// w/h independently and does not force 16:9. Since 2026-07-17 RenderOneSnapshot frames it at
// the OVERLAY's aspect (ComputeInspectFrustum, see the view-modes note above), so the render
// fills the target and the draw site blits it edge to edge with no UV crop (uvCropMin/Max
// always 0..1) and no un-stretch. cellExtentX/Y are still stored on the snapshot (Glance
// reads them via its world->UV measure, not the box aspect). The +2000/side pad is only for
// g_camBounds (SetAreaBounds), never the frustum. No MapMenu visit; refresh throttled to
// key-edge / mode-toggle / cell change / 2s held.
// Live snapshot driver: HUDMenu::PreDisplay vtable idx 7 hook; engine only calls PreDisplay
// when kRendersOffscreenTargets is set on the menu, so RequestSnapshotRefresh sets that flag
// on HUDMenu (verified live: without it the hook never fires). Blank-render fix: FULL
// menu-block ctor SE 52076 / AE 52964, then 16336 node + 16332 frustum, render-gate globals
// (527793/414746 forced 1, 513141/390865 forced 0) around the 16335 call. Findings:
// findings-LocalMap-LiveRender-Blank.md + findings-MiniMapMenu-RenderPipeline.md (RE of
// Luckycheech_MiniMapMenu.dll, Ghidra project F:\SOFTWARE\projects\minimapmenu; that mod
// renders this pipeline per frame, we deliberately stay demand-driven).
// Fog of war updates per refresh: FogOfWarOverlay is a baked mesh (seen data read only at
// build), so RenderOneSnapshot re-runs the culling-node init 16095/16336 every refresh to
// rebuild it (findings-LocalMap-FogOfWar-Update.md).
// Quick-mark (2026-07-17): holding Inspect + pressing navQuickMarkKey (default F)
// drops a default-named custom marker ("Custom Marker N") at the player's current
// position, no note dialog. Interior only (Inspect can't be held over exterior
// cells, BuildFrame already gates on that). Edge-triggered + 500ms cooldown in
// Draw(); InputHook independently swallows the same key by DX scancode so vanilla
// toggle-POV never fires. Mode badge grows a second muted line underneath showing
// both live-rebindable keys ("R: View  F: Mark"). A confirmation toast ("Marker N
// Added") pops in under the hint for ~1.8s on each placement (2026-08-21): the pin
// itself doesn't appear until the next snapshot refresh, so without it a press
// looked like a no-op and got repeated.
// Camera view cone (both modes, under the player arrow): camera yaw from
// PlayerCamera->cameraRoot->world.rotate column 1, atan2(x, y); arrow stays body yaw.
// [NavigationOverlay] RefreshOnInspect=0 = frozen mode: hard-block refresh requests and
// queued PreDisplay work, then borrow whatever vanilla (or a minimap mod) last wrote to
// kLOCAL_MAP; background only, no projection. This covers the exterior Inspect mode-coercion
// request that occurs before Draw reaches the frozen-texture branch.
// Logging: per-refresh lines are log::debug (gated by INI Logging Level); install/ctor info.
// The frustum/DIAG debug lines are PERMANENT (user call 2026-07-16, ~4 lines per 2s refresh
// is acceptable): they're the only visibility into framing regressions. Don't strip them.
// SE untested: ids are RelocationID(se, ae) but SE side came from symbol-cluster alignment
// (LiveRender-Blank finding section 4) and struct offsets (+0x302a0 camera, +0x40 culling)
// are AE-verified only. Needs one SE runtime pass.
// Per-mode visibility toggles (2026-07-16): navGlanceShowTrail/navInspectShowTrail
// gate trail DRAWING only, independently per mode; recording and the entity cell
// walk are untouched so toggling reacts without a refresh.
// Overlay visibility + entity categories, RESTRUCTURED 2026-07-17: four
// independent show/hide toggles (navGlance/InspectInterior/ExteriorVisible)
// replace the old navGlanceVisible/navInspectVisible masters + the
// GlanceExteriorVisible outdoor gate (which used to bail BuildFrame's whole
// frame build outdoors and wrongly killed Inspect with it -- fixed: BuildFrame
// now only bails if NONE of the four apply to the current environment).
// Entity categories are now 20 bools (view x Int/Ext x 5 categories) instead of
// 10 + 2 exterior "masters"; GlanceAnyEntity(exterior)/InspectAnyEntity(exterior)
// take the environment as a param and OR the matching 5. Pin size on the
// overlay (Glance disc + Inspect box) is its own knob per view
// (navGlancePinSize/navInspectPinSize), NOT markerIconScale (world/local map).
// Entity layer (Inspect + Glance): EntityMarkerSource walks the cell in
// RenderOneSnapshot (gated on GlanceAnyEntity(isExterior) ||
// InspectAnyEntity(isExterior)), draws corpse/door/container/loot glyphs under
// the custom pins + player arrow. Glance mirrors the same glyphs at ~70% size
// via WorldToGlance (player-centred, off-radar entities not rim-clamped).
// Entity layer (Glance + Inspect, both) draws PNG icons (corpse/door/container/
// loot) via TextureManager::GetIconByName; falls back to the old vector glyph
// per-category if the icon SRV is null. Both sizes are user-configurable:
// Settings::navGlanceEntityIconSize (4-16, default 7) is the Glance icon box
// side length (and scales the vector-glyph fallback off the old 7px baseline);
// navInspectEntityIconSize (10-40, default 18) is the Inspect icon box side
// length. SettingsMenu -> Overlay Visibility -> "Glance/Inspect: Item Icon Size"
// sliders.
// Proximity-discovered (reveal within ~one room),
// session-memory only (cleared on load/new game), cosave persistence later.
// Shared trail visuals: MapDraw::DrawPathTrail applies the same breadcrumb path,
// turn diamonds, branch anchors, age fade, revisit glow, head cone, and pulse
// used by LocalMapRenderer, with the PathTrail settings live in all views.
// Exterior overlay (spec-findings-Exterior-Navigation-Overlay.md, live-test gate
// PASSED): both views now work outdoors. Inspect is forced Radar mode outdoors
// (Cover/cell-fit is meaningless with no cell to contain); Glance frames a
// player-centred max(2*GlanceRangeUnits*1.5, 4000)-wide 16:9 patch. DIAG log
// showed the engine enforces a MINIMUM outdoor frustum (requested 1000x562.5,
// got l/r +-5374 t/b +-3023) -- do not try to defeat this clamp, it's an
// engine floor, not a bug in our framing. Marker filter/quick-mark/staleness
// all branch on worldSpaceID outdoors instead of cellID (see BuildFrame,
// SnapshotIsStale, and the marker-filter sameSpace predicate at the top of
// DrawGlance/DrawInspect). Entity layer outdoors uses the new
// EntityMarkerSource::Collect(origin, radius, ...) radius walk gated by the
// Ext category set (navGlance/InspectExtEntity*, default Glance off / Inspect
// on, matching the old master defaults) via GlanceAnyEntity(true)/
// InspectAnyEntity(true). Path trail stays OFF outdoors (v1).
// Actor radar layer (2026-07-17, spec-findings-Actor-Radar-Layer.md): living,
// non-player actors drawn as plain colored dots (Ally blue / Hostile red /
// Neutral dim grey, MapTheme kActor*), BELOW custom pins and the player
// arrow, above the entity glyph layer. Transient, no persistence, no cosave.
// DECOUPLED from the snapshot (2026-07-17): CollectActorsIfDue runs the walk on
// its OWN 2s cadence from Draw() (Present thread), NOT inside RenderOneSnapshot,
// because the snapshot's <16-unit stationary skip suppressed discovery so an NPC
// approaching a standing player never appeared until Inspect forced a snapshot.
// Own module/own walk (ActorMarkerSource.h explains why it stays separate from
// EntityMarkerSource's discovered-set model). MergeActorGhosts (Present thread,
// no mutex) applies each fresh scan into _actorGhosts and ages them: fade-in
// ~0.3s while live (covers first sight + re-entry), fade-out ~1s at last live
// position once out of the (INI-tunable) proximity radius or dead, then dropped.
// Proximity gate is Settings::navActorProximityUnits (200-4000, default 1000,
// "Detection Range" slider) checked per-frame so a leaving actor freezes at the
// rim immediately, not up to a scan later. Perf: two logs, both SKSE::log::info,
// both mandatory until confirmed in-game -- the 2s "actor scan took Xus" (walk
// cost) and the 5s-windowed "ghost merge avg/max us" (per-frame merge cost,
// scales with crowd size via one LookupByID per live ghost).
// OPEN TWEAKS (known, not started): snap.stale never set true so the stale dim-tint never fires;
// frozen mode stretches vanilla's framing into our 16:9 box (its content aspect is
// whatever vanilla last rendered). Glance ZOOM (world units shown, not disc px) is now
// INI-driven via [NavigationOverlay] GlanceRangeUnits (default 1200, Settings::
// navGlanceRangeUnits); the RenderOneSnapshot Glance-branch frustum pad follows the
// same value, so a tighter range also sharpens the backdrop with no extra plumbing.
// Custom marker range gate (2026-07-20, spec-findings-Marker-Range-Gate.md):
// custom markers past Settings::navMarkerRange ([NavigationOverlay] MarkerRange,
// 200-4000, default 3000) world units from the player are hidden on BOTH Glance
// and Inspect (GetSquaredDistance vs rangeSq, hoisted per loop). The old Glance
// rim-clamp for custom markers was removed: out-of-range = gone, not pinned to
// the rim. One global range for both views; slider "Marker Range" in settings.
// Actor ghosts keep their own navActorProximityUnits; quest targets un-gated.
// Active quest markers (2026-07-20, findings-HUDMarker-Pipeline-QuestMarkers.md):
// both views draw the game's active quest markers with active.png. Source is the
// engine HUD marker pipeline, NOT a quest walk: HUDMarkerHook entry-detours the
// engine's per-marker AddMarker (RELOCATION_ID 50851/51728) and buffers the world
// position of every quest/questDoor marker per compass pass. These are the engine's
// already tracked-filtered, alias-resolved, door-retargeted markers (NPC positions
// live), the exact set vanilla's compass shows. BuildFrame reads
// HUDMarkerHook::GetQuestMarkerPositions() into NavigationFrame::questTargets;
// DrawGlance/DrawInspect blit active.png at each via DrawQuestTargetIcon (sized off
// navGlance/InspectPinSize), same projection as custom pins. Null SRV (missing
// active.png) = silent skip. Replaced the old DataHandler+kActive walk (kActive was
// NOT the tracked flag; most targets are NPCs the static filter dropped). Per-icon
// (quest vs questDoor) art and an on/off toggle are future work.
class MinimapRenderer {
public:
    static MinimapRenderer* GetSingleton() {
        static MinimapRenderer singleton;
        return &singleton;
    }

    void Install();   // HUDMenu::PreDisplay hook for snapshot refresh
    void Draw();      // Present hook ImGui draw, both modes

    // True while the Inspect overlay is on screen this frame. InputHook reads this to
    // swallow the mode-toggle key (default R) before the engine sees it — R is vanilla
    // sheathe-weapon, and Inspect runs during gameplay, not in a menu. Atomic: set on
    // the present thread, read on the input thread.
    static bool IsInspectShown() { return GetSingleton()->_inspectShown.load(); }
    static bool IsInteriorInspectShown() { return GetSingleton()->_interiorInspectShown.load(); }
    static bool IsInspectToggleActive() { return GetSingleton()->_inspectToggleActive.load(); }
    static void ToggleInspect() {
        auto* inst = GetSingleton();
        inst->_inspectToggleActive.store(!inst->_inspectToggleActive.load());
    }
    static void ResetInspectToggle() { GetSingleton()->_inspectToggleActive.store(false); }
    static bool IsGlanceVisible() { return GetSingleton()->_glanceVisible.load(); }
    static void ToggleGlanceVisible() {
        auto* inst = GetSingleton();
        inst->_glanceVisible.store(!inst->_glanceVisible.load());
    }

private:
    MinimapRenderer() = default;
    ~MinimapRenderer() = default;
    MinimapRenderer(const MinimapRenderer&) = delete;
    MinimapRenderer& operator=(const MinimapRenderer&) = delete;

public:
    // Inspect overlay framing. Cover = fit the whole cell to the overlay aspect
    // (crop the long axis). Radar = fixed-range crop centred on the player.
    enum class InspectViewMode { Cover = 0, Radar = 1 };

    struct NavigationFrame {
        bool valid = false;
        bool inspectHeld = false;
        RE::FormID cellID = 0;
        // 0 = interior (matches the MarkerManager/quick-mark sentinel).
        // Exterior worldspace FormID otherwise. See spec-findings-Exterior-
        // Navigation-Overlay.md decision/gotcha on worldSpaceID==0 as sentinel.
        RE::FormID worldSpaceID = 0;
        bool isExterior = false;
        RE::NiPoint3 playerPos{};
        float playerAngle = 0.0f;   // body yaw (data.angle.z)
        float cameraAngle = 0.0f;   // camera yaw from cameraRoot; == body yaw when camera aligned
        std::vector<PathTracker::Segment> segments;
        std::vector<MapMarker> markers;
        // Live static quest-objective targets (doors/containers/activators/
        // statics) resolved this frame. Actor + location targets deferred. World
        // positions only, same-space filtered like markers. See
        // spec-findings-Active-Quest-Markers.md.
        std::vector<RE::NiPoint3> questTargets;
        // Where the player is, shown as the Inspect overlay's title. Interior:
        // the cell's own full name. Exterior: current LOCATION name (city/hold
        // POI) when the player is inside one, else the worldspace name -- an
        // exterior cell's own name is usually empty or a grid label.
        std::string locationName;
    };

    struct SnapshotProjection {
        bool valid = false;
        bool refreshInFlight = false;
        bool stale = true;
        RE::FormID cellID = 0;
        // Exterior pair (spec-findings-Exterior-Navigation-Overlay.md decision 4):
        // SnapshotIsStale compares worldSpaceID outdoors instead of cellID (the
        // exterior grid cellID changes every 4096 units and would force a refresh
        // on every grid-cell boundary). 0 = interior/unset.
        RE::FormID worldSpaceID = 0;
        bool isExterior = false;
        // Frustum half-width this snapshot was framed at, used by the exterior
        // staleness check to scale the move-refresh threshold to 25% of the
        // current patch (decision 4): a fixed unit distance is too tight at
        // small Radar ranges and too loose at large Glance patches.
        float frustumHalfWidth = 0.0f;
        RE::NiPoint3 playerPos{};
        float northRot = 0.0f;
        float worldToCam[4][4]{};
        RE::NiRect<float> port{};
        // Sub-rect of the snapshot texture the cell actually occupies. Since 2026-07-16
        // the frustum is anisotropic (matches the cell exactly), so the cell fills the
        // target edge to edge and this is always (0,0)/(1,1). Kept for the frozen-texture
        // DIAG path and any old snapshot that predates the change. Set in RenderOneSnapshot.
        ImVec2 uvCropMin{0.0f, 0.0f};
        ImVec2 uvCropMax{1.0f, 1.0f};
        // True cell world extents this snapshot was framed to (trueExtentX x trueExtentY,
        // pre-margin). The anisotropic frustum stretches the cell into the 16:9 target, so
        // draw sites un-stretch by sizing the drawn image to cellExtentX : cellExtentY.
        // 0/0 = unknown (frozen/legacy snapshot) -> draw sites fall back to a 16:9 box.
        float cellExtentX = 0.0f;
        float cellExtentY = 0.0f;
        ID3D11ShaderResourceView* srv = nullptr;
        std::chrono::steady_clock::time_point takenAt{};
        // Entity-layer glyphs collected during this snapshot's cell walk. Copied
        // by value (positions + FormID + category + discovered only, never a
        // TESObjectREFR*), guarded by _snapshotMutex like every other snap field.
        std::vector<EntityMarkerSource::EntityMarker> entities;
        // NOTE: actors are NOT stored here. The actor radar layer collects on
        // its own 2s cadence into MinimapRenderer::_lastActorScan (Present
        // thread, snapshot-independent), so it is not gated by the snapshot
        // stationary skip. See CollectActorsIfDue / MergeActorGhosts.
    };

private:
    NavigationFrame BuildFrame();
    // a_alpha: eased 0..1 crossfade alpha (smoothstep of _glanceFade/_inspectFade).
    void DrawGlance(const NavigationFrame& a_frame, float a_alpha);
    void DrawInspect(const NavigationFrame& a_frame, float a_alpha);
    void RequestSnapshotRefresh(const NavigationFrame& a_frame, const char* a_reason);
    void RenderOneSnapshot();  // PreDisplay only

    // Actor ghost merge (spec-findings-Actor-Radar-Layer.md decision 5): folds
    // the latest scan (_lastActorScan) into _actorGhosts, aging out anything not
    // seen this scan via freeze+fade. Present-thread only, called once per
    // Draw() before DrawGlance/DrawInspect so both surfaces read the same
    // merged state for this frame. Consumes _lastActorScan (the decoupled
    // collect below), NOT the snapshot: actor discovery must not be gated on
    // the snapshot's stationary skip (2026-07-17: NPC approaching a standing
    // player never appeared until a snapshot was forced by opening Inspect).
    void MergeActorGhosts();

    // Self-paced actor collect, independent of the snapshot render. Runs on the
    // Present thread from Draw() but only actually walks refs every
    // kActorScanSeconds; the whole point is that it keeps discovering actors
    // even while the snapshot is frozen by the stationary skip. Cheap (~55us
    // measured, vs ~3-5ms for a snapshot), so a fixed 2s cadence is fine.
    void CollectActorsIfDue(const NavigationFrame& a_frame);

    // Single switch point for Inspect framing. Turns the runtime mode + cell/player
    // geometry into the frustum rect (center + full width/height in world units) the
    // frustum setter (16332) receives. Future modes drop in here. w/h are already at
    // a_overlayAspect (frustum aspect == overlay aspect => full-fill, no un-stretch).
    struct FrustumRect { float centerX, centerY, width, height; };
    FrustumRect ComputeInspectFrustum(InspectViewMode a_mode,
                                      float a_cellExtentX, float a_cellExtentY,
                                      float a_cellCenterX, float a_cellCenterY,
                                      const RE::NiPoint3& a_playerPos,
                                      float a_overlayAspect) const;

    static void HookedPreDisplay(RE::IMenu* a_this);

    std::mutex _snapshotMutex;
    SnapshotProjection _snapshot;
    std::atomic<bool> _refreshPending{false};
    bool _inspectWasHeld = false;
    // Inspect view-mode state. _inspectMode is the live framing; seeded from
    // InspectDefaultMode on first Inspect of the session (_inspectModeInited).
    // _toggleWasDown edge-triggers the toggle key (fire once per press).
    // Read by RenderOneSnapshot on the PreDisplay thread, set in Draw, hence atomic.
    std::atomic<InspectViewMode> _inspectMode{InspectViewMode::Cover};
    bool _inspectModeInited = false;
    bool _toggleWasDown = false;
    // Quick-mark edge state + cooldown timestamp. Read/written on the Present
    // thread only (same as _toggleWasDown), no atomics needed.
    bool _quickMarkWasDown = false;
    std::chrono::steady_clock::time_point _lastQuickMarkTime{};
    // Quick-mark confirmation toast (2026-08-21). A placed breadcrumb was
    // otherwise invisible until Inspect closed, so a stray F read as "nothing
    // happened" and users pressed again. _quickMarkToastTime is the placement
    // instant; DrawInspect animates a pop-in/fade-out from it and draws
    // _quickMarkToastText (already localized + numbered at placement time, so
    // the draw path does no formatting).
    std::chrono::steady_clock::time_point _quickMarkToastTime{};
    std::string _quickMarkToastText;
    // Overlay aspect (w/h) captured in Draw so RenderOneSnapshot can frame the
    // frustum to the same rect the overlay draws into. Screen aspect; set each
    // frame Inspect is shown.
    std::atomic<float> _overlayAspect{16.0f / 9.0f};
    std::atomic<bool> _inspectShown{false};  // see IsInspectShown()
    std::atomic<bool> _interiorInspectShown{false};  // see IsInteriorInspectShown()
    std::atomic<bool> _inspectToggleActive{false};   // see IsInspectToggleActive()
    std::atomic<bool> _glanceVisible{true};          // session-only hotkey state
    bool _glanceBackdropActive = false;  // one-shot log gate for the Glance backdrop

    // Glance/Inspect crossfade (2026-07-17): the old cut was same-frame and felt
    // abrupt (radar vanishing/appearing with no transition). Each is 0..1 and
    // chases its target at a fixed ~0.15s rate every Draw() call; both can be
    // > 0 during the ~150ms crossfade, so Glance and Inspect draw simultaneously
    // while swapping. Read on the Present thread only, no atomics needed.
    float _glanceFade = 1.0f;
    float _inspectFade = 0.0f;

    // Space-transition fade (2026-08-15): entering a new cell/worldspace invalidates
    // the snapshot (SnapshotIsStale bails on a space mismatch before the timer), and
    // the replacement only renders once HUDMenu::PreDisplay next fires. For that gap
    // the radar has no backdrop and no same-space layers, so it used to sit visibly
    // blank for ~1-2s on every load door. Instead: slam _glanceFade to 0 on the
    // transition and hold it there until a same-space snapshot actually lands, so the
    // radar fades back in with content rather than popping into an empty disc.
    // Gated on snapshot arrival, NOT on a timer - the engine gap is variable (exterior
    // grid is still streaming after a door) and a fixed delay would guess it wrong.
    // _lastDrawnSpace: 0 = nothing drawn yet. Interior uses cellID, exterior
    // worldSpaceID, matching the comparisons in SnapshotIsStale.
    std::uint32_t _lastDrawnSpace = 0;
    bool _lastDrawnExterior = false;
    bool _spaceFadeHold = false;
    // Set on every space transition. Drives BOTH the fade-hold safety release and the
    // snapshot settling window in SnapshotIsStale (early post-transition snapshots
    // catch a half-streamed cell and can't be detected as bad, so they're re-rendered
    // on a short cadence until this window closes).
    std::chrono::steady_clock::time_point _spaceEnteredAt{};

    // Retracing arrow tint (2026-07-17): ramps toward 1 while the player walks
    // over already-tracked ground (PathTracker revisit hits non-empty), toward
    // 0 otherwise. Same ~0.25s linear-ramp/smoothstep pattern as the crossfade
    // faders above; read on the Present thread only, no atomics needed.
    float _retraceFade = 0.0f;

    // Combat rim pulse (2026-07-17): ramps toward 1 while the player is in
    // combat (debounced, raw IsInCombat() flaps false for a few frames when
    // sheathing mid-fight), toward 0 otherwise. Same ramp pattern as above.
    float _combatFade = 0.0f;
    float _lastCombatTime = -10.0f;

    // Actor radar layer transient state (spec-findings-Actor-Radar-Layer.md
    // decision 3). Present-thread only, no mutex: only ever touched from
    // Draw()/DrawGlance()/DrawInspect()/CollectActorsIfDue(), all on the
    // Present thread. RenderOneSnapshot (PreDisplay thread) no longer touches
    // the actor layer at all (collect was decoupled 2026-07-17).
    struct ActorGhost {
        RE::NiPoint3 position;                            // live per-frame while in radius; frozen after leave
        ActorMarkerSource::ActorMarker::Allegiance allegiance;
        std::chrono::steady_clock::time_point lastSeen;   // refreshed while live
        bool  frozen = false;                             // true once out of radius/dead
        float alpha  = 0.0f;                              // 0..1, ramped by the merge fade machine; draw multiplies by it
    };
    std::unordered_map<RE::FormID, ActorGhost> _actorGhosts;
    std::chrono::steady_clock::time_point _lastGhostTick{};  // dt source for the ghost fade machine
    // Decoupled actor scan (CollectActorsIfDue): the latest walk result + when
    // it was taken. MergeActorGhosts applies it once per new scan (compares
    // takenAt against _lastMergedScanAt) so a stationary player still gets fresh
    // discovery on the 2s cadence. Present-thread only, no mutex.
    std::vector<ActorMarkerSource::ActorMarker> _lastActorScan;
    std::chrono::steady_clock::time_point _lastActorScanAt{};   // when _lastActorScan was collected
    std::chrono::steady_clock::time_point _lastMergedScanAt{};  // _lastActorScanAt last applied to _actorGhosts
    static constexpr float kActorScanSeconds = 2.0f;

    using PreDisplayFn = void (*)(RE::IMenu*);
    PreDisplayFn _origPreDisplay = nullptr;
};
