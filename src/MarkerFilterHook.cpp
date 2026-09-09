// Filters map markers by category without triggering full menu reloads.

#include "MarkerFilterHook.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "MMF/Interfaces.h"

#include <cstring>

#include <polyhook2/Detour/x64Detour.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#define CMM_FILTER_LEAK_DEBUG 0

#if CMM_FILTER_LEAK_DEBUG
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <utility>
#include <string>
#endif

// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):

namespace {
    using namespace MarkerFilterHook;

    constexpr std::size_t kModeCount = static_cast<std::size_t>(FocusMode::kCount);

    std::array<std::atomic<bool>, kModeCount> g_modeEnabled;

    constexpr const char* kModeNames[kModeCount] = {
        "Cities", "Travel", "Dungeons", "Camps",
        "Industry", "Landmarks", "Dwellings", "Quests",
        "Shops", "Loot", "Modded",
    };

    FocusMode ModeForType(RE::MARKER_TYPE a_type) {
        using T = RE::MARKER_TYPE;
        switch (a_type) {
        case T::kCity:
        case T::kTown:
        case T::kSettlement:
        case T::kRiftenCastle:    case T::kRiftenCapitol:
        case T::kWindhelmCastle:  case T::kWindhelmCapitol:
        case T::kWhiterunCastle:  case T::kWhiterunCapitol:
        case T::kSolitudeCastle:  case T::kSolitudeCapitol:
        case T::kMarkarthCastle:  case T::kMarkarthCapitol:
        case T::kWinterholdCastle:case T::kWinterholdCapitol:
        case T::kMorthalCastle:   case T::kMorthalCapitol:
        case T::kFalkreathCastle: case T::kFalkreathCapitol:
        case T::kDawnstarCastle:  case T::kDawnstarCapitol:
        case T::kDLC02RavenRock:
            return FocusMode::kCities;
        case T::kStable:
        case T::kDocks:
        case T::kFarm:
        case T::kLighthouse:
        case T::kImperialTower:
        case T::kDLC02ToSkyrim:
        case T::kDLC02ToSolstheim:
            return FocusMode::kTravel;
        case T::kCave:
        case T::kNordicRuin:
        case T::kDwemerRuin:
        case T::kFort:
        case T::kShipwreck:
        case T::kDragonLair:
        case T::kGrove:
        case T::kDLC02MiraakTemple:
        case T::kDLC02CastleKarstaag:
            return FocusMode::kDungeons;
        case T::kCamp:
        case T::kImperialCamp:
        case T::kStormcloakCamp:
        case T::kGiantCamp:
        case T::kOrcStronghold:
            return FocusMode::kCamps;
        case T::kMine:
        case T::kWoodMill:
        case T::kWheatMill:
        case T::kSmelter:
            return FocusMode::kIndustry;
        case T::kLandmark:
        case T::kDoomstone:
        case T::kAltar:
        case T::kShrine:
        case T::kRock:
        case T::kClearing:
        case T::kPass:
        case T::kDLC02StandingStone:
        case T::kDLC02TelvanniTower:
            return FocusMode::kLandmarks;
        case T::kShack:
        case T::kNordicTower:
        case T::kNordicDwelling:
            return FocusMode::kDwellings;
        case T::kQuestTarget:
        case T::kMultipleQuestTarget:
        case T::kPlayerSet:
            return FocusMode::kQuests;
        default:
            if (static_cast<std::uint16_t>(a_type) > static_cast<std::uint16_t>(T::kYouAreHere)) {
                return FocusMode::kModded;
            }
            return FocusMode::kCount;
        }
    }

    struct IconRule {
        std::string_view keyword;
        FocusMode        mode;
    };

    constexpr IconRule kIconRules[] = {
        { "Jewel",       FocusMode::kLoot },
        { "Book",        FocusMode::kLoot },
        { "Graveyard",   FocusMode::kLoot },
        { "Battle",      FocusMode::kLoot },
        { "Alchemist",   FocusMode::kShops },
        { "Smith",       FocusMode::kShops },
        { "Tailor",      FocusMode::kShops },
        { "Fletcher",    FocusMode::kShops },
        { "Warehouse",   FocusMode::kShops },
        { "Caravan",     FocusMode::kShops },
        { "Bank",        FocusMode::kShops },
        { "Fence",       FocusMode::kShops },
        { "Store",       FocusMode::kShops },
        { "Vendor",      FocusMode::kShops },
        { "DarkBrotherhood", FocusMode::kQuests },
        { "Companions",      FocusMode::kQuests },
        { "PenitusOculatus", FocusMode::kQuests },
        { "EastEmpire",      FocusMode::kQuests },
        { "BardsCollege",    FocusMode::kQuests },
        { "Thieves",         FocusMode::kQuests },
        { "Nightingale",     FocusMode::kQuests },
        { "Vigilants",       FocusMode::kQuests },
        { "Thalmor",         FocusMode::kQuests },
        { "MageTower",       FocusMode::kQuests },
        { "Mage",            FocusMode::kQuests },
        { "AkaviriTemple",   FocusMode::kQuests },
        { "FortDawnguard",   FocusMode::kQuests },
        { "SoulCairnRuins", FocusMode::kDungeons },
        { "AnimalDen",      FocusMode::kDungeons },
        { "Crypt",          FocusMode::kDungeons },
        { "Mineshaft",      FocusMode::kDungeons },
        { "Jail",           FocusMode::kDungeons },
        { "FalmerHive",     FocusMode::kDungeons },
        { "GlacialCave",    FocusMode::kDungeons },
        { "Shipwreck",      FocusMode::kDungeons },
        { "DwemerLift",     FocusMode::kDungeons },
        { "Coven",          FocusMode::kDungeons },
        { "RootDungeon",    FocusMode::kDungeons },
        { "Ruin",           FocusMode::kDungeons },
        { "Cave",           FocusMode::kDungeons },
        { "Barrow",         FocusMode::kDungeons },
        { "Mine",        FocusMode::kIndustry },
        { "Mill",        FocusMode::kIndustry },
        { "Smelter",     FocusMode::kIndustry },
        { "Oven",        FocusMode::kIndustry },
        { "MinotaurCamp", FocusMode::kCamps },
        { "ReachTribe",   FocusMode::kCamps },
        { "Riekling",     FocusMode::kCamps },
        { "Corsairs",     FocusMode::kCamps },
        { "Pirates",      FocusMode::kCamps },
        { "Camp",         FocusMode::kCamps },
        { "Tavern",        FocusMode::kTravel },
        { "BorderCrossing",FocusMode::kTravel },
        { "Checkpoint",    FocusMode::kTravel },
        { "TravelIcon",    FocusMode::kTravel },
        { "Dock",          FocusMode::kTravel },
        { "Stable",        FocusMode::kTravel },
        { "Lighthouse",    FocusMode::kTravel },
        { "Castle",     FocusMode::kCities },
        { "Palace",     FocusMode::kCities },
        { "City",       FocusMode::kCities },
        { "Town",       FocusMode::kCities },
        { "Settlement", FocusMode::kCities },
        { "Stronghold", FocusMode::kCities },
        { "Manor",      FocusMode::kCities },
        { "ThroatOfTheWorld", FocusMode::kLandmarks },
        { "HighHrothgar",     FocusMode::kLandmarks },
        { "SoulCairn",        FocusMode::kLandmarks },
        { "OblivionGate",     FocusMode::kLandmarks },
        { "Grove",            FocusMode::kLandmarks },
        { "Clearing",         FocusMode::kLandmarks },
        { "Volcano",          FocusMode::kLandmarks },
        { "Oasis",           FocusMode::kLandmarks },
        { "Island",           FocusMode::kLandmarks },
        { "Peak",             FocusMode::kLandmarks },
        { "Shrine",           FocusMode::kLandmarks },
        { "Altar",            FocusMode::kLandmarks },
        { "Tower",            FocusMode::kLandmarks },
        { "Shack",    FocusMode::kDwellings },
        { "Farm",     FocusMode::kDwellings },
        { "Dwelling", FocusMode::kDwellings },
    };

    FocusMode ModeForIconName(std::string_view a_name) {
        if (a_name.empty()) {
            return FocusMode::kModded;
        }
        for (const auto& rule : kIconRules) {
            if (a_name.find(rule.keyword) != std::string_view::npos) {
                return rule.mode;
            }
        }
        return FocusMode::kModded;
    }

    bool IsTypeFilteredImpl(RE::MARKER_TYPE a_type) {
        const auto mode = ModeForType(a_type);
        if (mode == FocusMode::kCount) {
            return false;
        }
        return !g_modeEnabled[static_cast<std::size_t>(mode)].load();
    }

    struct MarkerEntry {
        std::uintptr_t namePtr;
        std::uint64_t  handle;
        std::uint64_t  pad10;
        std::uint32_t  type;
    };
    static_assert(offsetof(MarkerEntry, type) == 0x18);

    std::uint64_t                              g_trampolineAddr = 0;
    std::unique_ptr<PLH::x64Detour>            g_detour;

    MMF::MapMarkerInterface* g_coMAP = nullptr;

#if CMM_FILTER_LEAK_DEBUG

    std::unordered_map<std::string, std::uint32_t> g_iconCensus;
    std::uint32_t                                  g_censusUnnamed = 0;

    bool                                           g_censusDone = false;
    std::unordered_set<std::uint64_t>              g_censusSeen;

    void CensusMarker(const MarkerEntry* a_entry) {
        if (g_censusDone) return;
        if (!g_censusSeen.insert(a_entry->handle).second) {
            g_censusDone = true;
            return;
        }
        if (a_entry->type != 0 || !a_entry->namePtr) return;
        const auto markerType = *reinterpret_cast<const std::uint16_t*>(a_entry->namePtr + 0x12);
        if (markerType <= static_cast<std::uint16_t>(RE::MARKER_TYPE::kYouAreHere)) return;
        RE::NiPointer<RE::TESObjectREFR> refr;
        if (!a_entry->handle || !RE::LookupReferenceByHandle(a_entry->handle, refr) || !refr) return;
        const auto iconName = GetCoMAPIconName(refr.get());
        if (iconName.empty()) {
            ++g_censusUnnamed;
            return;
        }
        ++g_iconCensus[iconName];
    }

    void DumpCensus() {
        if (g_iconCensus.empty() && g_censusUnnamed == 0) return;
        std::vector<std::pair<std::string, std::uint32_t>> rows(
            g_iconCensus.begin(), g_iconCensus.end());
        std::sort(rows.begin(), rows.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::uint32_t named = 0;
        for (const auto& [n, c] : rows) named += c;
        logger::info("[IconCensus] === {} distinct names, {} named markers, {} unnamed, CoMAP={} ===",
                     rows.size(), named, g_censusUnnamed, g_coMAP ? "yes" : "NO");
        for (const auto& [n, c] : rows) {
            logger::info("[IconCensus] {:4} x {}", c, n);
        }
    }
#endif

    std::unordered_map<std::uint16_t, FocusMode> g_moddedModeCache;

    FocusMode ResolveModeForRef(RE::TESObjectREFR* a_refr, std::uint16_t a_type) {
        if (a_type <= static_cast<std::uint16_t>(RE::MARKER_TYPE::kYouAreHere)) {
            return ModeForType(static_cast<RE::MARKER_TYPE>(a_type));
        }
        if (auto it = g_moddedModeCache.find(a_type); it != g_moddedModeCache.end()) {
            return it->second;
        }
        if (!a_refr) {
            return FocusMode::kModded;
        }
        const FocusMode mode = ModeForIconName(GetCoMAPIconName(a_refr));
        g_moddedModeCache[a_type] = mode;
        return mode;
    }

    FocusMode ResolveMode(const MarkerEntry* a_entry, std::uint16_t a_type) {
        if (a_type <= static_cast<std::uint16_t>(RE::MARKER_TYPE::kYouAreHere)) {
            return ModeForType(static_cast<RE::MARKER_TYPE>(a_type));
        }
        if (auto it = g_moddedModeCache.find(a_type); it != g_moddedModeCache.end()) {
            return it->second;
        }
        RE::NiPointer<RE::TESObjectREFR> refr;
        if (a_entry->handle && RE::LookupReferenceByHandle(a_entry->handle, refr) && refr) {
            return ResolveModeForRef(refr.get(), a_type);
        }
        return FocusMode::kModded;
    }

    std::array<std::atomic<std::uint32_t>, kModeCount> g_modeCountPublished{};
    std::array<std::uint32_t, kModeCount>              g_modeCountBuilding{};
    std::unordered_set<std::uint64_t>                  g_countSeen;

    void CountMarker(const MarkerEntry* a_entry, FocusMode a_mode) {
        if (a_mode == FocusMode::kCount) {
            return;
        }
        if (!g_countSeen.insert(a_entry->handle).second) {
            for (std::size_t i = 0; i < kModeCount; ++i) {
                g_modeCountPublished[i].store(g_modeCountBuilding[i]);
                g_modeCountBuilding[i] = 0;
            }
            g_countSeen.clear();
            g_countSeen.insert(a_entry->handle);
        }
        ++g_modeCountBuilding[static_cast<std::size_t>(a_mode)];
    }

    bool HookedIsMarkerVisible(MarkerEntry* a_entry, void* a_arr) {
        using func_t = bool(*)(MarkerEntry*, void*);
        const bool engineVisible = REL::Relocation<func_t>(g_trampolineAddr)(a_entry, a_arr);
        if (!engineVisible || !a_entry) {
            return engineVisible;
        }
#if CMM_FILTER_LEAK_DEBUG
        CensusMarker(a_entry);
#endif
        if (a_entry->type == 0 && a_entry->namePtr) {
            const auto type = *reinterpret_cast<const std::uint16_t*>(a_entry->namePtr + 0x12);
            const auto mode = ResolveMode(a_entry, type);
            CountMarker(a_entry, mode);
            if (mode != FocusMode::kCount &&
                !g_modeEnabled[static_cast<std::size_t>(mode)].load()) {
                return false;
            }
        }
        return true;
    }
}

namespace MarkerFilterHook {
    void Install() {
        for (auto& m : g_modeEnabled) {
            m.store(true);
        }
        REL::Relocation<std::uintptr_t> target{ REL::RelocationID(52192, 53084) };
        const std::uint64_t targetAddr = target.address();
        const std::uint64_t hookAddr   = reinterpret_cast<std::uint64_t>(&HookedIsMarkerVisible);
        g_detour = std::make_unique<PLH::x64Detour>(targetAddr, hookAddr, &g_trampolineAddr);
        if (!g_detour->hook()) {
            logger::error("MarkerFilterHook: PolyHook x64Detour::hook() returned false");
            g_detour.reset();
            return;
        }
        logger::info("MarkerFilterHook installed | target={:X} | trampoline={:X}",
                     targetAddr, g_trampolineAddr);
    }

    const char* GetModeName(FocusMode a_mode) {
        const auto i = static_cast<std::size_t>(a_mode);
        return i < kModeCount ? kModeNames[i] : "";
    }

    bool IsTypeFiltered(std::uint16_t a_markerType) {
        return IsTypeFilteredImpl(static_cast<RE::MARKER_TYPE>(a_markerType));
    }

    std::uint32_t GetModeCount(FocusMode a_mode) {
        const auto i = static_cast<std::size_t>(a_mode);
        return i < kModeCount ? g_modeCountPublished[i].load() : 0u;
    }

    bool IsMarkerFiltered(RE::TESObjectREFR* a_refr, std::uint16_t a_markerType) {
        const auto mode = ResolveModeForRef(a_refr, a_markerType);
        if (mode == FocusMode::kCount) {
            return false;
        }
        return !g_modeEnabled[static_cast<std::size_t>(mode)].load();
    }

    void OnForeignMessage(SKSE::MessagingInterface::Message* a_msg) {
        auto* before = g_coMAP;
        MMF::QueryMapMarkerInterface(a_msg, g_coMAP);
        if (g_coMAP && g_coMAP != before) {
            logger::info("CoMAP MapMarkerInterface acquired (version {}).", g_coMAP->Version());
        }
    }

    std::string GetCoMAPIconName(RE::TESObjectREFR* a_refr) {
        if (!g_coMAP || !a_refr) {
            return {};
        }
        return g_coMAP->GetCustomMarkerName(a_refr);
    }

    bool IsModeEnabled(FocusMode a_mode) {
        const auto i = static_cast<std::size_t>(a_mode);
        return i < kModeCount && g_modeEnabled[i].load();
    }

    void SetModeEnabled(FocusMode a_mode, bool a_enabled) {
        const auto i = static_cast<std::size_t>(a_mode);
        if (i >= kModeCount) {
            return;
        }
        g_modeEnabled[i].store(a_enabled);
        logger::debug("[MarkerFilter] {} -> {}", kModeNames[i], a_enabled ? "on" : "off");
#if CMM_FILTER_LEAK_DEBUG
        DumpCensus();
        g_iconCensus.clear();
        g_censusUnnamed = 0;
        g_censusSeen.clear();
        g_censusDone = false;
#endif
    }
}
