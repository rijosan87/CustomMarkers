#include "HoverImageRenderer.h"

#include "TextureManager.h"
#include "MarkerManager.h"
#include "MapMarkerRenderer.h"  // IsMapFading()
#include "MarkerFilterHook.h"   // IsMarkerFiltered() — focus-mode gate
#include "MapTheme.h"
#include "MapDrawHelpers.h"  // DrawTextShadowed
#include "LocalizationManager.h"
#include "Settings.h"
#include "HoldBounty.h"
#include "FontManager.h"
#include "MenuUtils.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <imgui.h>
#include <nlohmann/json.hpp>

// Flip to 1 to log the hold-bounty resolve on every hover re-resolve. Off by
// default (spams once per identity change). Kept because custom/mod-added
// worldspaces need the same hold-LCTN -> crime-FACT verification the Solstheim
// port needed: if a modded hold's bounty reads 0, this line shows whether the
// hold walk landed, what FormID it hit, and whether the faction resolved.
#define CMM_BOUNTY_DEBUG 0

#include <d3d11.h>
#include <cfloat>       // FLT_MAX (ImFont::CalcTextSizeA max_width)
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

// ============================================================================
// HoverImageRenderer - vanilla-marker stat card and right-click menu
// ----------------------------------------------------------------------------
// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):
//   Working:
//     - Samples the hovered index at MapMenu+0x30558 and resolves identity by
//       walking the marker array at +0x30470. A type-1 stacked City/Quest entry
//       resolves through the type-0 base entry with the same handle.
//     - Child-worldspace positions walk parentWorld and add
//       worldMapOffsetData.mapOffsetX/Y into the displayed map's frame.
//       MapDraw::MapProjectionPoint owns the camera-mode Z rule: flat maps use
//       the tested high plane; tilted maps retain ground Z.
//       Authored high-plane maps retain flat mode during camera tilt; confirmed
//       in-game on FWMF 1.6.1170, 2026-09-09.
//     - The focus-mode gate clears the whole sampled identity when the hovered
//       ref is filtered. The Flash visibility hook does not clear +0x30558, so
//       this gate must cover the stat card, context menu, and identity queries.
//     - Draw() renders one projected stat card for both discovered and undiscovered
//       markers (vanilla shows the card title for undiscovered too): location name;
//       localized Type, Hold, Bounty, and Cleared rows; optional Notes block; optional PNG.
//       Rows share a left edge and fixed value column. Measure and draw must use
//       the same long-label clamp. Notes remain a full-width wrapped block.
//     - Hold resolves through the BGSLocation parent chain to LocTypeHold.
//       Bounty delegates to HoldBounty; Cleared comes from the marker's own
//       BGSLocation::IsCleared(), never from a marker flag.
//     - Init() maps "Plugin.esm|0xLOCALID" entries in CMM_MarkerImages.json to
//       runtime FormIDs, including ESL compile indices. Images load from
//       Data\Interface\CustomMapMarkers\Images and preserve source aspect ratio.
//     - Vanilla world-map card suppression requires both halves: MapHoverHook
//       skips the populator (SE 52187 / AE 53079), and this file hides
//       MarkerDescriptionHolder every frame because vanilla re-shows it.
//       Use GFxValue::SetMember("_visible"), not an Invoke through the mismatched
//       CommonLib NG Scaleform vtable. Local-map door cards remain enabled.
//     - IsMapFading() hides the stat card, image, and right-click menu while
//       FaderMenu is open.
//     - A blocking SKSE Menu Framework window hides the map hover layer and
//       discards deferred context-menu opens so they cannot appear afterward.
//     - Local-map doors use LocalMapMenu's own marker array: base +0x00, count
//       +0x10, stride 0x38, hovered index +0x303F8. Never resolve that index
//       against the world array at MapMenu+0x30470; the overlapping address
//       ranges can return a valid but unrelated world marker instead of failing.
//     - The local-door menu stores the exterior door's position/worldspace, doorFormID
//       source, and sets fromLocalDoor=true. This keeps the world-map ring suppressed over
//       bare terrain and lets the exterior local map draw it under the door.
//       Favorites remain unavailable because they have no world-map pan target.
//   Runtime layout:
//     - +0x30558 hovered index, +0x30540 MapMovie GFxValue, and +0x30470 marker
//       array pointer are shared by supported runtimes. Marker count is SE
//       +0x30480 / AE +0x30478 via MarkerArrayCountOffset().
//   Known issue:
//     - isDiscovered reads flags & 2, which is kCanTravelTo. No discovered flag
//       exists; the name remains intentionally unchanged.
//   Cleanup debt:
//     - SampleCardState still reads and stores the hidden Flash card's geometry
//       (center/size/alpha/title anchors), but no current consumer reads it.
//   Evidence: findings-World-Map-Hover-Identity.md,
//             findings-LocalMap-Hover-Identity.md,
//             findings/spec-findings-Hover-Card-Takeover.md,
//             findings-SE-MapMenu-Offsets.md.
// ============================================================================

namespace {
    constexpr std::ptrdiff_t kHoveredMarkerOffset = 0x30558;
    // MapMenu marker-array (BSTArray) count field. SE puts it 0x10 after the
    // data ptr (+0x30470), AE 0x08 after. The data ptr, GFxValue (+0x30540) and
    // hovered index (+0x30558) are the same on both. See
    // findings/findings-SE-MapMenu-Offsets.md. Resolved at the use site via
    // REL::Relocate (SE-first), not constexpr because it's a runtime branch.
    inline std::ptrdiff_t MarkerArrayCountOffset() {
        return REL::Relocate<std::ptrdiff_t>(0x30480, 0x30478);
    }
    // MapMenu+0x10 holds GFxMovieView*. Same pattern used by the
    // description-card suppression code.
    constexpr std::ptrdiff_t kMovieViewOffset     = 0x10;

    // Local-map mode byte. MapMenu+0x60 = LocalMapMenu; +0x303FC == 1 means the
    // local map is the one on screen, anything else = world map. Same predicate
    // LocalMapRenderer::GetActiveLocalMapBase uses (src/LocalMapRenderer.cpp:77,
    // 80, 168); identical on SE and AE.
    constexpr std::uintptr_t kLocalMapBaseOffset = 0x60;
    constexpr std::uintptr_t kLocalMapModeOffset = 0x303FC;

    // True when the WORLD map is the visible one. The vanilla-card suppression
    // is world-map-only: the card populator (SE 52187 / AE 53079) is SHARED with
    // the local map, whose door-name card we still want, so every suppression
    // site must be gated on this. Mirrored in MapHoverHook.cpp.
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
        // Raw MARKER_TYPE behind typeLabel. Kept separate because typeLabel is
        // empty for unlabeled/modded types, so it can't drive the focus-mode
        // gate. kNone reads as unfiltered, which matches the filter hook.
        std::uint16_t    markerType   = 0;
        std::string      holdName;          // Hold name (engine string), "" if none
        // Cleared is a LOCATION property (BGSLocation::IsCleared), NOT a marker
        // flag. Vanilla surfaced it as a hover-card row ({Item:"", Value:"Cleared"});
        // we suppress that card, so we resolve and draw it ourselves.
        bool             isCleared    = false;
        // Outstanding bounty the player owes this marker's hold (violent +
        // non-violent). PLAYER state keyed by faction, not a location property.
        // 0 = no row. Resolved once in ResolveIdentity (cached), never per-frame.
        std::uint32_t    bounty       = 0;
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

    // Identity of one local-map door. Declared here (not next to
    // ResolveLocalIdentity below) because the context-menu state above needs the
    // complete type.
    struct LocalIdentity {
        RE::FormID   doorFormID = 0;
        // The DOOR's own space, which is what a beacon needs: the player has to
        // walk to the door. Set from the door ref itself, not from where it
        // leads. Exterior door -> worldSpaceID set, cellID 0.
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

    // Local-map door menu. Separate from the world-map pair above because the
    // two maps have separate marker arrays and only one is ever on screen.
    // Identity is snapshotted at open time: the popup outlives the hover (the
    // cursor moves off the door onto the menu itself), so reading it live would
    // lose the target the moment the menu appears.
    bool          g_openLocalContextMenu = false;
    LocalIdentity g_localContextMenuState;

    // FormID (lowercased 8-char hex) -> PNG file stem. Populated once at
    // Init() from Data/SKSE/Plugins/CMM_MarkerImages.json. Read-only after
    // that, so no mutex needed for Draw() reads.
    std::unordered_map<RE::FormID, std::string> g_formIDToImage;

    // MapMenu+0x30470 is stable while the world-map marker array is stable.
    // Build the stacked-alias dispatch table once per array generation instead
    // of linearly searching the whole vanilla marker list every hover frame.
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

    // Diagnostic gating. Log only when we cross from one state to another so
    // the log isn't spammed at ~60fps. Cleared on map close indirectly when
    // idx goes -1 for the first time.
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

    // Layout of one MapMenu+0x30470 entry (0x38 bytes). Verified via the
    // Ghidra decompile of FUN_140982750 (the card-population function). The
    // array stacks multiple entries on the same map position; type==0 is the
    // base MapMarker and holds the name, type==1 is a stacked City/Quest
    // alias that re-uses the base entry's handle. Reading +0x00 as
    // TESFullName* is only safe when type==0; that's why earlier code
    // crashed on Whiterun/Solitude (their hovered entry is type==1).
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

    // ---- LOCAL map marker array -------------------------------------------
    // The local map keeps its OWN marker list, separate from the world map's
    // MapMenu+0x30470. Layout from findings-LocalMap-Hover-Identity.md
    // (decompiled from RefreshMarkers AE 0x97d690 / CardPopulator AE 0x982750).
    //
    // Base is LocalMapMenu (= MapMenu + 0x60), NOT MapMenu. Do not mix them:
    // LocalMapMenu's struct extends past 0x30470, so the two offset spaces
    // OVERLAP and a wrong-base read returns plausible nonsense, never an error.
    // Resolving this array's hovered index against MapMenu+0x30470 cost a full
    // debug session -- it named a real WORLD location for every local door hover.
    //
    // Count sits at LocalMapMenu+0x10 on BOTH runtimes (embedded BSTArray), so
    // unlike the world array there is no version-dependent count offset here.
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

    // Local-map twin of ResolveIdentity. Same stacked-alias dispatch as the
    // world map (type==1 re-uses a base entry's handle; read identity off the
    // type==0 entry with the matching handle, never off the alias).
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
                // WorldBeaconRenderer's space check needs the door's OWN space:
                // outdoors it requires worldSpaceID != 0 matching the player's
                // current worldspace, so a city door must carry Tamriel here.
                if (auto* ws = doorRef->GetWorldspace()) a_out.doorWorldSpaceID = ws->GetFormID();
                if (auto* cell = doorRef->GetParentCell()) a_out.doorCellID = cell->GetFormID();

                // Load door: the useful destination is where it LEADS, not the
                // door itself. teleportData->position is the arrival point and is
                // always populated, so it stands alone as the destination even when
                // the linked door ref is not currently loaded. linkedDoor is an
                // ObjectRefHandle (RE/E/ExtraTeleport.h), so .get() is the accessor;
                // it only resolves while the target cell is loaded, which is why
                // cell/worldspace are filled opportunistically and position is not.
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

    // Resolve the location identity for the currently-hovered vanilla marker.
    // Replicates the engine's dispatch (findings-World-Map-Hover-Identity.md):
    // for stacked type==1 entries, walk the array to find the type==0 base
    // entry with the same handle and read identity from there.
    // Engine MARKER_TYPE enum (RE/E/ExtraMapMarker.h) -> localization key
    // under the [MarkerType] section of the translation files. Returns the
    // key suffix only (e.g. "NordicRuin"); the caller prefixes "MarkerType."
    // and runs it through LocalizationManager so the shown word is translated.
    // Returns nullptr for types we deliberately don't label: 0 (None),
    // 57/58 (travel doors), 60+ (doors, quest targets, modded/out-of-range).
    // Capitals + castles (35-52) collapse to "City": the city name is already
    // on the hover card, so "Whiterun Capital" would be redundant. Verified
    // against live hover logs across multiple holds (2026-06-05).
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

    // LocTypeHold keyword (Skyrim.esm 0x00016771), base-game form so the local
    // ID is stable across load orders. Walk parentLoc until a location carries
    // this; that location's name is the Hold.
    constexpr RE::FormID kLocTypeHoldLocalID = 0x00016771;

    // Walk the BGSLocation parent chain from the marker ref's editor location
    // until a location carries LocTypeHold; return that Hold location (nullptr if
    // the ref has no location link or no Hold in the chain). Returning the Hold
    // itself lets the caller get name AND bounty from one walk.
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

    // "Cleared" is what vanilla printed as a hover-card row. It is NOT a marker
    // flag: MapMarkerData::Flag has no such bit (only kVisible / kCanTravelTo /
    // kShowAllHidden + 5 unknowns). It lives on the LOCATION
    // (RE/B/BGSLocation.h:114 IsCleared(), :136 cleared, :137 everCleared).
    // Read it off the marker's own editor location, NOT the parent Hold: the
    // Hold is never "cleared", the dungeon is. Suppressing the card killed
    // vanilla's row, so we draw it ourselves from here.
    bool ResolveCleared(RE::TESObjectREFR* a_refr) {
        if (!a_refr) return false;
        auto* loc = a_refr->GetEditorLocation();
        if (!loc) return false;
        return loc->IsCleared();
    }

    // Convert a marker position from its own worldspace into the frame the world
    // map is currently drawing (normally Tamriel). See the call site for the
    // measured offsets that prove the transform is plain addition.
    //
    // Reads worldMapOffsetData off TESWorldSpace. CommonLib types the struct as
    // { mapScale, mapOffsetX, mapOffsetY, mapOffsetZ }. Only X/Y move a child
    // worldspace into its parent map frame. Projection Z is selected later by
    // MapDraw::MapProjectionPoint from the live map camera geometry.
    //
    // Returns false when the chain never reaches the map's worldspace (a
    // standalone mod world like Wyrmstooth shown on Skyrim's map). No sane
    // conversion exists there, so the caller should not trust the position.
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

        // Dispatch: type==1 means the entry is a stacked City/Quest alias;
        // the real name lives on the matching type==0 base entry. Array size
        // is the BSTArray count after the +0x30470 data ptr: AE +0x30478,
        // SE +0x30480 (see MarkerArrayCountOffset()).
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
                    // Child worldspaces (Karthspire/Sky Haven, Dayspring Canyon/
                    // Fort Dawnguard) store marker positions in their OWN origin,
                    // but the world map draws in the PARENT (Tamriel) frame. The
                    // ref position is correct data in the wrong frame, so project
                    // it as-is and the card flies off to the screen edge.
                    //
                    // worldMapOffsetData.mapOffsetX/Y converts child -> parent by
                    // plain addition; there is NO scale term (mapScale is map-IMAGE
                    // scale, always 50, unrelated). Verified live 2026-07-25:
                    //   Karthspire  off=(-272179, 9011) : 130399 -> -141780 (far west, correct)
                    //   Dayspring   off=(+212992,-110592):   6686 ->  219678 (far east, correct)
                    //   MarkarthWorld off=(0,0)             : unchanged, already Tamriel-aligned
                    // Markarth having zero offsets is why Understone Keep always
                    // looked fine while these two did not. Walk the parent chain so
                    // nested child worlds and mod-added ones work with no ID table;
                    // markers already in the map's worldspace add nothing and are
                    // bit-for-bit unaffected.
                    ConvertToMapWorldspace(ws, a_out.position);
                }
                if (auto* cell = refr->GetParentCell()) {
                    a_out.cellID = cell->GetFormID();
                }

                if (auto* extra = refr->extraList.GetByType<RE::ExtraMapMarker>()) {
                    if (extra->mapData) {
                        a_out.isDiscovered = (extra->mapData->flags.underlying() & 2) != 0;

                        // Translate the marker type to a display word. nullptr
                        // for unlabeled types (None, doors, modded); leave
                        // typeLabel empty so the draw shows Hold only.
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

    // MapMenu's field at +0x30540 is the cached MapMovie root GFxValue
    // (the slots +0x30540/+0x30548/+0x30550 are GFxValue::_objectInterface,
    // _type, _value respectively). Confirmed via Ghidra: the engine calls
    // GFxValue::GetMember(MapMovie, "MarkerDescriptionObj", out) inside
    // UpdateHoveredMarker. MarkerDescriptionObj is the inner Description
    // clip; its _parent is MarkerDescriptionHolder (the card we want).
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

    // All values pulled from the holder clip. _x/_y are stage coords
    // (already include MarkerContainer offsets, per the finding); _width
    // and _height are in stage units; _xscale/_yscale are percent (100 = 1x).
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

    // ---- Stat-card takeover: hide the vanilla card ------------------------
    // MapHoverHook skips the card populator (SE 52187 / AE 53079) on the
    // world map, so vanilla never writes Title.text. But the AS TextField then
    // falls back to its DESIGN-TIME placeholder, the literal string
    // "Marker Name" authored into map.swf. Confirmed in-game 2026-07-14: the
    // card renders "Marker Name" on every hover. Skipping the populator is not
    // enough on its own; the clip has to be hidden.
    //
    // _visible=false on the HOLDER (not the Description clip) kills the whole
    // card: title, rows, backdrop. Deliberately a SetMember, not an Invoke:
    // a GFxValue member set is not a virtual dispatch, so it sidesteps the NG
    // Scaleform vtable off-by-one hazard entirely (memory
    // feedback_ng_scaleform_vtables: NG's GFx* vtables are shifted vs the
    // binary and silently dispatch to the wrong slot; cost 3 CTDs). Calling
    // SetDescription("", []) would have been the other route and is NOT worth
    // the vtable risk.
    //
    // Vanilla re-shows the holder every hover via gotoAndPlay("Show") in
    // SetSelectedMarker, so this must be re-applied per frame, not once.
    // Order matters: this runs AFTER the transform reads above, because the
    // holder's _x/_y still track the selected marker's pin every frame
    // (MapMenu.RefreshMarkers) even while invisible, and that is what our
    // hover layer anchors to.
    const bool hideVanillaCard = IsWorldMapShowing();
    if (hideVanillaCard) {
        RE::GFxValue vis;
        vis.SetBoolean(false);
        holder.SetMember("_visible", vis);
    }
    // ---- end takeover -----------------------------------------------------

    // Guard against a degenerate holder. NOTE: when we hide the card above, the
    // engine may report _alpha as 0 during its Hide tween, and a hidden clip can
    // read back zero width/height. Do NOT bail in that case: bailing clears the
    // state, which kills identity resolution and takes our own hover layer down
    // with the vanilla card. Only enforce the guard when we are NOT the ones
    // hiding it.
    if (!hideVanillaCard && (widthS <= 0.0 || heightS <= 0.0 || alpha <= 0.0)) {
        LogStage(Stage::BadSize,
                 "BadSize (zero w/h/alpha)");
        clearState();
        return;
    }
    // With the vanilla card hidden, its _alpha is meaningless as a fade source
    // for OUR layer. Pin it to fully opaque so the hover layer stays visible.
    // The stat card will own its own fade later (spec Decision 8).
    if (hideVanillaCard) {
        alpha = 100.0;
    }

    // The vanilla title is not at the holder origin. holder._x is the pin
    // position (anchor finding §2); the card content is laid out beside the
    // pin inside the Description clip. Title center, in holder-local units,
    // is descObj._x + Title._x + Title._width/2, then scaled by the holder's
    // _xscale and added to the stage origin. descObj._x and Title._x are
    // layout constants (-23.8 and 30.0 across every marker, verified in-game
    // 2026-06-05); Title._width tracks the name. Result is the title's true
    // stage X, which our custom text centers on so it sits over the title
    // regardless of name length. Falls back to the pin if Title is missing.
    // Title position in holder-local units. _x/_y of descObj and Title are
    // layout constants per-marker EXCEPT Title._y, which moves up when the
    // marker is cleared (the "Cleared" line grows the card and the centered
    // layout pushes the title up: verified -19.8 uncleared vs -33.6 cleared,
    // 2026-06-05). So anchor BOTH X and Y to the title clip, not the box,
    // and the text rides the title regardless of cleared state or name length.
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

    // Stage -> screen: use the viewport directly. Per
    // findings-World-Map-Hover-Animation-And-Card-Anchor.md Section 3,
    // Skyrim's UI stage is height-locked at 720 nominal, width scales with
    // aspect. So:
    //   scale = viewport.height / 720
    //   screenX = viewport.left + stageX * scale
    //   screenY = viewport.top  + stageY * scale
    // The holder's _x/_y are already root-relative stage coords, so this
    // single multiply gives the card center.
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

    // Per the finding, holder local (0,0) is the vertical center of the
    // card. Horizontally it's also centered on the marker. So screenPt is
    // already the card center; no further offset needed.
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

    // Focus-mode gate. Vanilla's own hover index still points at a marker the
    // MarkerFilterHook detour has hidden (the hook forces Flash _visible=false,
    // it does not clear +0x30538), so without this the stat card and the
    // right-click menu still fire over a marker the player filtered away.
    // Gating here rather than in Draw() covers every g_state consumer at once:
    // the card, OpenContextMenu, and the discovered/undiscovered queries.
    // Ref-aware form: a CoMAP-retyped marker's type alone resolves to kModded,
    // so the type-only call would keep popping the card for a marker that
    // Dungeons/Shops/Loot just hid. Cheap here — one lookup per hover change,
    // not per frame, and ResolveModeForRef caches per type after the first.
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

    // Compose the live-runtime FormID from a (plugin, localID) pair.
    // Pattern from PartySheet/src/PlayerStatus.cpp ResolveFormID. ESL uses
    // the small-file compile index in the middle 12 bits with prefix 0xFE.
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

        // Expected key format: "Plugin.esm|0xLOCALID". Pipe is the separator,
        // 0x prefix is required on the local ID. We tolerate any case on
        // both sides; LookupModByName is case-insensitive.
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
        return;
    }
    if (MenuUtils::ShouldBlockMapOverlay()) {
        g_openContextMenu = false;
        g_openLocalContextMenu = false;
        return;
    }

    // Never paint over the black fade (FaderMenu up during open/close
    // transitions), so the hover layer doesn't pop on the black screen. No
    // effect during normal interactive map use. See plugin.cpp.
    if (IsMapFading()) {
        return;
    }

    // ------------------------------------------------------------------------
    // Right-Click Context Menu (Must execute every frame, even if nothing hovered)
    // ------------------------------------------------------------------------
    if (g_openContextMenu) {
        g_openContextMenu = false;
        ImGui::OpenPopup("VanillaContextMenu");
    }

    // CHROME, NOT CONTENT (2026-08-25). Context menus are pinned to the fixed
    // kPopup* theme on EVERY channel, background and border included. They used
    // to take bg/border from Appearance > Global Styles like the panels do, and
    // that was wrong: a menu is a transient interaction surface, not a surface
    // the user is decorating. With a red card set in the Visuals tile the menu
    // came up red too and read as part of the card. The hover band and disabled
    // rows are also tuned against a dark ground, so a bright user bg costs
    // contrast the card never loses. Deliberately NOT user-settable.
    ImGui::PushStyleColor(ImGuiCol_PopupBg,       MapTheme::kPopupBg());
    ImGui::PushStyleColor(ImGuiCol_Border,        MapTheme::kPopupBorder());
    ImGui::PushStyleColor(ImGuiCol_Text,          MapTheme::kPopupText());
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, MapTheme::kPopupHoverBg());
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  MapTheme::kPopupHoverBg());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(8.0f,  6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    if (ImGui::BeginPopup("VanillaContextMenu")) {
        ImGui::SetWindowFontScale(std::clamp(Settings::GetSingleton()->bodyScale, 0.5f, 3.0f));
        const auto& cs = g_contextMenuState;

        ImGui::TextColored(MapTheme::kPopupHeaderText(), "%s", cs.name.c_str());
        ImGui::Separator();

        if (cs.hasPosition) {
            bool isDest = MarkerManager::GetSingleton()->IsVanillaDestination(cs.name);
            if (ImGui::MenuItem(LocalizationManager::Get(isDest ? "Menu.RemoveDestination"
                                                                 : "Menu.SetDestination").c_str())) {
                logger::debug("[HoverCard] Toggle Vanilla Destination clicked for {}", cs.name);
                if (isDest) {
                    MarkerManager::GetSingleton()->RemoveVanillaDestination();
                } else {
                    MarkerManager::GetSingleton()->SetVanillaDestination(cs.name, cs.formID, cs.worldSpaceID, cs.cellID, cs.position);
                }
            }

            bool isFav = MarkerManager::GetSingleton()->IsFavorite(cs.formID);
            if (!cs.isDiscovered) ImGui::BeginDisabled();
            if (ImGui::MenuItem(LocalizationManager::Get(isFav ? "Menu.RemoveFavorite"
                                                                : "Menu.AddToFavorites").c_str())) {
                logger::debug("[HoverCard] Toggle Favorite clicked for {}", cs.name);
                MarkerManager::GetSingleton()->ToggleFavorite(cs.formID, cs.worldSpaceID, cs.cellID, cs.position, cs.name);
                // Jump the list panel to Favorites so the new row is visible.
                // Only on ADD: after a remove there is nothing to show, and
                // isFav is the state from BEFORE the toggle.
                if (!isFav) {
                    MapMarkerRenderer::GetSingleton()->ShowListTab(
                        MapMarkerRenderer::ListTab::Favorites);
                }
            }
            if (!cs.isDiscovered) ImGui::EndDisabled();

            // Note on the vanilla marker. Keyed by ref FormID; the dialog hides
            // the category/destination widgets in this mode. The popup closes on
            // select, then the dialog opens (sequential, not stacked).
            if (cs.formID != 0) {
                bool hasNote = !MarkerManager::GetSingleton()->GetVanillaNote(cs.formID).empty();
                // GetLabel returns the key itself when the translation file
                // doesn't have it yet, so fall back to the English string.
                auto* loc = LocalizationManager::GetSingleton();
                std::string noteLabel = loc->GetLabel(hasNote ? "VanillaMenu.EditNote" : "VanillaMenu.AddNote");
                if (noteLabel == "VanillaMenu.EditNote") noteLabel = "Edit Note";
                else if (noteLabel == "VanillaMenu.AddNote") noteLabel = "Add Note";
                if (ImGui::MenuItem(noteLabel.c_str())) {
                    logger::debug("[HoverCard] Vanilla note clicked for {} ({:08X})", cs.name, cs.formID);
                    MapMarkerRenderer::GetSingleton()->BeginEditVanillaNote(cs.formID, cs.name);
                }
            }
        } else {
            ImGui::TextDisabled("%s", LocalizationManager::Get("Menu.LocationDataUnavailable").c_str());
        }

        ImGui::EndPopup();
    }

    // ------------------------------------------------------------------------
    // Local-map door menu (Set/Remove Destination).
    // ------------------------------------------------------------------------
    // Reuses the style scope pushed above for the vanilla menu, so both popups
    // look identical. Scope is deliberately narrow: destination only, no
    // favorites. A favorited door has nowhere to pan to from the world map (it
    // has no world marker of its own), which is an unsolved list-panel question.
    //
    // The destination stored is where the door LEADS (teleportData->position),
    // not the door's own position: the beacon should point at the interior the
    // player wants to reach. Falls back to the door itself for a marker with no
    // teleport data.
    if (g_openLocalContextMenu) {
        g_openLocalContextMenu = false;
        ImGui::OpenPopup("LocalDoorContextMenu");
    }

    if (ImGui::BeginPopup("LocalDoorContextMenu")) {
        ImGui::SetWindowFontScale(std::clamp(Settings::GetSingleton()->bodyScale, 0.5f, 3.0f));

        const auto& d = g_localContextMenuState;
        ImGui::TextColored(MapTheme::kPopupHeaderText(), "%s", d.name.c_str());
        ImGui::Separator();

        // Keyed by NAME, same as the world-map vanilla destination: that is what
        // MarkerManager's vanilla-destination API stores and compares.
        const bool isDest = MarkerManager::GetSingleton()->IsVanillaDestination(d.name);
        if (ImGui::MenuItem(LocalizationManager::Get(isDest ? "Menu.RemoveDestination"
                                                             : "Menu.SetDestination").c_str())) {
            if (isDest) {
                MarkerManager::GetSingleton()->RemoveVanillaDestination();
                logger::info("[LocalDoor] cleared destination ({})", d.name);
            } else {
                // The DOOR's position and space, not the interior it leads to.
                // First cut stored destPosition and produced no beacon at all:
                // interior coords in an interior cell (worldSpaceID 0) fail
                // WorldBeaconRenderer's exterior space check, which requires a
                // non-zero worldSpaceID matching the player's current worldspace.
                // Walking TO the door is the navigation problem worth solving.
                // fromLocalDoor=true: a door has no world-map icon, so the
                // world map suppresses its ring (it would float over empty
                // terrain). The exterior local map draws it under the door icon.
                MarkerManager::GetSingleton()->SetVanillaDestination(
                    d.name, d.doorFormID, d.doorWorldSpaceID, d.doorCellID, d.doorPosition, /*fromLocalDoor=*/true);
                logger::info("[LocalDoor] set destination {} ({:08X}) ws {:08X} cell {:08X} pos ({:.0f}, {:.0f}, {:.0f})",
                             d.name, d.doorFormID, d.doorWorldSpaceID, d.doorCellID,
                             d.doorPosition.x, d.doorPosition.y, d.doorPosition.z);
            }
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

    // ========================================================================
    // STAT CARD (hover layer)
    // ------------------------------------------------------------------------
    // This replaces vanilla's hover card, which we suppress (see the STATUS
    // block). One self-contained panel drawn at the marker pin:
    //
    //     +-----------------------+
    //     | Whiterun              |  name   (was vanilla's card title)
    //     | City - Whiterun       |  type - hold
    //     | Cleared               |  only when the location is cleared
    //     | --------------------- |
    //     | <note text>           |  only when a note exists
    //     +-----------------------+
    //
    // HARDCODED ON PURPOSE (2026-07-14). The old [HoverText]/[NoteText] align +
    // offset settings existed to dodge vanilla's card, which recentred itself on
    // every populate and whose width tracked the name. We own the geometry now,
    // so a panel lays its own rows out and cannot misalign. Those settings are
    // therefore obsolete, but nothing is deleted yet: prove the data draws
    // correctly first, then decide what settings the panel actually needs.
    //
    // ANCHOR: projected from the marker's WORLD position through the map camera
    // (FindMapCamera + WorldPtToScreenPt3), exactly as MapMarkerRenderer already
    // does for our own custom pins. It deliberately does NOT use s.centerX/
    // centerY/textAnchorX/titleWidthPx: those derive from the vanilla card clip,
    // which is now hidden and never populated, so Title._width is meaningless.
    // Those fields are still sampled (harmless) but are no longer load-bearing.
    // ========================================================================

    if (!s.hasPosition) {
        return;
    }

    auto* ui2 = RE::UI::GetSingleton();
    auto  mapMenuPtr = ui2 ? ui2->GetMenu<RE::MapMenu>() : nullptr;
    if (!mapMenuPtr) {
        return;
    }

    // Map camera: MapMenu::GetRuntimeData2().camera.cameraRoot->GetChildren()[0].
    // RE::MapCamera::zoom is known-broken (returns 0.0), hence the child walk.
    // Same path as MapMarkerRenderer::FindMapCamera.
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

    // ---- Build the rows ----------------------------------------------------
    // Scales come from the GLOBAL appearance settings, same ones the list panel,
    // Map Filters, and the local-map renderer use. The stat card is not a special
    // case: title = the location name, body = every other row. Notes keep their
    // own scale because they are free text and can run long.
    const auto* set = Settings::GetSingleton();
    const float titleScale = std::clamp(set->titleScale,    0.5f, 3.0f);
    const float bodyScale  = std::clamp(set->bodyScale,     0.5f, 3.0f);
    const float noteScale  = std::clamp(set->noteTextScale, 0.5f, 3.0f);

    const float baseFS = ImGui::GetFontSize();
    // Title sizes off the HEADING font's own size × titleScale, same as the list
    // panel / dialog headers (MapMarkerRenderer.cpp). Sizing off baseFS (the body
    // font, ~FontSize) made the card title smaller than every other title by the
    // FontSize/HeadingFontSize ratio. Falls back to baseFS when no heading font.
    ImFont* headingFont = FontManager::GetSingleton()->GetHeadingFont();
    const float titleBaseFS = headingFont ? headingFont->FontSize : baseFS;
    const float nameFS = titleBaseFS * titleScale;
    const float rowFS  = baseFS * bodyScale;
    const float noteFS = baseFS * noteScale;

    // Stat rows are {label, value} pairs drawn as TWO columns: label at the
    // card's left edge, value at a fixed indent shared by every row, so the
    // values line up regardless of label length. Built as a vector so measure
    // and draw iterate the same list and cannot disagree about which rows exist.
    //
    // EVERY row draws in the body colour, with its label faded to kLabelAlpha.
    // Bounty and Cleared used to be flagged "muted" and painted the hardcoded
    // MapTheme::kMutedText grey, which left those two rows ignoring the Visuals
    // tile while Type and Hold followed it. They are data like any other row;
    // the label/value contrast alone carries the hierarchy.
    struct StatRow {
        std::string label;
        std::string value;
    };
    std::vector<StatRow> rows;

    if (!s.typeLabel.empty()) {
        rows.push_back({ LocalizationManager::Get("StatCard.Type"), s.typeLabel });
    }
    if (!s.holdName.empty()) {
        rows.push_back({ LocalizationManager::Get("StatCard.Hold"), s.holdName });
    }

    // Bounty: the player's outstanding crime gold for this hold. Driven by
    // PLAYER state, so the same marker shows no row with a clean record and
    // grows one after a crime. No row when zero: the card stays quiet.
    if (s.bounty > 0) {
        // Raw number, no translation key: a bare "{}" key carries no translatable
        // text, so it would be a key nobody can ever translate.
        rows.push_back({ LocalizationManager::Get("StatCard.Bounty"),
                         std::to_string(s.bounty) });
    }

    // Cleared: resolved from BGSLocation::IsCleared(), NOT a marker flag.
    // Vanilla drew this as a card row; we suppressed the card, so we draw it.
    // The row vector owns its strings, so the old "keep clearedStr alive or the
    // char* dangles" dance is gone.
    if (s.isCleared) {
        rows.push_back({ LocalizationManager::Get("StatCard.Cleared"),
                         LocalizationManager::Get("StatCard.ClearedYes") });
    }

    // Note: our own annotation, keyed by ref FormID. Runtime 0xFF... FormIDs are
    // our CNO-compat XMarker pool, never real locations.
    std::string note;
    if (s.formID != 0 && (s.formID & 0xFF000000u) != 0xFF000000u) {
        note = MarkerManager::GetSingleton()->GetVanillaNote(s.formID);
    }

    // ---- Measure -----------------------------------------------------------
    // Equal on both axes so the card's inset reads the same all the way round.
    // kPadY was 9.0f, which looked tight against the 12 horizontal.
    constexpr float kPadX      = 12.0f;
    constexpr float kPadY      = 12.0f;
    constexpr float kRowGap    = 3.0f;
    constexpr float kSepGap    = 6.0f;
    constexpr float kNoteWrapW = 260.0f;   // hardcoded for now; was [NoteText] Width
    constexpr float kPinGap    = 14.0f;    // gap between pin and panel bottom
    // Value column, in multiples of the row font size so it tracks the body-size
    // setting instead of drifting at large fonts. ~4.5em fits "Cleared"/"Bounty"
    // in English with room to spare for longer translations.
    constexpr float kValueColEm = 4.5f;
    constexpr float kLabelGap   = 8.0f;    // min gap when a label overruns the column

    // Measure in SCREEN pixels, using the same wrap value that AddText will get.
    //
    // The first cut of this got it wrong and the note bled out of the panel at
    // large note scales: it measured with `wrap / k` but drew with a differently
    // scaled wrap, so measure and draw disagreed on the line count as noteFS
    // grew. ImFont::CalcTextSizeA takes the wrap width in the SAME space as the
    // size arg it is given, so pass `fs` and the raw screen wrap to both, and do
    // not rescale the result afterwards. One number, both paths.
    auto* measureFont = ImGui::GetFont();
    auto measure = [measureFont](const char* txt, float fs, float wrap = 0.0f) -> ImVec2 {
        return measureFont->CalcTextSizeA(fs, FLT_MAX, wrap, txt);
    };
    // Measure the name with the heading font (declared above) so the panel width
    // matches the drawn glyphs. Falls back to the base font when none is set.
    ImFont* nameFont = headingFont ? headingFont : measureFont;

    const ImVec2 nameSz = nameFont->CalcTextSizeA(nameFS, FLT_MAX, 0.0f, s.name.c_str());

    // Value column x, relative to the card's text edge. FIXED, not measured from
    // the widest label: a measured column drifts between languages and between
    // cards, and the point of the column is that values sit in the SAME place
    // every time. Scales with rowFS so it tracks the body-size setting.
    const float valueColX = kValueColEm * rowFS;

    // Rows are measured as (column + value), not as one joined string, because
    // the value starts at valueColX no matter how wide its label is.
    //
    // The clamp is the one concession to a FIXED column: a translated label
    // longer than valueColX would otherwise be overrun by its own value. When
    // that happens the row (and only that row) pushes its value right, which
    // costs alignment on that line but never loses text.
    float rowsW = 0.0f;
    for (const auto& r : rows) {
        const float labelW = measure(r.label.c_str(), rowFS).x;
        const float valueX = (std::max)(valueColX, labelW + kLabelGap);
        rowsW = (std::max)(rowsW, valueX + measure(r.value.c_str(), rowFS).x);
    }

    // Wrap the note to the widest of (the header rows, the note cap). A long
    // location name already makes the panel wide, so wrapping the note at a fixed
    // 260 there would leave a narrow text column inside a wide box.
    float headerW = (std::max)(nameSz.x, rowsW);
    const float noteWrapW = (std::max)(kNoteWrapW, headerW);

    const ImVec2 noteSz = note.empty() ? ImVec2{0,0}
                                       : measure(note.c_str(), noteFS, noteWrapW);

    // "Notes" heading above the note body, so the note reads as a labeled field
    // like Type/Hold instead of loose text under a rule. Measured at rowFS (not
    // noteFS): it is a LABEL, and must not scale with the note's own size knob.
    const std::string notesLabel = note.empty() ? std::string{}
                                                : LocalizationManager::Get("StatCard.Notes");
    const ImVec2 notesLabelSz = notesLabel.empty() ? ImVec2{0,0}
                                                   : measure(notesLabel.c_str(), rowFS);

    float contentW = headerW;
    contentW = (std::max)(contentW, noteSz.x);
    // The Notes label is measured after headerW is fixed (deliberately: a label
    // must not widen the note's wrap width), so fold it into the panel width
    // here or a long translated "Notes" would draw past the border.
    contentW = (std::max)(contentW, notesLabelSz.x);

    float contentH = nameSz.y;
    // Every stat row is one line at rowFS, so height is uniform per row.
    const float statRowH = rows.empty() ? 0.0f : measure("Ag", rowFS).y;
    contentH += static_cast<float>(rows.size()) * (kRowGap + statRowH);
    if (!note.empty())     contentH += kSepGap + 1.0f + kSepGap +
                                       notesLabelSz.y + kRowGap + noteSz.y;

    const float panelW = contentW + kPadX * 2.0f;
    const float panelH = contentH + kPadY * 2.0f;

    // ---- Place: direction-from-pin anchor + user offset, clamped on screen --
    // Anchor: 0=Top (above), 1=Bottom (below), 2=Left, 3=Right. For Top/Bottom
    // the panel is horizontally centred on the pin; for Left/Right it's
    // vertically centred. OffsetX/Y nudge on top of the chosen direction.
    const auto* place = Settings::GetSingleton();
    const float offX = static_cast<float>(place->statCardOffsetX);
    const float offY = static_cast<float>(place->statCardOffsetY);

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

    constexpr float kEdge = 8.0f;
    px = std::clamp(px, kEdge, (std::max)(kEdge, disp.x - panelW - kEdge));
    py = std::clamp(py, kEdge, (std::max)(kEdge, disp.y - panelH - kEdge));

    // ---- Draw --------------------------------------------------------------
    // Alpha: the vanilla card's _alpha is meaningless now (we hide the clip), so
    // the panel is drawn fully opaque. A proper fade is a later step.
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

    // Colors come from the GLOBAL appearance settings (Visuals tile), NOT from
    // hardcoded MapTheme tokens: the stat card must match the list panel and the
    // Map Filters window, which already read these. MapTheme stays the source of
    // the DEFAULTS (Settings.h seeds from it) and of border/rounding, which are
    // not user-facing.
    const ImU32 colName = withA(IM_COL32(set->titleColorR, set->titleColorG,
                                         set->titleColorB, 255));
    const ImU32 colBody = withA(IM_COL32(set->bodyColorR, set->bodyColorG,
                                         set->bodyColorB, 255));

    MapDraw::DrawPanelCard(dl, pMin, pMax, set->BgCol(), set->BorderCol(), a);

    const ImU32 colSep    = withA(MapTheme::kSeparator);

    auto* font = ImGui::GetFont();
    float cy = py + kPadY;

    // LEFT-ALIGNED (2026-08-25, phase 2). Every row shares one text edge instead
    // of each being centred on its own width. The centring was inherited from
    // vanilla's card, which centred its own title; that card is suppressed now,
    // so the reason is gone and ragged-centre rows read as unanchored.
    // One x for the whole card: rows, name and the Notes label all start here.
    const float tx = px + kPadX;

    // Name (header).
    {
        // Was a single hard 1px offset copy; now the shared spread shadow so the
        // card header matches the list panel and Legend headings.
        MapDraw::DrawTextShadowed(dl, nameFont, nameFS, { tx, cy }, colName, s.name.c_str());
        cy += nameSz.y;
    }

    // Stat rows: label at the text edge, value at the shared column. The label
    // is the SAME colour as its value at MapDraw::kLabelAlpha, not a separate
    // grey, so a user-set body colour tints both halves of the row together.

    const ImU32 colLabel = MapDraw::FadeAlpha(colBody, MapDraw::kLabelAlpha);

    for (const auto& r : rows) {
        cy += kRowGap;
        dl->AddText(font, rowFS, { tx, cy }, colLabel, r.label.c_str());

        // Same clamp the measure pass used; the two must agree or a long label
        // draws over its value on one runtime and not the other.
        const float labelW = measure(r.label.c_str(), rowFS).x;
        const float vx     = tx + (std::max)(valueColX, labelW + kLabelGap);
        dl->AddText(font, rowFS, { vx, cy }, colBody, r.value.c_str());

        cy += statRowH;
    }

    if (!note.empty()) {
        cy += kSepGap;
        dl->AddLine({ px + kPadX, cy }, { px + panelW - kPadX, cy }, colSep, 1.0f);
        cy += 1.0f + kSepGap;

        dl->AddText(font, rowFS, { tx, cy },
                    MapDraw::FadeAlpha(colBody, MapDraw::kLabelAlpha),
                    notesLabel.c_str());
        cy += notesLabelSz.y + kRowGap;

        // Draw with the SAME noteWrapW that measure() used above, so the line
        // count cannot disagree between measure and draw and the text can never
        // bleed past the panel. (The first cut divided the wrap by the font ratio
        // on one side only; at large note scales the drawn text wrapped into more
        // lines than the panel had been sized for, and it spilled out the bottom.)
        dl->AddText(font, noteFS, { tx, cy }, colBody,
                    note.c_str(), nullptr, noteWrapW);
    }

    // ---- Marker image ------------------------------------------------------
    // Optional PNG, keyed on FormID via CMM_MarkerImages.json. Now anchored to
    // the panel top instead of the (dead) vanilla card top. Skips our own
    // runtime 0xFF... XMarker FormIDs.
    if (!set->markerImageEnabled) {
        return;
    }
    if (s.formID == 0 || (s.formID & 0xFF000000u) == 0xFF000000u) {
        return;
    }

    auto mapIt = g_formIDToImage.find(s.formID);
    if (mapIt == g_formIDToImage.end()) {
        return;
    }

    const std::string& stem = mapIt->second;
    const std::string  path = "Data\\Interface\\CustomMarkers\\Images\\" + stem + ".png";
    auto* srv = TextureManager::GetTexture(path);
    if (!srv) {
        return;
    }

    const auto size = TextureManager::GetTextureSize(srv);
    if (size.width == 0 || size.height == 0) {
        return;
    }

    const float aspect = static_cast<float>(size.width) / static_cast<float>(size.height);
    // Fraction of the panel width: 1.0 sits flush with the card edges, lower
    // insets it. Centred on the panel so it stays aligned at any width.
    const float imgW   = panelW * std::clamp(set->markerImageWidth, 0.25f, 1.0f);
    const float imgH   = imgW / aspect;
    const float imgX   = px + (panelW - imgW) * 0.5f;

    // Multiplied into the card's own fade alpha, never replacing it, so the
    // image still fades in/out with the panel instead of popping.
    const float imgA = a * std::clamp(set->markerImageAlpha, 0.0f, 1.0f);
    const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(255 * imgA));
    dl->AddImage(reinterpret_cast<ImTextureID>(srv),
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
    // Shared by IsLocalMarkerHovered/OpenLocalContextMenu. Returns the hovered
    // index on the local map, or -1. Unlike the world map there is no
    // SampleCardState cache to read: the local map drives hover from C++ (see
    // MapHoverHook), so both callers resolve live off the menu.
    std::int32_t GetLocalHoveredIndex(RE::MapMenu** a_outMenu = nullptr) {
        if (a_outMenu) *a_outMenu = nullptr;
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) return -1;
        auto mapMenu = ui->GetMenu<RE::MapMenu>();
        if (!mapMenu) return -1;

        const auto lmBase =
            reinterpret_cast<std::uintptr_t>(mapMenu.get()) + kLocalMapBaseOffset;
        // Local map only. Confirmed in-game 2026-08-16: this byte reads 0 on the
        // world map and 1 on the local map.
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
