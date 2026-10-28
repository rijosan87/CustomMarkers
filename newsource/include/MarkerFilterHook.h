#pragma once

#include <array>
#include <cstdint>
#include <string>

#include <SKSE/Interfaces.h>  // MessagingInterface::Message (OnForeignMessage)

namespace RE { class TESObjectREFR; }

// Focus-mode filter for vanilla world map markers. A marker shows if its
// MARKER_TYPE category belongs to at least one enabled focus mode. All modes
// enabled (the default) reproduces vanilla behavior. No modes enabled hides
// every marker except kYouAreHere.
namespace MarkerFilterHook {
    enum class FocusMode : std::size_t {
        kCities = 0,
        kTravel,
        kDungeons,
        kCamps,
        kIndustry,
        kLandmarks,
        kDwellings,
        kQuests,
        // Vendors and crafters. Only reachable through CoMAP icon names —
        // vanilla has no shop MARKER_TYPE, which is a large part of why people
        // install these frameworks in the first place.
        kShops,
        // Item caches, not places: skill books and stashed goods. 47 of the 264
        // modded markers on the reference load order, the single biggest group.
        kLoot,
        // Everything above vanilla's MARKER_TYPE range. CoMAP
        // (MapMarkerFramework.dll) rewrites ExtraMapMarker::mapData->type to
        // customIconIndex + iconNameSlot for any marker its JSONs cover, so
        // these are real types we simply cannot name. NOT a category: a Fort
        // that CoMAP retyped lands here, not in kDungeons. Holding pen until
        // the CoMAP interface lets us bucket by icon NAME instead of index.
        kModded,
        kCount
    };

    void Install();

    // ImGui label for the checkbox row.
    const char* GetModeName(FocusMode a_mode);

    bool IsModeEnabled(FocusMode a_mode);

    // Toggles the mode and triggers a live map rebuild (RefreshMarkers) so the
    // change is visible without closing the map.
    void SetModeEnabled(FocusMode a_mode, bool a_enabled);

    // True when this MARKER_TYPE's focus mode is currently OFF, i.e. the engine
    // hook is hiding the vanilla marker. Takes the raw uint16 rather than
    // RE::MARKER_TYPE because that enum has no fixed underlying type and so
    // cannot be forward-declared; every caller already holds the raw value from
    // MapMarkerData::type.underlying().
    //
    // Our own overlays (fav star, note dot, cleared check, stat card, vanilla
    // right-click menu) draw from our data, not from the engine's marker
    // visibility, so they must call this or they keep painting on top of
    // markers the filter has hidden.
    bool IsTypeFiltered(std::uint16_t a_markerType);

    // Same question, but for a marker whose ref we have. PREFER THIS on any
    // overlay that already holds the ref: for a CoMAP-retyped marker the type
    // alone resolves to kModded, while this consults the icon name and returns
    // the real bucket. Using the type-only form on a badge would hide the
    // marker with Dungeons unchecked and leave its badge floating.
    bool IsMarkerFiltered(RE::TESObjectREFR* a_refr, std::uint16_t a_markerType);

    // How many markers currently sit in each bucket, for the filter panel's
    // "Dungeons (47)" labels. Counts ALL markers in the bucket, including ones
    // the filter is presently hiding — a count that zeroed itself when you
    // unchecked the box would tell you nothing.
    //
    // This is the LOADED MapMenu marker array, not a world census: it moves
    // with worldspace and with tmm state, and it counts MARKERS, not locations.
    // It will NOT agree with the List panel's Overview stats, which walk
    // BGSLocations by keyword instead — different population, both correct.
    std::uint32_t GetModeCount(FocusMode a_mode);

    // --- CoMAP (MapMarkerFramework) soft dependency ---------------------
    // Optional. CoMAP retypes markers to runtime-allocated icon indices, so
    // the index alone can't be categorized (see ModeForType's default case).
    // Its interface hands back the icon NAME instead, which IS stable across
    // load orders — that is what we can eventually bucket on.

    // Feed every foreign-plugin message here; ignores anything that isn't
    // CoMAP's interface broadcast. Called from plugin.cpp's single
    // ForeignPluginMessageListener rather than a listener of our own: SKSE
    // routes a foreign broadcast to only the FIRST null-sender listener per
    // receiving plugin handle, so a second registration returns true and then
    // never fires.
    void OnForeignMessage(SKSE::MessagingInterface::Message* a_msg);

    // Icon name for a marker ("AnimalDenMarker"), or "" when CoMAP is absent,
    // the marker isn't one of its custom icons, or the ref is null.
    //
    // Deliberately wraps GetCustomMarkerName ONLY. The interface also declares
    // GetVanillaMarkerType, but CoMAP's own Get() builds the struct with
    // designated initializers that skip that member, leaving the pointer NULL —
    // calling it crashes. We read the vanilla type ourselves anyway.
    std::string GetCoMAPIconName(RE::TESObjectREFR* a_refr);
}
