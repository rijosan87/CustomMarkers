#include "AutomationTelemetry.h"
#include "MarkerManager.h"
#include <SKSE/SKSE.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <format>
#include <chrono>
#include <mutex>

namespace AutomationTelemetry {
    using json = nlohmann::json;
    namespace {
        std::mutex mutex;
        json staged, published = {{"apiVersion", 1}, {"frame", 0}, {"items", json::array()}};
        json pan = nullptr;
        std::uint64_t frame = 0, panSerial = 0;
        std::atomic<std::int64_t> armedUntil{0};
        bool capturing = false; // render thread only
        std::int64_t ClockMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
        json Point(const RE::NiPoint3& p) { return {p.x, p.y, p.z}; }
        const char* Read(RE::FormID) {
            armedUntil.store(ClockMs() + 5000, std::memory_order_relaxed);
            thread_local std::string result;
            json data;
            { std::lock_guard lock(mutex); data = published; data["pan"] = pan; }
            data["markers"] = json::array();
            for (const auto& marker : MarkerManager::GetSingleton()->GetUserMarkers())
                data["markers"].push_back({{"id", marker.id}, {"worldSpaceId", marker.worldSpaceID},
                    {"cellId", marker.cellID}, {"position", Point(marker.position)}, {"destination", marker.isDestination}});
            data["map"] = nullptr;
            auto* ui = RE::UI::GetSingleton();
            auto menu = ui ? ui->GetMenu<RE::MapMenu>() : nullptr;
            if (menu && ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
                auto& camera = menu->GetRuntimeData2()->camera;
                // Same state slots used by the existing glide busy guard. CommonLib exposes the typed world states.
                if (camera.unk60 < 2 && camera.unk68[camera.unk60]) {
                    auto* world = camera.unk68[camera.unk60].get();
                    const auto p = world->currentPosition + world->currentPositionScrollOffset;
                    const auto base = reinterpret_cast<std::uintptr_t>(&camera);
                    const auto active = *reinterpret_cast<std::uintptr_t*>(base + 0x28);
                    data["map"] = {{"position", Point(p)}, {"busy", active != reinterpret_cast<std::uintptr_t>(world)},
                        {"worldSpaceId", camera.worldSpace ? camera.worldSpace->GetFormID() : 0}};
                }
            }
            result = data.dump(); return result.c_str();
        }
    }
    void RegisterProvider() {
        struct Registration { const char* name; const char* (*callback)(RE::FormID); };
        Registration registration{"HUD:CustomMarkers", Read};
        if (auto* messaging = SKSE::GetMessagingInterface())
            logger::info("Automation provider registration: {}", messaging->Dispatch(19847, &registration, sizeof(registration), "LiveIntrospectionBridge"));
    }
    void BeginFrame() {
        // Idle gameplay pays one atomic/time check; JSON work runs only during bridge inspection.
        capturing = ClockMs() < armedUntil.load(std::memory_order_relaxed);
        if (!capturing) return;
        staged = {{"apiVersion", 1}, {"items", json::array()}, {"radar", nullptr}, {"localMap", nullptr}, {"dropped", 0}};
        const auto& io = ImGui::GetIO();
        staged["display"] = {io.DisplaySize.x, io.DisplaySize.y};
        staged["cursor"] = {io.MousePos.x, io.MousePos.y};
    }
    void MapMode(bool local) { if (!capturing) return; staged["localMap"] = local; }
    void EndFrame() {
        if (!capturing) return;
        staged["frame"] = ++frame;
        staged["sampledAtUnixMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::lock_guard lock(mutex); published = std::move(staged);
    }
    void Item(const char* kind, std::uint32_t id, const char* label, float x0, float y0, float x1, float y1, const RE::NiPoint3* target) {
        if (!capturing) return;
        if (staged["items"].size() >= 1024) { staged["dropped"] = staged["dropped"].get<int>() + 1; return; }
        if (x1 <= x0 || y1 <= y0) return;
        json item = {{"id", std::format("{}:{}", kind, id)}, {"kind", kind}, {"objectId", id}, {"label", label},
            {"window", "CustomMarkers.Map"}, {"rect", {x0,y0,x1,y1}}, {"visible", true}};
        if (target) item["target"] = Point(*target);
        staged["items"].push_back(std::move(item));
    }
    void Pan(RE::FormID quest, const RE::NiPoint3& requested, const RE::NiPoint3& target) {
        std::lock_guard lock(mutex); pan = {{"serial", ++panSerial}, {"questId", quest}, {"requested", Point(requested)}, {"target", Point(target)}};
    }
    void Radar(const RE::NiPoint3& player, float alpha, ImVec2 center, float radius, float scale, float rotation) {
        if (!capturing) return;
        staged["radar"] = {{"player", Point(player)}, {"alpha", alpha}, {"center", {center.x,center.y}},
            {"radius", radius}, {"scale", scale}, {"rotation", rotation}, {"markers", json::array()}};
    }
    void RadarMarker(std::uint32_t id, const RE::NiPoint3& position, ImVec2 projected, ImVec2 drawn) {
        if (!capturing) return;
        if (staged["radar"].is_null() || staged["radar"]["markers"].size() >= 1024) return;
        staged["radar"]["markers"].push_back({{"id", id}, {"position", Point(position)},
            {"projected", {projected.x,projected.y}}, {"drawn", {drawn.x,drawn.y}}});
    }
}
