#pragma once

// Renders preview location artwork and screenshots in hover stat cards.


#include <RE/Skyrim.h>

namespace HoverImageRenderer {
    void Init();

    void SampleCardState(RE::MapMenu* a_menu);

    void Draw();

    void OpenContextMenu();

    bool IsLocalMarkerHovered();

    void OpenLocalContextMenu();

    RE::FormID GetHoveredFormID();

    bool IsHoveredDiscovered();

    bool IsHoveredUndiscovered();
}
