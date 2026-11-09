#pragma once

// Hooks map menu right-clicks to spawn custom markers and context menus.


namespace PlaceMarkerHook {
    void Install();

    void SuppressNextPlacement();

    void AllowNextPlacement();

    bool CallOriginalSetDestination(const RE::NiPoint3& a_pos, RE::TESWorldSpace* a_worldspace);
}
