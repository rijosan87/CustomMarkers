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
#include "FontManager.h"
#include "UIScale.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <string>
#include <string_view>

namespace {
    constexpr float kRayLength     = 40000.0f;
    constexpr float kLandStepSize  = 512.0f;
    constexpr float kLandStepStart = 2000.0f;   // skip the player's own footing
    constexpr float kFadeDuration  = 0.25f;

    constexpr float kMaxTagRange = 150000.0f;

    constexpr double kTargetHoldSec = 0.35;

    constexpr float kNearHitReject = 600.0f;

    constexpr float kRayStartOffset = 60.0f;
    constexpr float kSelfHitReject  = 80.0f;

    // Reticle geometry, screen pixels.
    constexpr float kReticleRadius   = 14.0f;
    constexpr float kReticleTickIn   = 6.0f;
    constexpr float kReticleTickOut  = 11.0f;
    constexpr float kReticleThick    = 2.0f;
    constexpr float kGhostPinRadius  = 7.0f;

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

    bool CameraForward(RE::PlayerCharacter* a_player, RE::NiPoint3& a_outFwd) {
        if (!a_player) return false;

        const float yaw = a_player->GetAngleZ();

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

    bool MarchLandHeight(const RE::NiPoint3& a_from, const RE::NiPoint3& a_fwd,
                         RE::NiPoint3& a_outHit) {
        auto* tes = RE::TES::GetSingleton();
        if (!tes) return false;

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

    auto* ui     = RE::UI::GetSingleton();
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* cell   = player ? player->GetParentCell() : nullptr;
    auto* cam    = RE::Main::WorldRootCamera();

    const bool gateOk =
        !SettingsMenu::IsVisible() &&
        ui && !MenuUtils::ShouldBlockOverlay() &&
        !PartySheetState::ShouldHideOverlay() &&
        !ui->GameIsPaused() &&
        !(Settings::GetSingleton()->hideInBeastForm && BeastForm::IsBeastForm()) &&
        player && cell && cam && cell->IsExteriorCell();

    if (!gateOk) {
        _active = false;
        g_active.store(false);
        _keyWasDown = false;
        _fade = 0.0f;
        return;
    }

    const int pinVk = InputHandler::DIKToVK(settings->lookPinKey);
    const bool keyDown = pinVk != 0 && ModifierMatches(settings->lookPinModifier) &&
        (GetAsyncKeyState(pinVk) & 0x8000) != 0;
    const bool pressed = keyDown && !_keyWasDown;
    _keyWasDown = keyDown;

    if (_active && !g_active.load()) {
        _active = false;
    }

    _hasTarget = false;
    _targetPos = RE::NiPoint3{};
    const double nowSecEarly = ImGui::GetTime();
    RE::NiPoint3 fwd{};
    if (_active && CameraForward(player, fwd)) {
        constexpr float kEyeHeight = 120.0f;
        RE::NiPoint3 camPos = player->GetPosition();
        camPos.z += kEyeHeight;

        camPos += fwd * kRayStartOffset;
        _targetIsLand = false;

        RE::NiPoint3 havokPos{};
        const bool havokHit = RaycastForward(cell, camPos, fwd, havokPos);
        const float havokDist = havokHit ? (havokPos - camPos).Length() : 0.0f;

        RE::NiPoint3 landPos{};
        bool landHit = false;
        if (!havokHit || havokDist < kNearHitReject) {
            landHit = MarchLandHeight(camPos, fwd, landPos);
        }

        const float landDist = landHit ? (landPos - camPos).Length() : 0.0f;

        const bool havokUsable = havokHit && havokDist > kSelfHitReject;

        if (havokUsable && (!landHit || landDist <= havokDist + kNearHitReject)) {
            _targetPos = havokPos;
            _hasTarget = true;
        } else if (landHit) {
            _targetPos = landPos;
            _targetIsLand = true;
            _hasTarget = true;
        }

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

        {
            static int s_lastDir = INT_MIN;
            const float fwdYaw = std::atan2(fwd.x, fwd.y) * 57.2957795f;
            const int dirKey = static_cast<int>(fwdYaw) * 1000 + static_cast<int>(fwd.z * 100.0f);
            if (dirKey != s_lastDir) {
                s_lastDir = dirKey;
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

    const float reticleRadius = UIScale::Px(kReticleRadius);
    const float tickIn        = UIScale::Px(kReticleTickIn);
    const float tickOut       = UIScale::Px(kReticleTickOut);
    const float reticleThick  = UIScale::Px(kReticleThick);
    const float shadowOffset  = UIScale::Px(1.0f);

    // 1. Reticle: ring + 4 ticks at screen centre. Drawn over a 1px-offset dark
    //    copy so it stays readable against snow and sky.
    dl->AddCircle(ImVec2(centre.x + shadowOffset, centre.y + shadowOffset), reticleRadius, shadowTint, 32, reticleThick);
    dl->AddCircle(centre, reticleRadius, tint, 32, reticleThick);
    for (int i = 0; i < 4; ++i) {
        const float ang = static_cast<float>(i) * 1.5707963f;   // 90 deg steps
        const float cx = std::cos(ang);
        const float sy = std::sin(ang);
        dl->AddLine(ImVec2(centre.x + cx * tickIn,  centre.y + sy * tickIn),
                    ImVec2(centre.x + cx * tickOut, centre.y + sy * tickOut),
                    tint, reticleThick);
    }

    if (_hasTarget) {
        ImVec2 pinPos;
        bool inFront = false;
        if (MapDraw::ProjectWorldPoint(cam, _targetPos, pinPos, inFront) && inFront) {
            const float ghostRadius  = UIScale::Px(kGhostPinRadius);
            const float ghostOutline = UIScale::Px(2.0f);
            const float ghostStemLen = ghostRadius * 2.4f;
            dl->AddCircleFilled(pinPos, ghostRadius, FadeCol(tint, 0.55f), 20);
            dl->AddCircle(pinPos, ghostRadius, tint, 20, ghostOutline);
            dl->AddLine(ImVec2(pinPos.x, pinPos.y - ghostRadius),
                        ImVec2(pinPos.x, pinPos.y - ghostStemLen), tint, ghostOutline);
        }
    }

    // Text font and size
    ImFont* font = FontManager::GetSingleton()->GetBaseFont();
    if (!font) font = ImGui::GetFont();
    const float fontSize = font->FontSize * UIScale::Font() *
        std::clamp(settings->bodyScale, 0.5f, 3.0f);

    const float labelY = centre.y + reticleRadius + UIScale::Px(8.0f);
    if (_hasTarget) {
        const RE::NiPoint3 playerPos = player->GetPosition();
        const float dx = _targetPos.x - playerPos.x;
        const float dy = _targetPos.y - playerPos.y;
        const float meters = std::sqrt(dx * dx + dy * dy) / Settings::kUnitsPerMeter;

        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f m", meters);
        const ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, buf);
        dl->AddText(font, fontSize, ImVec2(centre.x - sz.x * 0.5f + shadowOffset, labelY + shadowOffset), shadowTint, buf);
        dl->AddText(font, fontSize, ImVec2(centre.x - sz.x * 0.5f, labelY), tint, buf);
    }

    // 4. "No target" after a commit that had nothing to place on.
    if (nowSec < _noTargetMsgUntil) {
        const char* msg = "No target";
        const ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, msg);
        const float y = labelY + fontSize + UIScale::Px(2.0f);
        dl->AddText(font, fontSize, ImVec2(centre.x - sz.x * 0.5f + shadowOffset, y + shadowOffset), shadowTint, msg);
        dl->AddText(font, fontSize, ImVec2(centre.x - sz.x * 0.5f, y), FadeCol(kReticleBad, _fade), msg);
    }
}
