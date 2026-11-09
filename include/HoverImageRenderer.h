#pragma once

// Renders preview location artwork and screenshots in hover stat cards.


#include <RE/Skyrim.h>
#include <string>

namespace HoverImageRenderer {
    void Init();

    void SampleCardState(RE::MapMenu* a_menu);
    bool IsWithinHoverRange(RE::MapMenu* a_menu, std::int32_t a_idx);

    void Draw();

    void OpenContextMenu();

    // Marker index the vanilla right-click menu is open for, or -1.
    std::int32_t GetOpenMenuMarkerIndex();

    bool IsLocalMarkerHovered();

    void OpenLocalContextMenu();

    RE::FormID GetHoveredFormID();

    bool IsHoveredDiscovered();

    bool IsHoveredUndiscovered();
    std::string FormatMarkerDistance(float a_units, int a_unitSystem);
}
