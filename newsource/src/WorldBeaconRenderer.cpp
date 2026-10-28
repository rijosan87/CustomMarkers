#include "PCH.h"
#include "WorldBeaconRenderer.h"

#include "MarkerManager.h"
#include "CompassHook.h"
#include "MenuUtils.h"
#include "BeastForm.h"
#include "PartySheetState.h"
#include "Settings.h"
#include "TextureManager.h"
#include "MapDrawHelpers.h"
#include "MapTheme.h"
#include "FontManager.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

// ============================================================================
// WorldBeaconRenderer — In-World 3D Beacons and Screen-Space Waypoints
// ----------------------------------------------------------------------------
// STATUS:
//   Working: Active destination beacons rendered directly in the 3D gameplay
//   view. Projects world-anchored positions via RE::Main::WorldRootCamera(),
//   draws a feathered light pillar and diamond destination badge through shared
//   MapDraw primitives, plus distance tags, edge-clamped directional indicators,
//   proximity ease-out, and screen-center aim fade.
//
//   AUTO-CLEAR ON ARRIVAL: entering Fade Near's horizontal radius clears the
//   active destination. Temporary Look & Pin and Inspect quick-mark destinations
//   are deleted; manual custom markers only lose destination status, and vanilla
//   map markers remain because only the plugin's synthesized destination record
//   is cleared. MarkerOrigin, serialized in CMMK v8, owns that distinction; do
//   not infer it from the displayed auto-name. Verified 2026-09-14 at a 5m
//   radius: origin 1/2 logged REMOVE_TEMPORARY_MARKER, origin 0 and synthesized
//   vanilla logged CLEAR_DESTINATION, including a newly set destination at 0m.
//
//   GROUND Z (2026-08-15, fixing "the beacon follows the player around"):
//   World-map and local-map placement store synthetic projection-plane Z values.
//   Look & Pin is the exception and stores a real hit Z. This renderer resolves
//   its own surface anchor for every destination instead of depending on which
//   placement path produced the marker.
//
//   The first cut fell back to playerPos.z, copied from CompassHook.cpp:375.
//   That is correct THERE and wrong HERE: CompassHook substitutes the player Z
//   into a DISTANCE calculation, where it cancels out. Substituting it into a
//   rendered POSITION welds the beacon to the player's altitude, so it rose and
//   sank as the player walked. That was the reported bug.
//
//   Now: in exteriors, the probe is anchored to the true landscape altitude via
//   tes->GetLandHeight(marker.pos, landZ). The ray starts only 1500 units above
//   the landscape, finding roofs, decks, and bridges without shooting down from
//   the sky and hitting high mountain cliffs, overhangs, or weather boxes
//   (e.g. Stonehollow in Wyrmstooth). Distant unloaded cells fall back to landZ.
//   Interiors probe down from player altitude.
//   Cached per marker id.
//   Diagnostics are gated behind [Logging] BeaconVerboseLog (default off).
//   The same gate emits a 300-active-frame performance summary with renderer
//   time, visible/edge counts, and separately timed Havok occlusion rays.
//
//   OCCLUSION (2026-08-15): now BUILT, reversing spec decision 7. That decision
//   was made when beacons were short; a 2000-unit pillar crossing the whole
//   screen is a different visual problem, and painting it over a building read
//   as broken rather than as "seen through terrain".
//
//   Still no depth buffer (an ImGui overlay in a Present hook cannot have one),
//   so this is a raycast approximation: kOcclusionSamples points up the beam,
//   each cast camera -> point, each dimming independently. Per-sample rather
//   than per-beacon because a tall pillar can be buried at the base and clear
//   above a roofline, which one ray cannot express. Blocked samples DIM
//   (kOccludedAlphaScale) instead of hiding: hiding pops the beacon on and off
//   when strafing past a lamppost, dimming reads as depth. The pin/label dim
//   half as much so they stay legible through a wall.
//
//   PERF VERIFIED (2026-09-06, one active destination at 60 FPS): fully visible
//   300-frame windows averaged 53.4-58.1us per draw; worst observed frame was
//   814us. Rays averaged 12.7-29.7us each and run on the 0.25s cadence. Looking
//   away averaged 5.5us and issued zero rays. BeaconVerboseLog keeps this test
//   repeatable and reports the tested active/visible/ray counts every 300 frames.
//
//   UNVERIFIED pending in-game confirmation: whether kSurfaceEpsilon (24 units)
//   is enough to stop a rooftop-mounted beacon reading its own roof as an
//   occluder, and whether kOccludedAlphaScale reads right at night.
//
//   VISUAL REDESIGN (2026-09-06): upgraded to match LootBeamRenderer aesthetics.
//   MapDraw::DrawFeatheredBeam owns the 5-rail zero-hard-edge mesh and segmented
//   white-hot spine; this file supplies its multi-step top/foot fade and sampled
//   occlusion profile. MapDraw::DrawDiamondBadgeFrame owns the shared frosted
//   frame. The beam terminates at the badge's bottom vertex. Ground contact rings
//   remain intentionally omitted; the 16px foot fade blends into terrain.
// ============================================================================

namespace {
    // Ground probe geometry. Start well above the marker so the ray clears
    // rooftops and cliff overhangs, and cast far enough down to reach a valley
    // floor. Both in Skyrim units.
    constexpr float kGroundProbeUp   = 8192.0f;
    constexpr float kGroundProbeDown = 32768.0f;

    // Downward Havok raycast to find real ground under a marker XY.
    //
    // Cast pattern from SkyParkourNG ParkourUtility::RayCast (verified shipping
    // code). The scale handling is the part that silently goes wrong: ONLY the
    // ray endpoints are multiplied by GetWorldScale() (Havok works in metres);
    // maxDist stays in Skyrim units, so maxDist * hitFraction comes back in
    // Skyrim units directly with no inverse scaling.
    bool RaycastGroundZ(RE::TESObjectCELL* a_cell, float a_x, float a_y,
                        float a_startZ, float a_castDown, float& a_outZ) {
        if (!a_cell) return false;
        auto* world = a_cell->GetbhkWorld();
        if (!world) return false;

        RE::bhkPickData pick;
        const float scale = RE::bhkWorld::GetWorldScale();

        const RE::NiPoint3 from{ a_x, a_y, a_startZ };
        const RE::NiPoint3 to{ a_x, a_y, a_startZ - a_castDown };
        pick.rayInput.from = from * scale;
        pick.rayInput.to   = to * scale;

        // kLOS collides with terrain and statics but ignores actors, so the
        // player standing on the spot cannot become the "ground".
        pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);

        if (!world->PickObject(pick) || !pick.rayOutput.HasHit()) return false;

        a_outZ = a_startZ - a_castDown * pick.rayOutput.hitFraction;
        return true;
    }

    // Is the straight line from the camera to a world point blocked by geometry?
    //
    // Same cast as RaycastGroundZ with different endpoints. Measured at ~8.4us
    // per ray in-game (probe_occlusion_raycast, 5 samples over a 1300-unit path,
    // 0.042ms total), so the sampling below is not a performance concern; the
    // cadence exists to keep Havok calls off the Present thread's hot path, not
    // to save time.
    //
    // Pulled back slightly from the target so a beacon sitting ON a surface
    // (the ground raycast plants it on rooftops) does not register that surface
    // as its own occluder.
    bool IsPointOccluded(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_camPos,
                         const RE::NiPoint3& a_target) {
        if (!a_cell) return false;
        auto* world = a_cell->GetbhkWorld();
        if (!world) return false;

        RE::NiPoint3 dir = a_target - a_camPos;
        const float len = dir.Length();
        if (len < 1e-3f) return false;
        dir /= len;

        constexpr float kSurfaceEpsilon = 24.0f;
        const float castLen = (std::max)(len - kSurfaceEpsilon, 1.0f);

        RE::bhkPickData pick;
        const float scale = RE::bhkWorld::GetWorldScale();
        const RE::NiPoint3 to = a_camPos + dir * castLen;
        pick.rayInput.from = a_camPos * scale;
        pick.rayInput.to   = to * scale;
        pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);

        return world->PickObject(pick) && pick.rayOutput.HasHit();
    }

    // Per-marker ground Z cache. Keyed by marker id; invalidated when the marker
    // moves in XY or the player changes cell (a cached Z from another interior is
    // meaningless). Value is the resolved world Z, NOT the stored map-plane Z.
    struct GroundCacheEntry {
        float x = 0.0f;
        float y = 0.0f;
        float groundZ = 0.0f;
        RE::FormID cellID = 0;
        bool  hit = false;      // false = raycast missed, using the fallback
    };
    std::unordered_map<std::uint32_t, GroundCacheEntry> g_groundCache;

    // Resolve the render anchor Z for a marker. The stored position.z is ALWAYS
    // synthetic (map plane or local-map plane), so it is never used.
    //
    // Order: cached value, then a downward raycast, then GetLandHeight (exterior
    // landscape only), then the player's Z as a last resort. That last fallback
    // is the one that made the beacon follow the player, so it is logged loudly
    // whenever it is reached.
    float ResolveGroundZ(const MapMarker& a_marker, RE::TESObjectCELL* a_cell,
                         const RE::NiPoint3& a_playerPos) {
        const RE::FormID cellID = a_cell ? a_cell->GetFormID() : 0;

        if (auto it = g_groundCache.find(a_marker.id); it != g_groundCache.end()) {
            const auto& e = it->second;
            if (e.cellID == cellID &&
                std::fabs(e.x - a_marker.position.x) < 1.0f &&
                std::fabs(e.y - a_marker.position.y) < 1.0f) {
                return e.groundZ;
            }
        }

        GroundCacheEntry entry;
        entry.x = a_marker.position.x;
        entry.y = a_marker.position.y;
        entry.cellID = cellID;

        auto* tes = RE::TES::GetSingleton();
        float landZ = 0.0f;
        const bool isExterior = a_cell && a_cell->IsExteriorCell();
        const bool hasLand = isExterior && tes && tes->GetLandHeight(a_marker.position, landZ);

        if (hasLand) {
            // Exterior with landscape heightmap: anchor the probe to the true landscape altitude.
            // Start 1500 units above the land so we catch rooftops, bridges, decks, and
            // boulders directly at the marker XY without shooting down from the sky
            // and catching overhead mountain cliffs, archways, or weather boxes.
            constexpr float kLandProbeUp   = 1500.0f;
            constexpr float kLandProbeDown = 2000.0f;
            const float startZ = landZ + kLandProbeUp;
            float z = 0.0f;
            if (RaycastGroundZ(a_cell, a_marker.position.x, a_marker.position.y, startZ, kLandProbeDown, z)) {
                entry.groundZ = z;
                entry.hit = true;
                if (Settings::GetSingleton()->beaconVerboseLog) {
                    SKSE::log::info(
                        "Beacon[{}] groundZ=RAYCAST_LAND {:.1f} (xy {:.1f},{:.1f} landZ {:.1f} startZ {:.1f} cell {:08X})",
                        a_marker.id, z, a_marker.position.x, a_marker.position.y, landZ, startZ, cellID);
                }
            } else {
                // Havok collision missed (e.g. distant cell outside active grid) or pure terrain:
                // landZ is already the exact ground altitude.
                entry.groundZ = landZ;
                entry.hit = true;
                if (Settings::GetSingleton()->beaconVerboseLog) {
                    SKSE::log::info("Beacon[{}] groundZ=LANDHEIGHT {:.1f} (raycast missed/outside grid)", a_marker.id, landZ);
                }
            }
        } else {
            // Interior or exterior edge without landscape heightmap:
            // Cast downward from above player altitude.
            constexpr float kFallbackProbeUp   = 1000.0f;
            constexpr float kFallbackProbeDown = 4000.0f;
            const float startZ = a_playerPos.z + kFallbackProbeUp;
            float z = 0.0f;
            if (RaycastGroundZ(a_cell, a_marker.position.x, a_marker.position.y, startZ, kFallbackProbeDown, z)) {
                entry.groundZ = z;
                entry.hit = true;
                if (Settings::GetSingleton()->beaconVerboseLog) {
                    SKSE::log::info(
                        "Beacon[{}] groundZ=RAYCAST {:.1f} (xy {:.1f},{:.1f} startZ {:.1f} playerZ {:.1f} cell {:08X})",
                        a_marker.id, z, a_marker.position.x, a_marker.position.y, startZ, a_playerPos.z, cellID);
                }
            } else {
                // Last resort: follow player altitude
                entry.groundZ = a_playerPos.z;
                entry.hit = false;
                SKSE::log::warn(
                    "Beacon[{}] groundZ=FALLBACK playerZ {:.1f} - raycast AND GetLandHeight both missed "
                    "(xy {:.1f},{:.1f} cell {:08X}). Beacon will follow player altitude.",
                    a_marker.id, a_playerPos.z, a_marker.position.x, a_marker.position.y, cellID);
            }
        }

        g_groundCache[a_marker.id] = entry;
        return entry.groundZ;
    }

    // ProjectWorld moved to MapDrawHelpers.h (MapDraw::ProjectWorldPoint) when
    // loot beams became a second caller. Thin alias so the call sites below and
    // the decision-5 in-front contract read the same as before.
    inline bool ProjectWorld(RE::NiCamera* a_cam, const RE::NiPoint3& a_world,
                             ImVec2& a_outScreen, bool& a_outInFront) {
        return MapDraw::ProjectWorldPoint(a_cam, a_world, a_outScreen, a_outInFront);
    }

    // Per-sample occlusion along the pillar, eased.
    //
    // One ray at the base is not enough: a 2000-unit pillar can be buried at the
    // bottom and clear above a roofline, and a single sample cannot express that.
    // Sampling up the beam lets the base dim behind a building while the top
    // stays lit, which is the whole reason a tall beacon reads as "behind that".
    //
    // Blocked samples DIM rather than hide. Hiding pops the beacon on and off as
    // the player strafes past a lamppost; dimming reads as depth.
    constexpr int   kOcclusionSamples   = 5;
    // Adaptive-height ramp reaches full height at this multiple of the proximity
    // fade distance. 8x350 = 2800 units (~40m), so the beam is at full height for
    // any genuinely distant target and only shrinks once the player is closing in.
    constexpr float kHeightRampFarMult  = 8.0f;
    constexpr float kOccludedAlphaScale = 0.22f;   // how dark a blocked sample goes
    // Distance at which an occluded PIN stops getting its readability relief and
    // dims as hard as the beam. Up close, a destination behind a wall is around
    // the corner and the label is the useful part; at 1300m it is just painted on
    // a building you are nowhere near. Ramps between kHeightRampFarMult*fadeNear
    // (~2800 units) and this.
    constexpr float kPinOccFullDimUnits = 11000.0f;
    constexpr float kOcclusionFadeSecs  = 0.18f;   // ease so nothing snaps
    constexpr float kOcclusionInterval  = 0.25f;   // re-cast cadence, seconds

    struct OcclusionState {
        // 0 = fully visible, 1 = fully blocked. One eased value per sample point,
        // index 0 at the base running to kOcclusionSamples-1 at the pillar top.
        float   blocked[kOcclusionSamples]{};
        // Raw cast result the eased value chases. Held between casts because the
        // cadence is slower than the frame rate.
        float   occTarget[kOcclusionSamples]{};
        double  lastCast = 0.0;
        bool    seeded   = false;
    };
    std::unordered_map<std::uint32_t, OcclusionState> g_occlusion;

    struct BeaconPerfFrame {
        double      rayTimeUs = 0.0;
        std::size_t rayCount = 0;
        std::size_t visibleCount = 0;
        std::size_t edgeIndicatorCount = 0;
    };

    // Re-cast on the cadence, then ease every sample toward its target. Returns
    // the state so the draw pass can scale per-vertex alpha by it.
    const OcclusionState& UpdateOcclusion(std::uint32_t a_markerID,
                                          RE::TESObjectCELL* a_cell,
                                          const RE::NiPoint3& a_camPos,
                                          const RE::NiPoint3& a_base,
                                          const RE::NiPoint3& a_top,
                                          double a_now, float a_dt,
                                          BeaconPerfFrame* a_perf) {
        auto& st = g_occlusion[a_markerID];

        if (!st.seeded || (a_now - st.lastCast) >= kOcclusionInterval) {
            st.lastCast = a_now;
            std::chrono::steady_clock::time_point rayStart{};
            if (a_perf) {
                rayStart = std::chrono::steady_clock::now();
            }
            for (int i = 0; i < kOcclusionSamples; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(kOcclusionSamples - 1);
                const RE::NiPoint3 p{
                    a_base.x + (a_top.x - a_base.x) * t,
                    a_base.y + (a_top.y - a_base.y) * t,
                    a_base.z + (a_top.z - a_base.z) * t
                };
                const float target = IsPointOccluded(a_cell, a_camPos, p) ? 1.0f : 0.0f;
                st.occTarget[i] = target;
                // Seed directly on the first cast: easing up from 0 would flash
                // the beacon fully bright for ~0.18s before it settled dim.
                if (!st.seeded) st.blocked[i] = target;
            }
            if (a_perf) {
                a_perf->rayTimeUs += std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - rayStart).count();
                a_perf->rayCount += kOcclusionSamples;
            }
            st.seeded = true;
        }

        const float rate = std::clamp(a_dt / kOcclusionFadeSecs, 0.0f, 1.0f);
        for (int i = 0; i < kOcclusionSamples; ++i) {
            const float step = std::clamp(st.occTarget[i] - st.blocked[i], -rate, rate);
            st.blocked[i] = std::clamp(st.blocked[i] + step, 0.0f, 1.0f);
        }
        return st;
    }

    // Occlusion at an arbitrary point along the beam, linearly interpolated
    // between the two nearest samples so the dim gradient is smooth instead of
    // stepping at sample boundaries.
    float SampleOcclusion(const OcclusionState& a_st, float a_t) {
        const float scaled = std::clamp(a_t, 0.0f, 1.0f) * (kOcclusionSamples - 1);
        const int   i0 = static_cast<int>(scaled);
        const int   i1 = (std::min)(i0 + 1, kOcclusionSamples - 1);
        const float frac = scaled - static_cast<float>(i0);
        return a_st.blocked[i0] + (a_st.blocked[i1] - a_st.blocked[i0]) * frac;
    }

    // Does the beam segment touch the viewport rect at all?
    //
    // Visibility CANNOT be a point test on either endpoint. Gating on the top
    // hid tall pillars until the player looked up; gating on the base hid them
    // when the player looked up too far (the base leaves the bottom edge while
    // the beam still crosses the whole screen). Both are the same mistake: the
    // beacon is a SEGMENT, so the test is segment-vs-rect.
    //
    // SegmentInRect moved to MapDrawHelpers.h (MapDraw::SegmentInRect) when loot
    // beams became a second caller. Same Liang-Barsky clip, unchanged.

    // Clamp an anchor to the padded viewport edge and report the direction the
    // arrow should point. Behind-camera points mirror through the centre, so
    // the offset is negated BEFORE clamping; skipping this is what puts the
    // arrow on the opposite edge when the target is behind the player.
    ImVec2 ClampToEdge(const ImVec2& a_pt, bool a_inFront, float a_pad, float& a_outAngle) {
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const ImVec2 c(disp.x * 0.5f, disp.y * 0.5f);
        float dx = a_pt.x - c.x;
        float dy = a_pt.y - c.y;
        if (!a_inFront) {
            dx = -dx;
            dy = -dy;
        }
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-4f) {
            a_outAngle = 0.0f;
            return c;
        }
        a_outAngle = std::atan2(dy, dx);
        // Scale the centre-to-point ray out to whichever axis hits the padded rect first.
        const float maxX = c.x - a_pad;
        const float maxY = c.y - a_pad;
        const float scale = (std::min)(maxX / std::fabs(dx), maxY / std::fabs(dy));
        return ImVec2(c.x + dx * scale, c.y + dy * scale);
    }

    void DrawEdgeIndicator(ImDrawList* dl, const ImVec2& pt, float angle, float size, ImU32 col) {
        const float half = size * 0.5f;
        const ImVec2 p0(pt.x + std::cos(angle) * size, pt.y + std::sin(angle) * size);
        const ImVec2 p1(pt.x + std::cos(angle + 2.35f) * half, pt.y + std::sin(angle + 2.35f) * half);
        const ImVec2 p2(pt.x + std::cos(angle - 2.35f) * half, pt.y + std::sin(angle - 2.35f) * half);

        const auto a = static_cast<std::uint8_t>((col >> IM_COL32_A_SHIFT) & 0xFF);
        dl->AddTriangleFilled(p0, p1, p2, col);
        dl->AddTriangle(p0, p1, p2, IM_COL32(0, 0, 0, static_cast<int>(a * 0.8f)), 1.2f);
    }
}

void WorldBeaconRenderer::Draw() {
    auto* settings = Settings::GetSingleton();
    if (!settings || (!settings->navBeaconVisible && !settings->navBeaconClearOnArrival)) {
        _beaconFade = 0.0f;
        return;
    }

    // Beacons are painted into the live gameplay view, so ANY vanilla menu must
    // hide them: Tween (Tab), Dialogue, Fader (the frame right after a load
    // completes), Journal, containers, crafting, etc. The old gate only caught
    // LoadingMenu + MapMenu, so 10-20 beacons stayed lit behind the Tab menu.
    // MenuUtils::ShouldBlockOverlay is the shared list, already used by
    // MinimapRenderer, and covers SKSEMenuFramework + Photo Mode too.
    // Party Sheet's panels are ImGui, not registered IMenus, so ShouldBlockOverlay
    // cannot see them. Its state API can: yields to a hidden HUD or a fullscreen
    // panel, and no-ops when Party Sheet isn't installed.
    auto* ui = RE::UI::GetSingleton();
    if (!ui || MenuUtils::ShouldBlockOverlay() || PartySheetState::ShouldHideOverlay()) {
        // Snap, don't fade: fading out over a menu that opened instantly would
        // leave the beacon visible for the first frames of the menu.
        _beaconFade = 0.0f;
        return;
    }

    // Beast form: no map, no fast travel, no inventory — not a navigation
    // state. Eases out via the normal fade rather than cutting, like the
    // combat gate: the transformation is a visible animation, not an instant
    // menu. Vampire Lord is exempt by design (include/BeastForm.h).
    if (Settings::GetSingleton()->hideInBeastForm && BeastForm::IsBeastForm()) {
        _beaconFade = 0.0f;
        return;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;


    auto* parentCell = player->GetParentCell();
    if (!parentCell) return;

    const RE::NiPoint3 playerPos = player->GetPosition();
    const bool isExterior = parentCell->IsExteriorCell();
    auto* currentWorld = player->GetWorldspace();
    const RE::FormID currentWorldID = isExterior && currentWorld ? currentWorld->GetFormID() : 0;
    const RE::FormID currentCellID = parentCell->GetFormID();

    // Find destination markers (custom or vanilla destination pseudo-marker).
    std::vector<MapMarker> activeDestinations;
    const auto allMarkers = MarkerManager::GetSingleton()->GetMarkers();
    for (const auto& m : allMarkers) {
        if (!m.isDestination) continue;

        // Space check
        bool spaceMatches = false;
        if (isExterior) {
            if (m.worldSpaceID != 0 && currentWorld && CompassHook::WorldMatches(currentWorld, m.worldSpaceID)) {
                spaceMatches = true;
            }
        } else {
            // Interior: marker in same cell, or marker has worldSpaceID==0 with matching cellID
            if (m.worldSpaceID == 0 && m.cellID == currentCellID) {
                spaceMatches = true;
            }
        }

        if (spaceMatches) {
            activeDestinations.push_back(m);
        }
    }

    if (settings->navBeaconClearOnArrival) {
        bool arrived = false;
        for (const auto& m : activeDestinations) {
            const float dx = m.position.x - playerPos.x;
            const float dy = m.position.y - playerPos.y;
            const float distUnits = std::sqrt(dx * dx + dy * dy);
            if (distUnits > settings->navBeaconFadeNearUnits) continue;

            const bool temporary =
                m.id != kVanillaDestId &&
                (m.origin == MarkerOrigin::LookAndPin ||
                 m.origin == MarkerOrigin::InspectQuickMark);

            auto* manager = MarkerManager::GetSingleton();
            const bool removed = temporary && manager->RemoveMarker(m.id);
            if (!removed) {
                manager->ClearDestination();
            }
            g_groundCache.erase(m.id);
            g_occlusion.erase(m.id);

            if (settings->beaconVerboseLog) {
                SKSE::log::info(
                    "Beacon ARRIVAL: id={} origin={} action={} dist={:.1f}m radius={:.1f}m",
                    m.id, static_cast<std::uint32_t>(m.origin),
                    removed ? "REMOVE_TEMPORARY_MARKER" : "CLEAR_DESTINATION",
                    distUnits / Settings::kUnitsPerMeter,
                    settings->navBeaconFadeNearUnits / Settings::kUnitsPerMeter);
            }

            arrived = true;
            break;
        }
        if (arrived) activeDestinations.clear();
    }

    // Arrival is destination state, not a rendering side effect. Keep processing
    // it when Show Beacons is off, then stop before camera/projection work.
    if (!settings->navBeaconVisible) {
        _beaconFade = 0.0f;
        return;
    }

    auto* cam = RE::Main::WorldRootCamera();
    if (!cam) return;

    const float dt = ImGui::GetIO().DeltaTime;
    constexpr float kFadeDuration = 0.25f;
    const float rate = std::clamp((kFadeDuration > 1e-4f) ? (dt / kFadeDuration) : 1.0f, 0.0f, 1.0f);
    const float targetFade = activeDestinations.empty() ? 0.0f : 1.0f;
    _beaconFade = std::clamp(_beaconFade + std::clamp(targetFade - _beaconFade, -rate, rate), 0.0f, 1.0f);

    // Diagnostic for "beacon never comes back after toggling Show Beacons off
    // and on". Logs only when the destination count or the bail reason CHANGES,
    // never per frame. Remove once the cause is confirmed in-game.
    if (settings->beaconVerboseLog) {
        static std::size_t s_lastCount = SIZE_MAX;
        static int s_lastStage = -1;
        const int stage = activeDestinations.empty() ? 0 : (_beaconFade <= 0.001f ? 1 : 2);
        if (s_lastCount != activeDestinations.size() || s_lastStage != stage) {
            s_lastCount = activeDestinations.size();
            s_lastStage = stage;
            SKSE::log::info(
                "Beacon gate: destinations={} fade={:.3f} dt={:.4f} stage={} ({}) "
                "markersTotal={} exterior={} worldID={:08X} cellID={:08X}",
                activeDestinations.size(), _beaconFade, dt, stage,
                stage == 0 ? "NO DESTINATION IN THIS SPACE" : (stage == 1 ? "fade still ~0" : "drawing"),
                allMarkers.size(), isExterior, currentWorldID, currentCellID);
        }
    }

    if (_beaconFade <= 0.001f || activeDestinations.empty()) {
        return;
    }

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    const bool perfLog = settings->beaconVerboseLog;
    std::chrono::steady_clock::time_point drawStart{};
    if (perfLog) {
        drawStart = std::chrono::steady_clock::now();
    }
    BeaconPerfFrame perfFrame;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const ImVec2 screenCenter(disp.x * 0.5f, disp.y * 0.5f);
    const float timeSec = std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const double nowSec = static_cast<double>(timeSec);
    constexpr float kPad = 45.0f;

    // Occlusion rays start at the camera, not the player: in third person the
    // two are far enough apart that player-origin rays would report the wrong
    // thing blocking the view. world.translate is the camera node's world
    // position, the same value dump_camera_state reports as `pos`.
    const RE::NiPoint3 camPos = cam->world.translate;

    for (const auto& m : activeDestinations) {
        // Horizontal distance only (Decision 3)
        const float dx = m.position.x - playerPos.x;
        const float dy = m.position.y - playerPos.y;
        const float distUnits = std::sqrt(dx * dx + dy * dy);
        const int distMeters = static_cast<int>(distUnits / Settings::kUnitsPerMeter + 0.5f);

        // Proximity fade-out below threshold (Decision 9)
        float proximityAlpha = 1.0f;
        if (distUnits < settings->navBeaconFadeNearUnits) {
            proximityAlpha = (std::max)(0.0f, distUnits / settings->navBeaconFadeNearUnits);
        }

        const float baseAlpha = _beaconFade * proximityAlpha;
        if (baseAlpha <= 0.01f) continue;

        // Ground Z substitution (Decision 2). Cached per marker: the marker is
        // static, so re-casting every frame buys nothing and costs a Havok query
        // per beacon per frame.
        const float baseZ = ResolveGroundZ(m, parentCell, playerPos);
        // Adaptive height: navBeaconPillarHeight is the FAR height, scaled down
        // toward navBeaconNearScale as the player closes in. A fixed world height
        // reads fine at range but perspective blows it up close, where the beam
        // ran off the top of the screen at ~17m. Ramps over the same distance
        // band the proximity fade uses, so the beacon shrinks and fades together
        // instead of shrinking on one schedule and fading on another.
        // NOT a ground-ring/arrival design: destinations are usually doors in
        // buildings, so anything drawn on the ground plane lands on a roof or
        // inside geometry.
        const float rampFar = (std::max)(settings->navBeaconFadeNearUnits * kHeightRampFarMult, 1.0f);
        const float rampT   = std::clamp(distUnits / rampFar, 0.0f, 1.0f);
        const float nearScale = std::clamp(settings->navBeaconNearScale, 0.05f, 1.0f);
        const float heightScale = nearScale + (1.0f - nearScale) * rampT;

        const RE::NiPoint3 basePos{ m.position.x, m.position.y, baseZ };
        const RE::NiPoint3 topPos{ m.position.x, m.position.y,
                                   baseZ + settings->navBeaconPillarHeight * heightScale };

        // ProjectWorld only writes its out-params when it returns true, so a
        // failed projection must never be treated as a usable screen position.
        // Folding topProj into topInFront keeps every later reader honest with
        // a single flag instead of two that must be checked together.
        ImVec2 baseScr, topScr;
        bool baseInFront = false, topInFront = false;
        const bool baseProj = ProjectWorld(cam, basePos, baseScr, baseInFront);
        const bool topProj  = ProjectWorld(cam, topPos, topScr, topInFront);
        baseInFront = baseInFront && baseProj;
        topInFront  = topInFront && topProj;

        if (!topProj && !baseProj) continue;

        // Screen-center aim fade (Decision 8): fade down if aiming directly at the beacon
        float aimAlpha = 1.0f;
        if (topInFront) {
            const float toCenterX = topScr.x - screenCenter.x;
            const float toCenterY = topScr.y - screenCenter.y;
            const float distToCenter = std::sqrt(toCenterX * toCenterX + toCenterY * toCenterY);
            constexpr float kAimFadeRadius = 140.0f;
            if (distToCenter < kAimFadeRadius) {
                aimAlpha = std::clamp(distToCenter / kAimFadeRadius, 0.15f, 1.0f);
            }
        }

        const float finalAlpha = baseAlpha * aimAlpha;
        const auto alphaByte = static_cast<std::uint8_t>(std::clamp(finalAlpha * 255.0f, 0.0f, 255.0f));
        if (alphaByte == 0) continue;

        // Visibility is a SEGMENT test over the whole beam, not a point test on
        // either end. Both point gates were tried and both failed: on the top,
        // a tall pillar hid until the player looked up; on the base, it hid when
        // the player looked up too far, with the beam still crossing the entire
        // screen (confirmed in-game at PillarHeight 2000).
        //
        // Both endpoints must be in front of the camera for the screen-space
        // segment to mean anything. When either is behind, the projection
        // mirrors and the "segment" is not the beam the player sees, so the
        // beacon is treated as off-screen and left to the edge indicator.
        float segT0 = 0.0f, segT1 = 1.0f;
        const bool beamUsable = baseProj && topProj && baseInFront && topInFront;
        const bool isOnScreen = beamUsable &&
                                MapDraw::SegmentInRect(baseScr, topScr,
                                                       kPad, kPad, disp.x - kPad, disp.y - kPad,
                                                       segT0, segT1);

        // Pin sits at the highest ON-SCREEN point of the beam. segT0/segT1 are
        // the clipped range, so this parks the pin at the top edge when the
        // pillar overshoots and at the real tip when it does not. Interpolating
        // along the beam keeps the pin on the line instead of drifting sideways.
        float tPin = 1.0f;
        ImVec2 pinScr = topScr;
        if (isOnScreen) {
            // The beam runs base -> top, so the smaller Y (higher on screen) is
            // whichever end of the clipped range sits nearer the top.
            tPin = (topScr.y <= baseScr.y) ? segT1 : segT0;
            pinScr = ImVec2(baseScr.x + (topScr.x - baseScr.x) * tPin,
                            baseScr.y + (topScr.y - baseScr.y) * tPin);
        }

        // Already the clean display name: the vanilla destination is a
        // synthesized entry whose note carries no internal prefix.
        std::string label = ResolveMarkerName(m);

        if (isOnScreen) {
            if (perfLog) {
                ++perfFrame.visibleCount;
            }

            // Edge indicators do not use depth, so only visible pillars spend
            // Havok rays or advance their occlusion easing state.
            const OcclusionState& occState = UpdateOcclusion(
                m.id, parentCell, camPos, basePos, topPos, nowSec, dt,
                perfLog ? &perfFrame : nullptr);

            // Destination palette (Cyan / Sky-Blue luminous hierarchy)
            constexpr ImU32 coreCol = IM_COL32(215, 240, 255, 255);
            constexpr ImU32 glowCol = IM_COL32(110, 200, 255, 255);
            constexpr ImU32 haloCol = IM_COL32( 50, 140, 255, 255);

            const float badgeR = static_cast<float>(settings->navBeaconPinSize) * 0.75f;
            const ImVec2 iconAt = pinScr;

            // Draw from the lower clipped endpoint to the diamond's bottom point.
            // Ending at pinScr would run the filament through the icon.
            const float tBeamBase = (topScr.y <= baseScr.y) ? segT0 : segT1;
            const ImVec2 beamBase(
                baseScr.x + (topScr.x - baseScr.x) * tBeamBase,
                baseScr.y + (topScr.y - baseScr.y) * tBeamBase);
            const ImVec2 beamTip(iconAt.x, iconAt.y + badgeR);

            constexpr float kBeamBaseW = 18.0f;
            constexpr float kBeamTopW = 6.0f;
            const float beamBaseW = kBeamBaseW + (kBeamTopW - kBeamBaseW) * tBeamBase;
            const float beamTipW = kBeamBaseW + (kBeamTopW - kBeamBaseW) * tPin;

            const float fullBeamLen = std::sqrt(
                (topScr.x - baseScr.x) * (topScr.x - baseScr.x) +
                (topScr.y - baseScr.y) * (topScr.y - baseScr.y));
            constexpr float kBottomFadePx = 16.0f;
            const bool baseIsGround = segT0 <= 0.001f;
            const float fadeSpanT = (baseIsGround && fullBeamLen > 1.0f)
                ? std::clamp(kBottomFadePx / fullBeamLen, 0.0f, 0.45f)
                : 0.0f;

            // Tall destination pillars need more rows than the short loot beams.
            // The profile callback keeps world-only fading and raycast policy out
            // of the shared five-rail geometry primitive.
            constexpr int kPillarSteps = 24;
            MapDraw::DrawFeatheredBeam(
                dl, beamBase, beamTip, kPillarSteps,
                beamBaseW, beamTipW,
                coreCol, glowCol, haloCol,
                finalAlpha,
                [&](float f) {
                    const float t = tBeamBase + (tPin - tBeamBase) * f;
                    const float fadeTop = (1.0f - t) * (1.0f - t) * (1.0f - t);
                    float foot = 1.0f;
                    if (fadeSpanT > 1e-4f) {
                        const float k = std::clamp((t - segT0) / fadeSpanT, 0.0f, 1.0f);
                        foot = k * k;
                    }
                    const float occ = SampleOcclusion(occState, t);
                    const float occMul = 1.0f - occ * (1.0f - kOccludedAlphaScale);
                    return fadeTop * foot * occMul;
                });

            // Pin/label occlusion, sampled where the pin actually sits on the
            // beam (tPin), not at the pillar top: when the top is clamped
            // off-screen the pin rides a lower point with different occlusion.
            const float pinOcc  = SampleOcclusion(occState, tPin);

            const float pinReliefNear = kHeightRampFarMult * settings->navBeaconFadeNearUnits;
            const float pinReliefT = std::clamp(
                (distUnits - pinReliefNear) / (std::max)(1.0f, kPinOccFullDimUnits - pinReliefNear),
                0.0f, 1.0f);
            const float pinRelief = 0.5f * (1.0f - pinReliefT);
            const float pinMul  = 1.0f - pinOcc * (1.0f - kOccludedAlphaScale) * pinRelief;
            const float pinAlpha  = finalAlpha * pinMul;
            const auto  pinAlphaB = static_cast<std::uint8_t>(std::clamp(pinAlpha * 255.0f, 0.0f, 255.0f));

            MapDraw::DrawDiamondBadgeFrame(
                dl, iconAt, badgeR, coreCol, glowCol, haloCol, pinAlpha,
                /*glowPadding=*/3.0f, /*innerAlpha=*/0.50f,
                /*innerMinRadius=*/3.0f);

            // Destination icon inside badge
            auto* pinSrv = TextureManager::GetIconByName("destination");
            if (pinSrv) {
                const float iconHalf = badgeR * 0.65f;
                MapDraw::DrawIconFx(dl, pinSrv,
                                    ImVec2(iconAt.x - iconHalf, iconAt.y - iconHalf),
                                    ImVec2(iconAt.x + iconHalf, iconAt.y + iconHalf),
                                    MapDraw::WithAlpha(IM_COL32_WHITE, pinAlpha));
            }

            // Distance & label text under the diamond badge
            const std::string distStr = std::to_string(distMeters) + "m";
            const ImVec2 distSz = ImGui::CalcTextSize(distStr.c_str());
            const float textY = iconAt.y + badgeR + 5.0f;

            if (!label.empty()) {
                const ImVec2 labelSz = ImGui::CalcTextSize(label.c_str());
                const float labelX = iconAt.x - labelSz.x * 0.5f;
                dl->AddText(ImVec2(labelX + 1.0f, textY + 1.0f), IM_COL32(0, 0, 0, pinAlphaB), label.c_str());
                dl->AddText(ImVec2(labelX, textY), IM_COL32(245, 235, 210, pinAlphaB), label.c_str());

                const float distY = textY + labelSz.y + 1.0f;
                const float distX = iconAt.x - distSz.x * 0.5f;
                dl->AddText(ImVec2(distX + 1.0f, distY + 1.0f), IM_COL32(0, 0, 0, pinAlphaB), distStr.c_str());
                dl->AddText(ImVec2(distX, distY), IM_COL32(110, 210, 255, pinAlphaB), distStr.c_str());
            } else {
                const float distX = iconAt.x - distSz.x * 0.5f;
                dl->AddText(ImVec2(distX + 1.0f, textY + 1.0f), IM_COL32(0, 0, 0, pinAlphaB), distStr.c_str());
                dl->AddText(ImVec2(distX, textY), IM_COL32(110, 210, 255, pinAlphaB), distStr.c_str());
            }
        } else if (settings->navBeaconEdgeIndicator) {
            if (perfLog) {
                ++perfFrame.edgeIndicatorCount;
            }

            // Off-screen or behind camera: clamp to viewport edge (Decision 5).
            //
            // Default OFF (2026-08-15). With beacons scoped to a single active
            // destination, an off-screen arrow duplicates what the compass and
            // the Glance radar already show, and it was the noisiest of the
            // three. The pillar is visible from a distance on its own.
            // Kept behind a setting rather than deleted: a marked ITEM or other
            // small target has no tall pillar to spot, and there the arrow is
            // the only cue. Turn it on for that case.
            float angle = 0.0f;
            const ImVec2 clampedPos = ClampToEdge(topScr, topInFront, kPad, angle);

            // Draw directional edge arrow
            DrawEdgeIndicator(dl, clampedPos, angle, 16.0f, IM_COL32(110, 210, 255, alphaByte));

            // Pin glyph slightly offset inward along the ray
            const ImVec2 iconPos(clampedPos.x - std::cos(angle) * 18.0f,
                                 clampedPos.y - std::sin(angle) * 18.0f);

            auto* pinSrv = TextureManager::GetIconByName("destination");
            const float pinScale = (static_cast<float>(settings->navBeaconPinSize) * 0.85f) / (2.0f * MapDraw::kPinRadius);
            MapDraw::DrawPin(dl, iconPos.x, iconPos.y, 0.0f, 0, pinSrv, pinScale,
                             Settings::GetSingleton()->PinTintCol(), finalAlpha);

            // Distance text adjacent to clamped beacon
            const std::string distStr = std::to_string(distMeters) + "m";
            const ImVec2 distSz = ImGui::CalcTextSize(distStr.c_str());
            const ImVec2 textPos(iconPos.x - distSz.x * 0.5f, iconPos.y + 12.0f);
            dl->AddText(ImVec2(textPos.x + 1.0f, textPos.y + 1.0f), IM_COL32(0, 0, 0, alphaByte), distStr.c_str());
            dl->AddText(ImVec2(textPos.x, textPos.y), IM_COL32(110, 210, 255, alphaByte), distStr.c_str());
        }
    }

    if (perfLog) {
        constexpr std::size_t kPerfReportFrames = 300;
        static std::size_t s_frameCount = 0;
        static std::size_t s_maxActive = 0;
        static std::size_t s_visibleCount = 0;
        static std::size_t s_edgeIndicatorCount = 0;
        static std::size_t s_rayCount = 0;
        static double s_drawTimeUs = 0.0;
        static double s_worstDrawTimeUs = 0.0;
        static double s_rayTimeUs = 0.0;

        const double drawTimeUs = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - drawStart).count();
        ++s_frameCount;
        s_maxActive = (std::max)(s_maxActive, activeDestinations.size());
        s_visibleCount += perfFrame.visibleCount;
        s_edgeIndicatorCount += perfFrame.edgeIndicatorCount;
        s_rayCount += perfFrame.rayCount;
        s_drawTimeUs += drawTimeUs;
        s_worstDrawTimeUs = (std::max)(s_worstDrawTimeUs, drawTimeUs);
        s_rayTimeUs += perfFrame.rayTimeUs;

        if (s_frameCount >= kPerfReportFrames) {
            const double meanDrawUs = s_drawTimeUs / static_cast<double>(s_frameCount);
            const double meanRayUs = s_rayCount > 0
                ? s_rayTimeUs / static_cast<double>(s_rayCount)
                : 0.0;
            SKSE::log::info(
                "WorldBeacons::Draw PERF: mean={:.1f}us worst={:.1f}us over {} active frames "
                "(maxActive={} visible={} edgeIndicators={} rays={} rayMean={:.1f}us "
                "meshRows=24 meshVertices=120 meshIndices=552)",
                meanDrawUs, s_worstDrawTimeUs, s_frameCount,
                s_maxActive, s_visibleCount, s_edgeIndicatorCount,
                s_rayCount, meanRayUs);

            s_frameCount = 0;
            s_maxActive = 0;
            s_visibleCount = 0;
            s_edgeIndicatorCount = 0;
            s_rayCount = 0;
            s_drawTimeUs = 0.0;
            s_worstDrawTimeUs = 0.0;
            s_rayTimeUs = 0.0;
        }
    }
}
