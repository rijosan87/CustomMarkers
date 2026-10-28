// include/MapStats.h
#pragma once
#include <RE/Skyrim.h>
#include <cstdint>
#include <mutex>

// Completionist-style aggregate stats for the currently displayed worldspace:
// dungeons cleared/total, locations discovered/total, standing stones
// found/total, and total outstanding bounty. See
// findings/spec-findings-Map-Stats.md for the full access-path writeup.
struct MapStatsData {
    int dungeonsCleared = 0, dungeonsTotal = 0;
    int locationsDiscovered = 0, locationsTotal = 0;
    int stonesFound = 0, stonesTotal = 0;
    std::uint32_t totalBounty = 0;
};

class MapStats {
public:
    static MapStats* GetSingleton();

    // Recomputes all fields scoped to a_currentWorld. Cheap to call every
    // frame: the world-changed guard is the FIRST thing Refresh does, so a
    // same-world call short-circuits before the form-array/marker-array walk.
    void Refresh(RE::MapMenu* a_menu, RE::TESWorldSpace* a_currentWorld);

    // Copy of the cached data under the mutex, for the draw path.
    MapStatsData Get();

private:
    MapStats() = default;
    MapStats(const MapStats&) = delete;
    MapStats& operator=(const MapStats&) = delete;

    std::mutex _mutex;
    MapStatsData _data;
    RE::FormID _computedForWorld = 0;
    // Marker-array size the cached walk was computed from. The array is
    // discovery-gated, so its size growing means a new location was found and
    // the counts are stale even though the worldspace did not change.
    std::uint32_t _computedArraySize = 0;

    // Marker-array settling probe state (diagnostic only, gated on
    // Settings::mapStatsLog). Render thread only, same as Refresh.
    RE::FormID    _probeWorld    = 0;
    std::uint32_t _probeFrame    = 0;
    std::uint32_t _probeLastSize = 0xFFFFFFFFu;
};
