#include "PCH.h"
#include "RimMarkerSource.h"
#include "CompassHook.h"
#include "MarkerManager.h"
#include "PNGIIBridge.h"
#include "Settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <utility>

namespace RimMarkerSource {
    namespace {
        constexpr float kCollectSeconds = 1.0f;
        constexpr std::uint16_t kLastLocationType = 59;
        constexpr std::uint16_t kLastEngineType = 66;

        static_assert(sizeof(Settings::navRimMode) / sizeof(bool) == static_cast<std::size_t>(MarkerFilterHook::FocusMode::kCount),
                      "navRimMode must stay in sync with FocusMode");

        struct CatalogEntry {
            RE::FormID formID = 0;
            RE::NiPoint3 rootPos;
            std::uint16_t type = 0;
            MarkerFilterHook::FocusMode mode{};
            RE::FormID locationID = 0;
            std::string path[2];
            bool resolved[2] = {};
        };

        std::vector<CatalogEntry> g_catalog;
        RE::TESWorldSpace* g_catalogRoot = nullptr;
        int g_newResolves = 0;

        std::atomic_bool g_catalogDirty = true;
        std::atomic_bool g_taskPending = false;
        std::mutex g_mutex;
        std::shared_ptr<const Snapshot> g_published;

        RE::NiPoint2 RootOffset(RE::TESWorldSpace* a_world) {
            constexpr int kMaxHops = 8;
            RE::NiPoint2 off{ 0.0f, 0.0f };
            auto* w = a_world;
            for (int hop = 0; w && w->parentWorld && hop < kMaxHops; ++hop) {
                off.x += w->worldMapOffsetData.mapOffsetX;
                off.y += w->worldMapOffsetData.mapOffsetY;
                w = w->parentWorld;
            }
            return off;
        }

        void BuildCatalog(RE::TESWorldSpace* a_root) {
            const auto t0 = std::chrono::steady_clock::now();
            g_catalog.clear();
            g_catalogRoot = a_root;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return;

            int worlds = 0;
            for (auto* world : dh->GetFormArray<RE::TESWorldSpace>()) {
                if (!world || !world->persistentCell || CompassHook::RootWorld(world) != a_root) continue;
                ++worlds;
                const auto off = RootOffset(world);
                world->persistentCell->ForEachReference([&](RE::TESObjectREFR* a_ref) {
                    if (!a_ref) return RE::BSContainer::ForEachResult::kContinue;
                    const auto* extra = a_ref->extraList.GetByType<RE::ExtraMapMarker>();
                    if (!extra || !extra->mapData) return RE::BSContainer::ForEachResult::kContinue;
                    const std::uint16_t type = extra->mapData->type.underlying();
                    const bool location = type >= 1 && (type <= kLastLocationType || type > kLastEngineType);
                    if (!location) return RE::BSContainer::ForEachResult::kContinue;
                    const auto mode = MarkerFilterHook::ModeForRef(a_ref, type);
                    if (mode == MarkerFilterHook::FocusMode::kCount) return RE::BSContainer::ForEachResult::kContinue;

                    CatalogEntry e;
                    e.formID = a_ref->GetFormID();
                    e.rootPos = a_ref->GetPosition();
                    e.rootPos.x += off.x;
                    e.rootPos.y += off.y;
                    e.type = type;
                    e.mode = mode;
                    if (auto* loc = a_ref->GetEditorLocation()) e.locationID = loc->GetFormID();
                    g_catalog.push_back(std::move(e));
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            logger::info("RimMarkers: catalog root={:08X} worlds={} markers={} {} us, engine compass radius {:.0f}",
                         a_root->GetFormID(), worlds, g_catalog.size(), us,
                         std::sqrt(CompassHook::SquaredLocationRadius()));
        }

        const std::string& ResolvedPath(CatalogEntry& a_entry, bool a_undiscovered) {
            const int i = a_undiscovered ? 1 : 0;
            if (!a_entry.resolved[i]) {
                a_entry.resolved[i] = PNGIIBridge::ResolveRefIcon(a_entry.formID, a_entry.locationID, a_entry.type,
                                                                  a_undiscovered, a_entry.path[i]);
                ++g_newResolves;
            }
            return a_entry.path[i];
        }

        void Collect() {
            auto* ui = RE::UI::GetSingleton();
            if (ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return;

            auto* player = RE::PlayerCharacter::GetSingleton();
            auto* world = player ? player->GetWorldspace() : nullptr;
            auto snapshot = std::make_shared<Snapshot>();
            if (!world) {
                std::lock_guard lock(g_mutex);
                g_published = std::move(snapshot);
                return;
            }

            const auto t0 = std::chrono::steady_clock::now();
            auto* root = CompassHook::RootWorld(world);
            if (g_catalogDirty.exchange(false) || root != g_catalogRoot) BuildCatalog(root);

            const auto* settings = Settings::GetSingleton();
            const auto dest = MarkerManager::GetSingleton()->GetVanillaDestination();
            const auto playerOff = RootOffset(world);
            RE::NiPoint3 me = player->GetPosition();
            me.x += playerOff.x;
            me.y += playerOff.y;
            const float rangeSq = settings->navRimMarkerRange * settings->navRimMarkerRange;

            std::vector<std::pair<float, CatalogEntry*>> ranged;
            for (auto& e : g_catalog) {
                if (!settings->navRimMode[static_cast<std::size_t>(e.mode)]) continue;
                const float dx = e.rootPos.x - me.x;
                const float dy = e.rootPos.y - me.y;
                const float dSq = dx * dx + dy * dy;
                if (dSq <= rangeSq) ranged.emplace_back(dSq, &e);
            }
            std::ranges::sort(ranged, {}, &std::pair<float, CatalogEntry*>::first);

            g_newResolves = 0;
            snapshot->worldSpaceID = world->GetFormID();
            for (const auto& [dSq, e] : ranged) {
                if (snapshot->entries.size() >= static_cast<std::size_t>(settings->navMaxRimMarkers)) break;
                if (dest.active && dest.sourceFormID == e->formID) continue;
                auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(e->formID);
                if (!ref || ref->IsDisabled() || ref->IsDeleted()) continue;
                const auto* extra = ref->extraList.GetByType<RE::ExtraMapMarker>();
                if (!extra || !extra->mapData) continue;
                const bool undiscovered = !extra->mapData->flags.all(RE::MapMarkerData::Flag::kCanTravelTo);
                if (undiscovered && !settings->navRimShowUndiscovered) continue;

                Entry out;
                out.position = e->rootPos;
                out.position.x -= playerOff.x;
                out.position.y -= playerOff.y;
                out.type = e->type;
                out.mode = e->mode;
                out.undiscovered = undiscovered;
                out.packPath = ResolvedPath(*e, undiscovered);
                out.dimmed = undiscovered && out.packPath.empty();
                out.formID = e->formID;
                if (const char* name = extra->mapData->locationName.GetFullName()) out.name = name;
                if (settings->rimMarkerLog) {
                    out.logLabel = std::format("'{}' form={:08X} type={} bucket={} undiscovered={} comap='{}'",
                                               extra->mapData->locationName.GetFullName(), e->formID, e->type,
                                               MarkerFilterHook::GetModeName(e->mode), undiscovered,
                                               MarkerFilterHook::GetCoMAPIconName(ref));
                }
                snapshot->entries.push_back(std::move(out));
            }

            if (settings->rimMarkerLog) {
                static std::chrono::steady_clock::time_point lastLog{};
                const auto now = std::chrono::steady_clock::now();
                if (now - lastLog >= std::chrono::seconds(5)) {
                    lastLog = now;
                    logger::info("RimMarkers: inRange={} published={} newResolves={} {} us", ranged.size(),
                                 snapshot->entries.size(), g_newResolves,
                                 std::chrono::duration_cast<std::chrono::microseconds>(now - t0).count());
                }
            }

            std::lock_guard lock(g_mutex);
            g_published = std::move(snapshot);
        }
    }

    void RequestUpdateIfDue() {
        static std::chrono::steady_clock::time_point last{};
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<float>(now - last).count() < kCollectSeconds) return;
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks || g_taskPending.exchange(true)) return;
        last = now;
        tasks->AddUITask([]() {
            Collect();
            g_taskPending.store(false);
        });
    }

    std::shared_ptr<const Snapshot> Get() {
        std::lock_guard lock(g_mutex);
        return g_published;
    }

    void Invalidate() {
        g_catalogDirty.store(true);
    }
}