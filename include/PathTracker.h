#pragma once

// Records player exploration breadcrumbs for path trails.


#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <chrono>
#include <deque>
#include <vector>
#include <mutex>
#include <unordered_map>

struct RevisitHit {
    RE::NiPoint3 pos;
    std::chrono::steady_clock::time_point when;
};

class PathTracker {
public:
    using Segment = std::vector<RE::NiPoint3>;
    static PathTracker* GetSingleton();
    void ToggleTracking();
    void Update();
    std::vector<Segment> GetSegments(RE::FormID cellID = 0);
    std::vector<RevisitHit> GetRecentRevisits(float maxAgeSec = 2.0f);
    std::vector<RE::NiPoint3> GetPoints(RE::FormID cellID = 0);
    bool IsTracking() const { return m_isTracking; }
    static constexpr std::uint32_t kSerializationSignature = 'PTRK';
    static constexpr std::uint32_t kSerializationVersion   = 1;
    void OnGameSave(SKSE::SerializationInterface* serde);
    void LoadRecord(SKSE::SerializationInterface* serde, std::uint32_t version, std::uint32_t length);
    void OnRevert();
private:
    PathTracker() = default;
    ~PathTracker() = default;
    PathTracker(const PathTracker&) = delete;
    PathTracker& operator=(const PathTracker&) = delete;
    std::mutex m_mutex;
    std::unordered_map<RE::FormID, std::vector<Segment>> m_paths;
    std::vector<RE::FormID> m_cellOrder;
    bool m_isTracking{false};
    RE::FormID m_currentCellID{0};
    static constexpr std::size_t kMaxTrackedCells = 12;
    static constexpr std::size_t kMaxRevisits = 32;
    std::deque<RevisitHit> m_revisits;
};
