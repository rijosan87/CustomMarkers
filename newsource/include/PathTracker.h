#pragma once

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
    // The trail is a list of segments, not one polyline. A new segment starts
    // when the player emerges from already-mapped ground somewhere far from
    // the last appended point — drawing across that gap produced phantom
    // straight runs through walls.
    using Segment = std::vector<RE::NiPoint3>;

    static PathTracker* GetSingleton();

    void ToggleTracking();
    void Update();

    // Thread-safe copy of the trail segments for the specified cell (or current if 0)
    std::vector<Segment> GetSegments(RE::FormID cellID = 0);

    // Thread-safe copy of recent revisit hits. These are transient and are not
    // part of the cosave record.
    std::vector<RevisitHit> GetRecentRevisits(float maxAgeSec = 2.0f);

    // Flattened view (segments concatenated). Kept for the parked
    // MinimapRenderer; live rendering should use GetSegments.
    std::vector<RE::NiPoint3> GetPoints(RE::FormID cellID = 0);

    bool IsTracking() const { return m_isTracking; }

    // Cosave persistence ("PTRK", separate record from MarkerManager's "CMMK"
    // stream but dispatched from the same GetNextRecordInfo loop).
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
    // Insertion order of m_paths keys, oldest first. Backs the cell cap
    // eviction below; unordered_map has no stable iteration order to evict by.
    std::vector<RE::FormID> m_cellOrder;
    bool m_isTracking{false};
    RE::FormID m_currentCellID{0};

    // Interior dungeons can be huge (courtyards, mega-dungeons with many
    // wings). Capping tracked cells bounds memory instead of growing forever
    // over a long play session.
    static constexpr std::size_t kMaxTrackedCells = 12;
    static constexpr std::size_t kMaxRevisits = 32;
    std::deque<RevisitHit> m_revisits;
};
