#pragma once

#include <RE/Skyrim.h>

namespace CompassHook {
    void Install();

    // True when a marker stored in a_markerWorldID belongs to the same map as
    // a_current. Compares worldspace ROOTS, so a marker in a child worldspace
    // (Whiterun, Solitude, any walled city) matches while the player stands in
    // Tamriel, and vice versa.
    //
    // Shared with MinimapRenderer on purpose: the radar used its own bare
    // `==` test and the two surfaces disagreed about which markers exist. Both
    // now answer the question the same way.
    bool WorldMatches(RE::TESWorldSpace* a_current, RE::FormID a_markerWorldID);
}
