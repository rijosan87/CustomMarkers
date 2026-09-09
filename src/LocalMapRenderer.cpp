// Renders local and interior map custom pins and breadcrumb trails.

#include "LocalMapRenderer.h"
#include "MapDrawHelpers.h"
#include "MarkerManager.h"
#include "MapMarkerRenderer.h"
#include "TextureManager.h"
#include "PathTracker.h"
#include "InputHandler.h"
#include "Settings.h"
#include "FontManager.h"
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

// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):

namespace {
    constexpr std::uintptr_t kLocalMapBaseOffset      = 0x60;
    constexpr std::uintptr_t kLocalMapCameraOffset    = 0x302A0;
    constexpr std::uintptr_t kNiCameraSubOffset       = 0x58;
    constexpr std::uintptr_t kModeByteOffset          = 0x303FC;

    constexpr REL::RelocationID kRefreshMarkersID{ 52090, 52980 };

    struct CachedMarker {
        std::uint32_t id;
        float         nx, ny;
        std::string   note;
        bool          isDestination;
        bool          isExitDoorBeacon;
        bool          isRingOnly;
        std::uint32_t iconType;
    };

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

    struct BeaconCache {
        RE::FormID  cellID         = 0;
        std::uint32_t destMarkerId = 0;
        bool        hasDoor        = false;
        RE::NiPoint3 doorPos{};
        std::string note;
    };
    BeaconCache g_beaconCache;

    std::uint64_t                              g_lmrTrampolineAddr = 0;
    std::unique_ptr<PLH::x64Detour>            g_lmrDetour;

    std::uintptr_t GetActiveLocalMapBase() {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) return 0;
        auto mapMenu = ui->GetMenu<RE::MapMenu>();
        if (!mapMenu) return 0;
        const auto lmBase = reinterpret_cast<std::uintptr_t>(mapMenu.get()) + kLocalMapBaseOffset;
        if (*reinterpret_cast<const std::uint8_t*>(lmBase + kModeByteOffset) != 1) return 0;
        return lmBase;
    }

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
            const bool sameScope = (currentWorld != 0)
                ? (m.worldSpaceID == currentWorld)
                : (m.worldSpaceID == 0 && m.cellID == currentCell);
            if (!sameScope) continue;
            const bool ringOnly = (m.id == kVanillaDestId);
            float sx, sy, sz;
            bool inFront = RE::NiCamera::WorldPtToScreenPt3(
                camData.worldToCam, camData2.port, m.position, sx, sy, sz, 1e-5f);
            if (!inFront || !MapDraw::ProjectionDepthValid(sz)) continue;
            next.push_back({ m.id, sx, 1.0f - sy, ResolveMarkerName(m), m.isDestination, false, ringOnly, m.iconType });
        }
        if (auto* player = RE::PlayerCharacter::GetSingleton(); player && !player->GetWorldspace()) {
            const MapMarker* destMarker = nullptr;
            for (const auto& m : markers) {
                if (m.isDestination && m.worldSpaceID != 0) { destMarker = &m; break; }
            }
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
                        g_beaconCache.note = LocalizationManager::Format("Navigation.ExitTo",
                                                                        destMarker->note);
                    }
                }
            }
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

    auto mapMenu = ui->GetMenu<RE::MapMenu>();
    if (!mapMenu) return;

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
        bool          offscreen;
        float         dirX, dirY;
    };
    std::vector<Projected> visible;
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        visible.reserve(g_cache.size());
        for (const auto& c : g_cache) {
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

    std::vector<std::vector<float>> visibleSegmentZ;
    float playerTrailZ = 0.0f;

    bool headNearPlayer = false;
    if (niCam) {
        auto& camData  = niCam->GetRuntimeData();
        auto& camData2 = niCam->GetRuntimeData2();
        auto segments = PathTracker::GetSingleton()->GetSegments();
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
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
                    vsegZ.push_back(pt.z);
                }
            }
            if (!vseg.empty()) {
                visibleSegments.push_back(std::move(vseg));
                visibleSegmentZ.push_back(std::move(vsegZ));
            }
        }
    }

    ImVec2 clipMin((vpL / cursorW) * dispSize.x, (vpT / cursorH) * dispSize.y);
    ImVec2 clipMax((vpR / cursorW) * dispSize.x, (vpB / cursorH) * dispSize.y);
    dl->PushClipRect(clipMin, clipMax, true);

    auto* scaleSet = Settings::GetSingleton();
    ImFont* titleFont = FontManager::GetSingleton()->GetHeadingFont();
    if (!titleFont) titleFont = ImGui::GetFont();
    const float fontRatio  = titleFont->FontSize / ImGui::GetFontSize();
    const float titleScale = std::clamp(scaleSet->titleScale, 0.5f, 3.0f) * fontRatio;
    const float bodyScale  = std::clamp(scaleSet->bodyScale,  0.5f, 3.0f);

    const Projected* active = nullptr;
    if (!MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen() &&
        !MapMarkerRenderer::GetSingleton()->IsCustomMenuActive()) {
        if (_activeMarkerId != 0) {
            for (const auto& v : visible) {
                if (v.isExitDoorBeacon || v.isRingOnly) continue;
                if (v.id != _activeMarkerId) continue;
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

    const Projected* activeBeacon = nullptr;
    for (const auto& v : visible) {
        if (!v.isExitDoorBeacon) continue;
        if (v.offscreen) continue;
        if (MapDraw::PointInRect(mouse, MapDraw::PinBox(v.px, v.py))) { activeBeacon = &v; break; }
    }

    const float timeSec = std::chrono::duration<float>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    if (Settings::GetSingleton()->pathTrailVisible) {
        MapDraw::DrawPathTrail(dl, visibleSegments, headNearPlayer,
                               PathTracker::GetSingleton()->IsTracking(),
                               ImVec2(0.0f, 0.0f), 0.0f, 0.0f, false,
                               nullptr, 0, 1.0f,
                               &visibleSegmentZ, playerTrailZ);
    }

    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* cell   = player ? player->GetParentCell() : nullptr;
        if (cell && cell->IsInteriorCell()) {
            bool isTracking = PathTracker::GetSingleton()->IsTracking();
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

    for (const auto& v : visible) {
        if (v.isDestination && !v.isExitDoorBeacon) MapDraw::DrawDestinationRing(dl, v.px, v.py, timeSec);
    }

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
        if (v.isExitDoorBeacon) continue;
        if (v.isRingOnly) continue;
        const std::uint32_t drawIconType = v.isDestination ? 0u : v.iconType;
        auto* pinSrv = v.isDestination ? TextureManager::GetIconByName("destination")
                                       : TextureManager::GetCategoryIcon(v.iconType);
        MapDraw::DrawPin(dl, v.px, v.py, _hoverAnim[v.id], drawIconType, pinSrv,
                         Settings::GetSingleton()->markerIconScale,
                         Settings::GetSingleton()->PinTintCol());
    }

    for (const auto& v : visible) {
        if (!v.isExitDoorBeacon) continue;
        if (!v.offscreen) {
            dl->AddCircleFilled(ImVec2(v.px, v.py), 8.0f, MapTheme::kBeaconBorder);
            dl->AddCircle(ImVec2(v.px, v.py), 12.0f, MapTheme::kBeaconBorder, 0, 1.5f);
            dl->AddCircle(ImVec2(v.px, v.py), 16.0f, MapTheme::kBeaconBorder, 0, 1.5f);
        } else {
            const float inset = 18.0f;
            float cx = std::clamp(v.px, clipMin.x + inset, clipMax.x - inset);
            float cy = std::clamp(v.py, clipMin.y + inset, clipMax.y - inset);
            const float ax = v.dirX, ay = v.dirY;
            const float bx = -ay,   by = ax;
            ImVec2 tip(cx + ax * 10.0f, cy + ay * 10.0f);
            ImVec2 l(cx - ax * 6.0f + bx * 8.0f, cy - ay * 6.0f + by * 8.0f);
            ImVec2 r(cx - ax * 6.0f - bx * 8.0f, cy - ay * 6.0f - by * 8.0f);
            dl->AddTriangleFilled(tip, l, r, MapTheme::kBeaconBorder);
            dl->AddTriangle(tip, l, r, IM_COL32(0, 0, 0, 200), 1.5f);
            const std::string exitStr = LocalizationManager::Get("Navigation.Exit");
            ImVec2 ts = ImGui::CalcTextSize(exitStr.c_str());
            ImVec2 tp(cx - ax * 14.0f - ts.x * 0.5f, cy - ay * 14.0f - ts.y * 0.5f);
            tp.x = std::clamp(tp.x, clipMin.x + 2.0f, clipMax.x - ts.x - 2.0f);
            tp.y = std::clamp(tp.y, clipMin.y + 2.0f, clipMax.y - ts.y - 2.0f);
            dl->AddText(tp, MapTheme::kBeaconText, exitStr.c_str());
        }
    }

    if (active) {
        auto pl = MapDraw::ComputeInfoPanel(active->px, active->py, active->note.c_str(), dispSize, titleScale, bodyScale,
                                            static_cast<float>(scaleSet->markerHoverOffsetX),
                                            static_cast<float>(scaleSet->markerHoverOffsetY));
        MapDraw::DrawPanelCard(dl, pl.panel.mn, pl.panel.mx, scaleSet->BgCol(), scaleSet->BorderCol());
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

    if (mouse.x >= 0.0f && mouse.y >= 0.0f)
        MapDraw::RequestCursor(mouse.x, mouse.y);
}
