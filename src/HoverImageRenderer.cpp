#include "HoverImageRenderer.h"

#include "TextureManager.h"
#include "MarkerManager.h"
#include "MapMarkerRenderer.h"  // IsMapFading()
#include "MarkerFilterHook.h"   // IsMarkerFiltered(), focus-mode gate
#include "MapTheme.h"
#include "MapDrawHelpers.h"  // DrawTextShadowed
#include "LocalizationManager.h"
#include "Settings.h"
#include "HoldBounty.h"
#include "FontManager.h"
#include "MenuUtils.h"
#include "UIScale.h"
#include "QuestTracker.h"
#include "CompanionTracker.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

#define CMM_BOUNTY_DEBUG 0

#include <d3d11.h>
#include <cfloat>       // FLT_MAX (ImFont::CalcTextSizeA max_width)
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace {
    constexpr std::ptrdiff_t kHoveredMarkerOffset = 0x30558;
    // BSTArray size on all runtimes; +0x30478 is the capacity.
    inline std::ptrdiff_t MarkerArrayCountOffset() {
        return 0x30480;
    }
    // MapMenu+0x10 holds GFxMovieView*. Same pattern used by the
    // description-card suppression code.
    constexpr std::ptrdiff_t kMovieViewOffset     = 0x10;

    constexpr std::uintptr_t kLocalMapBaseOffset = 0x60;
    constexpr std::uintptr_t kLocalMapModeOffset = 0x303FC;

    bool IsWorldMapShowing() {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) return false;
        auto mapMenu = ui->GetMenu<RE::MapMenu>();
        if (!mapMenu) return false;
        const auto lmBase =
            reinterpret_cast<std::uintptr_t>(mapMenu.get()) + kLocalMapBaseOffset;
        return *reinterpret_cast<const std::uint8_t*>(lmBase + kLocalMapModeOffset) != 1;
    }

    struct CardScreenRect {
        bool             valid        = false;
        bool             isDiscovered = true;
        float            centerX      = 0.0f;  // screen px, card center
        float            centerY      = 0.0f;
        float            widthPx      = 0.0f;  // already scaled
        float            heightPx     = 0.0f;
        float            alpha01      = 0.0f;  // 0..1
        RE::FormID       formID       = 0;     // hovered marker ref FormID, 0 if unknown
        std::string      name;              // resolved location name, "<ID=...>" prefix stripped
        RE::FormID       worldSpaceID = 0;  // parent worldspace
        RE::FormID       cellID       = 0;  // parent cell (for interiors)
        RE::NiPoint3     position;          // 3D coordinates
        bool             hasPosition  = false;
        std::string      typeLabel;         // translated marker-type word, "" if none
        std::uint16_t    markerType   = 0;
        std::string      holdName;          // Hold name (engine string), "" if none
        bool             isCleared    = false;
        std::uint32_t    bounty       = 0;
        // Resolved with the identity. Empty text or bossState -1 = no row.
        bool             posInMapFrame = false;
        std::string      distanceText;
        std::string      servicesText;
        std::string      homesText;
        std::string      questsText;
        std::string      companionsText;
        std::string      spouseText;
        std::int8_t      bossState    = -1;
        float            textAnchorX  = 0.0f;  // screen px, vanilla title center
        float            titleWidthPx = 0.0f;  // screen px, vanilla title width (for left/right align)
        float            textAnchorY  = 0.0f;  // screen px, vanilla title top (Y anchor, cleared-safe)
        bool             haveTitle    = false; // title clip resolved; else fall back to box anchors
    };

    std::mutex     g_stateMutex;
    CardScreenRect g_state;

    // State for the right-click context menu
    bool g_openContextMenu = false;
    CardScreenRect g_contextMenuState; // Cached state for the open menu

    struct LocalIdentity {
        RE::FormID   doorFormID = 0;
        RE::FormID   doorWorldSpaceID = 0;
        RE::FormID   doorCellID = 0;
        // Where the door LEADS. Kept for later (an interior beacon once the
        // player is inside), not used for the outside beacon.
        RE::FormID   destWorldSpaceID = 0;
        RE::FormID   destCellID = 0;
        std::string  name;
        RE::NiPoint3 doorPosition{ 0.0f, 0.0f, 0.0f };
        RE::NiPoint3 destPosition{ 0.0f, 0.0f, 0.0f };
        bool         hasDestination = false;
    };

    bool          g_openLocalContextMenu = false;
    LocalIdentity g_localContextMenuState;

    std::unordered_map<RE::FormID, std::string> g_formIDToImage;

    struct MarkerArrayCache {
        std::uintptr_t arrPtr = 0;
        std::uint32_t  arrSize = 0;
        std::unordered_map<RE::RefHandle, std::uint32_t> baseIndexByHandle;
    };
    MarkerArrayCache g_markerArrayCache;

    struct IdentityCache {
        std::uintptr_t arrPtr = 0;
        std::uint32_t  arrSize = 0;
        std::int32_t   idx = -1;
        RE::RefHandle  handle = 0;
        std::uint32_t  type = 0;
        CardScreenRect identity;
        bool           valid = false;
    };
    IdentityCache g_identityCache;

    enum class Stage : std::uint8_t {
        Unknown, NoMenu, NoHover, NoMovie, NoHolder, BadHolderType,
        NoRoot, NoTranslateA, NoTranslateB, BadSize, Ok
    };
    Stage g_lastStage = Stage::Unknown;

    void LogStage(Stage a_s, const char* a_msg) {
        if (g_lastStage == a_s) return;
        g_lastStage = a_s;
        logger::debug("[Hover] stage -> {}", a_msg);
    }

    // GFxValue helpers: read a numeric member as double. Returns false if the
    // member is missing or not numeric.
    bool ReadMemberNum(const RE::GFxValue& a_obj, const char* a_name,
                       double& a_out) {
        RE::GFxValue v;
        if (!a_obj.GetMember(a_name, &v)) return false;
        if (!v.IsNumber()) return false;
        a_out = v.GetNumber();
        return true;
    }

    struct MarkerEntry {
        RE::TESFullName*  namePtr;    // +0x00  valid when type==0; polymorphic (often BGSLocation)
        RE::RefHandle     handle;     // +0x08  valid for all types
        std::uint32_t     pad0C;      // +0x0C
        RE::BSFixedString nameStr;    // +0x10  fallback string when namePtr==0 and type==0
        std::uint32_t     type;       // +0x18  0 = base MapMarker, 1 = stacked alias
        std::uint32_t     pad1C;      // +0x1C
        std::int32_t      nextIndex;  // +0x20  next stacked marker in array, or -1
        std::uint32_t     pad24;      // +0x24
        void*             questObj;   // +0x28  used when type==1 and nextIndex != -1
    };
    static_assert(sizeof(MarkerEntry) == 0x30);  // engine stride is 0x38; trailing 0x08 unused here

    constexpr std::uintptr_t kLocalMarkerArrayOffset = 0x00;   // from LocalMapMenu
    constexpr std::uintptr_t kLocalMarkerCountOffset = 0x10;   // from LocalMapMenu
    constexpr std::uintptr_t kLocalHoveredIdxOffset  = 0x303F8;

    struct LocalMarkerEntry {
        void*             namePtr;    // +0x00  name object; null for doors
        RE::RefHandle     handle;     // +0x08  door/object reference handle
        std::uint32_t     pad0C;      // +0x0C
        RE::BSFixedString nameStr;    // +0x10  used when namePtr == nullptr
        std::int32_t      type;       // +0x18  0 = base, 1 = stacked alias, 2 = player
        std::int32_t      subType;    // +0x1C  1 = load door
        std::int32_t      nextIndex;  // +0x20  next stacked entry, or -1
        std::uint32_t     pad24;      // +0x24
        RE::TESQuest*     quest;      // +0x28  quest target, if any
        std::uint8_t      isVisible;  // +0x30
        std::uint8_t      pad31[7];   // +0x31
    };
    static_assert(sizeof(LocalMarkerEntry) == 0x38);

    bool ResolveLocalIdentity(RE::MapMenu* a_menu, std::int32_t a_idx, LocalIdentity& a_out) {
        a_out = {};
        if (!a_menu || a_idx < 0) return false;

        const auto lmBase = reinterpret_cast<std::uintptr_t>(a_menu) + kLocalMapBaseOffset;
        auto* arr = *reinterpret_cast<LocalMarkerEntry* const*>(lmBase + kLocalMarkerArrayOffset);
        const auto count = *reinterpret_cast<const std::uint32_t*>(lmBase + kLocalMarkerCountOffset);
        if (!arr || static_cast<std::uint32_t>(a_idx) >= count) return false;

        const LocalMarkerEntry* target = &arr[a_idx];
        if (target->type == 1) {
            for (std::uint32_t i = 0; i < count; ++i) {
                if (arr[i].type == 0 && arr[i].handle == target->handle) {
                    target = &arr[i];
                    break;
                }
            }
        }

        // Name object is polymorphic: the C-string sits at +0x08 inside it.
        const char* raw = nullptr;
        if (target->namePtr) {
            raw = *reinterpret_cast<const char* const*>(
                reinterpret_cast<std::uintptr_t>(target->namePtr) + 8);
        } else {
            raw = target->nameStr.c_str();
        }
        if (raw && *raw) {
            // Engine-internal "<ID=xxxxxxxx>" prefix, 13 chars, stripped by the
            // vanilla card populator too.
            constexpr std::string_view kIdPrefix{ "<ID=" };
            a_out.name = std::string_view{ raw }.starts_with(kIdPrefix) ? (raw + 13) : raw;
        }

        if (target->handle) {
            RE::NiPointer<RE::TESObjectREFR> doorRef;
            if (RE::LookupReferenceByHandle(target->handle, doorRef) && doorRef) {
                a_out.doorFormID  = doorRef->GetFormID();
                a_out.doorPosition = doorRef->GetPosition();
                if (auto* ws = doorRef->GetWorldspace()) a_out.doorWorldSpaceID = ws->GetFormID();
                if (auto* cell = doorRef->GetParentCell()) a_out.doorCellID = cell->GetFormID();

                if (auto* tele = doorRef->extraList.GetByType<RE::ExtraTeleport>();
                    tele && tele->teleportData) {
                    a_out.destPosition   = tele->teleportData->position;
                    a_out.hasDestination = true;

                    if (auto destRef = tele->teleportData->linkedDoor.get()) {
                        if (auto* ws = destRef->GetWorldspace()) a_out.destWorldSpaceID = ws->GetFormID();
                        if (auto* cell = destRef->GetParentCell()) a_out.destCellID = cell->GetFormID();
                    }
                }
                return true;
            }
        }
        return !a_out.name.empty();
    }

    const char* MarkerTypeKey(std::uint16_t a_t) {
        switch (a_t) {
            case 1:  return "City";
            case 2:  return "Town";
            case 3:  return "Settlement";
            case 4:  return "Cave";
            case 5:  return "Camp";
            case 6:  return "Fort";
            case 7:  return "NordicRuin";
            case 8:  return "DwemerRuin";
            case 9:  return "Shipwreck";
            case 10: return "Grove";
            case 11: return "Landmark";
            case 12: return "DragonLair";
            case 13: return "Farm";
            case 14: return "WoodMill";
            case 15: return "Mine";
            case 16: return "ImperialCamp";
            case 17: return "StormcloakCamp";
            case 18: return "Doomstone";
            case 19: return "WheatMill";
            case 20: return "Smelter";
            case 21: return "Stable";
            case 22: return "ImperialTower";
            case 23: return "Clearing";
            case 24: return "Pass";
            case 25: return "Altar";
            case 26: return "Rock";
            case 27: return "Lighthouse";
            case 28: return "OrcStronghold";
            case 29: return "GiantCamp";
            case 30: return "Shack";
            case 31: return "NordicTower";
            case 32: return "NordicDwelling";
            case 33: return "Docks";
            case 34: return "Shrine";
            // 35-52: per-city capital/castle icon variants -> "City".
            case 35: case 36: case 37: case 38: case 39: case 40:
            case 41: case 42: case 43: case 44: case 45: case 46:
            case 47: case 48: case 49: case 50: case 51: case 52:
                return "City";
            case 53: return "MiraakTemple";
            case 54: return "RavenRock";
            case 55: return "StandingStone";
            case 56: return "TelvanniTower";
            case 59: return "City";  // Castle Karstaag
            default: return nullptr; // None, travel doors, modded/unknown
        }
    }

    constexpr RE::FormID kLocTypeHoldLocalID = 0x00016771;

    RE::BGSLocation* ResolveHoldLocation(RE::TESObjectREFR* a_refr) {
        if (!a_refr) return nullptr;
        auto* loc = a_refr->GetEditorLocation();
        if (!loc) return nullptr;

        auto* holdKw = RE::TESForm::LookupByID<RE::BGSKeyword>(kLocTypeHoldLocalID);
        if (!holdKw) return nullptr;

        int guard = 0;
        for (RE::BGSLocation* cur = loc; cur && guard < 16; cur = cur->parentLoc, ++guard) {
            if (cur->HasKeyword(holdKw)) return cur;
        }
        return nullptr;
    }

    bool ResolveCleared(RE::TESObjectREFR* a_refr) {
        if (!a_refr) return false;
        auto* loc = a_refr->GetEditorLocation();
        if (!loc) return false;
        return loc->IsCleared();
    }

    // BGSLocation has no child list; built once, location records are static.
    const std::unordered_map<RE::FormID, std::vector<RE::BGSLocation*>>& LocationChildren() {
        static std::unordered_map<RE::FormID, std::vector<RE::BGSLocation*>> map;
        static bool built = false;
        if (!built) {
            if (auto* dh = RE::TESDataHandler::GetSingleton()) {
                for (auto* loc : dh->GetFormArray<RE::BGSLocation>()) {
                    if (loc && loc->parentLoc) {
                        map[loc->parentLoc->GetFormID()].push_back(loc);
                    }
                }
                built = true;
            }
        }
        return map;
    }

    // Service keywords sit on child locations, so the walk is recursive.
    void CollectChildren(RE::BGSLocation* a_loc, std::vector<RE::BGSLocation*>& a_out, int a_depth = 0) {
        if (!a_loc || a_depth >= 8) return;
        const auto& map = LocationChildren();
        auto it = map.find(a_loc->GetFormID());
        if (it == map.end()) return;
        for (auto* c : it->second) {
            a_out.push_back(c);
            CollectChildren(c, a_out, a_depth + 1);
        }
    }

    struct ServiceDef {
        RE::FormID  kw;
        const char* key;
    };
    // LocTypeStables is KeywordsFixed.esp, not vanilla: left out.
    constexpr RE::FormID kLocTypeStore = 0x0001CB86;
    constexpr ServiceDef kServices[] = {
        { 0x0001CB87,    "StatCard.Service.Inn" },
        { kLocTypeStore, "StatCard.Service.Store" },
        { 0x0001CD56,    "StatCard.Service.Temple" },
        { 0x0001CD5A,    "StatCard.Service.Guild" },
        { 0x0001CD55,    "StatCard.Service.Barracks" },
        { 0x0001CD59,    "StatCard.Service.Jail" },
        { 0x0001CD57,    "StatCard.Service.Castle" },
    };
    constexpr RE::FormID kLocTypePlayerHouse = 0x000FC1A3;
    // LocTypeCity, LocTypeTown, LocTypeSettlement.
    constexpr RE::FormID kLocTypeSettlementKeywords[] = { 0x00013168, 0x00013166, 0x00013167 };
    constexpr RE::FormID kLocTypeClearableID = 0x000F5E80;
    constexpr RE::FormID kLocRefTypeBoss     = 0x000130F7;   // "Boss" LCRT; DLC2 boss types not covered

    // -1 = no boss ref in memory, 0 = alive, 1 = every boss dead.
    std::int8_t ResolveBoss(RE::BGSLocation* a_loc) {
        if (!a_loc) return -1;
        int bosses = 0, dead = 0;
        for (const auto& sr : a_loc->specialRefs) {
            if (!sr.type || sr.type->GetFormID() != kLocRefTypeBoss) continue;
            auto* ref   = RE::TESForm::LookupByID<RE::TESObjectREFR>(sr.refData.refID);
            auto* actor = ref ? ref->As<RE::Actor>() : nullptr;
            if (!actor) continue;
            ++bosses;
            if (actor->IsDead()) ++dead;
        }
        if (bosses == 0) return -1;
        return dead == bosses ? 1 : 0;
    }

    // Parent-chain walk, bounded so bad mod data cannot loop.
    bool LocationWithin(RE::BGSLocation* a_loc, RE::BGSLocation* a_target) {
        int guard = 0;
        for (auto* cur = a_loc; cur && guard < 16; cur = cur->parentLoc, ++guard) {
            if (cur == a_target) return true;
        }
        return false;
    }

    // 1 game unit = 0.01428 m. Unit words are appended, never used as a format pattern.
    std::string FormatDistance(float a_units) {
        const float m = a_units * 0.01428f;
        const auto* set = Settings::GetSingleton();
        if (set && set->statCardDistanceUnits == 1) {
            const float ft = m * 3.28084f;
            const float mi = m / 1609.344f;
            if (mi < 0.1f) {
                return std::format("{} {}", static_cast<int>(std::round(ft / 10.0f) * 10.0f),
                                   LocalizationManager::Get("StatCard.UnitFeet"));
            }
            return std::format("{:.1f} {}", mi, LocalizationManager::Get("StatCard.UnitMiles"));
        }
        if (m < 1000.0f) {
            return std::format("{} {}", static_cast<int>(std::round(m / 10.0f) * 10.0f),
                               LocalizationManager::Get("StatCard.UnitMeters"));
        }
        return std::format("{:.1f} {}", m / 1000.0f, LocalizationManager::Get("StatCard.UnitKilometers"));
    }

    bool ConvertToMapWorldspace(RE::TESWorldSpace* a_from, RE::NiPoint3& a_pos) {
        auto* ui = RE::UI::GetSingleton();
        auto  menu = ui ? ui->GetMenu<RE::MapMenu>() : nullptr;
        if (!menu || !a_from) {
            return false;
        }
        auto* mapWorld = menu->GetRuntimeData2()->worldSpace;
        if (!mapWorld) {
            return false;
        }

        // Depth cap: parentWorld is data-driven, and a broken mod record could
        // in principle make it cyclic. 8 is far beyond any real hierarchy.
        constexpr int kMaxHops = 8;
        auto* ws = a_from;
        for (int hop = 0; ws && hop < kMaxHops; ++hop) {
            if (ws == mapWorld) {
                return true;   // arrived; a_pos now in the map's frame
            }
            auto* parent = ws->parentWorld;
            if (!parent) {
                return false;  // standalone world, nothing to convert into
            }
            a_pos.x += ws->worldMapOffsetData.mapOffsetX;
            a_pos.y += ws->worldMapOffsetData.mapOffsetY;
            ws = parent;
        }
        return false;
    }

    void ResolveIdentity(RE::MapMenu* a_menu, std::int32_t a_idx,
                         CardScreenRect& a_out) {
        a_out.formID = 0;
        a_out.name.clear();
        a_out.worldSpaceID = 0;
        a_out.cellID = 0;
        a_out.hasPosition = false;
        a_out.isDiscovered = true;
        a_out.typeLabel.clear();
        a_out.markerType = 0;
        a_out.holdName.clear();
        a_out.isCleared = false;
        a_out.bounty = 0;
        a_out.posInMapFrame = false;
        a_out.distanceText.clear();
        a_out.servicesText.clear();
        a_out.homesText.clear();
        a_out.questsText.clear();
        a_out.companionsText.clear();
        a_out.spouseText.clear();
        a_out.bossState = -1;
        if (!a_menu || a_idx < 0) return;

        auto base   = reinterpret_cast<std::uintptr_t>(a_menu);
        auto arrPtr = *reinterpret_cast<std::uintptr_t*>(base + 0x30470);
        if (!arrPtr) return;
        const auto arrSize = *reinterpret_cast<std::uint32_t*>(base + MarkerArrayCountOffset());
        if (static_cast<std::uint32_t>(a_idx) >= arrSize) return;

        constexpr std::size_t kStride = 0x38;
        auto getEntry = [arrPtr](std::uint32_t i) -> const MarkerEntry& {
            return *reinterpret_cast<const MarkerEntry*>(
                arrPtr + static_cast<std::uintptr_t>(i) * kStride);
        };

        const MarkerEntry* target = &getEntry(static_cast<std::uint32_t>(a_idx));
        const RE::RefHandle hoveredHandle = target->handle;
        const std::uint32_t hoveredType = target->type;

        if (g_identityCache.valid &&
            g_identityCache.arrPtr == arrPtr &&
            g_identityCache.arrSize == arrSize &&
            g_identityCache.idx == a_idx &&
            g_identityCache.handle == hoveredHandle &&
            g_identityCache.type == hoveredType) {
            a_out.formID = g_identityCache.identity.formID;
            a_out.name = g_identityCache.identity.name;
            a_out.worldSpaceID = g_identityCache.identity.worldSpaceID;
            a_out.cellID = g_identityCache.identity.cellID;
            a_out.position = g_identityCache.identity.position;
            a_out.hasPosition = g_identityCache.identity.hasPosition;
            a_out.isDiscovered = g_identityCache.identity.isDiscovered;
            a_out.typeLabel = g_identityCache.identity.typeLabel;
            a_out.markerType = g_identityCache.identity.markerType;
            a_out.holdName = g_identityCache.identity.holdName;
            a_out.isCleared = g_identityCache.identity.isCleared;
            a_out.bounty = g_identityCache.identity.bounty;
            a_out.posInMapFrame = g_identityCache.identity.posInMapFrame;
            a_out.distanceText = g_identityCache.identity.distanceText;
            a_out.servicesText = g_identityCache.identity.servicesText;
            a_out.homesText = g_identityCache.identity.homesText;
            a_out.questsText = g_identityCache.identity.questsText;
            a_out.companionsText = g_identityCache.identity.companionsText;
            a_out.spouseText = g_identityCache.identity.spouseText;
            a_out.bossState = g_identityCache.identity.bossState;
            return;
        }

        if (g_markerArrayCache.arrPtr != arrPtr ||
            g_markerArrayCache.arrSize != arrSize) {
            g_markerArrayCache.arrPtr = arrPtr;
            g_markerArrayCache.arrSize = arrSize;
            g_markerArrayCache.baseIndexByHandle.clear();
            g_markerArrayCache.baseIndexByHandle.reserve(arrSize);
            for (std::uint32_t i = 0; i < arrSize; ++i) {
                const auto& entry = getEntry(i);
                if (entry.type == 0 && entry.handle != 0) {
                    g_markerArrayCache.baseIndexByHandle.try_emplace(entry.handle, i);
                }
            }
            g_identityCache.valid = false;
        }

        if (target->type == 1) {
            if (auto it = g_markerArrayCache.baseIndexByHandle.find(target->handle);
                it != g_markerArrayCache.baseIndexByHandle.end()) {
                target = &getEntry(it->second);
            }
        }

        const char* rawName = nullptr;
        if (target->namePtr) {
            rawName = target->namePtr->fullName.c_str();
        } else {
            rawName = target->nameStr.c_str();
        }
        if (rawName && *rawName) {
            constexpr std::string_view kIdPrefix{ "<ID=" };
            if (std::string_view{ rawName }.starts_with(kIdPrefix)) {
                a_out.name.assign(rawName + 13);
            } else {
                a_out.name.assign(rawName);
            }
        }

        // Fast path: namePtr is often a TESForm subclass (e.g. BGSLocation).
        if (target->namePtr) {
            if (auto* form = skyrim_cast<RE::TESForm*>(target->namePtr)) {
                a_out.formID = form->GetFormID();
            }
        }
        // Resolve through the handle to get FormID (if missing) and Position
        if (target->handle != 0) {
            RE::NiPointer<RE::TESObjectREFR> refr;
            if (RE::LookupReferenceByHandle(target->handle, refr) && refr) {
                if (a_out.formID == 0) {
                    a_out.formID = refr->GetFormID();
                }
                a_out.position = refr->GetPosition();
                a_out.hasPosition = true;
                if (auto* ws = refr->GetWorldspace()) {
                    a_out.worldSpaceID = ws->GetFormID();
                    a_out.posInMapFrame = ConvertToMapWorldspace(ws, a_out.position);
                }
                if (auto* cell = refr->GetParentCell()) {
                    a_out.cellID = cell->GetFormID();
                }

                if (auto* extra = refr->extraList.GetByType<RE::ExtraMapMarker>()) {
                    if (extra->mapData) {
                        a_out.isDiscovered = (extra->mapData->flags.underlying() & 2) != 0;

                        const std::uint16_t mt = extra->mapData->type.underlying();
                        a_out.markerType = mt;
                        if (const char* key = MarkerTypeKey(mt)) {
                            a_out.typeLabel = LocalizationManager::GetSingleton()
                                ->GetLabel(std::string{ "MarkerType." } + key);
                        }
                    }
                }

                // One parentLoc walk yields the Hold; name and bounty both hang
                // off it. Cleared reads the marker's OWN location, not the Hold.
                if (auto* hold = ResolveHoldLocation(refr.get())) {
                    const char* nm = hold->GetFullName();
                    a_out.holdName = (nm && *nm) ? std::string{ nm } : std::string{};
                    a_out.bounty   = HoldBounty::ResolveBounty(hold);
#if CMM_BOUNTY_DEBUG
                    logger::info("[BountyDbg] hold=0x{:08X} name='{}' bounty={} | SolstheimHold=0x{:08X} RRFact=0x{:08X}",
                        hold->GetFormID(), a_out.holdName, a_out.bounty,
                        HoldBounty::SolstheimHold() ? HoldBounty::SolstheimHold()->GetFormID() : 0,
                        HoldBounty::RavenRockCrimeFaction() ? HoldBounty::RavenRockCrimeFaction()->GetFormID() : 0);
#endif
                }
#if CMM_BOUNTY_DEBUG
                else {
                    logger::info("[BountyDbg] ResolveHoldLocation returned null (no editor loc or no LocTypeHold in chain)");
                }
#endif
                a_out.isCleared = ResolveCleared(refr.get());

                // Read from location keywords, not marker type: Atlas Map Markers retypes cities.
                bool cityOrTown = false;
                auto* loc = refr->GetEditorLocation();
                const char* editorLocName = loc ? loc->GetFullName() : nullptr;
                const bool markerIsLoc = editorLocName && a_out.name == editorLocName;
                if (loc && markerIsLoc) {
                    for (RE::FormID kwID : kLocTypeSettlementKeywords) {
                        auto* kw = RE::TESForm::LookupByID<RE::BGSKeyword>(kwID);
                        if (kw && loc->HasKeyword(kw)) { cityOrTown = true; break; }
                    }
                }

                if (loc) {
                    std::vector<RE::BGSLocation*> kids;
                    CollectChildren(loc, kids);

                    // Shop markers carry the city as editor location: match the child by name.
                    std::vector<RE::BGSLocation*> serviceLocs;
                    if (cityOrTown) {
                        serviceLocs = kids;
                    } else {
                        if (markerIsLoc) {
                            serviceLocs.push_back(loc);
                        } else {
                            for (auto* k : kids) {
                                const char* nm = k->GetFullName();
                                if (nm && a_out.name == nm) {
                                    serviceLocs.push_back(k);
                                    break;
                                }
                            }
                        }
                    }

                    std::string svc;
                    for (const auto& def : kServices) {
                        auto* kw = RE::TESForm::LookupByID<RE::BGSKeyword>(def.kw);
                        if (!kw) continue;
                        int n = 0;
                        for (auto* k : serviceLocs) {
                            if (k->HasKeyword(kw)) ++n;
                        }
                        if (n == 0) continue;
                        if (!svc.empty()) svc += ", ";
                        svc += LocalizationManager::Get(def.key);
                        if (def.kw == kLocTypeStore && n > 1) svc += std::format(" ({})", n);
                    }
                    a_out.servicesText = std::move(svc);

                    // Lists unbought houses too; ownership is not checked.
                    auto* homeKw = cityOrTown
                        ? RE::TESForm::LookupByID<RE::BGSKeyword>(kLocTypePlayerHouse) : nullptr;
                    if (homeKw) {
                        std::string homes;
                        for (auto* k : kids) {
                            if (!k->HasKeyword(homeKw)) continue;
                            const char* nm = k->GetFullName();
                            if (!nm || !*nm) continue;
                            if (!homes.empty()) homes += ", ";
                            homes += nm;
                        }
                        a_out.homesText = std::move(homes);
                    }

                    // Same place rule as Services. No place resolved = no companion row.
                    RE::BGSLocation* placeLoc = (cityOrTown || markerIsLoc) ? loc
                        : (serviceLocs.empty() ? nullptr : serviceLocs.front());
                    if (placeLoc) {
                        std::string comp, spouse;
                        for (const auto& c : CompanionTracker::Get()) {
                            if (!LocationWithin(c.loc, placeLoc)) continue;
                            std::string& out = c.spouse ? spouse : comp;
                            if (!out.empty()) out += ", ";
                            out += c.name;
                        }
                        a_out.companionsText = std::move(comp);
                        a_out.spouseText     = std::move(spouse);
                    }

                    // Dungeons only; undiscovered ones stay hidden so the boss is not spoiled.
                    auto* clearKw = RE::TESForm::LookupByID<RE::BGSKeyword>(kLocTypeClearableID);
                    if (a_out.isDiscovered && !cityOrTown && clearKw && loc->HasKeyword(clearKw)) {
                        a_out.bossState = ResolveBoss(loc);
                    }
                }

                // Cached with the identity: the player cannot move while the map is open.
                if (a_out.posInMapFrame) {
                    if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
                        if (auto* pws = pc->GetWorldspace()) {
                            RE::NiPoint3 pp = pc->GetPosition();
                            if (ConvertToMapWorldspace(pws, pp)) {
                                const float dx = pp.x - a_out.position.x;
                                const float dy = pp.y - a_out.position.y;
                                a_out.distanceText = FormatDistance(std::sqrt(dx * dx + dy * dy));
                            }
                        }
                    }
                }

                // Position match in the map frame. Radii are trial values (cities span several cells).
                if (a_out.posInMapFrame) {
                    const float r = cityOrTown ? 8192.0f : 2048.0f;
                    std::string first;
                    int         count = 0;
                    for (const auto& q : QuestTracker::GetSingleton()->Get()) {
                        if (!q.hasPosition) continue;
                        auto*        qws = RE::TESForm::LookupByID<RE::TESWorldSpace>(q.worldSpaceID);
                        RE::NiPoint3 qp  = q.position;
                        if (!qws || !ConvertToMapWorldspace(qws, qp)) continue;
                        const float dx = qp.x - a_out.position.x;
                        const float dy = qp.y - a_out.position.y;
                        if (dx * dx + dy * dy > r * r) continue;
                        if (count++ == 0) first = q.isMiscellaneous ? q.objectiveText : q.name;
                    }
                    if (count > 0) {
                        a_out.questsText = count > 1 ? std::format("{} +{}", first, count - 1) : first;
                    }
                }
            }
        }

        g_identityCache.arrPtr = arrPtr;
        g_identityCache.arrSize = arrSize;
        g_identityCache.idx = a_idx;
        g_identityCache.handle = hoveredHandle;
        g_identityCache.type = hoveredType;
        g_identityCache.identity = a_out;
        g_identityCache.valid = true;
    }
}

void HoverImageRenderer::SampleCardState(RE::MapMenu* a_menu) {
    auto clearState = []() {
        std::scoped_lock lock{ g_stateMutex };
        g_state.valid = false;
    };

    if (!a_menu) { LogStage(Stage::NoMenu, "NoMenu"); clearState(); return; }

    auto base = reinterpret_cast<std::uintptr_t>(a_menu);
    auto idx  = *reinterpret_cast<std::int32_t*>(base + kHoveredMarkerOffset);
    if (idx < 0) {
        // Hover ended: drop the identity cache so a recycled RefHandle inside
        // the same array generation can never serve a stale identity.
        g_identityCache.valid = false;
        LogStage(Stage::NoHover, "NoHover (idx=-1)");
        clearState();
        return;
    }

    auto movie = *reinterpret_cast<RE::GFxMovieView**>(base + kMovieViewOffset);
    if (!movie) { LogStage(Stage::NoMovie, "NoMovie"); clearState(); return; }

    auto& mapMovie = a_menu->GetRuntimeData2()->mapMovie;
    if (!mapMovie.IsObject() && !mapMovie.IsDisplayObject()) {
        LogStage(Stage::NoRoot, "NoRoot (MapMovie root not ready)");
        clearState();
        return;
    }
    RE::GFxValue descObj;
    if (!mapMovie.GetMember("MarkerDescriptionObj", &descObj)) {
        LogStage(Stage::NoHolder, "NoHolder (MarkerDescriptionObj missing on MapMovie)");
        clearState();
        return;
    }
    if (!descObj.IsObject() && !descObj.IsDisplayObject()) {
        LogStage(Stage::BadHolderType, "BadHolderType (descObj not displayobject)");
        clearState();
        return;
    }
    RE::GFxValue holder;
    if (!descObj.GetMember("_parent", &holder)) {
        LogStage(Stage::NoTranslateA, "NoHolder2 (_parent missing on descObj)");
        clearState();
        return;
    }
    if (!holder.IsObject() && !holder.IsDisplayObject()) {
        LogStage(Stage::BadSize, "BadHolder2 (_parent not displayobject)");
        clearState();
        return;
    }

    double xStage = 0.0, yStage = 0.0;
    double widthS = 0.0, heightS = 0.0;
    double xScale = 100.0, yScale = 100.0;
    double alpha  = 100.0;

    ReadMemberNum(holder, "_x",      xStage);
    ReadMemberNum(holder, "_y",      yStage);
    ReadMemberNum(holder, "_width",  widthS);
    ReadMemberNum(holder, "_height", heightS);
    ReadMemberNum(holder, "_xscale", xScale);
    ReadMemberNum(holder, "_yscale", yScale);
    ReadMemberNum(holder, "_alpha",  alpha);

    const bool hideVanillaCard = IsWorldMapShowing();
    if (hideVanillaCard) {
        RE::GFxValue vis;
        vis.SetBoolean(false);
        holder.SetMember("_visible", vis);
    }
    // ---- end takeover -----------------------------------------------------

    if (!hideVanillaCard && (widthS <= 0.0 || heightS <= 0.0 || alpha <= 0.0)) {
        LogStage(Stage::BadSize,
                 "BadSize (zero w/h/alpha)");
        clearState();
        return;
    }
    if (hideVanillaCard) {
        alpha = 100.0;
    }

    double titleCenterStage = xStage;  // pin fallback (X)
    double titleWidthStage  = 0.0;     // 0 => no title width known (align falls back to center)
    double titleTopStage    = yStage;  // box fallback (Y)
    bool   haveTitle        = false;
    {
        double dX = 0.0, dY = 0.0, tX = 0.0, tY = 0.0, tW = 0.0;
        ReadMemberNum(descObj, "_x", dX);
        ReadMemberNum(descObj, "_y", dY);
        RE::GFxValue titleVal;
        if (descObj.GetMember("Title", &titleVal) &&
            (titleVal.IsObject() || titleVal.IsDisplayObject())) {
            ReadMemberNum(titleVal, "_x", tX);
            ReadMemberNum(titleVal, "_y", tY);
            ReadMemberNum(titleVal, "_width", tW);
            const double sx = xScale / 100.0;
            const double sy = yScale / 100.0;
            titleCenterStage = xStage + (dX + tX + tW * 0.5) * sx;
            titleWidthStage  = tW * sx;
            titleTopStage    = yStage + (dY + tY) * sy;
            haveTitle        = true;
        }
    }

    RE::GViewport vp{};
    movie->GetViewport(&vp);
    if (vp.height <= 0) {
        LogStage(Stage::NoTranslateA, "NoTranslateA (viewport height <= 0)");
        clearState();
        return;
    }
    const float scale = static_cast<float>(vp.height) / 720.0f;
    const RE::GPointF screenPt{
        static_cast<float>(vp.left) + static_cast<float>(xStage) * scale,
        static_cast<float>(vp.top)  + static_cast<float>(yStage) * scale,
    };
    const float wPx = static_cast<float>(widthS)  * scale;
    const float hPx = static_cast<float>(heightS) * scale;

    CardScreenRect next;
    next.valid    = true;
    next.centerX  = screenPt.x;
    next.centerY  = screenPt.y;
    next.widthPx  = wPx;
    next.heightPx = hPx;
    next.alpha01  = static_cast<float>(std::clamp(alpha / 100.0, 0.0, 1.0));
    // Title center + width + top -> screen, same viewport mapping as the rest.
    next.textAnchorX  = static_cast<float>(vp.left) +
                        static_cast<float>(titleCenterStage) * scale;
    next.titleWidthPx = static_cast<float>(titleWidthStage) * scale;
    next.textAnchorY  = static_cast<float>(vp.top) +
                        static_cast<float>(titleTopStage) * scale;
    next.haveTitle    = haveTitle;
    ResolveIdentity(a_menu, idx, next);

    if (MarkerFilterHook::IsMarkerFiltered(
            RE::TESForm::LookupByID<RE::TESObjectREFR>(next.formID), next.markerType)) {
        g_identityCache.valid = false;
        clearState();
        return;
    }

    {
        std::scoped_lock lock{ g_stateMutex };
        g_state = std::move(next);
    }
    g_lastStage = Stage::Ok;
}

void HoverImageRenderer::Init() {
    g_formIDToImage.clear();

    std::filesystem::path jsonPath{ "Data/SKSE/Plugins/CustomMarkers_Images.json" };
    if (!std::filesystem::exists(jsonPath)) {
        jsonPath = "Data/SKSE/Plugins/CMM_MarkerImages.json";
    }
    if (!std::filesystem::exists(jsonPath)) {
        logger::warn("[Hover] Marker image mapping JSON not found; no images will load");
        return;
    }

    std::ifstream in{ jsonPath };
    if (!in) {
        logger::warn("[Hover] Failed to open {}", jsonPath.string());
        return;
    }

    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const std::exception& e) {
        logger::warn("[Hover] JSON parse failed: {}", e.what());
        return;
    }

    auto it = doc.find("markers");
    if (it == doc.end() || !it->is_object()) {
        logger::warn("[Hover] JSON missing top-level 'markers' object");
        return;
    }

    auto* dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) {
        logger::warn("[Hover] TESDataHandler not ready; image lookup disabled");
        return;
    }

    auto resolve = [&](const std::string& a_plugin,
                       RE::FormID a_localID) -> RE::FormID {
        auto* modInfo = dataHandler->LookupModByName(a_plugin);
        if (!modInfo) return 0;
        if (modInfo->IsLight()) {
            return 0xFE000000u
                 | (static_cast<RE::FormID>(modInfo->GetSmallFileCompileIndex()) << 12)
                 | (a_localID & 0xFFFu);
        }
        return (static_cast<RE::FormID>(modInfo->GetCompileIndex()) << 24)
             | (a_localID & 0xFFFFFFu);
    };

    std::size_t loaded = 0;
    for (auto& [key, val] : it->items()) {
        if (!val.is_object()) continue;
        auto imgIt = val.find("image");
        if (imgIt == val.end() || !imgIt->is_string()) continue;

        const auto pipe = key.find('|');
        if (pipe == std::string::npos) {
            logger::warn("[Hover] Skipping key without '|' separator: '{}'", key);
            continue;
        }
        std::string pluginName = key.substr(0, pipe);
        std::string idText     = key.substr(pipe + 1);
        if (pluginName.empty() || idText.empty()) {
            logger::warn("[Hover] Empty plugin or id half in key: '{}'", key);
            continue;
        }
        // std::stoul handles "0x"/"0X" prefix when base=0; require it
        // explicitly so a missing prefix doesn't silently parse as decimal.
        if (!(idText.starts_with("0x") || idText.starts_with("0X"))) {
            logger::warn("[Hover] Local ID missing '0x' prefix: '{}'", key);
            continue;
        }
        RE::FormID localID = 0;
        try {
            localID = static_cast<RE::FormID>(std::stoul(idText, nullptr, 16));
        } catch (...) {
            logger::warn("[Hover] Bad hex in key: '{}'", key);
            continue;
        }

        const RE::FormID runtime = resolve(pluginName, localID);
        if (runtime == 0) {
            logger::debug("[Hover] Plugin '{}' not loaded; skipping '{}'",
                         pluginName, key);
            continue;
        }
        g_formIDToImage[runtime] = imgIt->get<std::string>();
        ++loaded;
        logger::debug("[Hover] Mapped {} -> runtime 0x{:08X} -> '{}'",
                     key, runtime, imgIt->get<std::string>());
    }
    logger::info("[Hover] Loaded {} marker image mappings", loaded);
}

void HoverImageRenderer::Draw() {
    auto ui = RE::UI::GetSingleton();
    if (!ui || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
        g_openContextMenu = false;
        g_openLocalContextMenu = false;
        return;
    }
    if (MenuUtils::ShouldBlockMapOverlay()) {
        g_openContextMenu = false;
        g_openLocalContextMenu = false;
        return;
    }

    if (IsMapFading()) {
        g_openContextMenu = false;
        g_openLocalContextMenu = false;
        return;
    }
    if (MenuUtils::IsMapModalOpen()) {
        g_openContextMenu = false;
        g_openLocalContextMenu = false;
        return;
    }

    if (g_openContextMenu) {
        g_openContextMenu = false;
        ImGui::OpenPopup("VanillaContextMenu");
    }

    ImGui::PushStyleColor(ImGuiCol_PopupBg,       MapTheme::kPopupBg());
    ImGui::PushStyleColor(ImGuiCol_Border,        MapTheme::kPopupBorder());
    ImGui::PushStyleColor(ImGuiCol_Text,          MapTheme::kPopupText());
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, MapTheme::kPopupHoverBg());
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  MapTheme::kPopupHoverBg());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(UIScale::Px(10.0f), UIScale::Px(8.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(UIScale::Px(8.0f),  UIScale::Px(6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, UIScale::Px(1.5f));
    if (ImGui::BeginPopup("VanillaContextMenu")) {
        const float effectiveBodyScale = std::clamp(Settings::GetSingleton()->bodyScale, 0.5f, 3.0f) * UIScale::Font();
        ImGui::SetWindowFontScale(effectiveBodyScale);
        const auto& cs = g_contextMenuState;
        auto* mapRenderer = MapMarkerRenderer::GetSingleton();
        const int controllerAction = mapRenderer->TakeControllerContextAction(MapMarkerRenderer::ContextKind::Vanilla);
        if (controllerAction == -2) ImGui::CloseCurrentPopup();
        MapMarkerRenderer::ContextRowHit rows[3]{};
        std::size_t rowCount = 0;
        bool actionRan = false;

        ImGui::TextColored(MapTheme::kPopupHeaderText(), "%s", cs.name.c_str());
        ImGui::Separator();

        if (cs.hasPosition) {
            bool isDest = MarkerManager::GetSingleton()->IsVanillaDestination(cs.name);
            const bool destClicked = ImGui::MenuItem(LocalizationManager::Get(isDest ? "Menu.RemoveDestination"
                                                                             : "Menu.SetDestination").c_str());
            const ImVec2 destMin = ImGui::GetItemRectMin();
            const ImVec2 destMax = ImGui::GetItemRectMax();
            rows[rowCount++] = { destMin.x, destMin.y, destMax.x, destMax.y, true };
            if (destClicked || controllerAction == 0) {
                logger::debug("[HoverCard] Toggle Vanilla Destination clicked for {}", cs.name);
                if (isDest) {
                    MarkerManager::GetSingleton()->RemoveVanillaDestination();
                } else {
                    MarkerManager::GetSingleton()->SetVanillaDestination(cs.name, cs.formID, cs.worldSpaceID, cs.cellID, cs.position);
                }
                actionRan = true;
                if (controllerAction == 0) ImGui::CloseCurrentPopup();
            }

            bool isFav = MarkerManager::GetSingleton()->IsFavorite(cs.formID);
            if (!cs.isDiscovered) ImGui::BeginDisabled();
            const bool favClicked = ImGui::MenuItem(LocalizationManager::Get(isFav ? "Menu.RemoveFavorite"
                                                                           : "Menu.AddToFavorites").c_str());
            const ImVec2 favMin = ImGui::GetItemRectMin();
            const ImVec2 favMax = ImGui::GetItemRectMax();
            rows[rowCount++] = { favMin.x, favMin.y, favMax.x, favMax.y, cs.isDiscovered };
            if (cs.isDiscovered && (favClicked || controllerAction == 1)) {
                logger::debug("[HoverCard] Toggle Favorite clicked for {}", cs.name);
                MarkerManager::GetSingleton()->ToggleFavorite(cs.formID, cs.worldSpaceID, cs.cellID, cs.position, cs.name);
                if (!isFav) {
                    MapMarkerRenderer::GetSingleton()->ShowListTab(
                        MapMarkerRenderer::ListTab::Favorites);
                }
                actionRan = true;
                if (controllerAction == 1) ImGui::CloseCurrentPopup();
            }
            if (!cs.isDiscovered) ImGui::EndDisabled();

            if (cs.formID != 0) {
                bool hasNote = !MarkerManager::GetSingleton()->GetVanillaNote(cs.formID).empty();
                // GetLabel returns the key itself when the translation file
                // doesn't have it yet, so fall back to the English string.
                auto* loc = LocalizationManager::GetSingleton();
                std::string noteLabel = loc->GetLabel(hasNote ? "VanillaMenu.EditNote" : "VanillaMenu.AddNote");
                if (noteLabel == "VanillaMenu.EditNote") noteLabel = "Edit Note";
                else if (noteLabel == "VanillaMenu.AddNote") noteLabel = "Add Note";
                const bool noteClicked = ImGui::MenuItem(noteLabel.c_str());
                const ImVec2 noteMin = ImGui::GetItemRectMin();
                const ImVec2 noteMax = ImGui::GetItemRectMax();
                rows[rowCount++] = { noteMin.x, noteMin.y, noteMax.x, noteMax.y, true };
                if (noteClicked || controllerAction == 2) {
                    logger::debug("[HoverCard] Vanilla note clicked for {} ({:08X})", cs.name, cs.formID);
                    MapMarkerRenderer::GetSingleton()->BeginEditVanillaNote(cs.formID, cs.name);
                    actionRan = true;
                    if (controllerAction == 2) ImGui::CloseCurrentPopup();
                }
            }
        } else {
            ImGui::TextDisabled("%s", LocalizationManager::Get("Menu.LocationDataUnavailable").c_str());
        }

        if (controllerAction != -2 && !actionRan) {
            const ImVec2 pos = ImGui::GetWindowPos();
            const ImVec2 size = ImGui::GetWindowSize();
            mapRenderer->PublishControllerContextPopup(MapMarkerRenderer::ContextKind::Vanilla,
                                                       pos.x, pos.y, pos.x + size.x, pos.y + size.y,
                                                       rows, rowCount);
        }
        ImGui::EndPopup();
    }

    if (g_openLocalContextMenu) {
        g_openLocalContextMenu = false;
        ImGui::OpenPopup("LocalDoorContextMenu");
    }

    if (ImGui::BeginPopup("LocalDoorContextMenu")) {
        const float effectiveBodyScale = std::clamp(Settings::GetSingleton()->bodyScale, 0.5f, 3.0f) * UIScale::Font();
        ImGui::SetWindowFontScale(effectiveBodyScale);
        auto* mapRenderer = MapMarkerRenderer::GetSingleton();
        const int controllerAction = mapRenderer->TakeControllerContextAction(MapMarkerRenderer::ContextKind::LocalDoor);
        if (controllerAction == -2) ImGui::CloseCurrentPopup();

        const auto& d = g_localContextMenuState;
        ImGui::TextColored(MapTheme::kPopupHeaderText(), "%s", d.name.c_str());
        ImGui::Separator();

        // Keyed by NAME, same as the world-map vanilla destination: that is what
        // MarkerManager's vanilla-destination API stores and compares.
        const bool isDest = MarkerManager::GetSingleton()->IsVanillaDestination(d.name);
        const bool destClicked = ImGui::MenuItem(LocalizationManager::Get(isDest ? "Menu.RemoveDestination"
                                                                         : "Menu.SetDestination").c_str());
        const ImVec2 destMin = ImGui::GetItemRectMin();
        const ImVec2 destMax = ImGui::GetItemRectMax();
        const MapMarkerRenderer::ContextRowHit row{ destMin.x, destMin.y, destMax.x, destMax.y, true };
        if (destClicked || controllerAction == 0) {
            if (isDest) {
                MarkerManager::GetSingleton()->RemoveVanillaDestination();
                logger::info("[LocalDoor] cleared destination ({})", d.name);
            } else {
                MarkerManager::GetSingleton()->SetVanillaDestination(
                    d.name, d.doorFormID, d.doorWorldSpaceID, d.doorCellID, d.doorPosition, /*fromLocalDoor=*/true);
                logger::info("[LocalDoor] set destination {} ({:08X}) ws {:08X} cell {:08X} pos ({:.0f}, {:.0f}, {:.0f})",
                             d.name, d.doorFormID, d.doorWorldSpaceID, d.doorCellID,
                             d.doorPosition.x, d.doorPosition.y, d.doorPosition.z);
            }
            if (controllerAction == 0) ImGui::CloseCurrentPopup();
        } else if (controllerAction != -2) {
            const ImVec2 pos = ImGui::GetWindowPos();
            const ImVec2 size = ImGui::GetWindowSize();
            mapRenderer->PublishControllerContextPopup(MapMarkerRenderer::ContextKind::LocalDoor,
                                                       pos.x, pos.y, pos.x + size.x, pos.y + size.y,
                                                       &row, 1);
        }

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);

    CardScreenRect s;
    {
        std::scoped_lock lock{ g_stateMutex };
        s = g_state;
    }
    if (!s.valid) {
        return;
    }

    auto* dl = ImGui::GetForegroundDrawList();

    if (!s.hasPosition) {
        return;
    }

    auto* ui2 = RE::UI::GetSingleton();
    auto  mapMenuPtr = ui2 ? ui2->GetMenu<RE::MapMenu>() : nullptr;
    if (!mapMenuPtr) {
        return;
    }

    RE::NiCamera* niCam = nullptr;
    {
        auto root = mapMenuPtr->GetRuntimeData2()->camera.cameraRoot;
        if (root) {
            auto& kids = root->GetChildren();
            if (!kids.empty() && kids[0]) {
                niCam = netimmerse_cast<RE::NiCamera*>(kids[0].get());
            }
        }
    }
    if (!niCam) {
        return;
    }

    const ImVec2 disp = ImGui::GetIO().DisplaySize;

    float pinX = 0.0f, pinY = 0.0f;
    {
        auto& camData  = niCam->GetRuntimeData();
        auto& camData2 = niCam->GetRuntimeData2();

        // Match pins and badges, including the authored flat-map tilt guard.
        const bool useFlatMapPlane = MapDraw::UsesFlatMapProjection(
            camData.worldToCam, mapMenuPtr->GetRuntimeData2()->worldSpace);
        RE::NiPoint3 mapPos = MapDraw::MapProjectionPoint(s.position, useFlatMapPlane);

        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
        if (!RE::NiCamera::WorldPtToScreenPt3(
                camData.worldToCam, camData2.port, mapPos, sx, sy, sz, 1e-5f) ||
            !MapDraw::ProjectionDepthValid(sz)) {
            return;   // behind the camera / unprojectable
        }

        // sx/sy are normalized 0..1. Y is flipped.
        pinX = sx * disp.x;
        pinY = (1.0f - sy) * disp.y;
    }

    const auto* set = Settings::GetSingleton();
    const float fontScale     = UIScale::Font();
    const float geometryScale = UIScale::Master();
    const float titleScale    = std::clamp(set->titleScale,    0.5f, 3.0f) * fontScale;
    const float bodyScale     = std::clamp(set->bodyScale,     0.5f, 3.0f) * fontScale;
    const float noteScale     = std::clamp(set->noteTextScale, 0.5f, 3.0f) * fontScale;

    const float baseFS = ImGui::GetFontSize();
    ImFont* headingFont = FontManager::GetSingleton()->GetHeadingFont();
    const float titleBaseFS = headingFont ? headingFont->FontSize : baseFS;
    const float nameFS = titleBaseFS * titleScale;
    const float rowFS  = baseFS * bodyScale;
    const float noteFS = baseFS * noteScale;

    struct StatRow {
        std::string label;
        std::string value;
        // Set by the measure pass, read back by draw, so both agree on layout.
        float valueX = 0.0f;
        float wrap   = 0.0f;
        float h      = 0.0f;
    };
    std::vector<StatRow> rows;

    if (set->statCardShowDetails && set->statCardShowType && !s.typeLabel.empty()) {
        rows.push_back({ LocalizationManager::Get("StatCard.Type"), s.typeLabel });
    }
    if (set->statCardShowDetails && set->statCardShowHold && !s.holdName.empty()) {
        rows.push_back({ LocalizationManager::Get("StatCard.Hold"), s.holdName });
    }
    if (set->statCardShowDetails && set->statCardShowDistance && !s.distanceText.empty()) {
        rows.push_back({ LocalizationManager::Get("StatCard.Distance"), s.distanceText });
    }

    if (set->statCardShowDetails && set->statCardShowBounty && s.bounty > 0) {
        // Raw number, no translation key: a bare "{}" key carries no translatable
        // text, so it would be a key nobody can ever translate.
        rows.push_back({ LocalizationManager::Get("StatCard.Bounty"),
                         std::to_string(s.bounty) });
    }

    if (set->statCardShowDetails && set->statCardShowCleared && s.isCleared) {
        rows.push_back({ LocalizationManager::Get("StatCard.Cleared"),
                         LocalizationManager::Get("StatCard.ClearedYes") });
    }
    if (set->statCardShowDetails && set->statCardShowBoss && s.bossState >= 0) {
        rows.push_back({ LocalizationManager::Get("StatCard.Boss"),
                         LocalizationManager::Get(s.bossState ? "StatCard.BossDead" : "StatCard.BossAlive") });
    }
    if (set->statCardShowDetails && set->statCardShowQuests && !s.questsText.empty()) {
        rows.push_back({ LocalizationManager::Get("StatCard.Quests"), s.questsText });
    }
    if (set->statCardShowDetails && set->statCardShowServices && !s.servicesText.empty()) {
        rows.push_back({ LocalizationManager::Get("StatCard.Services"), s.servicesText });
    }
    if (set->statCardShowDetails && set->statCardShowPlayerHome && !s.homesText.empty()) {
        rows.push_back({ LocalizationManager::Get("StatCard.PlayerHome"), s.homesText });
    }
    if (set->statCardShowDetails && set->statCardShowSpouse && !s.spouseText.empty()) {
        rows.push_back({ LocalizationManager::Get("StatCard.Spouse"), s.spouseText });
    }
    if (set->statCardShowDetails && set->statCardShowCompanions && !s.companionsText.empty()) {
        rows.push_back({ LocalizationManager::Get("StatCard.Companions"), s.companionsText });
    }

    // Note: our own annotation, keyed by ref FormID. Runtime 0xFF... FormIDs are
    // our CNO-compat XMarker pool, never real locations.
    std::string note;
    if (s.formID != 0 && (s.formID & 0xFF000000u) != 0xFF000000u) {
        note = MarkerManager::GetSingleton()->GetVanillaNote(s.formID);
    }

    // Resolved before measuring: the image minimum width feeds the layout.
    ID3D11ShaderResourceView* imgSrv = nullptr;
    float imgAspect = 1.0f;
    if (set->markerImageEnabled && s.formID != 0 && (s.formID & 0xFF000000u) != 0xFF000000u) {
        if (auto mapIt = g_formIDToImage.find(s.formID); mapIt != g_formIDToImage.end()) {
            const std::string path = "Data\\Interface\\CustomMarkers\\Images\\" + mapIt->second + ".png";
            if (auto* srv = TextureManager::GetTexture(path)) {
                const auto size = TextureManager::GetTextureSize(srv);
                if (size.width != 0 && size.height != 0) {
                    imgSrv    = srv;
                    imgAspect = static_cast<float>(size.width) / static_cast<float>(size.height);
                }
            }
        }
    }

    // ---- Measure -----------------------------------------------------------
    // Equal on both axes so the card's inset reads the same all the way round.
    constexpr float kValueColEm = 4.5f;
    const float kPadX      = 12.0f * geometryScale;
    const float kPadY      = 12.0f * geometryScale;
    const float kRowGap    = 3.0f * geometryScale;
    const float kSepGap    = 6.0f * geometryScale;
    const float kNoteWrapW = 260.0f * geometryScale;   // hardcoded for now; was [NoteText] Width
    const float kPinGap    = 14.0f * geometryScale;    // gap between pin and panel bottom
    const float kLabelGap  = 8.0f * geometryScale;     // min gap when a label overruns the column
    // Values wrap at this width so Services and Quests cannot widen the card.
    const float kMaxTextW  = 270.0f * geometryScale;
    // Pixels at Interface Scale 1.0; applies only when the card draws an image.
    const float imgMinPanelW = imgSrv
        ? static_cast<float>(std::clamp(set->markerImageMinWidth, 0, 800)) * geometryScale
        : 0.0f;

    auto* measureFont = ImGui::GetFont();
    auto measure = [measureFont](const char* txt, float fs, float wrap = 0.0f) -> ImVec2 {
        return measureFont->CalcTextSizeA(fs, FLT_MAX, wrap, txt);
    };
    ImFont* nameFont = headingFont ? headingFont : measureFont;

    const ImVec2 nameSz = nameFont->CalcTextSizeA(nameFS, FLT_MAX, 0.0f, s.name.c_str());

    const float valueColX = kValueColEm * rowFS;

    // Wrapped lines start at valueX; the cap widens to fit the name and image minimum.
    const float maxTextW = (std::max)({ kMaxTextW, nameSz.x, imgMinPanelW - kPadX * 2.0f });
    const float lineH    = measure("Ag", rowFS).y;
    float rowsW = 0.0f;
    for (auto& r : rows) {
        const float labelW = measure(r.label.c_str(), rowFS).x;
        r.valueX = (std::max)(valueColX, labelW + kLabelGap);
        r.wrap   = (std::max)(maxTextW - r.valueX, 4.0f * rowFS);
        const ImVec2 vs = measure(r.value.c_str(), rowFS, r.wrap);
        r.h = (std::max)(vs.y, lineH);
        rowsW = (std::max)(rowsW, r.valueX + vs.x);
    }

    // Image minimum folds in so its width is a real note column.
    float headerW = (std::max)({ nameSz.x, rowsW, imgMinPanelW - kPadX * 2.0f });
    const float noteWrapW = (std::max)(kNoteWrapW, headerW);

    const ImVec2 noteSz = note.empty() ? ImVec2{0,0}
                                       : measure(note.c_str(), noteFS, noteWrapW);

    const std::string notesLabel = note.empty() ? std::string{}
                                                : LocalizationManager::Get("StatCard.Notes");
    const ImVec2 notesLabelSz = notesLabel.empty() ? ImVec2{0,0}
                                                   : measure(notesLabel.c_str(), rowFS);

    float contentW = headerW;
    contentW = (std::max)(contentW, noteSz.x);
    contentW = (std::max)(contentW, notesLabelSz.x);

    float contentH = nameSz.y;
    // Per-row heights: a wrapped value makes its row taller.
    for (const auto& r : rows) {
        contentH += kRowGap + r.h;
    }
    if (!note.empty())     contentH += kSepGap + 1.0f * geometryScale + kSepGap +
                                       notesLabelSz.y + kRowGap + noteSz.y;

    const float panelW = contentW + kPadX * 2.0f;
    const float panelH = contentH + kPadY * 2.0f;

    const auto* place = Settings::GetSingleton();
    const float offX = static_cast<float>(place->statCardOffsetX) * geometryScale;
    const float offY = static_cast<float>(place->statCardOffsetY) * geometryScale;

    float px, py;
    switch (place->statCardAnchor) {
        case 1:  // Bottom
            px = pinX - panelW * 0.5f;
            py = pinY + kPinGap;
            break;
        case 2:  // Left
            px = pinX - kPinGap - panelW;
            py = pinY - panelH * 0.5f;
            break;
        case 3:  // Right
            px = pinX + kPinGap;
            py = pinY - panelH * 0.5f;
            break;
        default: // Top
            px = pinX - panelW * 0.5f;
            py = pinY - kPinGap - panelH;
            break;
    }
    px += offX;
    py += offY;

    const float kEdge = 8.0f * geometryScale;
    px = std::clamp(px, kEdge, (std::max)(kEdge, disp.x - panelW - kEdge));
    py = std::clamp(py, kEdge, (std::max)(kEdge, disp.y - panelH - kEdge));

    constexpr float a = 1.0f;

    const ImVec2 pMin{ px, py };
    const ImVec2 pMax{ px + panelW, py + panelH };

    // MapTheme tokens are already ImU32 (IM_COL32), not ImVec4. Scale the alpha
    // byte in place; no ColorConvertFloat4ToU32 needed.
    auto withA = [a](ImU32 c) -> ImU32 {
        const float ca = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * a;
        return (c & ~IM_COL32_A_MASK) |
               (static_cast<ImU32>(ca) << IM_COL32_A_SHIFT);
    };

    const ImU32 colName = withA(IM_COL32(set->titleColorR, set->titleColorG,
                                         set->titleColorB, 255));
    const ImU32 colBody = withA(IM_COL32(set->bodyColorR, set->bodyColorG,
                                         set->bodyColorB, 255));

    MapDraw::DrawPanelCard(dl, pMin, pMax, set->BgCol(), set->BorderCol(), a, geometryScale);

    const ImU32 colSep    = withA(MapTheme::kSeparator);

    auto* font = ImGui::GetFont();
    float cy = py + kPadY;

    const float tx = px + kPadX;

    // Name (header).
    {
        // Was a single hard 1px offset copy; now the shared spread shadow so the
        // card header matches the list panel and Legend headings.
        MapDraw::DrawTextShadowed(dl, nameFont, nameFS, { tx, cy }, colName, s.name.c_str());
        cy += nameSz.y;
    }

    const ImU32 colLabel = MapDraw::FadeAlpha(colBody, MapDraw::kLabelAlpha);

    for (const auto& r : rows) {
        cy += kRowGap;
        dl->AddText(font, rowFS, { tx, cy }, colLabel, r.label.c_str());

        // valueX and wrap come from the measure pass so draw cannot drift from it.
        dl->AddText(font, rowFS, { tx + r.valueX, cy }, colBody,
                    r.value.c_str(), nullptr, r.wrap);

        cy += r.h;
    }

    if (!note.empty()) {
        cy += kSepGap;
        dl->AddLine({ px + kPadX, cy }, { px + panelW - kPadX, cy }, colSep, 1.0f * geometryScale);
        cy += 1.0f * geometryScale + kSepGap;

        dl->AddText(font, rowFS, { tx, cy },
                    MapDraw::FadeAlpha(colBody, MapDraw::kLabelAlpha),
                    notesLabel.c_str());
        cy += notesLabelSz.y + kRowGap;

        dl->AddText(font, noteFS, { tx, cy }, colBody,
                    note.c_str(), nullptr, noteWrapW);
    }

    if (!imgSrv) {
        return;
    }

    // The minimum width already widened the card, so the image fills it.
    const float imgW = panelW;
    const float imgH = imgW / imgAspect;
    const float imgX = px;

    // Multiplied into the card's own fade alpha, never replacing it, so the
    // image still fades in/out with the panel instead of popping.
    const float imgA = a * std::clamp(set->markerImageAlpha, 0.0f, 1.0f);
    const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(255 * imgA));
    dl->AddImage(reinterpret_cast<ImTextureID>(imgSrv),
                 { imgX, py - imgH }, { imgX + imgW, py },
                 { 0.0f, 0.0f }, { 1.0f, 1.0f }, tint);
}

void HoverImageRenderer::OpenContextMenu() {
    std::scoped_lock lock{ g_stateMutex };
    if (g_state.valid) {
        g_contextMenuState = g_state;
        g_openContextMenu = true;
    }
}


namespace {
    std::int32_t GetLocalHoveredIndex(RE::MapMenu** a_outMenu = nullptr) {
        if (a_outMenu) *a_outMenu = nullptr;
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) return -1;
        auto mapMenu = ui->GetMenu<RE::MapMenu>();
        if (!mapMenu) return -1;

        const auto lmBase =
            reinterpret_cast<std::uintptr_t>(mapMenu.get()) + kLocalMapBaseOffset;
        if (*reinterpret_cast<const std::uint8_t*>(lmBase + kLocalMapModeOffset) != 1) return -1;

        if (a_outMenu) *a_outMenu = mapMenu.get();
        return *reinterpret_cast<const std::int32_t*>(lmBase + kLocalHoveredIdxOffset);
    }
}

bool HoverImageRenderer::IsLocalMarkerHovered() {
    return GetLocalHoveredIndex() >= 0;
}

void HoverImageRenderer::OpenLocalContextMenu() {
    RE::MapMenu* menu = nullptr;
    const auto idx = GetLocalHoveredIndex(&menu);
    if (idx < 0 || !menu) return;

    LocalIdentity id{};
    if (!ResolveLocalIdentity(menu, idx, id)) return;
    // A door with no name is not something the player can meaningfully mark.
    if (id.name.empty()) return;

    g_localContextMenuState = id;
    g_openLocalContextMenu  = true;
}

RE::FormID HoverImageRenderer::GetHoveredFormID() {
    std::scoped_lock lock{ g_stateMutex };
    return g_state.valid ? g_state.formID : 0;
}

bool HoverImageRenderer::IsHoveredDiscovered() {
    std::scoped_lock lock{ g_stateMutex };
    return g_state.valid && g_state.isDiscovered;
}

bool HoverImageRenderer::IsHoveredUndiscovered() {
    std::scoped_lock lock{ g_stateMutex };
    return g_state.valid && !g_state.isDiscovered;
}
