#pragma once

namespace LocalMapClickHook {
    void Install();

    // Run the local-map cursor raycast and open the note dialog at the hit.
    // Called by the local-map right-click "Add Marker Here" menu. Returns true
    // if placement started. Interior cells only.
    bool PlaceAtCursor();
}
