#include "MinimapEntityTransition.h"
#include "MapDrawHelpers.h"
#include "Settings.h"

#include <SKSE/SKSE.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>

namespace MinimapEntityTransition {
namespace {
    using Clock = std::chrono::steady_clock;
    using Marker = EntityMarkerSource::EntityMarker;
    constexpr float kFadeInSeconds = 0.15f;
    constexpr float kFadeOutSeconds = 0.20f;
    constexpr std::size_t kMaxVisuals = EntityMarkerSource::kMaxEntities * 2;
    struct State {
        Marker marker{};
        float alpha = 0.0f;
        bool present = false;
        bool leaving = false;
    };
    std::unordered_map<RE::FormID, State> g_states;
    std::vector<VisualEntity> g_visible;
    int g_lastFrame = -1;
    bool g_exterior = false;
    RE::FormID g_space = 0;
    Clock::time_point g_tick{}, g_lastLog{};
    std::size_t g_added = 0, g_leaving = 0, g_finished = 0, g_evicted = 0;

    float Smooth(float value) { return value * value * (3.0f - 2.0f * value); }

    void Clear() {
        if (!g_states.empty() && Settings::GetSingleton()->minimapSnapshotLog)
            SKSE::log::info("MinimapEntityFade: reset cached={}", g_states.size());
        g_states.clear();
        g_tick = {};
    }
}

const std::vector<VisualEntity>& Update(const MinimapRenderer::SnapshotProjection& snapshot,
                                      const MinimapRenderer::NavigationFrame& frame) {
    g_visible.clear();
    auto* settings = Settings::GetSingleton();
    const int drawFrame = ImGui::GetFrameCount();
    const RE::FormID space = frame.isExterior ? frame.worldSpaceID : frame.cellID;
    if (g_lastFrame != drawFrame - 1 || g_space != space || g_exterior != frame.isExterior) Clear();
    g_lastFrame = drawFrame;
    g_space = space;
    g_exterior = frame.isExterior;
    const bool sameSpace = snapshot.isExterior == frame.isExterior &&
        (frame.isExterior ? snapshot.worldSpaceID == frame.worldSpaceID : snapshot.cellID == frame.cellID);
    if (!snapshot.valid || !sameSpace || !settings->GlanceAnyEntity()) {
        Clear();
        return g_visible;
    }

    const auto now = Clock::now();
    const float dt = g_tick == Clock::time_point{} ? 0.0f :
        std::max(0.0f, std::chrono::duration<float>(now - g_tick).count());
    g_tick = now;
    for (auto& [id, state] : g_states) state.present = false;

    // Mark surviving IDs first so a full cache only evicts departed tails.
    for (const auto& marker : snapshot.entities) {
        const bool allowed = marker.discovered && settings->GlanceSubEnabled(marker.category, marker.subCategory);
        if (!allowed || marker.category == EntityMarkerSource::Category::Door) {
            g_states.erase(marker.formID);
        } else if (auto it = g_states.find(marker.formID); it != g_states.end()) {
            it->second.present = true;
        }
    }
    for (const auto& marker : snapshot.entities) {
        if (!marker.discovered || !settings->GlanceSubEnabled(marker.category, marker.subCategory)) continue;
        if (marker.category == EntityMarkerSource::Category::Door) {
            g_visible.push_back({marker, 1.0f});
            continue;
        }
        auto it = g_states.find(marker.formID);
        if (it == g_states.end()) {
            if (g_states.size() >= kMaxVisuals) {
                auto victim = g_states.end();
                for (auto candidate = g_states.begin(); candidate != g_states.end(); ++candidate) {
                    if (!candidate->second.present &&
                        (victim == g_states.end() || candidate->second.alpha < victim->second.alpha)) victim = candidate;
                }
                if (victim != g_states.end()) { g_states.erase(victim); ++g_evicted; }
                else { g_visible.push_back({marker, 1.0f}); continue; }
            }
            it = g_states.emplace(marker.formID, State{}).first;
            ++g_added;
        }
        auto& state = it->second;
        if (state.marker.category != marker.category || state.marker.subCategory != marker.subCategory) state.alpha = 0.0f;
        state.marker = marker;
        state.present = true;
        state.leaving = false;
        state.alpha = std::min(1.0f, state.alpha + dt / kFadeInSeconds);
        g_visible.push_back({marker, Smooth(state.alpha)});
    }
    for (auto it = g_states.begin(); it != g_states.end();) {
        auto& state = it->second;
        if (state.present) { ++it; continue; }
        // Explicit filter changes take effect immediately, even for fading tails.
        if (!settings->GlanceSubEnabled(state.marker.category, state.marker.subCategory)) {
            it = g_states.erase(it);
            continue;
        }
        if (!state.leaving) { state.leaving = true; ++g_leaving; }
        state.alpha = std::max(0.0f, state.alpha - dt / kFadeOutSeconds);
        if (state.alpha <= 0.0f) { it = g_states.erase(it); ++g_finished; continue; }
        g_visible.push_back({state.marker, Smooth(state.alpha)});
        ++it;
    }
    if (settings->minimapSnapshotLog && now - g_lastLog >= std::chrono::seconds(5)) {
        SKSE::log::info("MinimapEntityFade: snapshot={} cached={} drawnCandidates={} added={} leaving={} finished={} evicted={} inMs=150 outMs=200",
            snapshot.entities.size(), g_states.size(), g_visible.size(), g_added, g_leaving, g_finished, g_evicted);
        g_added = g_leaving = g_finished = g_evicted = 0;
        g_lastLog = now;
    }
    return g_visible;
}

float EdgeAlpha(const MapDraw::RoundedRectBoundary& boundary, const ImVec2& point,
                float extent, float fadeWidth) {
    // Signed distance to the same inset rounded boundary used by Contains.
    // Reach zero before hard clipping, without clamping or sliding the icon.
    const float half = std::max(0.0f, boundary.halfSize - extent);
    const float round = std::clamp(boundary.rounding - extent, 0.0f, half);
    const float x = std::abs(point.x - boundary.center.x) - (half - round);
    const float y = std::abs(point.y - boundary.center.y) - (half - round);
    const float distance = std::hypot(std::max(x, 0.0f), std::max(y, 0.0f)) +
        std::min(std::max(x, y), 0.0f) - round;
    const float alpha = std::clamp(-distance / std::max(fadeWidth, 1e-4f), 0.0f, 1.0f);
    return Smooth(alpha);
}
}
