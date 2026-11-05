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

    bool IsLocalMarkerHovered();

    void OpenLocalContextMenu();

    RE::FormID GetHoveredFormID();

    bool IsHoveredDiscovered();

    bool IsHoveredUndiscovered();
    std::string FormatMarkerDistance(float a_units, int a_unitSystem);
}
