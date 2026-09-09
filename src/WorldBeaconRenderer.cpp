// Renders 3D light pillars and distance counters at destination pins.

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

// STATUS:

namespace {
    constexpr float kGroundProbeUp   = 8192.0f;
    constexpr float kGroundProbeDown = 32768.0f;

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
        pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
        if (!world->PickObject(pick) || !pick.rayOutput.HasHit()) return false;
        a_outZ = a_startZ - a_castDown * pick.rayOutput.hitFraction;
        return true;
    }

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

    struct GroundCacheEntry {
        float x = 0.0f;
        float y = 0.0f;
        float groundZ = 0.0f;
        RE::FormID cellID = 0;
        bool  hit = false;
    };
    std::unordered_map<std::uint32_t, GroundCacheEntry> g_groundCache;

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
                entry.groundZ = landZ;
                entry.hit = true;
                if (Settings::GetSingleton()->beaconVerboseLog) {
                    SKSE::log::info("Beacon[{}] groundZ=LANDHEIGHT {:.1f} (raycast missed/outside grid)", a_marker.id, landZ);
                }
            }
        } else {
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

    inline bool ProjectWorld(RE::NiCamera* a_cam, const RE::NiPoint3& a_world,
                             ImVec2& a_outScreen, bool& a_outInFront) {
        return MapDraw::ProjectWorldPoint(a_cam, a_world, a_outScreen, a_outInFront);
    }

    constexpr int   kOcclusionSamples   = 5;

    constexpr float kHeightRampFarMult  = 8.0f;
    constexpr float kOccludedAlphaScale = 0.22f;

    constexpr float kPinOccFullDimUnits = 11000.0f;
    constexpr float kOcclusionFadeSecs  = 0.18f;
    constexpr float kOcclusionInterval  = 0.25f;

    struct OcclusionState {
        float   blocked[kOcclusionSamples]{};
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

    float SampleOcclusion(const OcclusionState& a_st, float a_t) {
        const float scaled = std::clamp(a_t, 0.0f, 1.0f) * (kOcclusionSamples - 1);
        const int   i0 = static_cast<int>(scaled);
        const int   i1 = (std::min)(i0 + 1, kOcclusionSamples - 1);
        const float frac = scaled - static_cast<float>(i0);
        return a_st.blocked[i0] + (a_st.blocked[i1] - a_st.blocked[i0]) * frac;
    }

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
    if (!settings || !settings->navBeaconVisible) {
        _beaconFade = 0.0f;
        return;
    }

    auto* ui = RE::UI::GetSingleton();
    if (!ui || MenuUtils::ShouldBlockOverlay() || PartySheetState::ShouldHideOverlay()) {
        _beaconFade = 0.0f;
        return;
    }

    if (Settings::GetSingleton()->hideInBeastForm && BeastForm::IsBeastForm()) {
        _beaconFade = 0.0f;
        return;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return;

    auto* parentCell = player->GetParentCell();
    if (!parentCell) return;

    auto* cam = RE::Main::WorldRootCamera();
    if (!cam) return;

    const RE::NiPoint3 playerPos = player->GetPosition();
    const bool isExterior = parentCell->IsExteriorCell();
    auto* currentWorld = player->GetWorldspace();
    const RE::FormID currentWorldID = isExterior && currentWorld ? currentWorld->GetFormID() : 0;
    const RE::FormID currentCellID = parentCell->GetFormID();

    std::vector<MapMarker> activeDestinations;
    const auto allMarkers = MarkerManager::GetSingleton()->GetMarkers();
    for (const auto& m : allMarkers) {
        if (!m.isDestination) continue;
        bool spaceMatches = false;
        if (isExterior) {
            if (m.worldSpaceID != 0 && currentWorld && CompassHook::WorldMatches(currentWorld, m.worldSpaceID)) {
                spaceMatches = true;
            }
        } else {
            if (m.worldSpaceID == 0 && m.cellID == currentCellID) {
                spaceMatches = true;
            }
        }
        if (spaceMatches) {
            activeDestinations.push_back(m);
        }
    }

    const float dt = ImGui::GetIO().DeltaTime;
    constexpr float kFadeDuration = 0.25f;
    const float rate = std::clamp((kFadeDuration > 1e-4f) ? (dt / kFadeDuration) : 1.0f, 0.0f, 1.0f);
    const float targetFade = activeDestinations.empty() ? 0.0f : 1.0f;
    _beaconFade = std::clamp(_beaconFade + std::clamp(targetFade - _beaconFade, -rate, rate), 0.0f, 1.0f);

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

    const RE::NiPoint3 camPos = cam->world.translate;

    for (const auto& m : activeDestinations) {
        const float dx = m.position.x - playerPos.x;
        const float dy = m.position.y - playerPos.y;
        const float distUnits = std::sqrt(dx * dx + dy * dy);
        const int distMeters = static_cast<int>(distUnits / Settings::kUnitsPerMeter + 0.5f);
        float proximityAlpha = 1.0f;
        if (distUnits < settings->navBeaconFadeNearUnits) {
            proximityAlpha = (std::max)(0.0f, distUnits / settings->navBeaconFadeNearUnits);
        }
        const float baseAlpha = _beaconFade * proximityAlpha;
        if (baseAlpha <= 0.01f) continue;
        const float baseZ = ResolveGroundZ(m, parentCell, playerPos);
        const float rampFar = (std::max)(settings->navBeaconFadeNearUnits * kHeightRampFarMult, 1.0f);
        const float rampT   = std::clamp(distUnits / rampFar, 0.0f, 1.0f);
        const float nearScale = std::clamp(settings->navBeaconNearScale, 0.05f, 1.0f);
        const float heightScale = nearScale + (1.0f - nearScale) * rampT;
        const RE::NiPoint3 basePos{ m.position.x, m.position.y, baseZ };
        const RE::NiPoint3 topPos{ m.position.x, m.position.y,
                                   baseZ + settings->navBeaconPillarHeight * heightScale };
        ImVec2 baseScr, topScr;
        bool baseInFront = false, topInFront = false;
        const bool baseProj = ProjectWorld(cam, basePos, baseScr, baseInFront);
        const bool topProj  = ProjectWorld(cam, topPos, topScr, topInFront);
        baseInFront = baseInFront && baseProj;
        topInFront  = topInFront && topProj;
        if (!topProj && !baseProj) continue;
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
        float segT0 = 0.0f, segT1 = 1.0f;
        const bool beamUsable = baseProj && topProj && baseInFront && topInFront;
        const bool isOnScreen = beamUsable &&
                                MapDraw::SegmentInRect(baseScr, topScr,
                                                       kPad, kPad, disp.x - kPad, disp.y - kPad,
                                                       segT0, segT1);
        float tPin = 1.0f;
        ImVec2 pinScr = topScr;
        if (isOnScreen) {
            tPin = (topScr.y <= baseScr.y) ? segT1 : segT0;
            pinScr = ImVec2(baseScr.x + (topScr.x - baseScr.x) * tPin,
                            baseScr.y + (topScr.y - baseScr.y) * tPin);
        }
        std::string label = ResolveMarkerName(m);
        if (isOnScreen) {
            if (perfLog) {
                ++perfFrame.visibleCount;
            }
            const OcclusionState& occState = UpdateOcclusion(
                m.id, parentCell, camPos, basePos, topPos, nowSec, dt,
                perfLog ? &perfFrame : nullptr);
            constexpr ImU32 coreCol = IM_COL32(215, 240, 255, 255);
            constexpr ImU32 glowCol = IM_COL32(110, 200, 255, 255);
            constexpr ImU32 haloCol = IM_COL32( 50, 140, 255, 255);
            const float badgeR = static_cast<float>(settings->navBeaconPinSize) * 0.75f;
            const ImVec2 iconAt = pinScr;
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
                3.0f, 0.50f,
                3.0f);
            auto* pinSrv = TextureManager::GetIconByName("destination");
            if (pinSrv) {
                const float iconHalf = badgeR * 0.65f;
                MapDraw::DrawIconFx(dl, pinSrv,
                                    ImVec2(iconAt.x - iconHalf, iconAt.y - iconHalf),
                                    ImVec2(iconAt.x + iconHalf, iconAt.y + iconHalf),
                                    MapDraw::WithAlpha(IM_COL32_WHITE, pinAlpha));
            }
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
            float angle = 0.0f;
            const ImVec2 clampedPos = ClampToEdge(topScr, topInFront, kPad, angle);
            DrawEdgeIndicator(dl, clampedPos, angle, 16.0f, IM_COL32(110, 210, 255, alphaByte));
            const ImVec2 iconPos(clampedPos.x - std::cos(angle) * 18.0f,
                                 clampedPos.y - std::sin(angle) * 18.0f);
            auto* pinSrv = TextureManager::GetIconByName("destination");
            const float pinScale = (static_cast<float>(settings->navBeaconPinSize) * 0.85f) / (2.0f * MapDraw::kPinRadius);
            MapDraw::DrawPin(dl, iconPos.x, iconPos.y, 0.0f, 0, pinSrv, pinScale,
                             Settings::GetSingleton()->PinTintCol(), finalAlpha);
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
