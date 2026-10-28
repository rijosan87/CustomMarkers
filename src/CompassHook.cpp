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

namespace {
    constexpr REL::RelocationID kCompassUpdateID{ 50773, 51668 };

    constexpr REL::RelocationID kCompassBufferID{ 519611, 406154 };

    inline std::uintptr_t CompassCountAddress() {
        if (REL::Module::IsSE()) {
            return REL::Offset(0x2F4DEBC).address();
        }
        return REL::Relocation<std::uintptr_t>{ REL::ID(406159) }.address();
    }

    inline std::uintptr_t CompassLocationsTypeAddress() {
        if (REL::Module::IsSE()) {
            return REL::Offset(0x2F4C960).address();
        }
        return REL::Relocation<std::uintptr_t>{ REL::ID(406122) }.address();
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
            std::uintptr_t base = REL::RelocationID(50870, 51744).address();
            std::uintptr_t callAddr = base + REL::VariantOffset(0x450, 0x473, 0x450).offset();
            if (*reinterpret_cast<std::uint8_t*>(callAddr) == 0xE8) {
                std::int32_t rel = *reinterpret_cast<std::int32_t*>(callAddr + 1);
                fn = reinterpret_cast<BuildEntryFn*>(callAddr + 5 + rel);
            } else {
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
            reinterpret_cast<std::uint32_t*>(CompassLocationsTypeAddress());
        if (!markerTypePtr || *markerTypePtr == 0) {
            reinterpret_cast<CompassUpdateFn>(g_trampolineAddr)(compassThis);
            HUDMarkerHook::EndPass();  // publish quest markers accumulated during the pass
            return;
        }

        auto* count = reinterpret_cast<std::uint32_t*>(CompassCountAddress());
        auto buffer = REL::Relocation<std::uintptr_t>{ kCompassBufferID }.address();

        // Hoisted for the exterior range gate below. Squared to avoid a sqrt per
        // marker. navMarkerRange is the shared Glance/Inspect/compass range knob.
        const RE::NiPoint3 playerPos = player->GetPosition();
        const auto* settings = Settings::GetSingleton();
        const float navRange = settings->navMarkerRange;
        const float markerRangeSq = navRange * navRange;

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

            if (!marker.isDestination && marker.iconType >= 100) continue;

            RE::TESObjectREFR* exitDoor = nullptr;
            if (isInterior) {
                if (marker.worldSpaceID == 0) {
                    // Interior marker: match against the current cell.
                    if (currentCell == 0 || marker.cellID != currentCell) continue;
                } else if (marker.isDestination) {
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

                if (!marker.isDestination) {
                    const float dx = marker.position.x - playerPos.x;
                    const float dy = marker.position.y - playerPos.y;
                    if (dx * dx + dy * dy > markerRangeSq) continue;
                    if (capActive && !allowedByCap.contains(marker.id)) continue;
                }
            }

            auto anchor = g_anchorHandles[anchorIdx].get();
            if (!anchor) continue;

            if (exitDoor) {
                anchor->data.location = exitDoor->data.location;
            } else {
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

            buildEntry(buffer, buffer, nullptr, &handle, frameToUse);

            *sqRadius = oldRadius;

            ++anchorIdx;
        }

        reinterpret_cast<CompassUpdateFn>(g_trampolineAddr)(compassThis);
        HUDMarkerHook::EndPass();  // publish quest markers accumulated during the pass
    }
}

namespace CompassHook {
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
        if (Settings::GetSingleton()->hudMarkerLog) {
            logger::info("CompassHook globals | buffer={:X} locationsType={:X} count={:X}",
                         REL::Relocation<std::uintptr_t>{ kCompassBufferID }.address(),
                         CompassLocationsTypeAddress(), CompassCountAddress());
        }
    }
}
