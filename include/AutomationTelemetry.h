#pragma once
#include <RE/Skyrim.h>
#include <imgui.h>
#include <cstdint>

// Read-only snapshots from the owning draw/input paths. No test-engine hooks or native actions.
namespace AutomationTelemetry {
    void RegisterProvider();
    void BeginFrame();
    void EndFrame();
    void MapMode(bool local);
    void Item(const char* kind, std::uint32_t id, const char* label,
              float x0, float y0, float x1, float y1, const RE::NiPoint3* target = nullptr);
    void Pan(RE::FormID quest, const RE::NiPoint3& requested, const RE::NiPoint3& target);
    void Radar(const RE::NiPoint3& player, float alpha, ImVec2 center, float radius, float scale, float rotation);
    void RadarMarker(std::uint32_t id, const RE::NiPoint3& position, ImVec2 projected, ImVec2 drawn);
}
