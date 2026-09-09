#pragma once

// Core data store and cosave serialization for custom markers and notes.


#include <mutex>
#include <vector>
#include <string>
#include <unordered_map>
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

enum class MarkerCategory : std::uint32_t {
    Custom = 0,
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
        case 30: return "temple";
        case 21: return "stable";
        case 23: return "clearing";
        case 25: return "altar";
        case 26: return "rock";
        case 27: return "lighthouse";
        case 32: return "dwelling";
        case 33: return "docks";
        case 34: return "shrine";
        case 22: return "imperial_tower";
        case 24: return "pass";
        case 28: return "orc_stronghold";
        case 29: return "giant_camp";
        case 31: return "nordic_tower";
        case 53: return "miraak_temple";
        case 54: return "raven_rock";
        case 55: return "standing_stone";
        case 56: return "telvanni_tower";
        case 59: return "castle_karstaag";
        case 61: return "door";
        default: return "custom";
    }
}

enum class AutoName : std::uint8_t {
    None = 0,
    Custom = 1,
    Pinned = 2,
    QuickMark = 3,
};

struct MapMarker {
    std::uint32_t id;
    RE::FormID worldSpaceID;
    RE::FormID cellID;
    RE::NiPoint3 position;
    std::string note;
    std::uint32_t iconType;
    std::uint64_t timestamp;
    bool isDestination = false;
    AutoName autoName = AutoName::None;
    std::uint32_t autoNameNumber = 0;
};

std::string ResolveMarkerName(const MapMarker& marker);

std::uint32_t NextAutoNameNumber(AutoName scheme);

inline constexpr std::uint32_t kVanillaDestId = 0xFFFFFFFFu;

struct VanillaDestination {
    RE::FormID   worldSpaceID = 0;
    RE::FormID   cellID       = 0;
    RE::NiPoint3 position{};
    std::string  name;
    RE::FormID   sourceFormID = 0;
    bool         active = false;
    bool         fromLocalDoor = false;
};

struct FavoriteMarker {
    RE::FormID formID;
    RE::FormID worldSpaceID;
    RE::FormID cellID;
    RE::NiPoint3 position;
    std::string name;
};

class MarkerManager {
public:
    static MarkerManager* GetSingleton() {
        static MarkerManager singleton;
        return &singleton;
    }
    std::uint32_t AddMarker(MapMarker marker);
    std::uint32_t AddMarker(RE::FormID worldSpaceID, RE::FormID cellID,
                            const RE::NiPoint3& position, std::uint32_t iconType,
                            const std::string& note);
    std::uint32_t AddAutoNamedMarker(RE::FormID worldSpaceID, RE::FormID cellID,
                                     const RE::NiPoint3& position, std::uint32_t iconType,
                                     AutoName scheme, std::uint32_t number = 0);
    bool RemoveMarker(std::uint32_t id);
    std::vector<MapMarker> GetUserMarkers();
    bool UpdateMarker(std::uint32_t id, const std::string& note, std::uint32_t iconType);
    std::vector<MapMarker> GetMarkers();
    void SetDestination(std::uint32_t id, bool flag);
    std::uint32_t GetDestinationID();
    bool SetVanillaDestination(const std::string& name, RE::FormID sourceFormID,
                               RE::FormID worldSpaceID, RE::FormID cellID,
                               const RE::NiPoint3& position, bool fromLocalDoor = false);
    bool IsVanillaDestFromLocalDoor() const;
    bool IsVanillaDestination(const std::string& name) const;
    void RemoveVanillaDestination();
    bool HasVanillaDestination() const;
    VanillaDestination GetVanillaDestination() const;
    void ClearDestination();
    bool ToggleFavorite(RE::FormID formID, RE::FormID worldSpaceID, RE::FormID cellID, const RE::NiPoint3& position, const std::string& name);
    bool IsFavorite(RE::FormID formID) const;
    std::vector<FavoriteMarker> GetFavorites() const;
    bool SetVanillaNote(RE::FormID formID, const std::string& note);
    std::string GetVanillaNote(RE::FormID formID) const;
    std::vector<std::pair<RE::FormID, std::string>> GetVanillaNotes() const;
    void OnGameSave(SKSE::SerializationInterface* serde);
    void OnGameLoad(SKSE::SerializationInterface* serde);
    void OnRevert(SKSE::SerializationInterface* serde);
private:
    MarkerManager() = default;
    MarkerManager(const MarkerManager&) = delete;
    MarkerManager& operator=(const MarkerManager&) = delete;
    std::uint32_t AllocateMarkerIDLocked();
    mutable std::mutex _mutex;
    std::vector<MapMarker> _markers;
    std::vector<FavoriteMarker> _favorites;
    std::unordered_map<RE::FormID, std::string> _vanillaNotes;
    VanillaDestination _vanillaDest;
    std::uint32_t _nextID = 1;
};
