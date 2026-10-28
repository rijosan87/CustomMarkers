#pragma once

namespace MapLookRemapHook {
    void Install();

    // Call once per frame (from the render/present loop). While middle-mouse
    // tilt is active, pins the MenuCursor to the press position so the cursor
    // can't reach the screen edge and trigger edge-pan during the tilt drag.
    void PinCursorIfTilting();

    // True while middle-mouse tilt (look mode) is active. Hover suppression
    // reads this: during a tilt drag the camera sweeps markers under the
    // frozen cursor, and each one registers a real hover, so the stat card
    // would flicker in and out for the whole drag.
    bool IsTilting();
}
