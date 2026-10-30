# Version 1.3.2

## Features

- **Stat card rows:** Hovering a vanilla marker now also shows Distance (metric or imperial), Boss (Dead/Alive, discovered dungeons), Quests at that location, Services (cities and towns list all; a shop or inn shows its own), and Player Home (cities and towns). Every row has its own toggle under Show Meta, in both settings menus. Confirmed in-game.
- **Stat card wrapping and image width:** Long values such as Services and Quests wrap inside the value column instead of widening the card. Marker Images > Width is now Minimum Width in pixels: cards with an image are at least that wide, and the image always fills the card. The old fraction setting is reset. Confirmed in-game.
- **Spouse and companions:** The stat card lists your spouse and any waiting or dismissed followers at that marker. The List panel Overview tab lists everyone away from you; click a name to pan to them, or to the door of the building they are in. Active followers are not listed. Works with vanilla, SFF, AFT, and NFF. Each part has its own toggle in both settings menus. Confirmed in-game.

## Fixes

- **AE world map marker count:** On AE, the world map read the marker list's reserved size instead of its real count, so the Overview stats and Cleared badges also walked leftover entries past the end. They now use the real count. Confirmed in-game on AE 1.6.1170: 394 markers read instead of 512.

# Version 1.3.1

## Fixes

- **SmoothCam compatibility:** Custom Markers no longer hooks the engine-wide Scaleform invoke that SmoothCam also hooks. The Ctrl hover-zoom suppression on the local map now hooks only the local map's own `SetSelectedMarker` call. Fixes a hook conflict reported with both mods installed, where new games and save loads took very long and the character occasionally locked up in combat. Hook Map no longer reports the conflict, and Ctrl suppression on the local map still works in-game.

# Version 1.3.0

## Features
- **Book loot beams:** Added Books to Loot Types, disabled by default, using the existing book icon and loot-beam filters.
- **Loot beam appearance:** Added separate Bottom width, Top width, and Ground glow settings for Modern and Wisp in both settings menus, with original defaults preserved.
- **Legacy of the Dragonborn loot beams:** With The Curator's Companion, loose museum items show dedicated New, Found, or Displayed icons in their original colors, with blue, amber, or green beams respectively. Other loot keeps its usual icon.
- **Track Dropped Weapons:** Weapons that leave your inventory into the world, such as from a disarm or a drop script, get a violet loot beam so they can be found in grass. The beam shows during combat and ignores Min Gold Value, range, Floor Band, and discovery. On by default. Confirmed in-game for inventory and console drops, including mid-combat.
- **Mini Radar compass directions:** N, E, S, and W letters sit just outside the radar rim and rotate with Follow Camera. Their size follows Body Scale. Toggle with Show Compass Directions, on by default.
- **Mini Radar location name:** The current location name shows under the radar in the same style as the Inspect title, sized by Title Scale. Toggle with Show Location Name, on by default.

## Fixes

- **Loose-item loot beam anchors:** Beams follow the settled model position, fixing gaps above dropped armor when its reference position remains higher. Confirmed in-game on armor, boots, weapons, and a necklace on steps.
- **Dialogue History compatibility:** Fixed Custom Markers overlays remaining visible over Dialogue History's full-history window. Overlays hide while history is open and return when it closes. Confirmed in-game.
- **Ash Spawn loot beams:** Fixed missing beams on lootable ash piles by reading the linked dead actor's inventory and anchoring the beam to the pile. Confirmed in-game, including hide-after-looting and hide-after-inspecting.
- **Active Quests click-to-pan:** Position fallbacks now respect the engine's eligible quest targets, fixing The Blessings of Nature selecting the wrong destination from inside Whiterun. Confirmed on several regular and miscellaneous objectives in Tamriel and Barrow of the Wyrm in Wyrmstooth. Quest collection now runs through a coalesced game task, with cached rows invalidated on map closure and save-session changes.
- **Off-screen destination arrow:** Fixed the arrow jumping between opposite screen edges as the destination passes behind the camera.
- **Mini Radar position range:** Position X and Y sliders now end exactly where the radar meets the screen edge, in both the standalone settings and SKSE Menu Framework. Fixes ultrawide displays (5120x1440 report) where the radar could not be moved to the right edge. Confirmed at 1080p with 100% and 150% interface scale.
- **MapMenu Extension compatibility:** While the extension's world picker is open, all Custom Markers map elements hide (pins, badges, hover panel, List panel, Legend) and right-click no longer opens our menus. They return when it closes. The List panel follows worldspace switches made through the extension. Confirmed in-game.
- **Active Quests across worldspaces:** Quests whose target is inside an interior now resolve to that interior's exterior entrance even when the player is in a different worldspace. Barrow of the Wyrm is clickable on the Wyrmstooth map while standing in Tamriel, where vanilla's Show on Map is unavailable. Confirmed in-game with no regressions across the tracked quests.

# Version 1.2.9

## Features
- **Mini Radar loot icons:** Added 150 ms fade-in, 200 ms fade-out, and softer visibility near the radar edge.
- **Mini Radar and Inspect - Transition fog of war:** Added a short dissolve between map snapshots to soften terrain and fog-of-war reveals.
- **Mini Radar view cone:** Shortened the cone and softened its far edge so more of the map stays visible.
- **Mini Radar and Inspect entity icons:** Added configurable background, border, padding, 3D shading, and shadow for loot, containers, corpses, and ingredients. Door icons keep their plain style. The controls appear in both settings menus under Entity Icon Style, alongside a collapsible General section.
- **Icon Design:** Split Shadow from 3D Icons in HUD Settings and SKSE Menu Framework. Tint, raised shading, and icon shadow can be adjusted independently; existing choices carry over.
- **Show Meta:** Added a toggle to hide Type, Hold, Bounty, and Cleared on map-marker hover cards.
- **Loot Beam Designs:** Added Modern and Wisp styles in standalone HUD Settings and SKSE Menu Framework. Modern keeps the 1.2.8 beam and remains the default; Wisp uses a shorter, softer plume and a quieter icon.
- **Hide After Inspecting:** Loot beams stay hidden for the session after opening a chest or corpse's full search menu, even when no item is taken. On by default; QuickLoot previews do not count. Confirmed in-game.
- **List Panel design:** Minor design tweaks
- **List Panel hint:** When the panel is hidden on the world map, a top-right reminder shows its current hotkey.

## Fixes

- **Quest markers in Mini Radar and Inspect:** Nearby visible targets now take priority within the existing marker limit, so distant quests do not hide a local objective. Confirmed in-game.
- **Look & Pin beacons in elevated cities:** Keep the placement's surface height instead of anchoring the beacon below raised streets. Confirmed on several Solitude building placements.

# Version 1.2.8
## Features
- **Mini Radar Orientation:** Added Fixed North and Follow Camera modes in HUD Settings and SKSE Menu Framework. Follow Camera rotates the map and markers with the camera in both Circle and Square shapes; Inspect remains North-Up.
- **Controller support:** Added configurable Inspect controls in standalone HUD Settings and SKSE Menu Framework. On the map, A/Cross activates List tabs and Legend controls, RB/R1 opens context menus for markers, local doors, and terrain, and A/Cross selects menu and note-dialog controls. B/Circle dismisses context menus or standalone HUD Settings. Marker names and notes still use keyboard text entry. Basic controls confirmed in-game.
- **Quest marker icons:** Mini Radar and Inspect show `activemarker.png` for direct quest targets and `active.png` for targets behind a load door. Confirmed in-game.

## Fixes
- **Square Mini Radar:** Fixed diagonal gaps in the rotating map backdrop.
- **Skyrim 1.6.640 Compass Navigation Overhaul markers:** Resolve the compass locations-type and marker-count globals through the matching Address Library database so custom markers reach CNO on 1.6.640 while preserving the existing runtime paths.
- **Better Carriage Destinations:** Undiscovered locations can be selected from the carriage map and show BCD's fare confirmation, while ordinary map clicks still suppress the vanilla "Place marker?" prompt.

## Diagnostics
- **Skyrim 1.5.97 AddMarker crash (2026-09-24):** Extended opt-in `HUDMarkerLog` crash capture with entry/jump-chain bytes, relay destinations, and allocation/image ownership. Hook behavior is unchanged; build and affected-user validation remain pending.

# Version 1.2.7
## Features
- **Master Interface Scale:** Added an `Interface Scale` slider (50% to 200%, default 100%) in HUD Settings and SKSE Menu Framework that scales Mini Radar, Inspect Overlay, World Beacon, Loot Beam, Look & Pin, and core world/local-map marker geometry and text.
- **Master Interface Scale (World Beacons):** Extended interface scaling to in-world destination beacons and edge indicators. Scales pillar widths, bottom fade, diamond badge radius, destination icon, edge arrow, border, offsets, and label/distance text while preserving world-space height, ground anchors, fade distances, arrival clearing, and occlusion.
- **Master Interface Scale (Loot Beams):** Extended interface scaling to in-world loot beams. Scales feathered beam widths, ground-contact stroked ring radii and line thicknesses, diamond badge frame radius, and item icons while preserving world-space length, range, reveal, floor band, and occlusion culling.
- **Master Interface Scale (Look & Pin):** Extended interface scaling to the Look & Pin reticle and placement preview. Scales reticle ring radius, ticks, line thicknesses, drop-shadow offsets, ghost-pin geometry, message offsets, and distance/prompt text while preserving raycasting, collision checks, terrain march, hold timing, and placed marker positions.
- **Master Interface Scale (Map Markers):** Extended interface scaling to world/local-map custom pins, hover bounds, destination rings, favorite/note/cleared badges, pan-to pulse rings, and the local-map exit indicator. Keeps `Marker Icon Scale` as a separate visual-size control so custom pins retain their normal base size at `100%`; confirmed on native 4K and a 4K client with a 1080p Display Tweaks target.
- **Master Interface Scale (List Panel):** Extended interface scaling to the world-map List panel, including its frame, tabs, rows, scrolling layout, icons, destination badges, text, hover bands, delete controls, hit targets, and drag footprint. Saved position and percentage-based height settings remain logical inputs. Confirmed on native 4K and a 4K client with a 1080p Display Tweaks target.
- **Master Interface Scale (Map Filters):** Extended interface scaling to the Map Filters panel, including its frame, expanded and collapsed layouts, icons, text, hover bands, tooltips, and hit targets while preserving its saved position, collapse animation, and filtering behavior. Confirmed on native 4K and a 4K client with a 1080p Display Tweaks target.
- **Master Interface Scale (Marker Dialogs):** Extended interface scaling to custom-marker placement and editing, vanilla-marker notes, and world/local-map category pickers. Dialog geometry, text, controls, icon cells, waypoint glyphs, and hit targets scale together while preserving normalized placement and input behavior. Confirmed on native 4K and a 4K client with a 1080p Display Tweaks target, including numbered and colored local-map glyphs.
- **Master Interface Scale (Context Menus):** Extended interface scaling to the empty-terrain, custom-marker, vanilla-marker, and local-door context menus. Popup padding, spacing, borders, text, rows, and hit targets scale together while preserving cursor anchoring and menu actions. Confirmed on native 4K and a 4K client with a 1080p Display Tweaks target.
- **Master Interface Scale (Map Hover Surfaces):** Extended interface scaling to world/local custom-pin hover panels, the vanilla-marker stat card and optional image, map-owned tooltips, and local-map exit labels. Confirmed on native 4K at `200%` and a 4K client with a 1080p Display Tweaks target at `100%`.
- **Fresh Kill Loot Beam Delay:** Added a 5-second hold before loot beams appear on newly killed enemies, preventing beams from popping up prematurely during death or killmove animations.
- **Versioned Startup Log:** The first `CustomMarkers.log` line now records both the plugin version and detected Skyrim runtime.

## Fixes
- **Missing UI translations:** Added English translation keys for Interface Scale, Clear On Arrival, Hide After Looting, Brightness, and the note-dialog title, and localized the corresponding standalone HUD Settings and SMF labels.
- **SSE Display Tweaks 4K-to-1080p UI scaling:** Preserved the `3840x2160` to `1920x1080` submission transform for Inspect, cursor, map-hover, and click alignment while compensating fixed-pixel master-scale geometry. Confirmed at `100%` interface scale with Glance and Inspect correctly sized.
- **Scaled map UI polish:** Normalized List-panel category PNG, waypoint, and fallback icons to the requested row footprint, and vertically centered collapsed Map Filters tooltips at higher interface scales.
- **HUD hidden during scripted cutscenes:** Gated gameplay HUD overlays (Mini Radar, Inspect Overlay, World Beacons, Look & Pin reticle, and Loot Beams) behind `MenuUtils::AreFightingControlsDisabled()` to ensure they stay hidden during cutscenes, intro scenes, and scripted events where player combat controls are locked.
- **Skyrim 1.6.640 Mini Radar crash:** Select the TES loaded-area bounds layout at the actual 1.6.1130 structure boundary, preventing snapshot refresh from passing an invalid bounds pointer to the local-map camera. Confirmed fixed by the affected 1.6.640 user; Skyrim 1.5.97, 1.6.1170, and 1.7.104 retain their existing offsets.

# Version 1.2.5

## Features
- **Mini Radar Size:** Increased maximum radar size from 300px to 500px.
- **Coin Purse Loot Beams:** Added dedicated coin-purse detection, icon support, and an independent toggle in standalone HUD Settings and SKSE Menu Framework.
- **Loot Beam Blacklist:** Added additive INI-based exclusions under `Data/SKSE/Plugins/CMMBlacklist/` to suppress beams for specific container bases (such as wardrobes), placed references, cells, and location hierarchies.

## Fixes
- **Skyrim 1.5.97 HUD Marker Diagnostics:** Corrected the `AddMarker` hook's Boolean return contract and added `[Logging] HUDMarkerLog=1` to capture the selected detour scheme, trampoline, resume address, and post-load hook integrity. The developer's 1.5.97 baseline selected `VALLOC2`, copied the complete 14-byte prologue, resumed at `target+0xE`, and remained unchanged through `kDataLoaded`; 1.6.1170 and 1.7.104 also passed load/save-load/gameplay smoke tests. Affected-user confirmation and the final 1.5.97 crash fix are pending.
- **Falmer Chest Loot Beams:** Added exact model recognition for regular and boss Falmer chests whose mesh names do not contain `chest`. Runtime confirmation remains pending.
- **Mini Radar Tree Flickering:** Preserve and restore each loaded tree's exact switch-node index after every offscreen snapshot while retaining the existing render-mode and accumulator restoration. Confirmed working in-game with zero flicker by affected users across SE 1.5.97, AE 1.6.1170, and AE 1.7.104.
- **Creature Corpse Loot Beams:** Fixed beams remaining over fully looted Frost Giants and other creatures with hidden weapons.
- **Quest Marker Target Resolution:** Fixed distant exterior targets panning to the player's nearby city or hold exit when clicked from an interior. True interior targets still prefer their precise exterior entrance. Confirmed with The Staff of Magnus and Bleak Falls Barrow.

# Version 1.2.4
## Features
- I’ve added a test build for the **tree-flickering issue**. Please keep Mini Radar enabled and test somewhere the flickering normally occurs. Let me know whether it still happens, and send your latest CustomMarkers.log after testing. The fix keeps the radar working normally and corrects render state after it refreshes.
- **Inspect Activation:** Added Hold or Toggle activation for the Inspect Overlay in both standalone HUD Settings and SKSE Menu Framework. Escape closes Inspect when Toggle is selected.
- **HUD Visibility Hotkeys:** Added unbound-by-default toggle keys for Mini Radar and Loot Beams in standalone HUD Settings and SKSE Menu Framework.
- **Persistent List Panel:** The world-map List panel now remembers its C-key show/hide state across map sessions and game restarts.

## Fixes
- **Undiscovered Map Marker Stat Cards:** Restored stat cards for undiscovered (black icon) markers on the world map, matching vanilla behavior where location names remain visible on hover.

# Version 1.2.3
## Features

- **SKSE Menu Framework:** Added complete HUD Settings support in the Mod Control Panel. Custom Markers map overlays hide while SMF is open and return when it closes.
- **Optional Settings Hotkey:** The standalone HUD Settings hotkey can be unbound when settings are accessed through SMF.
- **Radar and Inspect Opacity:** Matched both backfills at 230/255 beneath the engine's partially transparent local-map snapshot. Transition fading remains unchanged.

## Fixes

- **Skyrim 1.5.97 Local-Map Placement:** Fixed `Add Marker Here` failing to open the note dialog because `NiCamera::WindowPointToRay` used the AE Address Library ID on SE. Confirmed in-game on Skyrim 1.5.97.
- **Dwarven Large Chest Loot Beams:** Fixed large Dwarven chests using `DweContainer01.nif` not being recognized as loot-beam chests.

# Version 1.2.2
## Features

- **Hide After Looting:** Added an optional loot-beam setting that hides a chest or corpse beam after the first item is taken. Opening the loot menu without taking anything keeps the beam. Confirmed in-game on chests and corpses.
- **Clear on Arrival:** Added an optional destination setting that clears the active destination upon entering the configured `Fade Near` radius. Vanilla and manual custom markers remain, while temporary Look & Pin and Inspect markers are removed. Confirmed in-game across all destination types.

## Fixes

- **Dwarven Small Chest Loot Beams:** Fixed small and jewelry Dwarven chests using `DweContainter02.nif` not being recognized as loot-beam chests. Confirmed in-game in Mzinchaleft.
- **Fully Looted Creature Corpse Beams:** Fixed beams remaining on fully looted creatures when hidden non-playable equipment, such as `ArmorChaurusSpitSkin`, remained in engine inventory. Confirmed in-game on a Chaurus in Mzinchaleft.
- **PrivateProfileRedirector** Fixed settings resets and lost panel positions when PrivateProfileRedirector is installed by migrating `CustomMarkers.ini` persistence to SimpleIni.

# Version 1.2.1
## Features
- **Active Miscellaneous Objectives:** Added miscellaneous objectives to the map List panel. Dynamically resolve alias tokens as the vanilla.

## Fixes
- Fix Scaleform cursor visibility persistence

# Version 1.2

## Features
- **Cursor diagnostics** so that we can find what's causing double cursor issues. set `[Logging] Level=info` and `CursorVerboseLog=1`

## Fixes

- **SSE Display Tweaks Upscaling:** Fixed blank Radar and Inspect backgrounds by preserving the existing local-map render targets. Corrected overlay scaling so custom map pins, badges, and animated rings stay aligned during map tilt and movement, with accurate cursor clicks. Confirmed in-game at 0.75 resolution scale.

# Version 1.1

## Features
- **General Settings Tab:** Restored the General tab in the settings menu (`X`) as a central hub for all major feature and visibility toggles (Beacons, Loot Beams, Look & Pin, Radar, Inspect, Path Tracking, and Map options).
- **Master Feature Toggles:** Added toggles for Path Tracking (`[PathTracking] Enabled`) to turn off recording entirely, and Map Badges (`[Markers] EnableBadges`) to show or hide favorite stars and note dots on vanilla markers.
- **Inspect Overlay Path Toggle:** Pressing the path tracking key (default `T`) while holding the Inspect Overlay (`N`) in interior cells now pauses or resumes path tracking without opening the vanilla Wait menu. Adds on-screen status feedback under the quick-mark hint.
- **Glance Radar Shape:** Added a Circle or Square shape option for the mini radar (`[NavigationOverlay] GlanceShape`). Circle remains the default; Square uses fixed rounded corners. Custom markers, destinations, and active quest markers retain their directional 8 px border alignment in both shapes.
- **Path Trail Performance:** Disabling path tracking now skips all trail processing and point projection behind the scenes instead of just hiding the drawn line.
- **Minimap Snapshot Diagnostic Telemetry:** Added `[Logging] MinimapSnapshotLog` INI setting for detailed diagnostics on snapshot generation, camera frustums, viewports, UV crop bounds, and throttled GPU readback pixel statistics.
- **Loot Beam Min Gold Value Range:** Expanded the in-game settings slider for minimum loose loot gold value from 500 up to 5,000 gold to match the INI upper bound.
- **Input diagnostics**: set `[Logging] Level=info` and `InputVerboseLog=1`
