#pragma once

namespace PlaceMarkerHook {
    void Install();

    // Set by MarkerHoverHook before forwarding a vanilla-marker click to the
    // trampoline (to surface "You cannot fast travel from here" in interiors).
    // While true, the placement hook returns without opening the note dialog.
    // Reset by the placement hook after one use, or after the click cycle ends.
    void SuppressNextPlacement();

    // Left-click no longer places markers. Placement is right-click only:
    // MapMarkerRenderer's "Add Marker Here" calls MapMenu::PlaceMarker, which
    // ends up in this hook. So the hook ignores every placement EXCEPT the one
    // immediately following this call. The right-click menu sets this; the hook
    // consumes it (one-shot). Without it, a plain left-click on terrain still
    // ran the engine's PlaceMarker path and opened the note dialog.
    void AllowNextPlacement();
}
