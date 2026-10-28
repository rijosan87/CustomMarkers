#include "MarkerFilterHook.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include "MMF/Interfaces.h"  // CoMAP consumer API (vendored, header-only)

#include <cstring>

#include <polyhook2/Detour/x64Detour.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

// Flip to 1 to census CoMAP icon names on the live map (name -> marker count,
// dumped on any filter toggle). This is how kIconRules was built and it is how
// to extend it: a load order with different CoMAP addons ships different names,
// and the census is the only way to see which ones actually appear and how
// often. Off by default; the per-bucket counts in the filter panel are always
// on and cover the everyday "did that rule land" question.
#define CMM_FILTER_LEAK_DEBUG 0

#if CMM_FILTER_LEAK_DEBUG
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <utility>
#include <string>
#endif

// ============================================================================
// MarkerFilterHook — focus-mode category filter for vanilla world map markers
// ----------------------------------------------------------------------------
// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):
//   Working: detours IsMarkerVisible (ID 53084), the per-marker visibility
//            check inside the per-frame AdvanceMovie loop. We let the engine
//            compute its own visibility, then force false when the marker's
//            focus mode is disabled. The next frame pushes _visible=false to
//            Flash, so toggling a mode hides/shows markers instantly with NO
//            clip teardown and NO blink. Filter state is just an atomic flip.
//   Type read is in-hand, no per-frame handle lookup: for base markers
//            (MarkerEntry.type==0), namePtr at +0x00 is a MapMarkerData* and
//            the MARKER_TYPE is the uint16 at namePtr+0x12 (engine's own
//            GetMarkerType does the same). Other dispatch types have implicit
//            categories and are left to the engine.
//   Filter state lives in memory only (g_modeEnabled). NOT persisted to INI.
//   IsTypeFiltered() is exported so our OWN overlays can gate themselves.
//            They draw from our data, not from engine marker visibility, so
//            without it a filtered-out marker keeps its fav star / note dot /
//            cleared check / stat card / right-click menu. Callers pass the raw
//            uint16 from MapMarkerData::type (RE::MARKER_TYPE has no fixed
//            underlying type, so it can't be forward-declared in the header).
//   kModded catches every type above kYouAreHere. CoMAP
//            (MapMarkerFramework.dll) rewrites ExtraMapMarker::mapData->type to
//            customIconIndex + iconNameSlot for markers its MapMarkers/*.json
//            cover, which is why an all-modes-off map still showed ~20 markers
//            (types 69..174). Confirmed by log: the read at namePtr+0x12 agreed
//            with ExtraMapMarker on all 120 sampled entries, so the values are
//            real, just unnameable. The indices are ALLOCATED at load
//            (_iconNames[name] = _lastIcon++ in CoMAP's MapConfigLoader.cpp),
//            so they shift whenever an addon is added or removed — the check is
//            range-based for exactly that reason. Never hardcode an index.
//            kModded is a holding pen, NOT a category: a Fort that CoMAP
//            retyped lands here instead of kDungeons. CoMAP publishes an SKSE
//            interface (GetCustomMarkerName -> a stable icon NAME) that would
//            let us bucket properly; not wired yet.
//   KNOWN GAP: only base entries (MarkerEntry.type==0) are filtered at all.
//            Stacked/city/quest dispatch entries pass through untouched. The
//            one such entry seen in the diag log was a Door (always-show), so
//            this has not actually cost anything yet.
//   AE 1.6.1170. RVA comes from Address Library ID 53084.
// ============================================================================
//
// See findings/findings-WorldMap-Marker-Refresh-Blink.md sections 5-6 for the
// hook point and the in-hand type read. Category mapping (ModeForType) and the
// MARKER_TYPE enum are from findings-WorldMap-Marker-Category-Filter.md.
namespace {
    using namespace MarkerFilterHook;

    constexpr std::size_t kModeCount = static_cast<std::size_t>(FocusMode::kCount);

    // All modes start enabled (set true in Install), so the map looks vanilla
    // until the player filters. atomic<bool> isn't copyable, so this array can't
    // be brace-initialized; it value-inits to false and Install flips them on.
    std::array<std::atomic<bool>, kModeCount> g_modeEnabled;

    constexpr const char* kModeNames[kModeCount] = {
        "Cities", "Travel", "Dungeons", "Camps",
        "Industry", "Landmarks", "Dwellings", "Quests",
        "Shops", "Loot", "Modded",
    };

    // Maps each MARKER_TYPE to the focus mode that governs it. kCount means the
    // type is always shown (kYouAreHere, doors, none).
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
        // kQuestTargetDoor is omitted: NG aliases it to kMultipleQuestTarget,
        // so listing both is a duplicate-case error.
        case T::kMultipleQuestTarget:
        case T::kPlayerSet:
            return FocusMode::kQuests;

        default:
            // Anything past the last vanilla enumerator is a CoMAP-assigned
            // custom icon index (customIconIndex + iconNameSlot, allocated at
            // load time from whichever MapMarkers/*.json files are installed —
            // so the NUMBERS shift when addons change and must never be
            // hardcoded). Bucket them all under kModded; the comparison is
            // range-based precisely so it survives that reshuffling.
            if (static_cast<std::uint16_t>(a_type) > static_cast<std::uint16_t>(T::kYouAreHere)) {
                return FocusMode::kModded;
            }
            // Genuine always-show vanilla types: kNone, kDoor, kYouAreHere.
            return FocusMode::kCount;
        }
    }

    // CoMAP icon name -> focus mode, by SUBSTRING. Names are stable across load
    // orders; the numeric type they resolve to is not (see ModeForType), so
    // this table is the only safe way to categorize a modded marker.
    //
    // Substring rather than exact match on purpose: CoMAP addons prefix and
    // suffix freely (CUTMageMarker, ATLASImperialOutpostMarker,
    // MasleaCityMarker), and an addon we have never seen still has a good
    // chance of matching a family keyword. Anything that misses falls through
    // to kModded, which stays as the honest catch-all.
    //
    // ORDER MATTERS — first match wins. Specific entries must precede the
    // generic ones they contain: "SoulCairnRuins" before "Ruins", "MageTower"
    // before "Mage", "GlacialCave" before "Cave". Reordering this table
    // silently changes categories.
    struct IconRule {
        std::string_view keyword;
        FocusMode        mode;
    };

    constexpr IconRule kIconRules[] = {
        // --- Loot caches. Not places: an item lies there, nothing else. ---
        // WARNING: a CoMAP icon name describes the ART, not the place. Atlas
        // reuses one icon across unrelated POIs, so these three are judgement
        // calls made by reading what the refs are actually called in
        // "Atlas Map Markers.json", not by trusting the icon name:
        //   Jewel(43)     -> chests, treasure, sunken boats        (clean)
        //   Book(24)      -> "Summit Stash", "Surplus Goods"       (clean)
        //   Graveyard(28) -> MIXED: "The Skeleton's Strong Box", "Bandit
        //                    Thief", "Pincushion Peter" dominate, but Morthal
        //                    Cemetery is in there too. Caches win on volume, so
        //                    a stray cemetery in Loot beats ~24 stashes in
        //                    Dungeons.
        // Counts are from parsing all 313 refID entries in Atlas Map Markers
        // .json, not from sampling — an early pass off the first six rows put
        // Crypt in kCities and was wrong.
        { "Jewel",       FocusMode::kLoot },
        { "Book",        FocusMode::kLoot },
        { "Graveyard",   FocusMode::kLoot },
        // Ambush/massacre sites: "Burning Caravan", "Khajiit Caravan
        // Massacre", "Lurker Ambush" x3. Lootable encounter spots, not places.
        { "Battle",      FocusMode::kLoot },

        // --- Shops. Before the generic place rules: a "StoreMarker" is a
        //     shop, and "Warehouse"/"Caravan" are trade, not industry. ---
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

        // --- Factions / guild halls. Before "Camp" so ThalmorCamp reads as a
        //     faction hold rather than a bandit camp, and before "Tower"/"Mage"
        //     so MageTower lands here too. ---
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

        // --- Dungeons. "SoulCairnRuins"/"SoulCairnTower" must precede the bare
        //     "SoulCairn" landmark rule further down. ---
        { "SoulCairnRuins", FocusMode::kDungeons },
        { "AnimalDen",      FocusMode::kDungeons },  // bear caves, sabre cat lairs
        // Halls of the Dead AND the "Black Chapel/Church/Mausoleum/Rectory"
        // family, which are dungeon interiors. Briefly moved to kCities off a
        // 6-row sample that showed only the city Halls; the full 18 do not
        // support that. Dungeons is the better fit for the mixed set.
        { "Crypt",          FocusMode::kDungeons },
        // Dungeon EXITS, not industry: "Helgen Escape Tunnel", "Bleak Falls
        // Sanctum", "Embershard Mine Exit". Must precede the "Mine" rule.
        { "Mineshaft",      FocusMode::kDungeons },
        // Dungeon sub-areas: "Fort Snowhawk Prison", "Silent City Catacombs",
        // "Ilinalta's Deluge" — not a faction/quest location.
        { "Jail",           FocusMode::kDungeons },
        { "FalmerHive",     FocusMode::kDungeons },
        { "GlacialCave",    FocusMode::kDungeons },
        { "Shipwreck",      FocusMode::kDungeons },
        { "DwemerLift",     FocusMode::kDungeons },
        { "Coven",          FocusMode::kDungeons },
        { "RootDungeon",    FocusMode::kDungeons },
        { "Ruin",           FocusMode::kDungeons },  // Daedric/Akaviri/Elven/Redguard/Snow Elf
        { "Cave",           FocusMode::kDungeons },
        { "Barrow",         FocusMode::kDungeons },

        // --- Industry. After Shops so a Smith is a shop, not a smelter. ---
        { "Mine",        FocusMode::kIndustry },
        { "Mill",        FocusMode::kIndustry },
        { "Smelter",     FocusMode::kIndustry },
        { "Oven",        FocusMode::kIndustry },

        // --- Camps. After the faction rules above. ---
        { "MinotaurCamp", FocusMode::kCamps },
        { "ReachTribe",   FocusMode::kCamps },
        { "Riekling",     FocusMode::kCamps },
        { "Corsairs",     FocusMode::kCamps },
        { "Pirates",      FocusMode::kCamps },
        { "Camp",         FocusMode::kCamps },

        // --- Travel. Taverns are how people navigate a map, so they group with
        //     stables/docks rather than with Cities. ---
        { "Tavern",        FocusMode::kTravel },
        { "BorderCrossing",FocusMode::kTravel },
        { "Checkpoint",    FocusMode::kTravel },
        { "TravelIcon",    FocusMode::kTravel },
        { "Dock",          FocusMode::kTravel },
        { "Stable",        FocusMode::kTravel },
        { "Lighthouse",    FocusMode::kTravel },

        // --- Cities / settlements. "Ruined*" variants are dead settlements,
        //     but they still read as places on the map, so they stay here. ---
        { "Castle",     FocusMode::kCities },
        { "Palace",     FocusMode::kCities },
        { "City",       FocusMode::kCities },
        { "Town",       FocusMode::kCities },
        { "Settlement", FocusMode::kCities },
        { "Stronghold", FocusMode::kCities },
        { "Manor",      FocusMode::kCities },

        // --- Landmarks. Generic catch-alls last. ---
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

        // --- Dwellings. Last: "Shack"/"Farm" are weak keywords. ---
        { "Shack",    FocusMode::kDwellings },
        { "Farm",     FocusMode::kDwellings },
        { "Dwelling", FocusMode::kDwellings },
    };

    // Case-sensitive on purpose: CoMAP names are PascalCase by convention and
    // a case-insensitive pass would make "Mine" match "Mineshaft" ambiguities
    // worse, not better.
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
            return false;  // always-show category
        }
        return !g_modeEnabled[static_cast<std::size_t>(mode)].load();
    }

    // One entry in the MapMenu+0x30470 marker array (0x38 stride). Only the
    // fields we touch are named; see findings-World-Map-Hover-Identity.md.
    // For base markers (type==0) namePtr is a MapMarkerData* and the icon
    // category is the uint16 at namePtr+0x12.
    struct MarkerEntry {
        std::uintptr_t namePtr;   // 0x00: MapMarkerData* when type==0
        std::uint64_t  handle;    // 0x08
        std::uint64_t  pad10;     // 0x10
        std::uint32_t  type;      // 0x18: 0 = base MapMarker, else stacked/quest/etc
    };
    static_assert(offsetof(MarkerEntry, type) == 0x18);

    std::uint64_t                              g_trampolineAddr = 0;
    std::unique_ptr<PLH::x64Detour>            g_detour;

    // CoMAP interface. Null until its kMapMarkerInterface broadcast arrives,
    // and stays null forever when CoMAP isn't installed — every read must
    // null-check. Same soft-dependency shape as the Party Sheet API listener
    // in plugin.cpp.
    MMF::MapMarkerInterface* g_coMAP = nullptr;

#if CMM_FILTER_LEAK_DEBUG
    // DIAG round 3 — CENSUS, not a leak hunt. Rounds 1-2 settled the mechanism:
    // the read at namePtr+0x12 is correct (120/120 "agree"), the leftovers are
    // real types above vanilla's 65 that CoMAP assigns from
    // customIconIndex + iconNameSlot. So there is nothing left to disprove.
    //
    // This round answers the one question that decides the keyword rules: on a
    // REAL load order, which icon names actually appear, and how many markers
    // carry each. CoMAP ships 212 names, but name count is not what matters —
    // a rule matching "TownMarker" may cover 40 pins while
    // "MasleaPentagramMarker" covers one. Writing rules against the JSON list
    // would optimize for the wrong names.
    //
    // Keyed by NAME (not handle) and counted, so one dump gives the
    // distribution directly. Reported on toggle rather than per marker: a
    // per-marker line would need sorting by hand to answer the same question.
    std::unordered_map<std::string, std::uint32_t> g_iconCensus;
    std::uint32_t                                  g_censusUnnamed = 0;  // modded type, no CoMAP name
    // One pass per toggle cycle. This hook runs per-marker per-FRAME and
    // GetCustomMarkerName returns std::string by value, so leaving it uncapped
    // would allocate hundreds of strings every frame and inflate every count by
    // the frame number. Latched false on toggle, back true once a pass is done.
    bool                                           g_censusDone = false;
    std::unordered_set<std::uint64_t>              g_censusSeen;

    void CensusMarker(const MarkerEntry* a_entry) {
        if (g_censusDone) return;
        // Same handle twice = the array wrapped, so one full pass is done.
        if (!g_censusSeen.insert(a_entry->handle).second) {
            g_censusDone = true;
            return;
        }
        // Only census the kModded bucket: vanilla-typed markers are already
        // classified and would just dilute the counts.
        if (a_entry->type != 0 || !a_entry->namePtr) return;
        const auto markerType = *reinterpret_cast<const std::uint16_t*>(a_entry->namePtr + 0x12);
        if (markerType <= static_cast<std::uint16_t>(RE::MARKER_TYPE::kYouAreHere)) return;

        RE::NiPointer<RE::TESObjectREFR> refr;
        if (!a_entry->handle || !RE::LookupReferenceByHandle(a_entry->handle, refr) || !refr) return;

        // GetCoMAPIconName returns "" when CoMAP is absent OR when it doesn't
        // own this marker. Both mean "cannot bucket by name" — counted apart so
        // the log distinguishes "CoMAP missing" from "genuinely unnamed".
        const auto iconName = GetCoMAPIconName(refr.get());
        if (iconName.empty()) {
            ++g_censusUnnamed;
            return;
        }
        ++g_iconCensus[iconName];
    }

    void DumpCensus() {
        if (g_iconCensus.empty() && g_censusUnnamed == 0) return;

        // Sort by count descending: the head of this list is what the keyword
        // rules must cover to matter.
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

    // Modded marker type -> resolved focus mode. CoMAP assigns one icon name
    // per type index for the whole session, so this mapping is fixed once
    // computed and the cache never needs invalidating.
    //
    // The cache is the point, not an optimization detail: this runs per-marker
    // per-FRAME inside AdvanceMovie, and GetCustomMarkerName returns a
    // std::string BY VALUE. Resolving every frame would allocate hundreds of
    // strings per frame. With the cache it is one lookup after the first sight
    // of each type (~47 entries on the reference load order).
    std::unordered_map<std::uint16_t, FocusMode> g_moddedModeCache;

    // a_refr may be null: then a modded type can only fall back to kModded.
    FocusMode ResolveModeForRef(RE::TESObjectREFR* a_refr, std::uint16_t a_type) {
        // Vanilla range: the static table already answers it, no ref needed.
        if (a_type <= static_cast<std::uint16_t>(RE::MARKER_TYPE::kYouAreHere)) {
            return ModeForType(static_cast<RE::MARKER_TYPE>(a_type));
        }

        if (auto it = g_moddedModeCache.find(a_type); it != g_moddedModeCache.end()) {
            return it->second;
        }
        if (!a_refr) {
            // Don't cache: a later call WITH a ref must still get the chance to
            // resolve this type properly.
            return FocusMode::kModded;
        }

        // First sight of this type: ask CoMAP for the icon name. Without CoMAP
        // this lands in kModded, same as before the interop existed.
        const FocusMode mode = ModeForIconName(GetCoMAPIconName(a_refr));

        // Cache even the kModded result: a name that matched no rule will not
        // start matching later, and re-resolving it every frame is the exact
        // per-frame allocation this cache exists to avoid.
        g_moddedModeCache[a_type] = mode;
        return mode;
    }

    FocusMode ResolveMode(const MarkerEntry* a_entry, std::uint16_t a_type) {
        // Resolve the handle only when the type actually needs a name: the
        // vanilla-range path is a table lookup and must not pay for a lookup.
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

    // Per-bucket marker counts for the filter panel labels. Two sets because
    // this hook is called per-marker per-FRAME with no "array pass started"
    // signal: markers accumulate into _building, and when a handle repeats (the
    // array wrapped, i.e. one full pass is done) _building is published to
    // _published and reset. Readers always see a complete, consistent pass
    // rather than a half-counted one.
    std::array<std::atomic<std::uint32_t>, kModeCount> g_modeCountPublished{};
    std::array<std::uint32_t, kModeCount>              g_modeCountBuilding{};
    std::unordered_set<std::uint64_t>                  g_countSeen;

    void CountMarker(const MarkerEntry* a_entry, FocusMode a_mode) {
        // kCount = always-show types (kNone/kDoor/kYouAreHere); no bucket.
        if (a_mode == FocusMode::kCount) {
            return;
        }
        if (!g_countSeen.insert(a_entry->handle).second) {
            // Pass complete: publish and start the next one. Markers can be
            // added or removed between passes (worldspace change, tmm), so
            // rebuilding from zero each time is what keeps the counts honest.
            for (std::size_t i = 0; i < kModeCount; ++i) {
                g_modeCountPublished[i].store(g_modeCountBuilding[i]);
                g_modeCountBuilding[i] = 0;
            }
            g_countSeen.clear();
            g_countSeen.insert(a_entry->handle);
        }
        ++g_modeCountBuilding[static_cast<std::size_t>(a_mode)];
    }

    // IsMarkerVisible(ID 53084): the engine's per-marker visibility check inside
    // the per-frame AdvanceMovie loop. We let it decide first, then force false
    // for filtered categories. Only base markers (type==0) carry a readable
    // MapMarkerData at namePtr+0x00; for those we read MARKER_TYPE at +0x12 with
    // no handle lookup. Other dispatch types keep the engine's own result.
    bool HookedIsMarkerVisible(MarkerEntry* a_entry, void* a_arr) {
        using func_t = bool(*)(MarkerEntry*, void*);
        const bool engineVisible = REL::Relocation<func_t>(g_trampolineAddr)(a_entry, a_arr);
        if (!engineVisible || !a_entry) {
            return engineVisible;
        }

#if CMM_FILTER_LEAK_DEBUG
        // Before the filter check, so a marker the user has already hidden
        // still gets counted — the census must describe the whole map, not
        // whatever happens to be visible when the dump runs.
        CensusMarker(a_entry);
#endif

        if (a_entry->type == 0 && a_entry->namePtr) {
            const auto type = *reinterpret_cast<const std::uint16_t*>(a_entry->namePtr + 0x12);
            const auto mode = ResolveMode(a_entry, type);
            // Count BEFORE the visibility decision: the panel labels report how
            // many markers are in each bucket, not how many are showing.
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

        // IsMarkerVisible: SE 52192 / AE 53084 (findings-SE-Hook-IDs.md row 5).
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
            return false;  // always-show category
        }
        return !g_modeEnabled[static_cast<std::size_t>(mode)].load();
    }

    void OnForeignMessage(SKSE::MessagingInterface::Message* a_msg) {
        // Called from plugin.cpp's single foreign-message listener, NOT from a
        // listener of our own: SKSE routes a foreign broadcast to only the
        // first null-sender listener per plugin handle, so registering a second
        // one here returned true and silently never fired.
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
        // Returns "" for markers CoMAP doesn't own, which is the same answer we
        // want for "not a custom icon" — no separate not-found path needed.
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
        // Dump on toggle: by now the hook has walked the array at least once,
        // so the counts are complete. Cleared after so the next toggle
        // re-censuses instead of doubling every count.
        DumpCensus();
        g_iconCensus.clear();
        g_censusUnnamed = 0;
        g_censusSeen.clear();
        g_censusDone = false;  // re-arm: next toggle re-censuses from scratch
#endif
        // No refresh call: the per-frame IsMarkerVisible hook picks up the new
        // state next frame and pushes _visible to Flash. That is the no-blink path.
    }
}
