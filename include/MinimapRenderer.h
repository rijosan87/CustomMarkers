#pragma once
#include <RE/Skyrim.h>
#include <mutex>
#include <vector>
#include <chrono>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <d3d11.h>
#include <imgui.h>  // ImVec2 for the Inspect crop rect
#include "PathTracker.h"
#include "MarkerManager.h"
#include "EntityMarkerSource.h"
#include "ActorMarkerSource.h"
#include "HUDMarkerHook.h"

class MinimapRenderer {
public:
    static MinimapRenderer* GetSingleton() {
        static MinimapRenderer singleton;
        return &singleton;
    }

    void Install();   // HUDMenu::PreDisplay hook for snapshot refresh
    void Draw();      // Present hook ImGui draw, both modes

    static bool IsInspectShown() { return GetSingleton()->_inspectShown.load(); }
    static bool IsInteriorInspectShown() { return GetSingleton()->_interiorInspectShown.load(); }
    static bool IsInspectToggleActive() { return GetSingleton()->_inspectToggleActive.load(); }
    static void ToggleInspect() {
        auto* inst = GetSingleton();
        inst->_inspectToggleActive.store(!inst->_inspectToggleActive.load());
    }
    static void ResetInspectToggle() { GetSingleton()->_inspectToggleActive.store(false); }
    static bool CanOpenControllerInspect();
    static bool IsControllerInspectActive() { return GetSingleton()->_controllerInspectActive.load(); }
    static void OpenControllerInspect() { GetSingleton()->_controllerInspectActive.store(true); }
    static void CloseControllerInspect() {
        auto* inst = GetSingleton();
        inst->_controllerInspectActive.store(false);
        inst->_controllerModeToggleRequested.store(false);
        inst->_controllerQuickMarkRequested.store(false);
    }
    static void RequestControllerModeToggle() {
        GetSingleton()->_controllerModeToggleRequested.store(true);
    }
    static void RequestControllerQuickMark() {
        GetSingleton()->_controllerQuickMarkRequested.store(true);
    }
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
        RE::FormID worldSpaceID = 0;
        bool isExterior = false;
        RE::NiPoint3 playerPos{};
        float playerAngle = 0.0f;   // body yaw (data.angle.z)
        float cameraAngle = 0.0f;   // camera yaw from cameraRoot; == body yaw when camera aligned
        std::vector<PathTracker::Segment> segments;
        std::vector<MapMarker> markers;
        // The engine's current compass quest markers, including the direct/door
        // frame type used by both overlay views.
        std::vector<HUDMarkerHook::QuestMarker> questTargets;
        std::string locationName;
    };

    struct SnapshotImage;
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
        // Presentation-only ownership. Never used by snapshot refresh decisions.
        std::shared_ptr<SnapshotImage> image;
        std::uint64_t generation = 0;
        int framing = 0;  // 0 unknown, 1 Glance, 2 Inspect Cover, 3 Inspect Radar.
        std::chrono::steady_clock::time_point takenAt{};
        std::vector<EntityMarkerSource::EntityMarker> entities;
    };

private:
    NavigationFrame BuildFrame();
    // a_alpha: eased 0..1 crossfade alpha (smoothstep of _glanceFade/_inspectFade).
    void DrawGlance(const NavigationFrame& a_frame, float a_alpha);
    void DrawInspect(const NavigationFrame& a_frame, float a_alpha);
    void RequestSnapshotRefresh(const NavigationFrame& a_frame, const char* a_reason);
    void RenderOneSnapshot();  // PreDisplay only

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
    // Quick-mark edge state + cooldown timestamp. Read/written on the Present
    // thread only (same as _toggleWasDown), no atomics needed.
    bool _quickMarkWasDown = false;
    std::chrono::steady_clock::time_point _lastQuickMarkTime{};
    std::chrono::steady_clock::time_point _quickMarkToastTime{};
    std::string _quickMarkToastText;
    std::atomic<float> _overlayAspect{16.0f / 9.0f};
    std::atomic<bool> _inspectShown{false};  // see IsInspectShown()
    std::atomic<bool> _interiorInspectShown{false};  // see IsInteriorInspectShown()
    std::atomic<bool> _inspectToggleActive{false};   // see IsInspectToggleActive()
    std::atomic<bool> _controllerInspectActive{false};
    std::atomic<bool> _controllerModeToggleRequested{false};
    std::atomic<bool> _controllerQuickMarkRequested{false};
    std::atomic<bool> _glanceVisible{true};
    bool _glanceBackdropActive = false;  // one-shot log gate for the Glance backdrop

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
        RE::NiPoint3 position;                            // live per-frame while in radius; frozen after leave
        ActorMarkerSource::ActorMarker::Allegiance allegiance;
        std::chrono::steady_clock::time_point lastSeen;   // refreshed while live
        bool  frozen = false;                             // true once out of radius/dead
        float alpha  = 0.0f;                              // 0..1, ramped by the merge fade machine; draw multiplies by it
    };
    std::unordered_map<RE::FormID, ActorGhost> _actorGhosts;
    std::chrono::steady_clock::time_point _lastGhostTick{};  // dt source for the ghost fade machine
    std::vector<ActorMarkerSource::ActorMarker> _lastActorScan;
    std::chrono::steady_clock::time_point _lastActorScanAt{};   // when _lastActorScan was collected
    std::chrono::steady_clock::time_point _lastMergedScanAt{};  // _lastActorScanAt last applied to _actorGhosts
    static constexpr float kActorScanSeconds = 2.0f;

    using PreDisplayFn = void (*)(RE::IMenu*);
    PreDisplayFn _origPreDisplay = nullptr;
};
