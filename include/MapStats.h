#pragma once

// Computes world map exploration and dungeon cleared statistics.

#include <RE/Skyrim.h>
#include <cstdint>
#include <mutex>

struct MapStatsData {
    int dungeonsCleared = 0, dungeonsTotal = 0;
    int locationsDiscovered = 0, locationsTotal = 0;
    int stonesFound = 0, stonesTotal = 0;
    std::uint32_t totalBounty = 0;
};

class MapStats {
public:
    static MapStats* GetSingleton();
    void Refresh(RE::MapMenu* a_menu, RE::TESWorldSpace* a_currentWorld);
    MapStatsData Get();
private:
    MapStats() = default;
    MapStats(const MapStats&) = delete;
    MapStats& operator=(const MapStats&) = delete;
    std::mutex _mutex;
    MapStatsData _data;
    RE::FormID _computedForWorld = 0;
    std::uint32_t _computedArraySize = 0;
    RE::FormID    _probeWorld    = 0;
    std::uint32_t _probeFrame    = 0;
    std::uint32_t _probeLastSize = 0xFFFFFFFFu;
};
