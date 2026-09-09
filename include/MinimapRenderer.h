#pragma once

// Renders HUD glance mini-radar and full-screen dungeon inspect overlay.

#include <RE/Skyrim.h>
#include <mutex>
#include <vector>
#include <chrono>
#include <atomic>
#include <unordered_map>
#include <d3d11.h>
#include <imgui.h>
#include "PathTracker.h"
#include "MarkerManager.h"
#include "EntityMarkerSource.h"
#include "ActorMarkerSource.h"

// STATUS: Active production navigation overlay. CONFIRMED IN-GAME 2026-07-16 (AE).

class MinimapRenderer {
public:
    static MinimapRenderer* GetSingleton() {
        static MinimapRenderer singleton;
        return &singleton;
    }
    void Install();
    void Draw();
    static bool IsInspectShown() { return GetSingleton()->_inspectShown.load(); }
private:
    MinimapRenderer() = default;
    ~MinimapRenderer() = default;
    MinimapRenderer(const MinimapRenderer&) = delete;
    MinimapRenderer& operator=(const MinimapRenderer&) = delete;
public:
    enum class InspectViewMode { Cover = 0, Radar = 1 };
    struct NavigationFrame {
        bool valid = false;
        bool inspectHeld = false;
        RE::FormID cellID = 0;
        RE::FormID worldSpaceID = 0;
        bool isExterior = false;
        RE::NiPoint3 playerPos{};
        float playerAngle = 0.0f;
        float cameraAngle = 0.0f;
        std::vector<PathTracker::Segment> segments;
        std::vector<MapMarker> markers;
        std::vector<RE::NiPoint3> questTargets;
        std::string locationName;
    };
    struct SnapshotProjection {
        bool valid = false;
        bool refreshInFlight = false;
        bool stale = true;
        RE::FormID cellID = 0;
        RE::FormID worldSpaceID = 0;
        bool isExterior = false;
        float frustumHalfWidth = 0.0f;
        RE::NiPoint3 playerPos{};
        float northRot = 0.0f;
        float worldToCam[4][4]{};
        RE::NiRect<float> port{};
        ImVec2 uvCropMin{0.0f, 0.0f};
        ImVec2 uvCropMax{1.0f, 1.0f};
        float cellExtentX = 0.0f;
        float cellExtentY = 0.0f;
        ID3D11ShaderResourceView* srv = nullptr;
        std::chrono::steady_clock::time_point takenAt{};
        std::vector<EntityMarkerSource::EntityMarker> entities;
    };
private:
    NavigationFrame BuildFrame();
    void DrawGlance(const NavigationFrame& a_frame, float a_alpha);
    void DrawInspect(const NavigationFrame& a_frame, float a_alpha);
    void RequestSnapshotRefresh(const NavigationFrame& a_frame, const char* a_reason);
    void RenderOneSnapshot();
    void MergeActorGhosts();
    void CollectActorsIfDue(const NavigationFrame& a_frame);
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
    std::atomic<InspectViewMode> _inspectMode{InspectViewMode::Cover};
    bool _inspectModeInited = false;
    bool _toggleWasDown = false;
    bool _quickMarkWasDown = false;
    std::chrono::steady_clock::time_point _lastQuickMarkTime{};
    std::chrono::steady_clock::time_point _quickMarkToastTime{};
    std::string _quickMarkToastText;
    std::atomic<float> _overlayAspect{16.0f / 9.0f};
    std::atomic<bool> _inspectShown{false};
    bool _glanceBackdropActive = false;
    float _glanceFade = 1.0f;
    float _inspectFade = 0.0f;
    std::uint32_t _lastDrawnSpace = 0;
    bool _lastDrawnExterior = false;
    bool _spaceFadeHold = false;
    std::chrono::steady_clock::time_point _spaceEnteredAt{};
    float _retraceFade = 0.0f;
    float _combatFade = 0.0f;
    float _lastCombatTime = -10.0f;
    struct ActorGhost {
        RE::NiPoint3 position;
        ActorMarkerSource::ActorMarker::Allegiance allegiance;
        std::chrono::steady_clock::time_point lastSeen;
        bool  frozen = false;
        float alpha  = 0.0f;
    };
    std::unordered_map<RE::FormID, ActorGhost> _actorGhosts;
    std::chrono::steady_clock::time_point _lastGhostTick{};
    std::vector<ActorMarkerSource::ActorMarker> _lastActorScan;
    std::chrono::steady_clock::time_point _lastActorScanAt{};
    std::chrono::steady_clock::time_point _lastMergedScanAt{};
    static constexpr float kActorScanSeconds = 2.0f;
    using PreDisplayFn = void (*)(RE::IMenu*);
    PreDisplayFn _origPreDisplay = nullptr;
};
