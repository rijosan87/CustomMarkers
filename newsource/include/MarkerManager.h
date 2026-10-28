#pragma once

#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// Define the Data Structure
enum class MarkerCategory : std::uint32_t {
    Custom = 0,         // Player set marker
    City = 1,
    Town = 2,
    Settlement = 3,
    Cave = 4,
    Camp = 5,
    Fort = 6,
    NordicRuin = 7,
    DwemerRuin = 8,
    Shipwreck = 9,
    Grove = 10,
    Landmark = 11,
    DragonLair = 12,
    Farm = 13,
    WoodMill = 14,
    Mine = 15,
    ImperialCamp = 16,
    StormcloakCamp = 17,
    Doomstone = 18,
    WheatMill = 19,
    Smelter = 20,
    Temple = 30
};

// Maps a marker iconType (MarkerCategory value) to the PNG filename stem in
// Data\Interface\CustomMapMarkers\Icons\<stem>.png. Single source of truth.
// Unknown values fall back to "custom" so a corrupted save still renders.
inline const char* CategoryToFilename(std::uint32_t iconType) {
    switch (iconType) {
        case 1:  return "city";
        case 2:  return "town";
        case 3:  return "settlement";
        case 4:  return "cave";
        case 5:  return "camp";
        case 6:  return "fort";
        case 7:  return "nordic_ruin";
        case 8:  return "dwemer_ruin";
        case 9:  return "shipwreck";
        case 10: return "grove";
        case 11: return "landmark";
        case 12: return "dragon_lair";
        case 13: return "farm";
        case 14: return "wood_mill";
        case 15: return "mine";
        case 16: return "imperial_camp";
        case 17: return "stormcloak_camp";
        case 18: return "doomstone";
        case 19: return "wheat_mill";
        case 20: return "smelter";
        case 30: return "temple";   // vanilla kShack; interior list labels it "Shack"
        // Interior category set (vanilla MARKER_TYPE ids). PNGs must exist in
        // the Icons folder with these stems or the note-dialog grid falls back
        // to a text cell (pins fall back to the default pin).
        case 21: return "stable";
        case 23: return "clearing";
        case 25: return "altar";
        case 26: return "rock";
        case 27: return "lighthouse";
        case 32: return "dwelling";
        case 33: return "docks";
        case 34: return "shrine";
        // Added 2026-08-15. Vanilla compass art matches the name for each of
        // these, so the map pin and the compass glyph agree.
        case 22: return "imperial_tower";
        case 24: return "pass";
        case 28: return "orc_stronghold";
        case 29: return "giant_camp";
        case 31: return "nordic_tower";
        // DLC02 (Solstheim) ids. The hudmenu.swf frames exist whether or not
        // Dragonborn is installed, so these render on the compass either way.
        // 57/58 (ToSkyrim/ToSolstheim travel arrows) deliberately skipped.
        case 53: return "miraak_temple";
        case 54: return "raven_rock";
        case 55: return "standing_stone";
        case 56: return "telvanni_tower";
        case 59: return "castle_karstaag";
        case 61: return "door";
        default: return "custom";
    }
}

// How a marker with no user-written note names itself. Stored as DATA, not as
// the finished English string, so switching language retranslates every
// auto-named marker including ones already in the save. Storing the text was
// also what forced NextPinNumber() to parse "Pinned Location N" back out of the
// note -- translate that string and the parse silently failed, restarting the
// numbering at 1.
enum class AutoName : std::uint8_t {
    None = 0,       // note holds the user's own words; never overwrite it
    Custom = 1,     // map/local-map placement    -> Labels.AutoCustomMarker
    Pinned = 2,     // Look & Pin                 -> Labels.AutoPinnedLocation
    QuickMark = 3,  // quick-mark breadcrumb      -> Labels.AutoQuickMark
};

// Creation provenance is separate from the display name. Exterior Inspect
// quick marks intentionally use the same Custom auto-name as unnamed manual
// map markers, so AutoName cannot safely decide which markers are temporary.
enum class MarkerOrigin : std::uint8_t {
    Manual = 0,
    LookAndPin = 1,
    InspectQuickMark = 2,
};

struct MapMarker {
    std::uint32_t id;           // Unique ID
    RE::FormID worldSpaceID;    // Which world is this in? (Tamriel, Sovngarde, etc.)
    RE::FormID cellID;          // Specific cell (for interiors)
    RE::NiPoint3 position;      // World coordinates
    std::string note;           // The user's custom note ("" when auto-named)
    std::uint32_t iconType;     // Future proofing: Icon style (0=default, 1=loot, etc)
    std::uint64_t timestamp;    // When was it created?
    bool isDestination = false; // Active destination marker (only one at a time).
    // v5. autoNameNumber is the N in "Custom Marker N" -- per-scheme, not the
    // marker id, so Pinned and Custom count independently.
    AutoName autoName = AutoName::None;
    std::uint32_t autoNameNumber = 0;
    // v8. Creation path controls whether arrival removes the whole marker.
    MarkerOrigin origin = MarkerOrigin::Manual;
};

// The name to draw for a marker: the user's note when they wrote one, otherwise
// the localized auto-name resolved fresh every call. Never cached and never
// written back into note -- that round trip is what made the old scheme
// untranslatable.
std::string ResolveMarkerName(const MapMarker& marker);

// Next free N for an auto-name scheme: max existing + 1, so deleting #3 leaves a
// gap rather than reusing the number.
std::uint32_t NextAutoNameNumber(AutoName scheme);

// Reserved id for the synthesized vanilla-destination entry returned by
// GetMarkers(). Runtime allocation skips this value, so it can never collide
// with a real marker. Consumers that must tell the synthesized entry
// from a real one compare against this instead of sniffing a note prefix: an id
// compare cannot be silently forgotten the way a string strip can.
inline constexpr std::uint32_t kVanillaDestId = 0xFFFFFFFFu;

// The active VANILLA destination: a map marker or local-map door the player
// chose to navigate to. Deliberately NOT a MapMarker in _markers. It is not
// something the player placed, and when it lived there it drew as a stray pin on
// the local map, could be renamed/removed like a real marker, and was reachable
// only by exact name match from the menu that created it.
// A custom marker set as destination is the OTHER case and still uses
// MapMarker::isDestination; the two are mutually exclusive.
struct VanillaDestination {
    RE::FormID   worldSpaceID = 0;   // 0 for an interior door
    RE::FormID   cellID       = 0;
    RE::NiPoint3 position{};
    std::string  name;               // display name, NO "[Destination] " prefix
    RE::FormID   sourceFormID = 0;   // 0 only for a destination migrated from v6.
    bool         active = false;     // false = no destination set
    // True when the record came from the LOCAL-map door menu. A door has no
    // world-map icon, so its ring on the world map floats over empty terrain and
    // is suppressed; the exterior local map (where the door icon lives) draws it.
    // A vanilla world MARKER set as destination leaves this false and keeps the
    // world-map ring, which sits correctly under its icon.
    bool         fromLocalDoor = false;
};

struct FavoriteMarker {
    RE::FormID formID;          // Hovered marker's identity
    RE::FormID worldSpaceID;    // Parent worldspace (or 0)
    RE::FormID cellID;          // Parent cell (for interiors, or 0)
    RE::NiPoint3 position;      // 3D coordinates
    std::string name;           // Display name
};

class MarkerManager {
public:
    // Singleton Access
    static MarkerManager* GetSingleton() {
        static MarkerManager singleton;
        return &singleton;
    }

    // Core Functions
    std::uint32_t AddMarker(MapMarker marker);
    std::uint32_t AddMarker(RE::FormID worldSpaceID, RE::FormID cellID,
                            const RE::NiPoint3& position, std::uint32_t iconType,
                            const std::string& note);
    // Auto-named variant: note stays empty and the name is resolved at draw
    // time from the scheme + number, so it follows the current language. Pass
    // number 0 to have the next free one picked for the scheme.
    std::uint32_t AddAutoNamedMarker(RE::FormID worldSpaceID, RE::FormID cellID,
                                     const RE::NiPoint3& position, std::uint32_t iconType,
                                     AutoName scheme, std::uint32_t number = 0,
                                     MarkerOrigin origin = MarkerOrigin::Manual);
    bool RemoveMarker(std::uint32_t id);
    // Markers the PLAYER placed, with no synthesized destination entry. Use this
    // anywhere the user can act on a row (list panel, hover, right-click).
    std::vector<MapMarker> GetUserMarkers();
    // Edit an existing marker in place (id/position/timestamp unchanged). Used by
    // the hover-panel "Edit Note" path. Returns false if no marker has that id or
    // string length exceeds limit.
    bool UpdateMarker(std::uint32_t id, const std::string& note, std::uint32_t iconType);
    // Snapshot of all markers. INCLUDES a synthesized entry for the active
    // vanilla destination (id == kVanillaDestId) so the compass, beacons, radar
    // and map draw it without special-casing. The entry does not exist in
    // _markers: it cannot be renamed, removed, or serialized as a marker.
    std::vector<MapMarker> GetMarkers();

    // Marks one marker as the active destination. Clears the flag on all
    // others so only one destination exists at a time. Pass flag=false to
    // clear the destination on the given marker (and leave others untouched).
    void SetDestination(std::uint32_t id, bool flag);

    // ID of the current destination marker, or 0 if none.
    std::uint32_t GetDestinationID();
    
    // Vanilla destination helper
    bool SetVanillaDestination(const std::string& name, RE::FormID sourceFormID,
                               RE::FormID worldSpaceID, RE::FormID cellID,
                               const RE::NiPoint3& position, bool fromLocalDoor = false);
    // True when the active vanilla destination came from a local-map door.
    bool IsVanillaDestFromLocalDoor() const;
    bool IsVanillaDestination(const std::string& name) const;
    void RemoveVanillaDestination();

    // Name-agnostic. True when ANY vanilla destination is set.
    bool HasVanillaDestination() const;
    // Copy of the record for READ-ONLY display (the list panel's Destination
    // row). Deliberately not a reference or a pointer into _vanillaDest: the
    // whole point of the record living outside _markers is that no UI can
    // reach it and rename or remove it. Check .active before using the rest.
    VanillaDestination GetVanillaDestination() const;
    // Clears whichever kind of destination is active (record or marker flag).
    void ClearDestination();

    // Favorites
    bool ToggleFavorite(RE::FormID formID, RE::FormID worldSpaceID, RE::FormID cellID, const RE::NiPoint3& position, const std::string& name);
    bool IsFavorite(RE::FormID formID) const;
    std::vector<FavoriteMarker> GetFavorites() const;

    // Vanilla-marker notes. A note attached to a vanilla map marker (a town,
    // dungeon, etc.), keyed by the marker's ref FormID like favorites are. The
    // vanilla marker keeps its own title; this note is shown as a separate body
    // line in the hover card, never stacked on the title. Empty note removes it.
    bool SetVanillaNote(RE::FormID formID, const std::string& note);
    std::string GetVanillaNote(RE::FormID formID) const;  // "" if none

    // Snapshot of all vanilla notes for the list panel's "Notes" section.
    // Same pattern as GetFavorites(): copy under lock, caller resolves the
    // ref/name live per frame since we don't cache display names here.
    std::vector<std::pair<RE::FormID, std::string>> GetVanillaNotes() const;

    // Serialization (Save/Load)
    void OnGameSave(SKSE::SerializationInterface* serde);
    void OnGameLoad(SKSE::SerializationInterface* serde);
    void OnRevert(SKSE::SerializationInterface* serde);

private:
    MarkerManager() = default;
    MarkerManager(const MarkerManager&) = delete;
    MarkerManager& operator=(const MarkerManager&) = delete;

    // Allocates next free ID avoiding 0, kVanillaDestId, and all existing marker IDs.
    // Must be called with _mutex held.
    std::uint32_t AllocateMarkerIDLocked();

    // Thread safety is good practice even if not strictly needed for basic tests
    mutable std::mutex _mutex;
    std::vector<MapMarker> _markers;
    std::vector<FavoriteMarker> _favorites;
    std::unordered_map<RE::FormID, std::string> _vanillaNotes;  // v4: vanilla-marker notes
    VanillaDestination _vanillaDest;                            // v6: active vanilla destination (v7: sourceFormID + fromLocalDoor)
    std::uint32_t _nextID = 1;
};
