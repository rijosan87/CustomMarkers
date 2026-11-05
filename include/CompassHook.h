#pragma once

// Hooks compass to render custom markers and destination pins.


#include <RE/Skyrim.h>

namespace CompassHook {
    void Install();

    bool WorldMatches(RE::TESWorldSpace* a_current, RE::FormID a_markerWorldID);
    RE::TESWorldSpace* RootWorld(RE::TESWorldSpace* a_world);

    float SquaredLocationRadius();
}
