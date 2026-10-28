#include "CompassHook.h"
#include "MarkerManager.h"
#include "HUDMarkerHook.h"
#include "Settings.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <string_view>
#include <unordered_set>
#include <vector>

// ============================================================================
// CompassHook — append custom world markers to the vanilla HUD compass
// ----------------------------------------------------------------------------
// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):
//   Working/pending in-game verification:
//     - Pre-detours Compass::Update (ID 51668).
//     - Appends plugin markers to the vanilla CompassTargetDataA GFx buffer at
//       0x1431AD490. Two separate paths:
//         Exterior: player has a worldspace. Markers with worldSpaceID != 0
//                   are matched via WorldMatches (covers parent worldspaces).
//         Interior: player worldspace is null. Markers with worldSpaceID == 0
//                   are matched against the player's current cellID. Exterior
//                   markers with isDestination == true are also pushed here:
//                   FindExitDoorForWorldspace iterates load doors in the
//                   current cell to find one whose linked door reaches the
//                   marker's worldspace, and the compass anchor is retargeted
//                   to that door's interior position. Mirrors vanilla's
//                   custom-destination behavior. Non-destination exterior
//                   markers do not show on the interior compass.
//                   See findings/findings-CompassExteriorDestinationFromInterior.md.
//                   PERF: FindExitDoorForWorldspace walks every ref in the
//                   cell. Compass::Update runs every frame (gameplay and behind
//                   both maps), so running this scan per frame cost ~10 fps in
//                   interiors whenever an exterior destination was set. The door
//                   is now cached in g_doorCache, keyed on (cell, dest marker
//                   id, dest worldspace), and only re-scanned when that key
//                   changes; the cached door is re-fetched via its handle each
//                   frame. Do NOT revert to a per-frame scan. The same fix lives
//                   in LocalMapRenderer's beacon cache for the same reason.
//                   A VANILLA destination arrives here as MarkerManager's
//                   synthesized entry, whose id is the constant kVanillaDestId.
//                   The cache key holds because it also compares the
//                   destination worldspace, which does change when the player
//                   picks a different door.
//     - Uses the registered CompassMarkerLocations type so vanilla Flash treats
//       custom rows like normal multi-instance compass location markers.
//     - Destination markers (marker.isDestination) use vanilla's playerSet
//       icon frame (HUDMarkerFrameOffsets+0x08) and MARKER_TYPE::kPlayerSet
//       so they render as the chevron destination icon on the compass. Works
//       on both interior and exterior paths.
//     - The vanilla trampoline runs after appending, naturally resizing and pushing
//       the expanded array to Flash, followed by a SetCompassAngle update that
//       ensures mods like Compass Navigation Overhaul see the new markers.
//     - Anchor Z override: marker.position.z is the WORLD-MAP mesh plane Z
//       (~kMapPlaneZ 180000), stored for map projection, NOT a real-world Z.
//       Compass Navigation Overhaul reports a full 3D distance, so that bogus Z
//       made far markers read ~4x too high (a ~600m marker showed ~2700m). We
//       substitute the player's Z for the exterior anchor so the distance is
//       effectively horizontal, close enough (elevated targets still differ by
//       their real height, which we can't know without terrain sampling). Door
//       anchors keep the real door Z. See exterior path in HookedCompassUpdate.
//     - Range gate (exterior, non-destination only): custom markers past
//       navMarkerRange ([NavigationOverlay] MarkerRange, the shared Glance/
//       Inspect knob) are not pushed to the compass, so 50-100 saved markers
//       don't clutter it. Horizontal distance only (marker Z is the map-plane
//       Z). Destination markers are exempt: a user-chosen destination always
//       shows regardless of distance.
//     - Count cap (exterior, non-destination only): navMaxCustomMarkers
//       ([NavigationOverlay] MaxCustomMarkers), the SAME knob as the Glance radar,
//       0 = uncapped. Keeps the N NEAREST by XY, resolved BEFORE the append loop
//       because nearest-N is a property of the whole set, not of one marker. The
//       compass is a ~180 degree strip, so it crowds the way the Glance disc does
//       and needs the same cap; Inspect draws every pin at its real spot on a full
//       cell and deliberately opts out (a_applyCountCap=false). Interior is exempt:
//       those markers match by cellID, so there is no distance to rank them by.
//   RE source: findings/findings-HUDCompass-CustomMarkers.md.
//   Dual-build for the hook itself (Compass::Update SE 50773 / AE 51668) and for
//   the compass buffer global (kCompassBufferID, SE 519611 / AE 406154).
//   SE COMPASS HISTORY (all fixed + confirmed in-game 2026-08-27). SE was broken by
//   THREE separate id/address bugs stacked on each other, each hidden by the one
//   above it:
//     1. Hook id. SE half was 51668 (the AE number), which on SE is StatsMenu::sub.
//        The detour installed and reported success against the wrong function, so
//        HookedCompassUpdate never ran at all. Now RelocationID(50773, 51668).
//     2. Data globals. kCompassCountRVA / kCompassLocationsTypeRVA were AE-only raw
//        RVAs, so on SE the type read 0 and the early-return fired every frame. Now
//        REL::Relocate pairs.
//     3. CNO BuildEntry owner. SE half was 51744, which on SE is FUN_1408cc9b0. The
//        0xE8 self-check saved it from crashing but silently fell through to the
//        direct vanilla builder, bypassing CNO -- markers drew with NO DISTANCE.
//        Now RelocationID(50870, 51744), and CNO distance works.
//   Bugs 1 and 3 came from the same bad method: findings-SE-Hook-IDs.md derived SE
//   ids by assuming id equality across runtimes. Both rows are now corrected there,
//   and the remaining "SE ID == AE ID" rows in that file are flagged unverified.
//   The two compass DATA globals (kCompassCountRVA, kCompassLocationsTypeRVA) are
//   now version-gated too, via REL::Relocate rather than RelocationID: neither SE
//   address carries an Address Library id. RVAs verified in Ghidra against five
//   distinct SE instructions. See findings-SE-Compass-Data-Globals.md.
// ============================================================================

namespace {
    // Compass::Update: SE 50773 / AE 51668.
    //
    // The SE half was 51668 until 2026-08-27 and that was the reason NOTHING we
    // append ever reached the SE compass. 51668 resolves cleanly on SE, so the
    // hook installed and logged success -- but on SE that id is StatsMenu::sub,
    // not Compass::Update. PolyHook happily detoured the wrong function.
    // findings-SE-Hook-IDs.md row 15 claims the ids are equal; it is wrong.
    // Do not "simplify" this back to one id because the two numbers look alike.
    constexpr REL::RelocationID kCompassUpdateID{ 50773, 51668 };

    // Compass HUD marker manager base. SE 519611 (DAT_142f4ca00 / 0x2f4ca00) /
    // AE 406154 (0x31AD490). The SE twin is CONFIRMED: line ~429 below already
    // resolves this exact AE address through RelocationID(519611, 406154) for the
    // CNO radius spoof, so the pair is proven by existing working code.
    //
    // NOTE the asymmetry: id 406154 is only meaningful as the AE HALF of that pair.
    // Resolving 406154 as an SE id yields UNWIND_INFO_141bab124 (unwind metadata),
    // which is why the two globals below cannot simply be wrapped the same way.
    constexpr REL::RelocationID kCompassBufferID{ 519611, 406154 };

    // Both globals sit in the same contiguous block as kCompassBufferID above, at
    // the SAME deltas on both runtimes: type at base-0x00A0, count at base+0x14BC.
    // RVAs verified in Ghidra against five distinct SE instructions
    // (findings-SE-Compass-Data-Globals.md):
    //   type  0x2f4c960 = the 5th dword of the 6-entry type-id table at 0x142f4c950,
    //                     written by the registration loop in Compass::Update where
    //                     entry 4 is the "CompassMarkerLocations" string.
    //   count 0x2f4debc = the +0x14BC field the SE builder at 0x140885fe0 caps at
    //                     0x30 and increments, read back at 0x140882482, compared in
    //                     the marker loop at 0x140882eda, and zeroed at 0x1400acbe4.
    //
    // Relocate, NOT RelocationID: neither SE address has an Address Library id, and
    // the AE ids (406159 / 406122) resolve on SE to unwind metadata
    // (UNWIND_INFO_141bab1e4 / 0x1baaf08). Wrapping them as an id pair would write
    // through metadata (ruled-out items 9 / 17 in CLAUDE.md).
    inline std::uintptr_t CompassCountRVA() {
        if (REL::Module::IsSE()) {
            return 0x2F4DEBC;
        }
        const auto& ver = REL::Module::get().version();
        if (ver.major() == 1 && ver.minor() == 6) {
            return 0x31AE94C;
        }
        return 0x3257C6C; // 1.7.104+
    }

    inline std::uintptr_t CompassLocationsTypeRVA() {
        if (REL::Module::IsSE()) {
            return 0x2F4C960;
        }
        const auto& ver = REL::Module::get().version();
        if (ver.major() == 1 && ver.minor() == 6) {
            return 0x31AD3F0;
        }
        return 0x32566A8; // 1.7.104+
    }

    constexpr std::uint32_t     kMaxCompassMarkers       = 48;

    std::uint64_t                              g_trampolineAddr = 0;
    std::unique_ptr<PLH::x64Detour>            g_detour;

    using CompassUpdateFn = void(*)(std::uintptr_t);

    struct RawGFxValue {
        void*         objectInterface;
        std::uint32_t type;
        std::uint32_t pad;
        double        numberValue;
    };
    static_assert(sizeof(RawGFxValue) == 0x18);

    void SetNumber(RawGFxValue& value, double number) {
        value.objectInterface = nullptr;
        value.type = 3;  // GFxValue::ValueType::kNumber
        value.pad = 0;
        value.numberValue = number;
    }

    RE::TESWorldSpace* PlayerWorldspace(RE::PlayerCharacter* player) {
        if (!player) return nullptr;
        return player->GetWorldspace();
    }

    // True when a marker stored in markerWorldID belongs to the same map as the
    // player's current worldspace.
    //
    // This USED to walk only UP from the player, which answers "is the marker at or
    // above me" -- and that is not the question. A marker stored in a CHILD
    // worldspace (Whiterun 0x1A26F, Solitude, any walled city) never matched while
    // the player stood in Tamriel, because the walk goes Tamriel -> null and never
    // visits the children. Setting the Whiterun or Dragonsreach map icon as a
    // destination therefore showed nothing on the compass OR the radar the moment
    // you stepped outside the city. Confirmed 2026-08-27 by a gated reject log:
    //   CompassReject: markerWs=0001A26F playerWs=0000003C
    //
    // Comparing ROOTS is the right test: two worldspaces share a map if they share
    // an ancestor, regardless of which one is the child. Skyrim's hierarchy is one
    // level deep in practice, but the walk handles nesting and mod-added worlds with
    // no ID table.
    RE::TESWorldSpace* RootWorld(RE::TESWorldSpace* a_world) {
        // Depth cap: parentWorld is data-driven and a broken mod record could in
        // principle make it cyclic. 8 is far beyond any real hierarchy.
        constexpr int kMaxHops = 8;
        auto* w = a_world;
        for (int hop = 0; w && w->parentWorld && hop < kMaxHops; ++hop) {
            w = w->parentWorld;
        }
        return w;
    }

    bool WorldMatches(RE::TESWorldSpace* current, RE::FormID markerWorldID) {
        if (!current) return false;
        // Fast path: exact match, or the marker is an ancestor of where we stand.
        for (auto* world = current; world; world = world->parentWorld) {
            if (world->GetFormID() == markerWorldID) return true;
        }
        // Shared-root test: covers the marker being in a SIBLING or CHILD world.
        auto* markerWorld = RE::TESForm::LookupByID<RE::TESWorldSpace>(markerWorldID);
        if (!markerWorld) return false;
        auto* markerRoot = RootWorld(markerWorld);
        auto* playerRoot = RootWorld(current);
        return markerRoot && playerRoot && markerRoot == playerRoot;
    }

    // Alias so the CompassHook:: forwarder below can reach the anonymous-namespace
    // definition without recursing into itself.
    constexpr auto g_worldMatches = &WorldMatches;

    // For an exterior destination marker shown while the player is inside an
    // interior, vanilla retargets the compass chevron at whichever load door
    // in the current cell leads to the destination's worldspace. We mirror
    // that here: iterate load doors in the player's cell, return the one
    // whose linked door sits in (or under) the marker's worldspace; fall back
    // to the first door reaching any exterior worldspace.
    // See findings/findings-CompassExteriorDestinationFromInterior.md.
    RE::TESObjectREFR* FindExitDoorForWorldspace(RE::TESObjectCELL* cell,
                                                 RE::FormID targetWorldspaceID) {
        if (!cell) {
            return nullptr;
        }

        RE::TESObjectREFR* foundDoor = nullptr;

        cell->ForEachReference([&](RE::TESObjectREFR* refPtr) {
            if (!refPtr) return RE::BSContainer::ForEachResult::kContinue;
            auto& ref = *refPtr;
            auto* extraTeleport = ref.extraList.GetByType<RE::ExtraTeleport>();
            if (extraTeleport && extraTeleport->teleportData) {
                auto linkedDoorRef = extraTeleport->teleportData->linkedDoor.get();
                if (linkedDoorRef) {
                    RE::TESWorldSpace* destWorld = linkedDoorRef->GetWorldspace();
                    if (destWorld) {
                        if (WorldMatches(destWorld, targetWorldspaceID)) {
                            foundDoor = &ref;
                            return RE::BSContainer::ForEachResult::kStop;
                        }
                    }
                }
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });

        return foundDoor;
    }

    using BuildEntryFn = bool(std::uintptr_t, std::uintptr_t,
                              RE::NiPoint3*, std::uint32_t*, std::uint32_t);

    BuildEntryFn* GetCNOLocationsBuildEntryFn() {
        static BuildEntryFn* fn = nullptr;
        if (!fn) {
            // CNO locations BuildEntry owner: the per-marker loop that calls the
            // compass marker builder once per location. CNO patches this call site,
            // so following the live CALL here is what makes us CNO-aware.
            //
            // SE 50870 (FUN_140886c60) / AE 51744 (0x926c40). The SE half was 51744
            // until 2026-08-27, copied from findings-SE-Hook-IDs.md row 16, which
            // derived its SE ids by assuming id equality across runtimes -- the same
            // mistake that made row 15 (Compass::Update) wrong and left the SE
            // compass dead. On SE, id 51744 is FUN_1408cc9b0 (0x8cc9b0), nowhere near
            // the compass region, so the 0xE8 probe below failed and every SE build
            // silently fell through to the direct vanilla builder, bypassing CNO.
            // Confirmed by the gated BuildEntry resolve probe: branch=fallback,
            // probeByte=0x48.
            //
            // Verified in Ghidra: FUN_140886c60 + 0x450 = 0x8870b0, bytes
            // E8 2B EF FF FF, displacement -0x10D5, target 0x885FE0 = the builder
            // that caps at 0x30 and increments the +0x14BC count. The VariantOffset
            // below was already correct; only the base id was wrong.
            std::uintptr_t base = REL::RelocationID(50870, 51744).address();
            std::uintptr_t callAddr = base + REL::VariantOffset(0x450, 0x473, 0x450).offset();
            // The 0xE8 check keeps this self-correcting: we only trust the call
            // site if the byte there really is a CALL. When CNO is installed the
            // target lands in ITS module, far outside SkyrimSE.exe -- that is the
            // intended outcome, not an error.
            if (*reinterpret_cast<std::uint8_t*>(callAddr) == 0xE8) {
                std::int32_t rel = *reinterpret_cast<std::int32_t*>(callAddr + 1);
                fn = reinterpret_cast<BuildEntryFn*>(callAddr + 5 + rel);
            } else {
                // Direct address of the compass marker builder, now version-gated.
                // SE 0x885fe0 is the function verified in Ghidra as the builder that
                // caps at 0x30 and increments the +0x14BC count
                // (findings-SE-Compass-Data-Globals.md section 2B); AE 0x925FC0 is
                // the long-standing raw fallback.
                //
                // Do NOT reach for an id pair here. AE id 51728 is a trap: on SE that
                // id is PerkSelectConfirmCallback::dtor, not this thunk.
                fn = reinterpret_cast<BuildEntryFn*>(
                    REL::Offset(REL::Relocate(0x885FE0, 0x925FC0)).address());
            }
        }
        return fn;
    }

    RE::MapMarkerData* CreateMapMarkerData() {
        auto* mem = malloc(sizeof(RE::MapMarkerData));
        memset(mem, 0, sizeof(RE::MapMarkerData));
        auto* data = reinterpret_cast<RE::MapMarkerData*>(mem);
        
        // Manually assign the TESFullName vtable to avoid linker errors with its unexported constructor
        *reinterpret_cast<std::uintptr_t*>(data) = REL::Relocation<std::uintptr_t>{ RE::VTABLE_TESFullName[0] }.address();
        
        // Safely placement-new the BSFixedString so operator= doesn't crash on uninitialized memory
        new (&data->locationName.fullName) RE::BSFixedString();
        
        return data;
    }

    struct HUDMarkerFrameOffsets {
        static HUDMarkerFrameOffsets* GetSingleton() {
            REL::Relocation<HUDMarkerFrameOffsets*> singleton{ REL::RelocationID(519587, 406118) };
            return singleton.get();
        }
        std::uint32_t quest;                 // 00
        std::uint32_t questDoor;             // 04
        std::uint32_t playerSet;             // 08
        std::uint32_t enemy;                 // 0C
        std::uint32_t location;              // 10
        std::uint32_t undiscoveredLocation;  // 14
    };

    RE::ExtraMapMarker* GetOrCreateExtraMapMarker(RE::ExtraDataList& extraList) {
        auto* mapMarker = extraList.GetByType<RE::ExtraMapMarker>();
        if (!mapMarker) {
            auto* mem = malloc(sizeof(RE::ExtraMapMarker));
            memset(mem, 0, sizeof(RE::ExtraMapMarker));
            mapMarker = reinterpret_cast<RE::ExtraMapMarker*>(mem);
            
            *reinterpret_cast<std::uintptr_t*>(mapMarker) = REL::Relocation<std::uintptr_t>{ RE::VTABLE_ExtraMapMarker[0] }.address();
            
            mapMarker->mapData = CreateMapMarkerData();
            extraList.Add(mapMarker);
        }
        return mapMarker;
    }

    static std::vector<RE::ObjectRefHandle> g_anchorHandles;

    // Exit-door cache for the interior path. FindExitDoorForWorldspace walks
    // every reference in the current cell, which is too expensive to run every
    // compass tick (it cost ~8 fps in dense interiors). Resolve the door once
    // per (cell, destination marker) and reuse the handle until the key
    // changes. Stored as a handle, never a raw pointer, so a stale door just
    // resolves to null on .get() instead of dangling.
    struct CompassDoorCache {
        RE::FormID          cellID       = 0;
        std::uint32_t       destMarkerId = 0;
        RE::FormID          destWorldID  = 0;
        RE::ObjectRefHandle door;        // invalid handle = no door resolved
    };
    static CompassDoorCache g_doorCache;

    void HookedCompassUpdate(std::uintptr_t compassThis) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            reinterpret_cast<CompassUpdateFn>(g_trampolineAddr)(compassThis);
            HUDMarkerHook::EndPass();  // publish quest markers accumulated during the pass
            return;
        }
        auto* world = PlayerWorldspace(player);

        // Resolve current cell. In interiors world is null and we instead
        // match markers by cellID. In exteriors we keep the worldspace match
        // (which already covers parent worldspaces via WorldMatches).
        RE::FormID currentCell = 0;
        if (auto* cell = player->GetParentCell()) {
            currentCell = cell->GetFormID();
        }
        const bool isInterior = (world == nullptr);

        const auto markers = MarkerManager::GetSingleton()->GetMarkers();
        if (markers.empty()) {
            reinterpret_cast<CompassUpdateFn>(g_trampolineAddr)(compassThis);
            HUDMarkerHook::EndPass();  // publish quest markers accumulated during the pass
            return;
        }

        // Check if our anchor refs were destroyed by a game reload.
        if (!g_anchorHandles.empty() && !g_anchorHandles[0].get()) {
            logger::info("Compass anchors invalid (likely game reload). Clearing handles.");
            g_anchorHandles.clear();
        }

        // Initialize the hidden anchor refs on the first compass update
        // or after they were cleared above.
        if (g_anchorHandles.empty()) {
            auto* xmarker = RE::TESForm::LookupByID<RE::TESBoundObject>(0x3B);
            if (xmarker) {
                for (std::uint32_t i = 0; i < kMaxCompassMarkers; ++i) {
                    if (auto anchorRef = player->PlaceObjectAtMe(xmarker, false)) {
                        anchorRef->Disable();
                        g_anchorHandles.push_back(anchorRef->GetHandle());
                    }
                }
            }
        }

        if (g_anchorHandles.empty()) {
            reinterpret_cast<CompassUpdateFn>(g_trampolineAddr)(compassThis);
            HUDMarkerHook::EndPass();  // publish quest markers accumulated during the pass
            return;
        }

        auto* markerTypePtr =
            reinterpret_cast<std::uint32_t*>(REL::Offset(CompassLocationsTypeRVA()).address());
        if (!markerTypePtr || *markerTypePtr == 0) {
            reinterpret_cast<CompassUpdateFn>(g_trampolineAddr)(compassThis);
            HUDMarkerHook::EndPass();  // publish quest markers accumulated during the pass
            return;
        }

        auto* count = reinterpret_cast<std::uint32_t*>(REL::Offset(CompassCountRVA()).address());
        auto buffer = REL::Relocation<std::uintptr_t>{ kCompassBufferID }.address();

        // Hoisted for the exterior range gate below. Squared to avoid a sqrt per
        // marker. navMarkerRange is the shared Glance/Inspect/compass range knob.
        const RE::NiPoint3 playerPos = player->GetPosition();
        const auto* settings = Settings::GetSingleton();
        const float navRange = settings->navMarkerRange;
        const float markerRangeSq = navRange * navRange;

        // COUNT cap, exterior only, layered on top of the range gate. Same knob and
        // meaning as the Glance radar (Settings::navMaxCustomMarkers, 0 = uncapped):
        // keep the N NEAREST by XY so the set stays stable as the player walks. An
        // unsorted "first N" would drop markers arbitrarily, since MarkerManager
        // hands them back in cosave order.
        //
        // Why the compass needs this and Inspect does not: the compass is a ~180
        // degree strip, so a dozen in-range pins collapse into an unreadable smear
        // the same way the Glance disc rim-clamps them into a pile. Inspect draws
        // every pin at its real spot on a full cell, so crowding never happens there
        // and MinimapRenderer deliberately passes a_applyCountCap=false for it.
        //
        // Interior is exempt: those markers are matched by cellID, not by range, so
        // there is no distance to rank them by and a room's worth of pins is already
        // bounded. Destinations are exempt everywhere and never consume a slot.
        //
        // Resolved BEFORE the loop because nearest-N is a property of the whole set,
        // not of one marker. Membership is then a lookup inside the loop.
        std::unordered_set<std::uint32_t> allowedByCap;
        const int maxCustom = settings->navMaxCustomMarkers;
        const bool capActive = !isInterior && maxCustom > 0;
        if (capActive) {
            std::vector<std::pair<float, std::uint32_t>> ranged;
            for (const auto& m : markers) {
                if (m.isDestination) continue;                       // exempt, no slot
                if (m.iconType >= 100) continue;                     // waypoints never reach the compass
                if (m.worldSpaceID == 0) continue;
                if (!WorldMatches(world, m.worldSpaceID)) continue;
                const float dx = m.position.x - playerPos.x;
                const float dy = m.position.y - playerPos.y;
                const float dSq = dx * dx + dy * dy;
                if (dSq > markerRangeSq) continue;                    // range gate first
                ranged.emplace_back(dSq, m.id);
            }
            if (ranged.size() > static_cast<std::size_t>(maxCustom)) {
                std::partial_sort(ranged.begin(), ranged.begin() + maxCustom, ranged.end(),
                                  [](const auto& a, const auto& b) { return a.first < b.first; });
                ranged.resize(static_cast<std::size_t>(maxCustom));
            }
            for (const auto& r : ranged) allowedByCap.insert(r.second);
        }

        std::uint32_t anchorIdx = 0;
        for (const auto& marker : markers) {
            if (*count >= kMaxCompassMarkers) break;
            if (anchorIdx >= g_anchorHandles.size()) break;

            // Waypoint glyph types (>= 100, MapDraw::IsWaypointType) are
            // interior-only vector icons with no hudmenu.swf frame; keep them
            // off the compass. Plain comparison instead of the helper so this
            // file stays ImGui-free. A waypoint set as destination still shows
            // (type remaps to kPlayerSet below).
            if (!marker.isDestination && marker.iconType >= 100) continue;

            RE::TESObjectREFR* exitDoor = nullptr;
            if (isInterior) {
                if (marker.worldSpaceID == 0) {
                    // Interior marker: match against the current cell.
                    if (currentCell == 0 || marker.cellID != currentCell) continue;
                } else if (marker.isDestination) {
                    // Exterior destination shown from interior: retarget to
                    // an exit door in the current cell that leads to the
                    // destination's worldspace. Mirrors vanilla's behavior.
                    // The cell scan is cached; only re-run when the cell or the
                    // destination marker changes (see g_doorCache).
                    const bool keyChanged =
                        g_doorCache.cellID != currentCell ||
                        g_doorCache.destMarkerId != marker.id ||
                        g_doorCache.destWorldID != marker.worldSpaceID;
                    if (keyChanged) {
                        g_doorCache.cellID       = currentCell;
                        g_doorCache.destMarkerId = marker.id;
                        g_doorCache.destWorldID  = marker.worldSpaceID;
                        g_doorCache.door         = {};
                        if (auto* d = FindExitDoorForWorldspace(player->GetParentCell(),
                                                                marker.worldSpaceID)) {
                            g_doorCache.door = d->GetHandle();
                        }
                    }
                    auto doorPtr = g_doorCache.door.get();
                    if (!doorPtr) {
                        continue;
                    }
                    exitDoor = doorPtr.get();
                } else {
                    // Non-destination exterior markers do not show on the
                    // interior compass.
                    continue;
                }
            } else {
                // Exterior path: match by worldspace (handles parent worlds).
                if (marker.worldSpaceID == 0) continue;
                if (!WorldMatches(world, marker.worldSpaceID)) continue;

                // Range gate: with 50-100 custom markers, showing every one on
                // the compass at all times is useless clutter. Hide markers past
                // navMarkerRange, same knob and meaning as the Glance/Inspect
                // range gate ([NavigationOverlay] MarkerRange).
                // A marker set as destination is EXEMPT: only one is active at a
                // time and it is the thing the player is navigating to, so it must
                // always show regardless of distance (matches vanilla). The
                // exterior branch handles destination and non-destination markers
                // alike, so the exemption must be checked here, not just on the
                // interior path above.
                // Horizontal distance only: marker.position.z is the map-plane Z
                // (~180000), not a real Z, so a 3D compare would be garbage.
                if (!marker.isDestination) {
                    const float dx = marker.position.x - playerPos.x;
                    const float dy = marker.position.y - playerPos.y;
                    if (dx * dx + dy * dy > markerRangeSq) continue;
                    // Count cap: allowedByCap holds the N nearest, resolved above.
                    // Only consulted when the cap is active, so 0 (uncapped) and
                    // the interior path both skip it entirely.
                    if (capActive && !allowedByCap.contains(marker.id)) continue;
                }
            }

            auto anchor = g_anchorHandles[anchorIdx].get();
            if (!anchor) continue;

            if (exitDoor) {
                anchor->data.location = exitDoor->data.location;
            } else {
                // marker.position.z is the WORLD-MAP mesh plane Z (~kMapPlaneZ
                // 180000), stored so the pin projects onto the map. It is NOT a
                // real-world Z. Compass Navigation Overhaul computes a full 3D
                // distance, so that bogus Z (180000 vs the player's ~few-thousand)
                // dominates and inflates the reading (a marker ~4000 units away
                // horizontally read as ~2700m). Substitute the player's Z so the
                // distance is effectively horizontal, matching what the compass
                // conveys and what quest markers (real ground Z) produce.
                anchor->data.location = { marker.position.x, marker.position.y,
                                          playerPos.z };
            }

            // Already the clean display name: the vanilla destination is a
            // synthesized entry whose note carries no internal prefix.
            const std::string resolved = ResolveMarkerName(marker);
            const char* label = resolved.c_str();

            auto* textData = anchor->extraList.GetByType<RE::ExtraTextDisplayData>();
            if (!textData) {
                textData = new RE::ExtraTextDisplayData();
                anchor->extraList.Add(textData);
            }
            textData->displayName = label;

            auto* mapMarker = GetOrCreateExtraMapMarker(anchor->extraList);
            mapMarker->mapData->locationName.fullName = label;
            mapMarker->mapData->flags.set(RE::MapMarkerData::Flag::kVisible);
            // Destination markers semantically map to vanilla's Player Set
            // Custom Marker type. Iconless markers (iconType 0) default to Landmark
            // (type 11, SkyUI frame 292) so they don't leak into the active quest frame.
            const auto effectiveType = marker.isDestination
                ? RE::MARKER_TYPE::kPlayerSet
                : (marker.iconType != 0 ? static_cast<RE::MARKER_TYPE>(marker.iconType) : RE::MARKER_TYPE::kLandmark);
            mapMarker->mapData->type = effectiveType;

            // Temporarily disable distance limits for CNO UpdateLocations
            // Same global as kCompassBufferID above (single source of truth for this address).
            auto* hmmAddr = reinterpret_cast<std::uint8_t*>(REL::Relocation<std::uintptr_t>{ kCompassBufferID }.address());
            float* sqRadius = reinterpret_cast<float*>(hmmAddr + 0x14B8);
            float oldRadius = *sqRadius;
            *sqRadius = 3.402823466e+38F;

            std::uint32_t frameToUse = *markerTypePtr;
            if (auto* offsets = HUDMarkerFrameOffsets::GetSingleton()) {
                if (marker.isDestination) {
                    // Use vanilla's "Player Set Custom Marker" icon frame so the
                    // destination reads as the chevron/destination icon on the
                    // compass, distinct from category-based location frames.
                    frameToUse = offsets->playerSet;
                } else if (marker.iconType != 0) {
                    // Calculate frame exactly how CNO/Vanilla does it for locations
                    frameToUse = offsets->location + marker.iconType - 1;
                } else {
                    // Default to Landmark (type 11, frame 292 in SkyUI hudmenu)
                    // when no icon is selected.
                    frameToUse = offsets->location + static_cast<std::uint32_t>(RE::MARKER_TYPE::kLandmark) - 1;
                }
            }

            std::uint32_t handle = g_anchorHandles[anchorIdx].native_handle();
            auto* buildEntry = GetCNOLocationsBuildEntryFn();

            // Arg 3 is outPosCache: the builder would cache the marker's world
            // position there for downstream distance work. We pass nullptr and
            // CNO still reports a correct distance, so it does not depend on it.
            buildEntry(buffer, buffer, nullptr, &handle, frameToUse);

            *sqRadius = oldRadius;

            ++anchorIdx;
        }

        // Now call the vanilla update, which will push our appended markers along with its own,
        // and importantly, call SetCompassAngle AFTER SetCompassMarkers so mods like CNO see them.
        reinterpret_cast<CompassUpdateFn>(g_trampolineAddr)(compassThis);
        HUDMarkerHook::EndPass();  // publish quest markers accumulated during the pass
    }
}

namespace CompassHook {
    // Public forwarder so MinimapRenderer shares the exact same test. The radar
    // used a bare `m.worldSpaceID == a_worldSpaceID`, which fails for every city
    // marker the same way the old walk-up-only form did here.
    bool WorldMatches(RE::TESWorldSpace* a_current, RE::FormID a_markerWorldID) {
        return g_worldMatches(a_current, a_markerWorldID);
    }

    void Install() {
        REL::Relocation<std::uintptr_t> target{ kCompassUpdateID };
        const std::uint64_t targetAddr = target.address();
        const std::uint64_t hookAddr   = reinterpret_cast<std::uint64_t>(&HookedCompassUpdate);

        g_detour = std::make_unique<PLH::x64Detour>(targetAddr, hookAddr, &g_trampolineAddr);

        if (!g_detour->hook()) {
            logger::error("CompassHook: PolyHook x64Detour::hook() returned false");
            g_detour.reset();
            return;
        }

        logger::info("CompassHook installed | target={:X} | trampoline={:X}",
                     targetAddr, g_trampolineAddr);
    }
}
