#pragma once

// Remaps map camera rotation/tilt to middle-mouse drag.


namespace MapLookRemapHook {
    void Install();

    void PinCursorIfTilting();

    bool IsTilting();
}
