#pragma once
// Consumer glue for Skyrim Party Sheet's public state API (PartySheetStateAPI.h).
// Party Sheet is an optional dependency: if it isn't installed we never receive
// kMsg_Interface and every query below returns false, so nothing changes.
//
// Every gameplay-view overlay yields: the navigation overlay (MinimapRenderer
// Glance + Inspect), destination beacons, loot beams, and Look & Pin. The world
// map is untouched: MapMenu blocks Party Sheet's panels anyway.
//
// This is NOT redundant with MenuUtils::ShouldBlockOverlay(). Party Sheet's
// panels are ImGui drawn from its own Present hook, not registered IMenus, so
// no IsMenuOpen() name can see them. Both gates are needed.

namespace PartySheetState {
    // Registered from SKSEPlugin_Load with a null sender so Party Sheet's
    // kMsg_Interface reaches us. Stashes the interface pointer.
    void RegisterListener();

    // True when Party Sheet says overlays should step aside: the user hid the
    // HUD, or a fullscreen panel is up (PartySheet, PartySheetV2,
    // CharacterSheet, SettingsMenu). InspectCard is deliberately NOT counted -
    // Party Sheet doesn't call it fullscreen and it coexists with the HUD.
    bool ShouldHideOverlay();

    // True when Party Sheet has a fullscreen panel open (PartySheet, PartySheetV2,
    // CharacterSheet, SettingsMenu). Excludes HUD-hidden intent.
    bool IsFullscreenPanelOpen();
}
