#pragma once

#include <vector>
#include "RE/Skyrim.h"

namespace HUDMarkerHook {
    void Install();

    void LogPostLoadState();

    void EndPass();

    struct QuestMarker {
        RE::NiPoint3 position;
        RE::FormID   questID = 0;
        bool         isDoor = false;
    };
    std::vector<QuestMarker> GetQuestMarkers();
}
