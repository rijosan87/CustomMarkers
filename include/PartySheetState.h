#pragma once

// Compatibility bridge for Skyrim Party Sheet state.


namespace PartySheetState {
    void RegisterListener();

    bool ShouldHideOverlay();

    bool IsFullscreenPanelOpen();
}
