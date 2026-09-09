#pragma once

// Hooks in-game HUD marker rendering to display custom destination pins.


#include <vector>
#include "RE/Skyrim.h"

namespace HUDMarkerHook {
    void Install();

    void EndPass();

    std::vector<RE::NiPoint3> GetQuestMarkerPositions();

    struct QuestMarker {
        RE::NiPoint3 position;
        RE::FormID   questID = 0;
    };
    std::vector<QuestMarker> GetQuestMarkers();
}
