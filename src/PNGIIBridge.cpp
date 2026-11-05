#include "PCH.h"
#include "PNGIIBridge.h"
#include "PNGII_API.h"

#include <chrono>
#include <cstddef>
#include <Windows.h>

namespace PNGIIBridge {
    namespace {
        constexpr std::uint32_t kTypeSlots = 62;
        constexpr std::uint16_t kDoorType = 61;
        constexpr std::uint16_t kLastLocationType = 59;

        const PNGII::API::InterfaceV2* g_api = nullptr;
        bool g_canRead = false;
        std::string g_paths[2][kTypeSlots];
        std::atomic_bool g_ready = false;
        std::atomic_bool g_fillPending = false;

        enum class Fill { kDone, kNotReady };

        void ToDataPath(const char* a_path, std::string& a_out) {
            a_out = std::string("Data\\") + a_path;
            std::ranges::replace(a_out, '/', '\\');
        }

        Fill Query(std::uint16_t a_type, bool a_local, bool a_door, std::string& a_out) {
            PNGII::API::MapMarkerQuery q{};
            q.structSize = sizeof(q);
            q.markerType = a_type;
            q.isLocal = a_local ? 1 : 0;
            q.isDoor = a_door ? 1 : 0;
            char buf[260] = {};
            std::uint32_t need = 0;
            const auto r = g_api->resolveMapMarkerIcon(&q, buf, sizeof(buf), &need);
            if (r == PNGII::API::Result::kNotReady) return Fill::kNotReady;
            if (r == PNGII::API::Result::kSuccess && buf[0]) ToDataPath(buf, a_out);
            return Fill::kDone;
        }

        void FillTask() {
            const auto t0 = std::chrono::steady_clock::now();
            int counts[2] = {};
            for (std::uint16_t type = 1; type <= kLastLocationType; ++type) {
                for (int local = 0; local < 2; ++local) {
                    if (Query(type, local != 0, false, g_paths[local][type]) == Fill::kNotReady) {
                        logger::warn("PNGII map icons: API not ready, using CMM art until the next load");
                        g_fillPending.store(false);
                        return;
                    }
                    if (!g_paths[local][type].empty()) ++counts[local];
                }
            }
            Query(kDoorType, true, true, g_paths[1][kDoorType]);
            g_paths[0][kDoorType] = g_paths[1][kDoorType];
            g_ready.store(true, std::memory_order_release);
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();
            logger::info("PNGII map icons: {} world, {} local, door {}, {} us", counts[0], counts[1],
                g_paths[1][kDoorType].empty() ? "no" : "yes", us);
        }
    }

    void Init() {
        HMODULE dll = GetModuleHandleA("PNGIconInjector.dll");
        auto request = dll ? reinterpret_cast<PNGII::API::RequestAPIFunction>(
            GetProcAddress(dll, PNGII::API::kExportName)) : nullptr;
        auto* api = request ? static_cast<const PNGII::API::InterfaceV2*>(request(PNGII::API::kVersion2)) : nullptr;
        const auto needed = offsetof(PNGII::API::InterfaceV2, resolveMapMarkerIcon) +
                            sizeof(PNGII::API::ResolveMapMarkerIconFunction);
        if (api && api->version == PNGII::API::kVersion2 && api->structSize >= needed && api->resolveMapMarkerIcon) {
            g_api = api;
            const auto readNeeded = offsetof(PNGII::API::InterfaceV2, readIconFile) +
                                    sizeof(PNGII::API::ReadIconFileFunction);
            g_canRead = api->structSize >= readNeeded && api->readIconFile;
        }
        logger::info("PNGII map icons: {}, BSA reads {}", g_api ? "V2 found" : "not available, using CMM art",
            g_canRead ? "yes" : "no");
    }

    void RequestFill() {
        auto* tasks = SKSE::GetTaskInterface();
        if (!g_api || !tasks || g_ready.load(std::memory_order_acquire) || g_fillPending.exchange(true)) return;
        tasks->AddUITask([]() { FillTask(); });
    }

    const std::string* MapIconPath(std::uint32_t a_iconType, bool a_local) {
        if (a_iconType >= kTypeSlots || !g_ready.load(std::memory_order_acquire)) return nullptr;
        const auto& path = g_paths[a_local ? 1 : 0][a_iconType];
        return path.empty() ? nullptr : &path;
    }

    bool CanReadFiles() {
        return g_canRead;
    }

    bool ReadPackFile(const std::string& a_dataPath, std::string& a_out) {
        a_out.clear();
        if (!g_canRead) return false;
        constexpr std::string_view kPrefix = "Data\\";
        const std::string path = a_dataPath.starts_with(kPrefix) ? a_dataPath.substr(kPrefix.size()) : a_dataPath;
        std::uint32_t need = 0;
        auto r = g_api->readIconFile(path.c_str(), nullptr, 0, &need);
        if (r == PNGII::API::Result::kBufferTooSmall && need > 0) {
            a_out.resize(need);
            r = g_api->readIconFile(path.c_str(), a_out.data(), need, &need);
        }
        if (r == PNGII::API::Result::kSuccess) {
            a_out.resize(need);
            return true;
        }
        a_out.clear();
        if (r != PNGII::API::Result::kNoMatch) {
            logger::warn("PNGII map icons: read failed for '{}', result {}", path, static_cast<std::uint32_t>(r));
        }
        return false;
    }

    bool ResolveRefIcon(std::uint32_t a_formID, std::uint32_t a_locationID, std::uint16_t a_type,
                        bool a_undiscovered, std::string& a_out) {
        a_out.clear();
        if (!g_api) return true;
        PNGII::API::MapMarkerQuery q{};
        q.structSize = sizeof(q);
        q.formID = a_formID;
        q.locationFormID = a_locationID;
        q.markerType = a_type;
        q.undiscovered = a_undiscovered ? 1 : 0;
        char buf[260] = {};
        std::uint32_t need = 0;
        const auto r = g_api->resolveMapMarkerIcon(&q, buf, sizeof(buf), &need);
        if (r == PNGII::API::Result::kNotReady) return false;
        if (r == PNGII::API::Result::kSuccess && buf[0]) ToDataPath(buf, a_out);
        return true;
    }
}