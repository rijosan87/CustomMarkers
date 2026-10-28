#include "PCH.h"
#include "LookPinRenderer.h"

#include "MarkerManager.h"
#include "MenuUtils.h"
#include "BeastForm.h"
#include "PartySheetState.h"
#include "Settings.h"
#include "SettingsMenu.h"
#include "InputHandler.h"
#include "MapDrawHelpers.h"
#include "MapTheme.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <string>
#include <string_view>

// ============================================================================
// LookPinRenderer — Look & Pin (in-world crosshair tagging)
// ----------------------------------------------------------------------------
// STATUS:
//   Working (pending first in-game confirm): Ctrl+N outdoors toggles a mode that
//   fires a camera-forward Havok ray each frame, draws a reticle at screen centre
//   (green = target found, red = nothing) plus a ghost pin at the hit point, and
//   on a second press stores that world position as a custom marker named
//   "Pinned Location N" using the assigned ID returned by AddMarker. Exterior only.
//
//   FORWARD VECTOR: built from the PLAYER'S OWN ANGLES (GetAngleX pitch,
//   GetAngleZ yaw), not from any camera rotation matrix. Two matrix attempts both
//   failed in-game and the log is worth keeping:
//
//     1. Main::WorldRootCamera()->world.rotate column 1. Pointed at 356 deg while
//        body yaw was 38 deg and did NOT move as the player turned. Every ray hit
//        within 2 units of the camera, planting the pin at the player's feet.
//     2. PlayerCamera::cameraRoot->world.rotate column 1. Better (rays finally
//        travelled ~9000 units) but still 50 deg off body yaw, AND the vector was
//        byte-identical across 13 consecutive frames while the view was moving.
//        A stale rotation aims at a fixed nearby surface no matter where the
//        player looks, which is what produced the "always 0 m" report.
//
//     3. Player angles for BOTH yaw and pitch. Yaw was finally right (fwdYaw
//        tracked bodyYaw every frame), but GetAngleX is the BODY pitch, which in
//        first person does not follow the view: it read +29 to +32 deg while the
//        player looked level at a distant shrine, aiming the ray into the ground
//        ~2 units ahead. That is the "places right next to me / always 0 m" report.
//
//   Final split, each half from the source that demonstrably tracks the view:
//     YAW   -> PlayerCharacter::GetAngleZ()
//     PITCH -> cameraRoot->world.rotate.entry[2][1], which is -sin(pitch)
//   The camera matrix is unusable as a WHOLE (stale, and 50 deg off in yaw) but
//   that single entry is live, confirmed against dump_camera_state: row2 z reads
//   0.997 when the player looks level.
//
//   ORIGIN: player position + eye height. cameraRoot->world.translate FREEZES --
//   it held one value across a 2-second sequence while the player moved and
//   turned. A stale origin with a live direction casts from where the player used
//   to be.
//
//   Main::WorldRootCamera() remains correct for PROJECTION, which is a different
//   question from where the player is looking. The trap throughout: every wrong
//   source compiled, ran, and returned a plausible unit vector, and only in-game
//   hit positions exposed them.
//
//   RAY SCALE: only the ENDPOINTS are multiplied by GetWorldScale() (Havok works
//   in metres). kRayLength stays in Skyrim units, so kRayLength * hitFraction
//   comes back in Skyrim units with no inverse scaling. Same handling as
//   WorldBeaconRenderer::RaycastGroundZ; this is the easiest thing here to get
//   silently wrong.
//
//   DISTANT TARGETS: kLOS collision only exists on loaded cells, so a mountain
//   30000 units out has no collision and PickObject misses. That distant target
//   is the whole point of the feature, so a Havok miss falls back to marching the
//   ray against the LAND heightfield (MarchLandHeight), which is record data and
//   works on unloaded terrain. Only the sky-aim case ends with no target.
//
//   RANGE: Havok reaches kRayLength (40000). The land march reaches kMaxTagRange
//   (150000, ~2100 m) with steps that coarsen past the Havok range, then binary-
//   refines the crossing so a coarse far step cannot overshoot a slope. The first
//   cut capped the march at kRayLength too, which is why a shrine on the horizon
//   was untaggable: the ray simply stopped short with nothing to report.
//
//   SELF-HITS: kLOS collides with the PLAYER's own body and equipped gear, and the
//   ray starts at the player, so the first thing it meets is the player at ~2
//   units. That produced a GREEN reticle reading "0 m" that planted the marker on
//   top of the player. Two guards, both needed: the origin steps forward
//   kRayStartOffset before casting, and any hit closer than kSelfHitReject is
//   discarded outright. Critically there is NO fallback to a self-hit when the
//   terrain march also fails -- the honest answer there is "no target".
//
//   KNOWN ISSUE, NOT YET FIXED (2026-08-16) -- HAVOK/LAND TARGET JITTER:
//   The two sources are mutually exclusive by construction: the march only runs
//   when Havok misses or hits inside kNearHitReject, so a given frame reports
//   land=n OR havok=n, almost never both. Holding still on the same view, the
//   winner flips frame to frame and the two answers can sit ~3000 units apart.
//   From the log, consecutive frames at near-identical aim:
//       chose=LAND   landD=13240  target=(158142.6,38650.6,-11703.2)
//       chose=HAVOK  havokD=9776  target=(159502.6,36089.5, -9808.8)
//   Also visible as an occasional chose=NONE frame mid-hold (reticle blinks red).
//
//   Impact is mild -- both answers are roughly where the player is looking, so
//   placement FEELS right -- but at long range the pin can land thousands of units
//   from where the reticle sat, and the choice is effectively whichever source won
//   on the press frame.
//
//   Preferred fix when this is picked up: run BOTH every frame and take the NEARER
//   hit. "The closest solid thing along the ray" is the honest definition of what
//   the player can see, and it removes the flicker as a side effect. Cost is one
//   extra terrain march per frame, which is a heightfield lookup walk, not a
//   physics query. Alternative considered: prefer LAND beyond some distance and
//   trust Havok only up close -- simpler, but it ignores real geometry (rooftops,
//   bridges, walls) at range, which is exactly what a player aiming at a building
//   wants to hit.
//
//   TARGET FLICKER: at the edge of loaded collision the SAME direction alternates
//   hit/miss frame to frame (visible in the log as HAVOK HIT / NO TARGET pairs
//   with near-identical fwd). Two consequences, both fixed:
//     - _targetPos is cleared every frame alongside _hasTarget. Leaving it stale
//       let a commit on a miss frame place a marker at the player's feet using the
//       previous frame's hit. That was a reported bug, not a theoretical one.
//     - A last-good-hit is held for kTargetHoldSec and reused ONLY if the aim has
//       barely moved (dot > 0.9995), so the reticle stops strobing without the
//       target surviving a genuine look-away.
//
//   STORED Z IS REAL, unlike every other marker path in this plugin (world-map
//   placement stores the map plane ~180000, local-map placement stores its own
//   plane Z). Nothing needs to special-case that: the world map lifts any Z below
//   100000 to kMapPlaneZ itself (MapMarkerRenderer.cpp), and WorldBeaconRenderer
//   re-raycasts ground Z regardless of what is stored. A real Z is strictly better
//   input to both.
//
//   UNVERIFIED pending in-game confirmation: whether hitFraction decodes to the
//   right distance in the OUTWARD direction. The live check that confirmed the
//   forward vector cast source->player, i.e. the reverse of this ray, so the
//   geometry is proven but the outward decode is not. The placement log line
//   stays in until a placed pin is confirmed to land where the player aimed.
// ============================================================================

namespace {
    // Ray length is NOT multiplied by GetWorldScale(): only the endpoints are, so
    // kRayLength * hitFraction returns Skyrim units directly. 40000 is roughly the
    // loaded-grid diagonal at uGridsToLoad=5; past that the land march takes over.
    constexpr float kRayLength     = 40000.0f;
    constexpr float kLandStepSize  = 512.0f;
    constexpr float kLandStepStart = 2000.0f;   // skip the player's own footing
    constexpr float kFadeDuration  = 0.25f;

    // How far the LAND march reaches, well past the Havok ray. Havok can only hit
    // loaded collision (~a few cells), but the heightfield is record data, so a
    // landmark on the horizon is still taggable. 150000 units is ~2100 m, which
    // covers "that peak across the hold" without marching the whole worldspace.
    constexpr float kMaxTagRange = 150000.0f;

    // Hold the last good hit this long through a dropout. Long enough to cover the
    // hit/miss flicker at the collision boundary, short enough that it cannot
    // survive an actual look-away.
    constexpr double kTargetHoldSec = 0.35;

    // A Havok hit closer than this is treated as "probably the ground right in
    // front of me", so the terrain march runs as well and wins if it finds
    // something meaningfully further out. 600 units is ~8.5 m: closer than that is
    // never the distant landmark the player is trying to tag, and anything the
    // player really is standing next to still resolves because the march agrees.
    constexpr float kNearHitReject = 600.0f;

    // Push the ray origin this far along the aim before casting, and discard any
    // Havok hit closer than kSelfHitReject. Both exist for the same reason: kLOS
    // collides with the PLAYER's own body and equipped gear, so a ray starting at
    // the player hits itself at ~2 units. Belt and braces on purpose -- the offset
    // stops most self-hits, the reject catches the rest (gear extends further in
    // some poses, and the offset must stay small so a wall the player is standing
    // against is not skipped).
    constexpr float kRayStartOffset = 60.0f;
    constexpr float kSelfHitReject  = 80.0f;

    // Reticle geometry, screen pixels.
    constexpr float kReticleRadius   = 14.0f;
    constexpr float kReticleTickIn   = 6.0f;
    constexpr float kReticleTickOut  = 11.0f;
    constexpr float kReticleThick    = 2.0f;
    constexpr float kGhostPinRadius  = 7.0f;

    // Green = a press will place, red = a press does nothing. Those two states are
    // the entire interaction contract, so they get saturated, unambiguous colors
    // rather than theme golds.
    constexpr ImU32 kReticleOk   = IM_COL32( 90, 220, 120, 255);
    constexpr ImU32 kReticleBad  = IM_COL32(220,  80,  80, 255);
    constexpr ImU32 kReticleShadow = IM_COL32(0, 0, 0, 200);

    // Written on the Present thread, read by InputHook on the input thread.
    std::atomic<bool> g_active{ false };

    // Scale an ImU32's alpha by a 0..1 fade.
    ImU32 FadeCol(ImU32 a_col, float a_fade) {
        const float a = static_cast<float>((a_col >> IM_COL32_A_SHIFT) & 0xFF) * a_fade;
        return (a_col & ~IM_COL32_A_MASK) |
               (static_cast<ImU32>(std::clamp(a, 0.0f, 255.0f)) << IM_COL32_A_SHIFT);
    }

    // Camera forward: column 1 of PlayerCamera::cameraRoot's world rotation.
    //
    // The camera to read is cameraRoot, NOT Main::WorldRootCamera(). They are
    // different objects with different matrices, and WorldRootCamera's rotation
    // does not track where the player is looking at all: measured in-game at
    // yaw 38.1 deg, its column 1 pointed at 356 deg and stayed there while the
    // player turned. Every ray then hit geometry within 2 units of the camera and
    // the pin landed at the player's feet. cameraRoot column 1 matches yaw exactly
    // and is the same vector the Live Bridge reports as forwardCol1.
    //
    // MinimapRenderer.cpp:326-328 already reads this exact field for the radar view
    // cone (verified live 2026-07-16); it is the precedent, and the spec cited the
    // wrong one.
    //
    // Returns false when the camera is unavailable, rather than a zero vector that
    // would silently cast a degenerate ray.
    bool CameraForward(RE::PlayerCharacter* a_player, RE::NiPoint3& a_outFwd) {
        if (!a_player) return false;

        // YAW from the player: GetAngleZ tracks the view exactly in first person
        // (measured, fwdYaw == bodyYaw every frame). Skyrim yaw is clockwise from
        // +Y (north), so XY is (sin, cos), NOT the (cos, sin) of maths convention.
        const float yaw = a_player->GetAngleZ();

        // PITCH from the camera node, NOT GetAngleX. GetAngleX is the BODY pitch
        // and in first person it does not follow where the player is looking: it
        // read +29 to +32 deg while the player was looking level at a distant
        // shrine, aiming the ray into the ground a couple of units ahead. That is
        // what produced "it places right next to me / 0 m".
        //
        // cameraRoot's rotation is unusable as a whole (see the STATUS block: it
        // goes stale and is 50 deg off in yaw), but entry[2][1] alone is reliable:
        // it is -sin(pitch), and the live camera dump confirms it tracks the view
        // (row2 z = 0.997 when looking level, i.e. pitch ~ 0).
        float sinPitch = 0.0f;
        if (auto* pcam = RE::PlayerCamera::GetSingleton(); pcam && pcam->cameraRoot) {
            sinPitch = std::clamp(pcam->cameraRoot->world.rotate.entry[2][1], -1.0f, 1.0f);
        }
        const float cosPitch = std::sqrt((std::max)(0.0f, 1.0f - sinPitch * sinPitch));

        a_outFwd = RE::NiPoint3{ cosPitch * std::sin(yaw),
                                 cosPitch * std::cos(yaw),
                                 sinPitch };
        return true;
    }

    // Camera-forward Havok ray. Endpoint/scale handling mirrors
    // WorldBeaconRenderer::IsPointOccluded; kLOS collides with terrain and statics
    // but ignores actors, so the player's own body in third person cannot become
    // the target.
    bool RaycastForward(RE::TESObjectCELL* a_cell, const RE::NiPoint3& a_from,
                        const RE::NiPoint3& a_fwd, RE::NiPoint3& a_outHit) {
        if (!a_cell) return false;
        auto* world = a_cell->GetbhkWorld();
        if (!world) return false;

        RE::bhkPickData pick;
        const float scale = RE::bhkWorld::GetWorldScale();

        const RE::NiPoint3 to = a_from + a_fwd * kRayLength;
        pick.rayInput.from = a_from * scale;
        pick.rayInput.to   = to * scale;
        pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);

        if (!world->PickObject(pick) || !pick.rayOutput.HasHit()) return false;

        a_outHit = a_from + a_fwd * (kRayLength * pick.rayOutput.hitFraction);
        return true;
    }

    // Fallback for targets outside loaded collision: the distant-peak case, which
    // is the signature use of this feature. Marches the ray against the LAND
    // heightfield, which is record data and exists for unloaded cells where kLOS
    // collision does not. First step whose ray Z drops to or below the terrain is
    // the intersection.
    bool MarchLandHeight(const RE::NiPoint3& a_from, const RE::NiPoint3& a_fwd,
                         RE::NiPoint3& a_outHit) {
        auto* tes = RE::TES::GetSingleton();
        if (!tes) return false;

        // Marches to kMaxTagRange, far beyond the Havok ray: this is heightfield
        // data, so it neither needs loaded collision nor cares about distance.
        // That is what makes a landmark on the horizon taggable.
        //
        // Step size GROWS with distance (kLandStepSize near, x8 far). Terrain
        // detail that matters shrinks with distance, and a flat fine step to
        // 150000 would be ~300 lookups per frame for no visible gain.
        //
        // No early-out on an upward ray: terrain can rise into it when the player
        // is in a valley looking at a peak, which is exactly this case.
        float prevZ = 0.0f;
        bool  havePrev = false;
        RE::NiPoint3 prevP{};

        for (float d = kLandStepStart; d <= kMaxTagRange; ) {
            const RE::NiPoint3 p = a_from + a_fwd * d;
            float landZ = 0.0f;
            if (tes->GetLandHeight(p, landZ)) {
                if (p.z <= landZ) {
                    // Refine between the last airborne sample and this one so a
                    // coarse far step does not overshoot the slope by ~4000 units.
                    if (havePrev) {
                        RE::NiPoint3 lo = prevP, hi = p;
                        for (int i = 0; i < 8; ++i) {
                            const RE::NiPoint3 mid{ (lo.x + hi.x) * 0.5f,
                                                    (lo.y + hi.y) * 0.5f,
                                                    (lo.z + hi.z) * 0.5f };
                            float midLand = 0.0f;
                            if (!tes->GetLandHeight(mid, midLand)) break;
                            if (mid.z <= midLand) hi = mid; else lo = mid;
                        }
                        float finalLand = 0.0f;
                        if (tes->GetLandHeight(hi, finalLand)) {
                            a_outHit = RE::NiPoint3{ hi.x, hi.y, finalLand };
                            return true;
                        }
                    }
                    a_outHit = RE::NiPoint3{ p.x, p.y, landZ };
                    return true;
                }
                prevP = p;
                prevZ = landZ;
                havePrev = true;
            }
            d += (d < kRayLength) ? kLandStepSize : (kLandStepSize * 8.0f);
        }
        (void)prevZ;
        return false;
    }

    // "Pinned Location N": max existing N + 1, so deleting #3 leaves a gap
    // instead of the number being reused. Reads the stored autoNameNumber --
    // this used to parse the number back out of the note text, which broke the
    // moment that text was translated.
    std::uint32_t NextPinNumber() { return NextAutoNameNumber(AutoName::Pinned); }
}

bool LookPinRenderer::IsActive()  { return g_active.load(); }
void LookPinRenderer::ForceExit() { g_active.store(false); }

void LookPinRenderer::Draw() {
    auto* settings = Settings::GetSingleton();
    if (!settings || !settings->lookPinEnabled) {
        _active = false;
        g_active.store(false);
        _fade = 0.0f;
        return;
    }

    // Force-exit gate, same shape as WorldBeaconRenderer::Draw. Covers the
    // load-door case for free: walking into an interior flips IsExteriorCell, and
    // the loading screen trips the menu check first.
    auto* ui     = RE::UI::GetSingleton();
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* cell   = player ? player->GetParentCell() : nullptr;
    auto* cam    = RE::Main::WorldRootCamera();

    // Any vanilla menu cancels the mode (the reticle is a gameplay-view overlay
    // and the poll below cannot see the keyboard meaningfully behind a menu).
    // GameIsPaused stays: it catches pause states no menu name covers.
    const bool gateOk =
        !SettingsMenu::IsVisible() &&
        ui && !MenuUtils::ShouldBlockOverlay() &&
        !PartySheetState::ShouldHideOverlay() &&
        !ui->GameIsPaused() &&
        // Beast form: tagging a location mid-rampage is off-tone, and the
        // pin would be unreachable anyway without the map. Cancels the mode
        // like any menu does. Vampire Lord is exempt (include/BeastForm.h).
        !(Settings::GetSingleton()->hideInBeastForm && BeastForm::IsBeastForm()) &&
        player && cell && cam && cell->IsExteriorCell();

    if (!gateOk) {
        _active = false;
        g_active.store(false);
        _keyWasDown = false;
        _fade = 0.0f;
        return;
    }

    // Edge-triggered <modifier>+<key>. GetAsyncKeyState does NOT consume the
    // press, so InputHook has a matching swallow block or vanilla's binding fires
    // too. Modifier is stored (default Ctrl), not hardcoded: it is what separates
    // this from Inspect, which defaults to the same key with no modifier.
    // Hotkeys store DIK, so convert for the poll; 0 = unbound or a device
    // GetAsyncKeyState cannot see.
    const int pinVk = InputHandler::DIKToVK(settings->lookPinKey);
    const bool keyDown = pinVk != 0 && ModifierMatches(settings->lookPinModifier) &&
        (GetAsyncKeyState(pinVk) & 0x8000) != 0;
    const bool pressed = keyDown && !_keyWasDown;
    _keyWasDown = keyDown;

    // ForceExit() (Esc, from the input thread) clears the atomic only. Adopt it
    // here or the _active republish at the end of this function would immediately
    // undo it and the mode would look unclosable.
    if (_active && !g_active.load()) {
        _active = false;
    }

    // This frame's ray. Only while active: no reason to pay for a raycast the
    // player cannot see the result of.
    //
    // DIAGNOSTIC (2026-08-16, "reticle is red standing still, green only while
    // walking"): logs which of the two paths produced the target, on CHANGE only,
    // never per frame. Remove once placement is confirmed in-game.
    // _targetPos is cleared alongside _hasTarget, NOT left holding the last hit.
    // Leaving it stale is how a missed frame placed a marker at the player's feet:
    // the ray flickers hit/miss at the edge of loaded collision, and a commit on a
    // miss frame would otherwise silently reuse whatever the previous hit was.
    _hasTarget = false;
    _targetPos = RE::NiPoint3{};
    const double nowSecEarly = ImGui::GetTime();
    RE::NiPoint3 fwd{};
    if (_active && CameraForward(player, fwd)) {
        // Origin: player position lifted to eye height. cameraRoot->world.translate
        // was tried and it FREEZES: it held one value for a 2-second sequence while
        // the player moved and turned (visible in the log as an unchanging camPos).
        // A stale origin plus a live direction casts from where the player used to
        // be, which drifts the hit point for no visible reason.
        //
        // Eye height is approximate on purpose: the exact value only shifts the ray
        // origin a few units, and any error is dwarfed by the ray length. Starting
        // at the feet would clip the ground immediately on a downward look.
        constexpr float kEyeHeight = 120.0f;
        RE::NiPoint3 camPos = player->GetPosition();
        camPos.z += kEyeHeight;

        // Step the origin forward past the player's own collision before casting.
        // kLOS hits the player's body and equipped gear, and in third person the
        // ray starts inside both. Without this the very first thing the ray meets
        // is the player, at ~2 units. The offset is small enough that it cannot
        // skip a wall the player is actually standing against (kSelfHitReject
        // still guards that case).
        camPos += fwd * kRayStartOffset;
        _targetIsLand = false;

        // Havok first, but a NEAR hit is not automatically the answer.
        //
        // Looking even slightly downhill, the ray stabs the ground a few metres
        // ahead long before it reaches the building being aimed at: at 33 deg down
        // from a 120-unit eye height, the ground is only ~220 units away. Taking
        // that hit is why the pin kept landing at the player's feet and the readout
        // sat at 0 m. It was never a bad ray, it was the ground winning the race.
        //
        // So: if Havok hits closer than kNearHitReject, ALSO march the terrain and
        // prefer the march when it finds something meaningfully further. The march
        // ignores loaded collision, so it sees the hillside the ground hit hid.
        // A near hit while genuinely aiming at something close (a wall right in
        // front) survives, because the march then returns a similar distance.
        RE::NiPoint3 havokPos{};
        const bool havokHit = RaycastForward(cell, camPos, fwd, havokPos);
        const float havokDist = havokHit ? (havokPos - camPos).Length() : 0.0f;

        RE::NiPoint3 landPos{};
        bool landHit = false;
        if (!havokHit || havokDist < kNearHitReject) {
            landHit = MarchLandHeight(camPos, fwd, landPos);
        }

        const float landDist = landHit ? (landPos - camPos).Length() : 0.0f;

        // A hit inside kSelfHitReject is the player's OWN body/gear: the ray starts
        // at the player and kLOS does not filter self, so in third person (or with
        // a drawn weapon) it strikes armor 2 units out. Measured: havokD=2 on the
        // frames that showed a green reticle reading "0 m" and then planted the
        // marker on the player. Such a hit is never a legitimate target, so it is
        // discarded outright rather than being used as a fallback.
        const bool havokUsable = havokHit && havokDist > kSelfHitReject;

        if (havokUsable && (!landHit || landDist <= havokDist + kNearHitReject)) {
            _targetPos = havokPos;
            _hasTarget = true;
        } else if (landHit) {
            _targetPos = landPos;
            _targetIsLand = true;
            _hasTarget = true;
        }
        // Deliberately no third branch: if the only Havok hit was the player's own
        // body and the terrain march found nothing, there is NO target. Falling
        // back to the self-hit is what produced the "0 m, places on me" bug.

        // Coast over a one-frame dropout. At the edge of loaded collision the same
        // direction alternates HAVOK HIT / NO TARGET frame to frame, which made the
        // reticle strobe red and let a commit land on a miss frame. Reuse the last
        // hit for kTargetHoldSec, but ONLY a hit that is still recent AND was taken
        // from nearly the same direction, so this smooths a flicker without
        // resurrecting a target the player has aimed away from.
        if (!_hasTarget && _lastHitValid &&
            (nowSecEarly - _lastHitTime) < kTargetHoldSec &&
            (fwd.x * _lastHitFwd.x + fwd.y * _lastHitFwd.y + fwd.z * _lastHitFwd.z) > 0.9995f) {
            _targetPos = _lastHitPos;
            _targetIsLand = _lastHitWasLand;
            _hasTarget = true;
        } else if (_hasTarget) {
            _lastHitPos = _targetPos;
            _lastHitFwd = fwd;
            _lastHitWasLand = _targetIsLand;
            _lastHitTime = nowSecEarly;
            _lastHitValid = true;
        }

        static int s_lastRayState = -1;
        const int rayState = !_hasTarget ? 0 : (_targetIsLand ? 1 : 2);
        if (rayState != s_lastRayState) {
            s_lastRayState = rayState;
            // Both candidates and their distances, so "why did it pick that one"
            // is answerable from the log alone. havokD tiny + landD large is the
            // ground-wins-the-race case that kNearHitReject exists to fix.
            SKSE::log::info(
                "LookPin ray: chose={} havok={} havokD={:.0f} land={} landD={:.0f} "
                "fwd=({:.3f},{:.3f},{:.3f}) target=({:.1f},{:.1f},{:.1f}) cam=({:.1f},{:.1f},{:.1f})",
                _targetIsLand ? "LAND" : (_hasTarget ? "HAVOK" : "NONE"),
                havokHit ? (havokUsable ? "y" : "SELF") : "n", havokDist,
                landHit ? "y" : "n", landDist,
                fwd.x, fwd.y, fwd.z,
                _targetPos.x, _targetPos.y, _targetPos.z,
                camPos.x, camPos.y, camPos.z);
        }

        // DIAGNOSTIC: fwdYaw is derived back OUT of the ray direction, bodyYaw
        // comes from the player, so the two must agree within a degree or the
        // vector is being built wrong. Logged on CHANGE of the rounded direction,
        // which also catches the previous failure: a frozen vector prints one line
        // and then nothing while the view keeps moving. Delete once placement is
        // confirmed in-game.
        {
            static int s_lastDir = INT_MIN;
            const float fwdYaw = std::atan2(fwd.x, fwd.y) * 57.2957795f;
            const int dirKey = static_cast<int>(fwdYaw) * 1000 + static_cast<int>(fwd.z * 100.0f);
            if (dirKey != s_lastDir) {
                s_lastDir = dirKey;
                // camPitch = what we now use (from the camera node), bodyPitch =
                // the GetAngleX value that was wrong. Logging both so the split is
                // visible: looking level should show camPitch ~0 whatever bodyPitch
                // says. camPos must CHANGE as the player moves (it froze before).
                SKSE::log::info(
                    "LookPin axis: fwdYaw={:.1f} bodyYaw={:.1f} camPitch={:.1f} bodyPitch={:.1f} "
                    "fwd=({:.3f},{:.3f},{:.3f}) camPos=({:.1f},{:.1f},{:.1f})",
                    fwdYaw, player->GetAngleZ() * 57.2957795f,
                    std::asin(std::clamp(-fwd.z, -1.0f, 1.0f)) * 57.2957795f,
                    player->GetAngleX() * 57.2957795f,
                    fwd.x, fwd.y, fwd.z, camPos.x, camPos.y, camPos.z);
            }
        }
    }

    const double nowSec = ImGui::GetTime();

    // DIAGNOSTIC: every Ctrl+key edge, with the state that decides the branch. If
    // a press produces NO line here, the edge never fired (key held across frames,
    // or the gate above returned before this point) rather than the ray failing.
    // Remove with the ray log once placement is confirmed.
    if (pressed) {
        SKSE::log::info("LookPin press: active={} hasTarget={} isLand={} -> {}",
                        _active, _hasTarget, _targetIsLand,
                        !_active ? "ENTER" : (_hasTarget ? "PLACE" : "no-target"));

        if (!_active) {
            _active = true;
        } else if (_hasTarget) {
            auto* ws = player->GetWorldspace();
            auto* mm = MarkerManager::GetSingleton();
            const std::uint32_t n = NextPinNumber();

            // Raw world position with its real Z, NO map-offset conversion.
            // ConvertToMapWorldspace is a DRAW-time transform that needs a live
            // MapMenu, which does not exist during gameplay.
            //
            // Built as a struct and consumes the assigned ID returned by AddMarker
            // for SetDestination.
            MapMarker marker;
            marker.worldSpaceID = ws ? ws->GetFormID() : 0;
            marker.cellID = cell->GetFormID();
            marker.position = _targetPos;
            marker.iconType = 0;
            marker.note.clear();               // empty note = auto-named
            marker.autoName = AutoName::Pinned;
            marker.autoNameNumber = n;
            marker.origin = MarkerOrigin::LookAndPin;
            marker.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
            const std::uint32_t assignedId = mm->AddMarker(marker);

            // Auto-set as destination so a beacon appears immediately.
            // WorldBeaconRenderer draws ONLY the destination marker
            // (WorldBeaconRenderer.cpp:413), so without this a fresh pin is
            // invisible in-world and the feature looks broken. SetDestination
            // clears the flag on every other marker, so "one destination at a
            // time" still holds; the previous destination is replaced, which
            // matches "I just tagged where I want to go".
            if (assignedId != 0) {
                mm->SetDestination(assignedId, true);
            }

            SKSE::log::info(
                "LookPin: placed #{} id={} at {:.1f},{:.1f},{:.1f} (source={}) ws={:08X} cell={:08X} [destination]",
                n, assignedId, _targetPos.x, _targetPos.y, _targetPos.z,
                _targetIsLand ? "landheight" : "havok",
                ws ? ws->GetFormID() : 0, cell->GetFormID());
            _active = false;
        } else {
            // Miss does NOT exit the mode: bad aim is the exact thing this mode
            // exists to help with, so it costs a message, not the session.
            _noTargetMsgUntil = nowSec + 1.5;
            SKSE::log::info("LookPin: commit with no target (ray missed both havok and landheight)");
        }
    }

    g_active.store(_active);

    const float dt = ImGui::GetIO().DeltaTime;
    const float rate = std::clamp((kFadeDuration > 1e-4f) ? (dt / kFadeDuration) : 1.0f, 0.0f, 1.0f);
    const float targetFade = _active ? 1.0f : 0.0f;
    _fade = std::clamp(_fade + std::clamp(targetFade - _fade, -rate, rate), 0.0f, 1.0f);
    if (_fade <= 0.001f) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const ImVec2 centre(disp.x * 0.5f, disp.y * 0.5f);
    const ImU32  tint       = FadeCol(_hasTarget ? kReticleOk : kReticleBad, _fade);
    const ImU32  shadowTint = FadeCol(kReticleShadow, _fade);

    // 1. Reticle: ring + 4 ticks at screen centre. Drawn over a 1px-offset dark
    //    copy so it stays readable against snow and sky.
    dl->AddCircle(ImVec2(centre.x + 1.0f, centre.y + 1.0f), kReticleRadius, shadowTint, 32, kReticleThick);
    dl->AddCircle(centre, kReticleRadius, tint, 32, kReticleThick);
    for (int i = 0; i < 4; ++i) {
        const float ang = static_cast<float>(i) * 1.5707963f;   // 90 deg steps
        const float cx = std::cos(ang);
        const float sy = std::sin(ang);
        dl->AddLine(ImVec2(centre.x + cx * kReticleTickIn,  centre.y + sy * kReticleTickIn),
                    ImVec2(centre.x + cx * kReticleTickOut, centre.y + sy * kReticleTickOut),
                    tint, kReticleThick);
    }

    // 2. Ghost pin at the hit point. ProjectWorldPoint's inFront out-param is
    //    mandatory: WorldPtToScreenPt3 returns mirrored garbage for points behind
    //    the camera rather than failing, and leaves outScreen untouched on false.
    if (_hasTarget) {
        ImVec2 pinPos;
        bool inFront = false;
        if (MapDraw::ProjectWorldPoint(cam, _targetPos, pinPos, inFront) && inFront) {
            dl->AddCircleFilled(pinPos, kGhostPinRadius, FadeCol(tint, 0.55f), 20);
            dl->AddCircle(pinPos, kGhostPinRadius, tint, 20, 2.0f);
            dl->AddLine(ImVec2(pinPos.x, pinPos.y - kGhostPinRadius),
                        ImVec2(pinPos.x, pinPos.y - kGhostPinRadius * 2.4f), tint, 2.0f);
        }
    }

    // 3. Distance readout under the reticle. Horizontal distance only: the player
    //    reads it as "how far do I walk", and a target 300 units below on a
    //    clifftop is not 300 units of travel.
    const float labelY = centre.y + kReticleRadius + 8.0f;
    if (_hasTarget) {
        const RE::NiPoint3 playerPos = player->GetPosition();
        const float dx = _targetPos.x - playerPos.x;
        const float dy = _targetPos.y - playerPos.y;
        const float meters = std::sqrt(dx * dx + dy * dy) / Settings::kUnitsPerMeter;

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f m", meters);
        const ImVec2 sz = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(centre.x - sz.x * 0.5f + 1.0f, labelY + 1.0f), shadowTint, buf);
        dl->AddText(ImVec2(centre.x - sz.x * 0.5f, labelY), tint, buf);
    }

    // 4. "No target" after a commit that had nothing to place on.
    if (nowSec < _noTargetMsgUntil) {
        const char* msg = "No target";
        const ImVec2 sz = ImGui::CalcTextSize(msg);
        const float y = labelY + ImGui::GetTextLineHeight() + 2.0f;
        dl->AddText(ImVec2(centre.x - sz.x * 0.5f + 1.0f, y + 1.0f), shadowTint, msg);
        dl->AddText(ImVec2(centre.x - sz.x * 0.5f, y), FadeCol(kReticleBad, _fade), msg);
    }
}
