// Renders HUD glance mini-radar and full-screen dungeon inspect overlay.

// STATUS / TERMINOLOGY:

#include "MinimapRenderer.h"
#include "SettingsMenu.h"
#include "PathTracker.h"
#include "MarkerManager.h"
#include "Settings.h"
#include "InputHandler.h"
#include "LocalizationManager.h"
#include "MenuUtils.h"
#include "BeastForm.h"
#include "MapTheme.h"
#include "MapDrawHelpers.h"
#include "TextureManager.h"
#include "RenderTargetUpscaler.h"
#include "HUDMarkerHook.h"
#include "CompassHook.h"
#include "PartySheetState.h"
#include "FontManager.h"

#include <imgui.h>
#include <d3d11.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <unordered_set>

#include <SKSE/SKSE.h>

namespace {
    constexpr float kInspectMoveRefreshDist = 600.0f;
    constexpr float kInspectRefreshSeconds  = 2.0f;

    constexpr long long kSpaceSettleMs = 5000;
    constexpr float kSpaceSettleRetrySeconds = 0.5f;

    constexpr long long kSpaceFadeMaxHoldMs = 5000;

    std::vector<const MapMarker*> CollectVisibleMarkers(
        const std::vector<MapMarker>& a_markers, bool a_isExterior,
        RE::FormID a_worldSpaceID, RE::FormID a_cellID, const RE::NiPoint3& a_playerPos,
        bool a_applyCountCap)
    {
        auto* settings = Settings::GetSingleton();
        const float rangeSq = settings->navMarkerRange * settings->navMarkerRange;
        const int   maxCustom = a_applyCountCap ? settings->navMaxCustomMarkers : 0;
        auto* playerWorld = a_isExterior
            ? RE::TESForm::LookupByID<RE::TESWorldSpace>(a_worldSpaceID)
            : nullptr;
        std::vector<const MapMarker*> out;
        std::vector<std::pair<float, const MapMarker*>> ranged;
        for (const auto& m : a_markers) {
            const bool sameSpace =
                a_isExterior ? CompassHook::WorldMatches(playerWorld, m.worldSpaceID)
                             : (m.worldSpaceID == 0 && m.cellID == a_cellID);
            if (!sameSpace) continue;
            if (m.isDestination) {
                out.push_back(&m);
                continue;
            }
            const float dx = m.position.x - a_playerPos.x;
            const float dy = m.position.y - a_playerPos.y;
            const float dSq = dx * dx + dy * dy;
            if (dSq > rangeSq) continue;
            ranged.emplace_back(dSq, &m);
        }
        if (maxCustom > 0 && ranged.size() > static_cast<std::size_t>(maxCustom)) {
            std::partial_sort(ranged.begin(), ranged.begin() + maxCustom, ranged.end(),
                              [](const auto& a, const auto& b) { return a.first < b.first; });
            ranged.resize(static_cast<std::size_t>(maxCustom));
        }
        for (const auto& r : ranged) out.push_back(r.second);
        return out;
    }

    bool SnapshotIsStale(const MinimapRenderer::SnapshotProjection& snap,
                         const MinimapRenderer::NavigationFrame& frame,
                         std::chrono::steady_clock::time_point a_spaceEnteredAt) {
        if (!snap.valid) return true;
        if (frame.isExterior || snap.isExterior) {
            if (snap.isExterior != frame.isExterior || snap.worldSpaceID != frame.worldSpaceID) return true;
        } else if (snap.cellID != frame.cellID) {
            return true;
        }
        const float distSq = frame.playerPos.GetSquaredDistance(snap.playerPos);
        float moveThreshold = kInspectMoveRefreshDist;
        if (frame.isExterior && snap.frustumHalfWidth > 0.0f) {
            moveThreshold = std::min(moveThreshold, snap.frustumHalfWidth * 0.25f);
        }
        if (distSq > moveThreshold * moveThreshold) return true;
        const auto now = std::chrono::steady_clock::now();
        const auto sinceSpaceEntry = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         now - a_spaceEnteredAt).count();
        const bool settling = sinceSpaceEntry < kSpaceSettleMs;
        if (!settling && distSq < 16.0f * 16.0f) return false;
        const auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - snap.takenAt).count();
        const float refreshSeconds = settling ? kSpaceSettleRetrySeconds
                                              : kInspectRefreshSeconds;
        return diff > refreshSeconds * 1000.0f;
    }

    ImVec2 WorldToGlance(const RE::NiPoint3& a_world,
                         const RE::NiPoint3& a_player,
                         float a_scale,
                         float a_northRot = 0.0f) {
        const float dx = a_world.x - a_player.x;
        const float dy = a_world.y - a_player.y;
        const float ca = std::cos(a_northRot);
        const float sa = std::sin(a_northRot);
        const float rx = dx * ca - dy * sa;
        const float ry = dx * sa + dy * ca;
        return ImVec2(rx * a_scale, -ry * a_scale);
    }

    bool TrailHeadNearPlayer(const std::vector<PathTracker::Segment>& a_segments,
                             const RE::NiPoint3& a_player) {
        if (a_segments.empty() || a_segments.back().empty()) return false;
        const auto& head = a_segments.back().back();
        const float dx = a_player.x - head.x;
        const float dy = a_player.y - head.y;
        const float dz = a_player.z - head.z;
        return (dx * dx + dy * dy + dz * dz) < (250.0f * 250.0f);
    }

    ImU32 FadeCol(ImU32 c, float a) {
        const auto ca = static_cast<ImU32>(((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
        return (c & ~IM_COL32_A_MASK) | (ca << IM_COL32_A_SHIFT);
    }

    void DrawQuestTargetIcon(ImDrawList* dl, ID3D11ShaderResourceView* srv,
                             float x, float y, float halfPx, float a_alpha) {
        if (!srv) return;
        dl->AddImage(reinterpret_cast<ImTextureID>(srv),
                     ImVec2(x - halfPx, y - halfPx), ImVec2(x + halfPx, y + halfPx),
                     ImVec2(0, 0), ImVec2(1, 1), FadeCol(IM_COL32_WHITE, a_alpha));
    }

    float SmoothstepLocal(float t) { return t * t * (3.0f - 2.0f * t); }

    constexpr float kViewConeHalf = 0.6108652f;

    void DrawViewCone(ImDrawList* dl, const ImVec2& a_origin, float a_mapAngle, float a_radius,
                       float a_alpha = 1.0f) {
        const float theta = a_mapAngle - 1.5707963f;
        dl->PathLineTo(a_origin);
        dl->PathArcTo(a_origin, a_radius, theta - kViewConeHalf, theta + kViewConeHalf, 16);
        dl->PathFillConvex(FadeCol(IM_COL32(255, 255, 255, 26), a_alpha));
        const ImVec2 e1(a_origin.x + std::cos(theta - kViewConeHalf) * a_radius,
                        a_origin.y + std::sin(theta - kViewConeHalf) * a_radius);
        const ImVec2 e2(a_origin.x + std::cos(theta + kViewConeHalf) * a_radius,
                        a_origin.y + std::sin(theta + kViewConeHalf) * a_radius);
        dl->AddLine(a_origin, e1, FadeCol(IM_COL32(255, 255, 255, 60), a_alpha), 1.0f);
        dl->AddLine(a_origin, e2, FadeCol(IM_COL32(255, 255, 255, 60), a_alpha), 1.0f);
    }

    bool ProjectWithSnapshot(const MinimapRenderer::SnapshotProjection& s,
                             const RE::NiPoint3& world,
                             const ImVec2& imageMin,
                             const ImVec2& imageSize,
                             ImVec2& out) {
        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
        const bool ok = RE::NiCamera::WorldPtToScreenPt3(
            s.worldToCam, s.port, world, sx, sy, sz, 1e-5f);
        if (!ok || !MapDraw::ProjectionDepthValid(sz)) return false;
        out = ImVec2(imageMin.x + sx * imageSize.x,
                     imageMin.y + (1.0f - sy) * imageSize.y);
        return true;
    }

    static constexpr std::size_t kCullingOffset = 0x40;
    static constexpr std::size_t kBlockSize = 0x30500;
    static constexpr std::size_t kCameraOffsetFromMenu = 0x302a0;

    using CtorFn   = void*(*)(void*);
    using RenderFn = void (*)(void*);
    using CamBeginFn  = void (*)(void*);
    using CamTargetFn = void (*)(void*, const float*);
    using CamBoundsFn = void (*)(void*, const float*, const float*);
    using CamApplyFn  = void (*)(void*, float);

    using CamFrustumFn = void (*)(void*, float, float);
    using InitNodeFn   = void (*)(void*);

    using MenuBlockCtorFn = void (*)(void*);
    static REL::Relocation<MenuBlockCtorFn> g_menuBlockCtor{ REL::RelocationID(52076, 52964) };

    static REL::Relocation<CtorFn>       g_ctor      { REL::RelocationID(16092, 16333) };
    static REL::Relocation<RenderFn>     g_render    { REL::RelocationID(16094, 16335) };
    static REL::Relocation<CamBeginFn>   g_camBegin  { REL::RelocationID(16086, 16327) };
    static REL::Relocation<CamTargetFn>  g_camTarget { REL::RelocationID(16087, 16328) };
    static REL::Relocation<CamBoundsFn>  g_camBounds { REL::RelocationID(16090, 16331) };
    static REL::Relocation<CamApplyFn>   g_camApply  { REL::RelocationID(16089, 16330) };
    static REL::Relocation<CamFrustumFn> g_camFrustum{ REL::RelocationID(16091, 16332) };
    static REL::Relocation<InitNodeFn>   g_initNode  { REL::RelocationID(16095, 16336) };

    static std::uint8_t* g_block      = nullptr;
    static bool          g_constructed = false;

    static void* CullingBase() { return g_block + kCullingOffset; }
    static void* CameraBase()  { return g_block + kCameraOffsetFromMenu; }

    void LogTextureDesc(ID3D11ShaderResourceView* a_srv) {
        if (!a_srv) {
            SKSE::log::warn("MinimapRenderer: DIAG srv null, no texture desc");
            return;
        }
        ID3D11Resource* res = nullptr;
        a_srv->GetResource(&res);
        if (!res) {
            SKSE::log::warn("MinimapRenderer: DIAG srv has no resource");
            return;
        }
        ID3D11Texture2D* tex = nullptr;
        res->QueryInterface(IID_PPV_ARGS(&tex));
        if (tex) {
            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);
            SKSE::log::debug("MinimapRenderer: DIAG texture {}x{} format={} usage={} bind={:#x}",
                desc.Width, desc.Height, static_cast<int>(desc.Format),
                static_cast<int>(desc.Usage), desc.BindFlags);
            tex->Release();
        }
        res->Release();
    }

    static void EnsureConstructed() {
        if (g_constructed) return;
        g_block = static_cast<std::uint8_t*>(RE::malloc(kBlockSize));
        if (!g_block) {
            SKSE::log::error("MinimapRenderer: malloc({:#x}) failed", kBlockSize);
            return;
        }
        std::memset(g_block, 0, kBlockSize);
        g_menuBlockCtor(g_block);
        g_initNode(CullingBase());
        g_camFrustum(CameraBase(), 4000.0f, 2250.0f);
        g_constructed = true;
        SKSE::log::info("MinimapRenderer: menu block constructed + node/frustum init");
    }
}

MinimapRenderer::NavigationFrame MinimapRenderer::BuildFrame() {
    NavigationFrame f;
    if (MenuUtils::ShouldBlockOverlay()) return f;

    if (Settings::GetSingleton()->hideInBeastForm && BeastForm::IsBeastForm()) return f;

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return f;

    auto* cell = player->GetParentCell();
    if (!cell) return f;

    auto* ws = player->GetWorldspace();
    f.isExterior = cell->IsExteriorCell();
    auto* navSettings = Settings::GetSingleton();
    const bool anyVisible = f.isExterior
        ? (navSettings->navGlanceExteriorVisible || navSettings->navInspectExteriorVisible)
        : (navSettings->navGlanceInteriorVisible || navSettings->navInspectInteriorVisible);
    if (f.isExterior && !ws) return f;
    if (!anyVisible) return f;
    f.worldSpaceID = f.isExterior ? ws->GetFormID() : 0;

    f.valid = true;
    f.cellID = cell->GetFormID();

    if (f.isExterior) {
        const char* n = nullptr;
        if (auto* loc = player->GetCurrentLocation()) n = loc->GetFullName();
        if ((!n || !*n) && ws) n = ws->GetFullName();
        f.locationName = (n && *n) ? n : "";
    } else {
        const char* n = cell->GetFullName();
        f.locationName = (n && *n) ? n : "";
    }
    f.playerPos = player->GetPosition();
    f.playerAngle = player->data.angle.z;

    f.cameraAngle = f.playerAngle;
    if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
        const auto& r = cam->cameraRoot->world.rotate;
        f.cameraAngle = std::atan2(r.entry[0][1], r.entry[1][1]);
    }

    f.segments = f.isExterior ? decltype(f.segments){} : PathTracker::GetSingleton()->GetSegments(f.cellID);
    f.markers = MarkerManager::GetSingleton()->GetMarkers();

    f.questTargets = HUDMarkerHook::GetQuestMarkerPositions();

    const bool inspectVisibleHere = f.isExterior ? navSettings->navInspectExteriorVisible
                                                  : navSettings->navInspectInteriorVisible;

    const bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    const int inspectVk = InputHandler::DIKToVK(Settings::GetSingleton()->navInspectKey);
    f.inspectHeld = !SettingsMenu::IsVisible() && inspectVisibleHere && !ctrlHeld && inspectVk != 0 &&
                     (GetAsyncKeyState(inspectVk) & 0x8000) != 0;
    return f;
}

void MinimapRenderer::Draw() {
    if (PartySheetState::ShouldHideOverlay()) {
        _inspectShown.store(false);
        _inspectWasHeld = false;
        _toggleWasDown = false;
        return;
    }

    const auto frame = BuildFrame();
    _inspectShown.store(frame.valid && frame.inspectHeld);
    if (!frame.valid) {
        _inspectWasHeld = false;
        return;
    }

    {
        const std::uint32_t space = frame.isExterior ? frame.worldSpaceID : frame.cellID;
        if (space != _lastDrawnSpace || frame.isExterior != _lastDrawnExterior) {
            const bool firstEverDraw = _lastDrawnSpace == 0;
            _lastDrawnSpace = space;
            _lastDrawnExterior = frame.isExterior;
            _spaceEnteredAt = std::chrono::steady_clock::now();
            if (!firstEverDraw) {
                _glanceFade = 0.0f;
                _spaceFadeHold = true;
            }
        }
    }

    const bool inspectEdge = frame.inspectHeld && !_inspectWasHeld;
    const bool inspectReleased = !frame.inspectHeld && _inspectWasHeld;
    _inspectWasHeld = frame.inspectHeld;

    const bool glanceVisibleHere = frame.isExterior ? Settings::GetSingleton()->navGlanceExteriorVisible
                                                     : Settings::GetSingleton()->navGlanceInteriorVisible;
    if (inspectReleased && glanceVisibleHere &&
        Settings::GetSingleton()->navRefreshOnInspect) {
        RequestSnapshotRefresh(frame, "glance reframe after inspect");
    }

    if (!_inspectModeInited) {
        _inspectMode.store(Settings::GetSingleton()->navInspectDefaultMode == 1
                               ? InspectViewMode::Radar
                               : InspectViewMode::Cover);
        _inspectModeInited = true;
    }

    if (frame.isExterior) {
        if (frame.inspectHeld && _inspectMode.load() != InspectViewMode::Radar) {
            _inspectMode.store(InspectViewMode::Radar);
            RequestSnapshotRefresh(frame, "exterior forces radar mode");
        }
        _toggleWasDown = false;
    } else if (frame.inspectHeld) {
        const int toggleVk =
            InputHandler::DIKToVK(Settings::GetSingleton()->navInspectModeToggleKey);
        const bool toggleDown = toggleVk != 0 && (GetAsyncKeyState(toggleVk) & 0x8000) != 0;
        if (toggleDown && !_toggleWasDown) {
            const auto cur = _inspectMode.load();
            _inspectMode.store(cur == InspectViewMode::Cover ? InspectViewMode::Radar
                                                             : InspectViewMode::Cover);
            RequestSnapshotRefresh(frame, "inspect mode toggle");
        }
        _toggleWasDown = toggleDown;
    } else {
        _toggleWasDown = false;
    }

    if (frame.inspectHeld) {
        const int markVk = InputHandler::DIKToVK(Settings::GetSingleton()->navQuickMarkKey);
        const bool markDown = markVk != 0 && (GetAsyncKeyState(markVk) & 0x8000) != 0;
        if (markDown && !_quickMarkWasDown) {
            const auto now = std::chrono::steady_clock::now();
            const auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - _lastQuickMarkTime).count();
            if (sinceLast > 500) {
                if (frame.isExterior) {
                    const std::uint32_t assignedId = MarkerManager::GetSingleton()->AddAutoNamedMarker(
                        frame.worldSpaceID, frame.cellID, frame.playerPos,
                        0, AutoName::Custom);
                    if (assignedId == 0) {
                        SKSE::log::warn("MinimapRenderer: quick mark placement rejected by MarkerManager");
                    } else {
                        _lastQuickMarkTime = now;
                        _quickMarkToastText = LocalizationManager::Get("Navigation.MarkerAdded");
                        _quickMarkToastTime = now;
                        SKSE::log::info("MinimapRenderer: quick mark placed (exterior, default pin) id={} worldSpace={:08X} cell={:08X}",
                            assignedId, frame.worldSpaceID, frame.cellID);
                    }
                } else {
                    std::uint32_t maxN = 0;
                    for (const auto& m : MarkerManager::GetSingleton()->GetUserMarkers()) {
                        const bool sameScope = m.worldSpaceID == 0 && m.cellID == frame.cellID;
                        if (sameScope && m.iconType >= MapDraw::kQuickMarkBase) {
                            maxN = std::max(maxN, m.iconType - MapDraw::kQuickMarkBase + 1);
                        }
                    }
                    const std::uint32_t number = maxN + 1;
                    const std::uint32_t assignedId = MarkerManager::GetSingleton()->AddAutoNamedMarker(
                        frame.worldSpaceID, frame.cellID, frame.playerPos,
                        MapDraw::kQuickMarkBase + number - 1,
                        AutoName::QuickMark, number);
                    if (assignedId == 0) {
                        SKSE::log::warn("MinimapRenderer: numbered quick mark placement rejected by MarkerManager");
                    } else {
                        _lastQuickMarkTime = now;
                        _quickMarkToastText = LocalizationManager::Format(
                            "Navigation.MarkerAddedNumbered", std::to_string(number));
                        _quickMarkToastTime = now;
                        SKSE::log::info("MinimapRenderer: quick mark placed number={} id={} worldSpace={:08X} cell={:08X}",
                            number, assignedId, frame.worldSpaceID, frame.cellID);
                    }
                }
            }
        }
        _quickMarkWasDown = markDown;
    } else {
        _quickMarkWasDown = false;
    }

    if (!Settings::GetSingleton()->navRefreshOnInspect) {
        if (inspectEdge) {
            std::lock_guard<std::mutex> lock(_snapshotMutex);
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (renderer) {
                _snapshot.srv = reinterpret_cast<ID3D11ShaderResourceView*>(
                    renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kLOCAL_MAP].SRV);
                _snapshot.valid = _snapshot.srv != nullptr;
                _snapshot.stale = false;
                _snapshot.cellID = frame.cellID;
                _snapshot.takenAt = std::chrono::steady_clock::now();
            }
            SKSE::log::debug("MinimapRenderer: DIAG frozen-texture mode, srv={}",
                static_cast<void*>(_snapshot.srv));
            LogTextureDesc(_snapshot.srv);
        }
    } else if (inspectEdge) {
        RequestSnapshotRefresh(frame, "inspect edge");
    } else if (frame.inspectHeld) {
        SnapshotProjection snap;
        {
            std::lock_guard<std::mutex> lock(_snapshotMutex);
            snap = _snapshot;
        }
        if (SnapshotIsStale(snap, frame, _spaceEnteredAt)) {
            RequestSnapshotRefresh(frame, "inspect stale");
        }
    } else if (glanceVisibleHere) {
        SnapshotProjection snap;
        {
            std::lock_guard<std::mutex> lock(_snapshotMutex);
            snap = _snapshot;
        }
        if (SnapshotIsStale(snap, frame, _spaceEnteredAt)) {
            RequestSnapshotRefresh(frame, "glance stale");
        }
    }

    {
        if (_spaceFadeHold) {
            SnapshotProjection snap;
            {
                std::lock_guard<std::mutex> lock(_snapshotMutex);
                snap = _snapshot;
            }
            const bool sameSpace = frame.isExterior
                ? (snap.isExterior && snap.worldSpaceID == frame.worldSpaceID)
                : (!snap.isExterior && snap.cellID == frame.cellID);
            if (snap.valid && snap.srv && sameSpace) {
                _spaceFadeHold = false;
            } else {
                const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - _spaceEnteredAt).count();
                if (held > kSpaceFadeMaxHoldMs) _spaceFadeHold = false;
            }
        }
    }

    constexpr float kFadeSeconds = 0.15f;

    constexpr float kSpaceFadeSeconds = 0.35f;
    const float dt = ImGui::GetIO().DeltaTime;
    const float rate = std::clamp((kFadeSeconds > 1e-4f) ? (dt / kFadeSeconds) : 1.0f, 0.0f, 1.0f);
    const float glanceRate = _glanceFade < 1.0f && !frame.inspectHeld && !_spaceFadeHold
        ? std::clamp(dt / kSpaceFadeSeconds, 0.0f, 1.0f)
        : rate;

    const float glanceTarget = (frame.inspectHeld || _spaceFadeHold) ? 0.0f : 1.0f;
    const float inspectTarget = frame.inspectHeld ? 1.0f : 0.0f;
    const float glanceStep = std::clamp(glanceTarget - _glanceFade, -glanceRate, glanceRate);
    const float inspectStep = std::clamp(inspectTarget - _inspectFade, -rate, rate);
    _glanceFade = std::clamp(_glanceFade + glanceStep, 0.0f, 1.0f);
    _inspectFade = std::clamp(_inspectFade + inspectStep, 0.0f, 1.0f);

    const float glanceAlpha = SmoothstepLocal(_glanceFade);
    const float inspectAlpha = SmoothstepLocal(_inspectFade);

    constexpr float kRetraceFadeSeconds = 0.25f;
    const float retraceRate = std::clamp((kRetraceFadeSeconds > 1e-4f) ? (dt / kRetraceFadeSeconds) : 1.0f, 0.0f, 1.0f);
    const float retraceTarget = PathTracker::GetSingleton()->GetRecentRevisits(0.6f).empty() ? 0.0f : 1.0f;
    const float retraceStep = std::clamp(retraceTarget - _retraceFade, -retraceRate, retraceRate);
    _retraceFade = std::clamp(_retraceFade + retraceStep, 0.0f, 1.0f);

    {
        const float now = static_cast<float>(ImGui::GetTime());
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player && player->IsInCombat()) _lastCombatTime = now;
        const float combatTarget = (now - _lastCombatTime < 1.5f) ? 1.0f : 0.0f;
        constexpr float kCombatFadeSeconds = 0.5f;
        const float combatRate = std::clamp(dt / kCombatFadeSeconds, 0.0f, 1.0f);
        const float combatStep = std::clamp(combatTarget - _combatFade, -combatRate, combatRate);
        _combatFade = std::clamp(_combatFade + combatStep, 0.0f, 1.0f);
    }

    CollectActorsIfDue(frame);
    MergeActorGhosts();

    if (glanceVisibleHere && glanceAlpha > 0.0f) {
        DrawGlance(frame, glanceAlpha);
    }
    if (inspectAlpha > 0.0f) {
        DrawInspect(frame, inspectAlpha);
    }
}

void MinimapRenderer::MergeActorGhosts() {
    const auto now = std::chrono::steady_clock::now();

    const bool newScan = _lastActorScanAt != _lastMergedScanAt;
    if (newScan) {
        _lastMergedScanAt = _lastActorScanAt;
        for (const auto& a : _lastActorScan) {
            auto& ghost = _actorGhosts[a.formID];
            ghost.position   = a.position;
            ghost.allegiance = a.allegiance;
            ghost.lastSeen   = now;
            ghost.frozen     = false;
        }
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    const RE::NiPoint3 playerPos = player ? player->GetPosition() : RE::NiPoint3();

    auto* st = Settings::GetSingleton();
    const float r = st ? st->navActorProximityUnits : ActorMarkerSource::kActorProximityRadius;
    const float r2 = r * r;
    for (auto& [formID, ghost] : _actorGhosts) {
        if (ghost.frozen) continue;
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);
        if (actor && actor->Is3DLoaded() && !actor->IsDead()) {
            const auto pos = actor->GetPosition();
            if (player) {
                const float dx = pos.x - playerPos.x;
                const float dy = pos.y - playerPos.y;
                if (dx * dx + dy * dy > r2) {
                    ghost.frozen   = true;
                    ghost.lastSeen = now;
                    continue;
                }
            }
            ghost.position = pos;
        } else {
            ghost.frozen   = true;
            ghost.lastSeen = now;
        }
    }

    if (newScan) {
        std::unordered_set<RE::FormID> seenThisScan;
        seenThisScan.reserve(_lastActorScan.size());
        for (const auto& a : _lastActorScan) seenThisScan.insert(a.formID);
        for (auto& [formID, ghost] : _actorGhosts) {
            if (seenThisScan.count(formID)) continue;
            if (!ghost.frozen) {
                ghost.frozen   = true;
                ghost.lastSeen = now;
            }
        }
    }

    constexpr float kActorFadeInSeconds  = 0.3f;
    constexpr float kActorFadeOutSeconds = 1.0f;
    float dt = std::chrono::duration<float>(now - _lastGhostTick).count();
    _lastGhostTick = now;
    dt = std::clamp(dt, 0.0f, 0.1f);
    for (auto it = _actorGhosts.begin(); it != _actorGhosts.end(); ) {
        auto& ghost = it->second;
        if (ghost.frozen) {
            ghost.alpha -= dt / kActorFadeOutSeconds;
            if (ghost.alpha <= 0.0f) { it = _actorGhosts.erase(it); continue; }
        } else {
            ghost.alpha = std::min(ghost.alpha + dt / kActorFadeInSeconds, 1.0f);
        }
        ++it;
    }
}

void MinimapRenderer::CollectActorsIfDue(const NavigationFrame& a_frame) {
    auto* navSettings = Settings::GetSingleton();
    if (!navSettings->navActorAnyVisible()) return;

    const auto now = std::chrono::steady_clock::now();
    if (_lastActorScanAt.time_since_epoch().count() != 0 &&
        std::chrono::duration<float>(now - _lastActorScanAt).count() < kActorScanSeconds) {
        return;
    }

    if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
        return;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->GetParentCell()) return;

    if (a_frame.isExterior) {
        _lastActorScan = ActorMarkerSource::Collect(
            player, navSettings->navInspectRadarRangeUnits, a_frame.playerPos);
    } else if (auto* cell = player->GetParentCell()) {
        _lastActorScan = ActorMarkerSource::Collect(cell, a_frame.playerPos);
    } else {
        _lastActorScan.clear();
    }
    _lastActorScanAt = now;

}

void MinimapRenderer::RequestSnapshotRefresh(const NavigationFrame& a_frame, const char* a_reason) {
    if (!_refreshPending.exchange(true)) {
    }
    (void)a_reason;

    if (auto ui = RE::UI::GetSingleton()) {
        if (auto hud = ui->GetMenu(RE::HUDMenu::MENU_NAME)) {
            hud->menuFlags.set(RE::UI_MENU_FLAGS::kRendersOffscreenTargets);
        }
    }
}

void MinimapRenderer::DrawGlance(const NavigationFrame& f, float a_alpha) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    auto* settings = Settings::GetSingleton();

    const float kGlanceRadius = settings->navGlanceSize * 0.5f;

    const float glanceScale = kGlanceRadius / settings->navGlanceRangeUnits;

    const float posX = std::clamp(static_cast<float>(settings->navGlancePosX), 0.0f, disp.x - 2.0f * kGlanceRadius);
    const float posY = std::clamp(static_cast<float>(settings->navGlancePosY), 0.0f, disp.y - 2.0f * kGlanceRadius);
    const ImVec2 center(posX + kGlanceRadius, posY + kGlanceRadius);

    dl->AddCircleFilled(center, kGlanceRadius, FadeCol(IM_COL32(10, 12, 14, 175), a_alpha), 64);

    SnapshotProjection snap;
    {
        std::lock_guard<std::mutex> lock(_snapshotMutex);
        snap = _snapshot;
    }
    {
        const bool snapSameSpace = f.isExterior
            ? (snap.isExterior && snap.worldSpaceID == f.worldSpaceID)
            : (!snap.isExterior && snap.cellID == f.cellID);
        if (snap.valid && snap.srv && snapSameSpace) {
            const float glanceWorldRadius = settings->navGlanceRangeUnits;
            ImVec2 uvCenter;
            const bool haveCenter = ProjectWithSnapshot(
                snap, f.playerPos, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), uvCenter);
            RE::NiPoint3 edgeWorldX = f.playerPos;
            edgeWorldX.x += glanceWorldRadius;
            RE::NiPoint3 edgeWorldY = f.playerPos;
            edgeWorldY.y += glanceWorldRadius;
            ImVec2 uvEdgeX, uvEdgeY;
            const bool haveEdgeX = ProjectWithSnapshot(
                snap, edgeWorldX, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), uvEdgeX);
            const bool haveEdgeY = ProjectWithSnapshot(
                snap, edgeWorldY, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), uvEdgeY);
            if (haveCenter && haveEdgeX && haveEdgeY) {
                const float Ax = uvEdgeX.x - uvCenter.x;
                const float Ay = uvEdgeX.y - uvCenter.y;
                const float Bx = uvEdgeY.x - uvCenter.x;
                const float By = uvEdgeY.y - uvCenter.y;
                const float uHalf = std::sqrt(Ax * Ax + Bx * Bx);
                const float vHalf = std::sqrt(Ay * Ay + By * By);
                if (uHalf > 1e-4f && vHalf > 1e-4f) {
                    ImVec2 uv0(uvCenter.x - uHalf, uvCenter.y - vHalf);
                    ImVec2 uv1(uvCenter.x + uHalf, uvCenter.y + vHalf);
                    uv0.x = std::clamp(uv0.x, 0.0f, 1.0f);
                    uv0.y = std::clamp(uv0.y, 0.0f, 1.0f);
                    uv1.x = std::clamp(uv1.x, 0.0f, 1.0f);
                    uv1.y = std::clamp(uv1.y, 0.0f, 1.0f);
                    const ImVec2 imgMin(center.x - kGlanceRadius, center.y - kGlanceRadius);
                    const ImVec2 imgMax(center.x + kGlanceRadius, center.y + kGlanceRadius);
                    dl->AddImageRounded(reinterpret_cast<ImTextureID>(snap.srv),
                                        imgMin, imgMax, uv0, uv1,
                                        FadeCol(MapDraw::kGlanceBackdropTint, a_alpha),
                                        kGlanceRadius, ImDrawFlags_RoundCornersAll);
                    if (!_glanceBackdropActive) {
                        _glanceBackdropActive = true;
                        SKSE::log::info(
                            "MinimapRenderer: Glance backdrop active. worldRadius={} uvCenter=({:.3f},{:.3f}) uHalf={:.3f} vHalf={:.3f} uv=({:.3f},{:.3f})-({:.3f},{:.3f})",
                            glanceWorldRadius, uvCenter.x, uvCenter.y, uHalf, vHalf,
                            uv0.x, uv0.y, uv1.x, uv1.y);
                    }
                }
            }
        } else {
            _glanceBackdropActive = false;
        }
    }

    const bool glanceBackdropShown = snap.valid && snap.srv &&
        (f.isExterior ? (snap.isExterior && snap.worldSpaceID == f.worldSpaceID)
                      : (!snap.isExterior && snap.cellID == f.cellID));
    const float glanceNorthRot = glanceBackdropShown ? snap.northRot : 0.0f;

    {
        const float combatT = SmoothstepLocal(_combatFade);
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.5f);
        const float redT = combatT * (0.45f + 0.55f * pulse);
        const ImU32 rimCol = MapDraw::LerpColor(Settings::GetSingleton()->BorderCol(),
                                                MapTheme::kCombatBorder, redT);
        const float rimW = 1.5f + 1.0f * combatT * pulse;
        dl->AddCircle(center, kGlanceRadius, FadeCol(rimCol, a_alpha), 64, rimW);
    }

    std::vector<std::vector<ImVec2>> visibleSegments;

    std::vector<std::vector<float>> visibleSegmentZ;
    visibleSegments.reserve(f.segments.size());
    visibleSegmentZ.reserve(f.segments.size());
    for (const auto& seg : f.segments) {
        std::vector<ImVec2> pts;
        std::vector<float>  ptsZ;
        pts.reserve(seg.size());
        ptsZ.reserve(seg.size());
        for (const auto& p : seg) {
            ImVec2 rp = WorldToGlance(p, f.playerPos, glanceScale, glanceNorthRot);
            pts.emplace_back(center.x + rp.x, center.y + rp.y);
            ptsZ.push_back(p.z);
        }
        if (!pts.empty()) {
            visibleSegments.push_back(std::move(pts));
            visibleSegmentZ.push_back(std::move(ptsZ));
        }
    }

    if (Settings::GetSingleton()->navGlanceShowTrail) {
        MapDraw::DrawPathTrail(dl, visibleSegments,
                               TrailHeadNearPlayer(f.segments, f.playerPos),
                               PathTracker::GetSingleton()->IsTracking(),
                               center, kGlanceRadius,
                               0.0f, true, nullptr,
                               0,
                               a_alpha,
                               &visibleSegmentZ, f.playerPos.z);
    }

    const bool glanceSnapSameSpace = f.isExterior
        ? (snap.isExterior && snap.worldSpaceID == f.worldSpaceID)
        : (!snap.isExterior && snap.cellID == f.cellID);
    if (snap.valid && glanceSnapSameSpace) {
        auto* s = Settings::GetSingleton();
        if (s->GlanceAnyEntity()) {
            const float rimInset = kGlanceRadius - 4.0f;
            const float rimSq = rimInset * rimInset;
            const float sizeScale = static_cast<float>(s->navGlanceEntityIconSize) / 7.0f;
            const float half = static_cast<float>(s->navGlanceEntityIconSize) * 0.5f;
            for (const auto& e : snap.entities) {
                if (!e.discovered) continue;
                if (!s->GlanceSubEnabled(e.category, e.subCategory)) continue;
                ImVec2 rp = WorldToGlance(e.position, f.playerPos, glanceScale, glanceNorthRot);
                if (rp.x * rp.x + rp.y * rp.y > rimSq) continue;
                ImVec2 g(center.x + rp.x, center.y + rp.y);
                ID3D11ShaderResourceView* icon = nullptr;
                if (const char* sub = EntityMarkerSource::IconNameFor(e.category, e.subCategory))
                    icon = TextureManager::GetIconByName(sub);
                if (!icon)
                    icon = TextureManager::GetIconByName(EntityMarkerSource::CategoryIconName(e.category));
                if (icon) {
                    MapDraw::DrawIconFx(dl, icon,
                                        ImVec2(g.x - half, g.y - half),
                                        ImVec2(g.x + half, g.y + half),
                                        FadeCol(IM_COL32_WHITE, a_alpha));
                    continue;
                }
                using Cat = EntityMarkerSource::Category;
                switch (e.category) {
                    case Cat::Corpse: {
                        const float arm = 3.0f * sizeScale;
                        dl->AddLine(ImVec2(g.x - arm, g.y - arm), ImVec2(g.x + arm, g.y + arm), FadeCol(MapTheme::kEntityCorpse, a_alpha), 1.5f);
                        dl->AddLine(ImVec2(g.x - arm, g.y + arm), ImVec2(g.x + arm, g.y - arm), FadeCol(MapTheme::kEntityCorpse, a_alpha), 1.5f);
                        break;
                    }
                    case Cat::Door: {
                        const float hw = 1.75f * sizeScale, hh = 3.25f * sizeScale;
                        dl->AddRectFilled(ImVec2(g.x - hw, g.y - hh), ImVec2(g.x + hw, g.y + hh),
                                          FadeCol(MapTheme::kEntityDoor, a_alpha), 1.0f);
                        break;
                    }
                    case Cat::Container: {
                        const float hs = 2.5f * sizeScale;
                        dl->AddRectFilled(ImVec2(g.x - hs, g.y - hs), ImVec2(g.x + hs, g.y + hs),
                                          FadeCol(MapTheme::kEntityContainer, a_alpha));
                        dl->AddRect(ImVec2(g.x - hs, g.y - hs), ImVec2(g.x + hs, g.y + hs),
                                    FadeCol(IM_COL32(20, 15, 5, 235), a_alpha));
                        break;
                    }
                    case Cat::LooseLoot: {
                        MapDraw::DrawDotFx(dl, g, 3.0f * sizeScale, FadeCol(MapTheme::kEntityLoot, a_alpha));
                        break;
                    }
                    case Cat::Ingredient: {
                        MapDraw::DrawDotFx(dl, g, 3.0f * sizeScale, FadeCol(MapTheme::kEntityIngredient, a_alpha));
                        break;
                    }
                }
            }
        }
    }

    if (Settings::GetSingleton()->navActorAnyVisible()) {
        auto* s = Settings::GetSingleton();
        for (const auto& [formID, ghost] : _actorGhosts) {
            using Al = ActorMarkerSource::ActorMarker::Allegiance;
            if (Al::Ally == ghost.allegiance && !s->navActorShowAlly) continue;
            if (Al::Hostile == ghost.allegiance && !s->navActorShowHostile) continue;
            if (Al::Neutral == ghost.allegiance && !s->navActorShowNeutral) continue;
            if (Al::Animal == ghost.allegiance && !s->navActorShowAnimal) continue;
            ImVec2 rp = WorldToGlance(ghost.position, f.playerPos, glanceScale, glanceNorthRot);
            if (rp.x * rp.x + rp.y * rp.y > kGlanceRadius * kGlanceRadius) continue;
            ImVec2 g(center.x + rp.x, center.y + rp.y);
            const ImU32 col = Al::Ally == ghost.allegiance ? MapTheme::kActorAlly
                             : Al::Hostile == ghost.allegiance ? MapTheme::kActorHostile
                             : Al::Animal == ghost.allegiance ? MapTheme::kActorAnimal
                             : MapTheme::kActorNeutral;
            MapDraw::DrawDotFx(dl, g, static_cast<float>(s->navGlanceActorDotSize), FadeCol(col, a_alpha * ghost.alpha));
        }
    }

    for (const MapMarker* mp : CollectVisibleMarkers(f.markers, f.isExterior,
                                                     f.worldSpaceID, f.cellID, f.playerPos,
                                                     true)) {
        const MapMarker& m = *mp;
        ImVec2 rp = WorldToGlance(m.position, f.playerPos, glanceScale, glanceNorthRot);
        float distSq = rp.x * rp.x + rp.y * rp.y;
        if (distSq > kGlanceRadius * kGlanceRadius) {
            float dist = std::sqrt(distSq);
            rp.x = (rp.x / dist) * (kGlanceRadius - 8.0f);
            rp.y = (rp.y / dist) * (kGlanceRadius - 8.0f);
        }
        ImVec2 drawPos(center.x + rp.x, center.y + rp.y);
        const float pinScale = Settings::GetSingleton()->navGlancePinSize
                               / (2.0f * MapDraw::kPinRadius);
        const std::uint32_t drawIconType = m.isDestination ? 0u : m.iconType;
        auto* pinSrv = m.isDestination ? TextureManager::GetIconByName("destination")
                                       : TextureManager::GetCategoryIcon(m.iconType);
        MapDraw::DrawPin(dl, drawPos.x, drawPos.y, 0.0f, drawIconType, pinSrv,
                         pinScale, Settings::GetSingleton()->PinTintCol(), a_alpha);
        if (m.isDestination) {
            const float timeSec = std::chrono::duration<float>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            MapDraw::DrawDestinationRing(dl, drawPos.x, drawPos.y, timeSec);
        }
    }

    if (auto* questSrv = TextureManager::GetIconByName("active")) {
        const float questHalf = 20.0f * settings->navGlancePinSize / (2.0f * MapDraw::kPinRadius);
        const int maxQuest = settings->navMaxQuestMarkers;
        int drawnQuest = 0;
        for (const auto& qpos : f.questTargets) {
            if (drawnQuest >= maxQuest) break;
            ImVec2 rp = WorldToGlance(qpos, f.playerPos, glanceScale, glanceNorthRot);
            float distSq = rp.x * rp.x + rp.y * rp.y;
            if (distSq > kGlanceRadius * kGlanceRadius) {
                float dist = std::sqrt(distSq);
                rp.x = (rp.x / dist) * (kGlanceRadius - 8.0f);
                rp.y = (rp.y / dist) * (kGlanceRadius - 8.0f);
            }
            DrawQuestTargetIcon(dl, questSrv, center.x + rp.x, center.y + rp.y, questHalf, a_alpha);
            ++drawnQuest;
        }
    }

    DrawViewCone(dl, center, f.cameraAngle - glanceNorthRot, kGlanceRadius - 4.0f, a_alpha);

    float c = std::cos(f.playerAngle - glanceNorthRot);
    float s = std::sin(f.playerAngle - glanceNorthRot);

    ImVec2 forward(s, -c);
    ImVec2 right(c, s);

    ImVec2 p1(center.x + forward.x * 12.0f, center.y + forward.y * 12.0f);
    ImVec2 p2(center.x - forward.x * 10.0f - right.x * 8.0f, center.y - forward.y * 10.0f - right.y * 8.0f);
    ImVec2 p3(center.x - forward.x * 4.0f, center.y - forward.y * 4.0f);
    ImVec2 p4(center.x - forward.x * 10.0f + right.x * 8.0f, center.y - forward.y * 10.0f + right.y * 8.0f);

    const float retraceT = SmoothstepLocal(_retraceFade);
    const ImU32 arrowFill = MapDraw::LerpColor(IM_COL32(255, 255, 255, 255), MapTheme::kRetraceArrow, retraceT);
    dl->AddQuadFilled(p1, p2, p3, p4, FadeCol(arrowFill, a_alpha));
    dl->AddQuad(p1, p2, p3, p4, FadeCol(IM_COL32(0, 0, 0, 255), a_alpha), 1.5f);
}

void MinimapRenderer::DrawInspect(const NavigationFrame& f, float a_alpha) {
    SnapshotProjection snap;
    {
        std::lock_guard<std::mutex> lock(_snapshotMutex);
        snap = _snapshot;
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    float pct = Settings::GetSingleton()->navInspectSizePct / 100.0f;

    const float boxW = disp.x * pct;
    const float boxH = disp.y * pct;
    const float overlayAspect = (boxH > 1e-3f) ? (boxW / boxH) : (16.0f / 9.0f);
    _overlayAspect.store(overlayAspect);

    const float cropW = 1.0f;
    const float cropH = 1.0f;
    ImVec2 uv0(0.0f, 0.0f);
    ImVec2 uv1(1.0f, 1.0f);

    const ImVec2 min((disp.x - boxW) * 0.5f, (disp.y - boxH) * 0.5f);
    const ImVec2 max(min.x + boxW, min.y + boxH);

    dl->AddRectFilled(min, max, FadeCol(IM_COL32(8, 9, 10, 230), a_alpha), MapTheme::kPanelRounding);
    dl->AddRect(min, max, FadeCol(Settings::GetSingleton()->BorderCol(), a_alpha), MapTheme::kPanelRounding, 0, MapTheme::kPanelBorderW);

    if (snap.valid && snap.srv) {
        const ImU32 tint = snap.stale ? IM_COL32(255, 255, 255, 150) : IM_COL32_WHITE;
        dl->AddImage(reinterpret_cast<ImTextureID>(snap.srv), min, max, uv0, uv1, FadeCol(tint, a_alpha));
    }

    float vW = boxW / cropW;
    float vH = boxH / cropH;
    const ImVec2 vMin(min.x - uv0.x * vW, min.y - uv0.y * vH);
    const ImVec2 vSize(vW, vH);

    if (snap.valid) {
        std::vector<std::vector<ImVec2>> visibleSegments;
        std::vector<std::vector<float>> visibleSegmentZ;
        visibleSegments.reserve(f.segments.size());
        visibleSegmentZ.reserve(f.segments.size());
        for (const auto& seg : f.segments) {
            std::vector<ImVec2> pts;
            std::vector<float>  ptsZ;
            pts.reserve(seg.size());
            ptsZ.reserve(seg.size());
            for (const auto& p : seg) {
                ImVec2 screen;
                if (ProjectWithSnapshot(snap, p, vMin, vSize, screen)) {
                    pts.push_back(screen);
                    ptsZ.push_back(p.z);
                }
            }
            if (!pts.empty()) {
                visibleSegments.push_back(std::move(pts));
                visibleSegmentZ.push_back(std::move(ptsZ));
            }
        }
        if (Settings::GetSingleton()->navInspectShowTrail) {
            dl->PushClipRect(min, max, true);
            MapDraw::DrawPathTrail(dl, visibleSegments,
                                   TrailHeadNearPlayer(f.segments, f.playerPos),
                                   PathTracker::GetSingleton()->IsTracking(),
                                   ImVec2(0.0f, 0.0f), 0.0f,
                                   f.playerAngle - snap.northRot, true,
                                   nullptr, 0, a_alpha,
                                   &visibleSegmentZ, f.playerPos.z);
            dl->PopClipRect();
        }
        {
            auto* s = Settings::GetSingleton();
            if (s->InspectAnyEntity()) {
                const float half = static_cast<float>(s->navInspectEntityIconSize) * 0.5f;
                dl->PushClipRect(min, max, true);
                for (const auto& e : snap.entities) {
                    if (!e.discovered) continue;
                    using Cat = EntityMarkerSource::Category;
                    if (!s->InspectSubEnabled(e.category, e.subCategory)) continue;
                    ImVec2 g;
                    if (!ProjectWithSnapshot(snap, e.position, vMin, vSize, g)) continue;
                    ID3D11ShaderResourceView* icon = nullptr;
                    if (const char* sub = EntityMarkerSource::IconNameFor(e.category, e.subCategory))
                        icon = TextureManager::GetIconByName(sub);
                    if (!icon)
                        icon = TextureManager::GetIconByName(EntityMarkerSource::CategoryIconName(e.category));
                    if (icon) {
                        MapDraw::DrawIconFx(dl, icon,
                                            ImVec2(g.x - half, g.y - half),
                                            ImVec2(g.x + half, g.y + half),
                                            FadeCol(IM_COL32_WHITE, a_alpha));
                        continue;
                    }
                    switch (e.category) {
                        case Cat::Corpse: {
                            dl->AddLine(ImVec2(g.x - 4, g.y - 4), ImVec2(g.x + 4, g.y + 4), FadeCol(MapTheme::kEntityCorpse, a_alpha), 2.0f);
                            dl->AddLine(ImVec2(g.x - 4, g.y + 4), ImVec2(g.x + 4, g.y - 4), FadeCol(MapTheme::kEntityCorpse, a_alpha), 2.0f);
                            break;
                        }
                        case Cat::Door: {
                            dl->AddRectFilled(ImVec2(g.x - 2.5f, g.y - 4.5f), ImVec2(g.x + 2.5f, g.y + 4.5f),
                                              FadeCol(MapTheme::kEntityDoor, a_alpha), 1.5f);
                            break;
                        }
                        case Cat::Container: {
                            dl->AddRectFilled(ImVec2(g.x - 3.5f, g.y - 3.5f), ImVec2(g.x + 3.5f, g.y + 3.5f),
                                              FadeCol(MapTheme::kEntityContainer, a_alpha));
                            dl->AddRect(ImVec2(g.x - 3.5f, g.y - 3.5f), ImVec2(g.x + 3.5f, g.y + 3.5f),
                                        FadeCol(IM_COL32(20, 15, 5, 235), a_alpha));
                            break;
                        }
                        case Cat::LooseLoot: {
                            MapDraw::DrawDotFx(dl, g, 4.0f, FadeCol(MapTheme::kEntityLoot, a_alpha));
                            break;
                        }
                        case Cat::Ingredient: {
                            MapDraw::DrawDotFx(dl, g, 4.0f, FadeCol(MapTheme::kEntityIngredient, a_alpha));
                            break;
                        }
                    }
                }
                dl->PopClipRect();
            }
        }
        if (Settings::GetSingleton()->navActorAnyVisible()) {
            auto* s = Settings::GetSingleton();
            dl->PushClipRect(min, max, true);
            for (const auto& [formID, ghost] : _actorGhosts) {
                using Al = ActorMarkerSource::ActorMarker::Allegiance;
                if (Al::Ally == ghost.allegiance && !s->navActorShowAlly) continue;
                if (Al::Hostile == ghost.allegiance && !s->navActorShowHostile) continue;
                if (Al::Neutral == ghost.allegiance && !s->navActorShowNeutral) continue;
                if (Al::Animal == ghost.allegiance && !s->navActorShowAnimal) continue;
                ImVec2 g;
                if (!ProjectWithSnapshot(snap, ghost.position, vMin, vSize, g)) continue;
                const ImU32 col = Al::Ally == ghost.allegiance ? MapTheme::kActorAlly
                                 : Al::Hostile == ghost.allegiance ? MapTheme::kActorHostile
                                 : Al::Animal == ghost.allegiance ? MapTheme::kActorAnimal
                                 : MapTheme::kActorNeutral;
                MapDraw::DrawDotFx(dl, g, static_cast<float>(s->navInspectActorDotSize), FadeCol(col, a_alpha * ghost.alpha));
            }
            dl->PopClipRect();
        }
        dl->PushClipRect(min, max, true);
        ImVec2 pScreen;
        if (ProjectWithSnapshot(snap, f.playerPos, vMin, vSize, pScreen)) {
            DrawViewCone(dl, pScreen, f.cameraAngle - snap.northRot, 48.0f, a_alpha);
            float c = std::cos(f.playerAngle - snap.northRot);
            float s = std::sin(f.playerAngle - snap.northRot);
            ImVec2 forward(s, -c);
            ImVec2 right(c, s);
            ImVec2 p1(pScreen.x + forward.x * 12.0f, pScreen.y + forward.y * 12.0f);
            ImVec2 p2(pScreen.x - forward.x * 10.0f - right.x * 8.0f, pScreen.y - forward.y * 10.0f - right.y * 8.0f);
            ImVec2 p3(pScreen.x - forward.x * 4.0f, pScreen.y - forward.y * 4.0f);
            ImVec2 p4(pScreen.x - forward.x * 10.0f + right.x * 8.0f, pScreen.y - forward.y * 10.0f + right.y * 8.0f);
            const float retraceT = SmoothstepLocal(_retraceFade);
            const ImU32 arrowFill = MapDraw::LerpColor(IM_COL32(255, 255, 255, 255), MapTheme::kRetraceArrow, retraceT);
            dl->AddQuadFilled(p1, p2, p3, p4, FadeCol(arrowFill, a_alpha));
            dl->AddQuad(p1, p2, p3, p4, FadeCol(IM_COL32(0, 0, 0, 255), a_alpha), 1.5f);
        }
        for (const MapMarker* mp : CollectVisibleMarkers(f.markers, f.isExterior,
                                                         f.worldSpaceID, f.cellID, f.playerPos,
                                                         false)) {
            const MapMarker& m = *mp;
            RE::NiPoint3 projPos{ m.position.x, m.position.y, f.playerPos.z };
            ImVec2 mScreen;
            if (ProjectWithSnapshot(snap, projPos, vMin, vSize, mScreen)) {
                const std::uint32_t drawIconType = m.isDestination ? 0u : m.iconType;
                auto* pinSrv = m.isDestination ? TextureManager::GetIconByName("destination")
                                               : TextureManager::GetCategoryIcon(m.iconType);
                const float pinScale = Settings::GetSingleton()->navInspectPinSize
                                       / (2.0f * MapDraw::kPinRadius);
                MapDraw::DrawPin(dl, mScreen.x, mScreen.y, 0.0f, drawIconType, pinSrv,
                                 pinScale,
                                 Settings::GetSingleton()->PinTintCol(), a_alpha);
                if (m.isDestination) {
                    const float timeSec = std::chrono::duration<float>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    MapDraw::DrawDestinationRing(dl, mScreen.x, mScreen.y, timeSec);
                }
            }
        }
        if (auto* questSrv = TextureManager::GetIconByName("active")) {
            const float questHalf = 20.0f * Settings::GetSingleton()->navInspectPinSize / (2.0f * MapDraw::kPinRadius);
            const int maxQuest = Settings::GetSingleton()->navMaxQuestMarkers;
            int drawnQuest = 0;
            for (const auto& qpos : f.questTargets) {
                if (drawnQuest >= maxQuest) break;
                ImVec2 qScreen;
                if (ProjectWithSnapshot(snap, qpos, vMin, vSize, qScreen)) {
                    DrawQuestTargetIcon(dl, questSrv, qScreen.x, qScreen.y, questHalf, a_alpha);
                    ++drawnQuest;
                }
            }
        }
        dl->PopClipRect();
    } else {
        dl->AddText(ImVec2(min.x + 10, min.y + 10), FadeCol(IM_COL32_WHITE, a_alpha), "Local map unavailable (waiting for snapshot)");
    }

    {
        const char* keyName =
            InputHandler::DIKToKeyName(Settings::GetSingleton()->navInspectModeToggleKey);
        const bool radarOn = _inspectMode.load() == InspectViewMode::Radar;
        const std::string badgeStr = LocalizationManager::Format(
            radarOn ? "Navigation.RadarModeOn" : "Navigation.RadarModeOff", keyName);
        const char* badge = badgeStr.c_str();
        const float badgeSize = ImGui::GetFontSize() * Settings::GetSingleton()->titleScale;
        const ImVec2 pad(8.0f, 4.0f);
        const ImVec2 pos(min.x + 10.0f, min.y + 10.0f);
        const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(badgeSize, FLT_MAX, 0.0f, badge);
        dl->AddRectFilled(ImVec2(pos.x - pad.x, pos.y - pad.y),
                          ImVec2(pos.x + sz.x + pad.x, pos.y + sz.y + pad.y),
                          FadeCol(MapTheme::kPanelBg, a_alpha), MapTheme::kPanelRounding);
        dl->AddText(ImGui::GetFont(), badgeSize, pos,
                    FadeCol(radarOn ? MapTheme::kBodyTextHover : MapTheme::kMutedText, a_alpha), badge);
        const char* markKeyName =
            InputHandler::DIKToKeyName(Settings::GetSingleton()->navQuickMarkKey);
        const std::string hintStr = LocalizationManager::Format("Navigation.AddMarkerHint", markKeyName);
        const char* hint = hintStr.c_str();
        const ImVec2 hintPos(pos.x, pos.y + sz.y + pad.y + 4.0f);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * Settings::GetSingleton()->bodyScale, hintPos,
                   FadeCol(MapTheme::kMutedText, a_alpha), hint);
    }

    if (!f.locationName.empty()) {
        ImFont* titleFont = FontManager::GetSingleton()->GetHeadingFont();
        if (!titleFont) titleFont = ImGui::GetFont();
        const float fontRatio = titleFont->FontSize / ImGui::GetFontSize();
        const float titleSize = ImGui::GetFontSize() *
            std::clamp(Settings::GetSingleton()->titleScale, 0.5f, 3.0f) * fontRatio;
        const char* title = f.locationName.c_str();
        const ImVec2 tsz = titleFont->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, title);
        const ImVec2 tpos((min.x + max.x) * 0.5f - tsz.x * 0.5f, min.y - tsz.y - 8.0f);
        dl->AddText(titleFont, titleSize, ImVec2(tpos.x + 1.0f, tpos.y + 1.0f),
                    FadeCol(IM_COL32(0, 0, 0, 200), a_alpha), title);
        dl->AddText(titleFont, titleSize, tpos,
                    FadeCol(MapTheme::kHeaderText, a_alpha), title);
    }

    if (!_quickMarkToastText.empty()) {
        const float age = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - _quickMarkToastTime).count();
        constexpr float kPopT = 0.15f, kHoldT = 1.25f, kEndT = 1.8f;
        if (age < kEndT) {
            float scale = 1.0f;
            float fade = 1.0f;
            if (age < kPopT) {
                const float t = age / kPopT;
                scale = 0.75f + 0.37f * t - 0.12f * t * t;
                fade = t;
            } else if (age > kHoldT) {
                fade = 1.0f - (age - kHoldT) / (kEndT - kHoldT);
            }
            const float toastAlpha = a_alpha * fade;
            const float toastSize =
                ImGui::GetFontSize() * Settings::GetSingleton()->titleScale * scale;
            const char* toast = _quickMarkToastText.c_str();
            const ImVec2 tsz = ImGui::GetFont()->CalcTextSizeA(toastSize, FLT_MAX, 0.0f, toast);
            const ImVec2 tpad(10.0f, 5.0f);
            const ImVec2 tpos((min.x + max.x) * 0.5f - tsz.x * 0.5f, min.y + 16.0f);
            dl->AddRectFilled(ImVec2(tpos.x - tpad.x, tpos.y - tpad.y),
                              ImVec2(tpos.x + tsz.x + tpad.x, tpos.y + tsz.y + tpad.y),
                              FadeCol(MapTheme::kPanelBg, toastAlpha), MapTheme::kPanelRounding);
            dl->AddRect(ImVec2(tpos.x - tpad.x, tpos.y - tpad.y),
                        ImVec2(tpos.x + tsz.x + tpad.x, tpos.y + tsz.y + tpad.y),
                        FadeCol(MapTheme::kPanelBorder, toastAlpha),
                        MapTheme::kPanelRounding, 0, MapTheme::kPanelBorderW);
            dl->AddText(ImGui::GetFont(), toastSize, tpos,
                        FadeCol(MapTheme::kBodyTextHover, toastAlpha), toast);
        } else {
            _quickMarkToastText.clear();
        }
    }
}

void MinimapRenderer::Install() {
    REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_HUDMenu[0] };
    _origPreDisplay = reinterpret_cast<PreDisplayFn>(vtbl.write_vfunc(0x7, &HookedPreDisplay));
    SKSE::log::info("MinimapRenderer: hooked HUDMenu::PreDisplay (IMenu vtable idx 7)");
}

void MinimapRenderer::HookedPreDisplay(RE::IMenu* a_this) {
    auto* self = MinimapRenderer::GetSingleton();
    self->_origPreDisplay(a_this);

    if (!self->_refreshPending.exchange(false)) return;

    self->RenderOneSnapshot();
}

MinimapRenderer::FrustumRect MinimapRenderer::ComputeInspectFrustum(
    InspectViewMode a_mode,
    float a_cellExtentX, float a_cellExtentY,
    float a_cellCenterX, float a_cellCenterY,
    const RE::NiPoint3& a_playerPos,
    float a_overlayAspect) const {
    FrustumRect r{};

    if (a_overlayAspect < 1e-3f) a_overlayAspect = 16.0f / 9.0f;

    const char* modeName = "Cover";
    switch (a_mode) {
    case InspectViewMode::Radar: {
        modeName = "Radar";
        const float w = Settings::GetSingleton()->navInspectRadarRangeUnits;
        r.centerX = a_playerPos.x;
        r.centerY = a_playerPos.y;
        r.width = w;
        r.height = w / a_overlayAspect;
        break;
    }
    case InspectViewMode::Cover:
    default: {
        const float cellAspect = (a_cellExtentY > 1e-3f) ? (a_cellExtentX / a_cellExtentY)
                                                         : a_overlayAspect;
        float w, h;
        if (cellAspect > a_overlayAspect) {
            w = a_cellExtentX;
            h = w / a_overlayAspect;
        } else {
            h = a_cellExtentY;
            w = h * a_overlayAspect;
        }
        r.centerX = a_cellCenterX;
        r.centerY = a_cellCenterY;
        r.width = w * 1.05f;
        r.height = h * 1.05f;
        break;
    }
    }

    (void)modeName;

    return r;
}

void MinimapRenderer::RenderOneSnapshot() {
    const auto perfStart = std::chrono::steady_clock::now();

    RenderTargetUpscaler::EnsureApplied();

    EnsureConstructed();
    if (!g_constructed) {
        SKSE::log::warn("MinimapRenderer: culling struct not constructed, skipping render");
        return;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* tes    = RE::TES::GetSingleton();
    if (!player || !tes) {
        SKSE::log::warn("MinimapRenderer: no player/TES, skipping render");
        return;
    }
    void* cam = CameraBase();

    auto* parentCell = player->GetParentCell();
    const bool isInterior = parentCell && parentCell->IsInteriorCell();
    const bool isExterior = parentCell && parentCell->IsExteriorCell();

    g_initNode(CullingBase());

    const float* playerPos = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(player) + 0x54);

    static const std::ptrdiff_t kTesBoundsBlockOffset = REL::Relocate(0x2b0, 0x2b8);
    auto* tesBlock = *reinterpret_cast<std::uint8_t**>(
        reinterpret_cast<std::uint8_t*>(tes) + kTesBoundsBlockOffset);

    g_camBegin(cam);
    g_camTarget(cam, playerPos);
    if (tesBlock) {
        g_camBounds(cam, reinterpret_cast<const float*>(tesBlock + 0x78), reinterpret_cast<const float*>(tesBlock + 0x84));
    }

    const float northRot = parentCell ? -parentCell->GetNorthRotation() : 0.0f;
    g_camApply(cam, northRot);

    const float* boundsMax = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(cam) + 0x38);
    const float* boundsMin = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(cam) + 0x44);
    const float trueExtentX = boundsMax[0] - boundsMin[0];
    const float trueExtentY = boundsMax[1] - boundsMin[1];
    const float cellCenterX = (boundsMax[0] + boundsMin[0]) * 0.5f;
    const float cellCenterY = (boundsMax[1] + boundsMin[1]) * 0.5f;

    if (isInterior) {
        const float pad = 2000.0f;
        float paddedMax[3] = { boundsMax[0] + pad, boundsMax[1] + pad, boundsMax[2] };
        float paddedMin[3] = { boundsMin[0] - pad, boundsMin[1] - pad, boundsMin[2] };
        g_camBounds(cam, paddedMax, paddedMin);
    }

    ImVec2 snapCropMin(0.0f, 0.0f);
    ImVec2 snapCropMax(1.0f, 1.0f);
    float snapCellX = trueExtentX;
    float snapCellY = trueExtentY;

    float snapFrustumHalfWidth = 0.0f;
    if (isExterior) {
        FrustumRect fr{};
        RE::NiPoint3 playerNi{ playerPos[0], playerPos[1], playerPos[2] };
        if (_inspectShown.load()) {
            fr.width = Settings::GetSingleton()->navInspectRadarRangeUnits;
            fr.height = fr.width / _overlayAspect.load();
        } else {
            const float w = std::max(2.0f * Settings::GetSingleton()->navGlanceRangeUnits * 1.5f, 4000.0f);
            fr.width = w;
            fr.height = w / (1920.0f / 1080.0f);
        }
        fr.centerX = playerNi.x;
        fr.centerY = playerNi.y;
        g_camFrustum(cam, fr.width, fr.height);
        float center[3] = { fr.centerX, fr.centerY, playerPos[2] };
        g_camTarget(cam, center);
        snapCellX = fr.width;
        snapCellY = fr.height;
        snapFrustumHalfWidth = fr.width * 0.5f;
    } else if (trueExtentX > 0.0f && trueExtentY > 0.0f) {
        FrustumRect fr{};
        if (_inspectShown.load()) {
            RE::NiPoint3 playerNi{ playerPos[0], playerPos[1], playerPos[2] };
            fr = ComputeInspectFrustum(
                _inspectMode.load(), trueExtentX, trueExtentY,
                cellCenterX, cellCenterY, playerNi, _overlayAspect.load());
        } else {
            const float glanceWorldRadius = Settings::GetSingleton()->navGlanceRangeUnits;
            const float needW = trueExtentX + 2.0f * glanceWorldRadius;
            const float needH = trueExtentY + 2.0f * glanceWorldRadius;
            constexpr float kTexAspect = 1920.0f / 1080.0f;
            fr.centerX = cellCenterX;
            fr.centerY = cellCenterY;
            fr.width = std::max(needW, needH * kTexAspect);
            fr.height = fr.width / kTexAspect;
        }
        g_camFrustum(cam, fr.width, fr.height);
        float center[3] = { fr.centerX, fr.centerY, playerPos[2] };
        g_camTarget(cam, center);
    }

    auto* lmCamera = reinterpret_cast<RE::LocalMapCamera*>(cam);
    lmCamera->enabled = true;
    lmCamera->Update();

    float w2c[4][4]{};
    RE::NiRect<float> port;
    if (auto* niCam = lmCamera->camera.get()) {
        std::memcpy(w2c, niCam->GetRuntimeData().worldToCam, sizeof(w2c));
        port = niCam->GetRuntimeData2().port;
        const auto& fr = niCam->GetRuntimeData2().viewFrustum;
        const float* pf = reinterpret_cast<const float*>(&port);
        (void)fr; (void)pf;
    }

    g_render(CullingBase());

    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    ID3D11ShaderResourceView* srv = nullptr;
    if (renderer) {
        auto& rt = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kLOCAL_MAP];
        srv = reinterpret_cast<ID3D11ShaderResourceView*>(rt.SRV);
    }

    SnapshotProjection snap;
    snap.valid = true;
    snap.stale = false;
    snap.cellID = parentCell ? parentCell->GetFormID() : 0;
    snap.isExterior = isExterior;
    snap.worldSpaceID = isExterior && player->GetWorldspace() ? player->GetWorldspace()->GetFormID() : 0;
    snap.frustumHalfWidth = snapFrustumHalfWidth;
    snap.playerPos = player->GetPosition();
    snap.northRot = northRot;
    std::memcpy(snap.worldToCam, w2c, sizeof(snap.worldToCam));
    snap.port = port;
    snap.uvCropMin = snapCropMin;
    snap.uvCropMax = snapCropMax;
    snap.cellExtentX = snapCellX;
    snap.cellExtentY = snapCellY;
    snap.srv = srv;
    snap.takenAt = std::chrono::steady_clock::now();

    auto* navSettings = Settings::GetSingleton();
    const bool wantGlanceEntities = navSettings->GlanceAnyEntity();
    const bool wantInspectEntities = navSettings->InspectAnyEntity();
    if (wantGlanceEntities || wantInspectEntities) {
        const bool needLOS = navSettings->navEntityRequireLOS;
        if (isExterior) {
            snap.entities = EntityMarkerSource::Collect(
                player, navSettings->navInspectRadarRangeUnits, snap.playerPos,
                EntityMarkerSource::kEntityRevealRadius, needLOS);
        } else if (parentCell) {
            snap.entities = EntityMarkerSource::Collect(
                parentCell, snap.playerPos,
                EntityMarkerSource::kEntityRevealRadius, needLOS);
        }
    }

    {
        std::lock_guard<std::mutex> lock(_snapshotMutex);
        _snapshot = snap;
    }
    const float perfMs = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - perfStart).count();

    SKSE::log::info("MinimapRenderer: snapshot render took {:.2f}ms (cell={:08X})",
        perfMs, snap.cellID);

}
