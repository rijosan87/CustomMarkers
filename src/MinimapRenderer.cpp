#include "AutomationTelemetry.h"
// Renders HUD glance mini-radar and full-screen dungeon inspect overlay.

#include "MinimapRenderer.h"
#include "MinimapSnapshotTransition.h"
#include "MinimapEntityTransition.h"
#include "SettingsMenu.h"
#include "PathTracker.h"
#include "MarkerManager.h"
#include "Settings.h"
#include "InputHandler.h"
#include "LocalizationManager.h"
#include "MenuUtils.h"
#include "BeastForm.h"
#include "MapTheme.h"
#include "MapDrawHelpers.h"
#include "TextureManager.h"
#include "RenderTargetUpscaler.h"
#include "HUDMarkerHook.h"
#include "CompassHook.h"   // WorldMatches() -- shared with the compass
#include "GlanceBounds.h"
#include "PartySheetState.h"
#include "FontManager.h"
#include "UIScale.h"
#include "RE/B/BSTreeManager.h"
#include "RE/N/NiSwitchNode.h"

#include <imgui.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <unordered_set>
#include <vector>


#include <SKSE/SKSE.h>

namespace {
    struct QuestDrawCandidate {
        const HUDMarkerHook::QuestMarker* marker;
        ImVec2 screenPos;
        float distanceSq;
        std::size_t order;
        bool inView;

        bool operator<(const QuestDrawCandidate& a_other) const {
            if (inView != a_other.inView) return inView;
            if (distanceSq != a_other.distanceSq) return distanceSq < a_other.distanceSq;
            return order < a_other.order;
        }
    };

    constexpr float kInspectMoveRefreshDist = 600.0f;
    constexpr float kInspectRefreshSeconds  = 2.0f;
    constexpr long long kSpaceSettleMs = 5000;
    constexpr float kSpaceSettleRetrySeconds = 0.5f;
    constexpr long long kSpaceFadeMaxHoldMs = 5000;

    std::vector<const MapMarker*> CollectVisibleMarkers(
        const std::vector<MapMarker>& a_markers, bool a_isExterior,
        RE::FormID a_worldSpaceID, RE::FormID a_cellID, const RE::NiPoint3& a_playerPos,
        bool a_applyCountCap)
    {
        auto* settings = Settings::GetSingleton();
        const float rangeSq = settings->navMarkerRange * settings->navMarkerRange;
        const int   maxCustom = a_applyCountCap ? settings->navMaxCustomMarkers : 0;

        // The frame carries only a worldspace FormID; WorldMatches needs the form to
        // walk parents. Resolved once here rather than per marker.
        auto* playerWorld = a_isExterior
            ? RE::TESForm::LookupByID<RE::TESWorldSpace>(a_worldSpaceID)
            : nullptr;

        std::vector<const MapMarker*> out;
        std::vector<std::pair<float, const MapMarker*>> ranged;  // capped candidates
        for (const auto& m : a_markers) {
            const bool sameSpace =
                a_isExterior ? CompassHook::WorldMatches(playerWorld, m.worldSpaceID)
                             : (m.worldSpaceID == 0 && m.cellID == a_cellID);
            if (!sameSpace) continue;
            if (m.isDestination) {
                out.push_back(&m);
                continue;
            }
            const float dx = m.position.x - a_playerPos.x;
            const float dy = m.position.y - a_playerPos.y;
            const float dSq = dx * dx + dy * dy;
            if (dSq > rangeSq) continue;
            ranged.emplace_back(dSq, &m);
        }
        if (maxCustom > 0 && ranged.size() > static_cast<std::size_t>(maxCustom)) {
            std::partial_sort(ranged.begin(), ranged.begin() + maxCustom, ranged.end(),
                              [](const auto& a, const auto& b) { return a.first < b.first; });
            ranged.resize(static_cast<std::size_t>(maxCustom));
        }
        for (const auto& r : ranged) out.push_back(r.second);
        return out;
    }

    bool SnapshotIsStale(const MinimapRenderer::SnapshotProjection& snap,
                         const MinimapRenderer::NavigationFrame& frame,
                         std::chrono::steady_clock::time_point a_spaceEnteredAt) {
        if (!snap.valid) return true;
        if (frame.isExterior || snap.isExterior) {
            if (snap.isExterior != frame.isExterior || snap.worldSpaceID != frame.worldSpaceID) return true;
        } else if (snap.cellID != frame.cellID) {
            return true;
        }
        const float distSq = frame.playerPos.GetSquaredDistance(snap.playerPos);
        float moveThreshold = kInspectMoveRefreshDist;
        if (frame.isExterior && snap.frustumHalfWidth > 0.0f) {
            moveThreshold = std::min(moveThreshold, snap.frustumHalfWidth * 0.25f);
        }
        if (distSq > moveThreshold * moveThreshold) return true;
        const auto now = std::chrono::steady_clock::now();
        const auto sinceSpaceEntry = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         now - a_spaceEnteredAt).count();
        const bool settling = sinceSpaceEntry < kSpaceSettleMs;
        if (!settling && distSq < 16.0f * 16.0f) return false;
        const auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - snap.takenAt).count();
        const float refreshSeconds = settling ? kSpaceSettleRetrySeconds
                                              : kInspectRefreshSeconds;
        return diff > refreshSeconds * 1000.0f;
    }

    ImVec2 WorldToGlance(const RE::NiPoint3& a_world,
                         const RE::NiPoint3& a_player,
                         float a_scale,
                         float a_rotation = 0.0f) {
        const float dx = a_world.x - a_player.x;
        const float dy = a_world.y - a_player.y;
        // Screen y is down; the arrow, view cone, and backdrop use this sign.
        const float ca = std::cos(a_rotation);
        const float sa = std::sin(a_rotation);
        const float rx = dx * ca - dy * sa;
        const float ry = dx * sa + dy * ca;
        return ImVec2(rx * a_scale, -ry * a_scale);
    }

    struct GlanceTextureVertex {
        ImVec2 pos;
        ImVec2 uv;
    };

    // The ImGui sampler wraps, so clip geometry at texture edges instead of
    // clamping individual UVs (which stretches the border pixels).
    bool DrawRotatedGlanceBackdrop(ImDrawList* a_dl,
                                   const MapDraw::RoundedRectBoundary& a_boundary,
                                   bool a_square, const ImVec2& a_uvCenter,
                                   const ImVec2& a_uvRight, const ImVec2& a_uvDown,
                                   ID3D11ShaderResourceView* a_texture, ImU32 a_color)
    {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr int kCornerSegments = 8;
        std::vector<GlanceTextureVertex> perimeter;
        perimeter.reserve(64);
        auto addPerimeterPoint = [&](float a_x, float a_y) {
            perimeter.push_back({
                ImVec2(a_boundary.center.x + a_x, a_boundary.center.y + a_y),
                ImVec2(a_uvCenter.x + a_x * a_uvRight.x + a_y * a_uvDown.x,
                       a_uvCenter.y + a_x * a_uvRight.y + a_y * a_uvDown.y)
            });
        };
        if (a_square) {
            const float inset = a_boundary.halfSize - a_boundary.rounding;
            for (int corner = 0; corner < 4; ++corner) {
                const float cx = corner < 2 ? inset : -inset;
                const float cy = (corner == 0 || corner == 3) ? -inset : inset;
                const float start = (static_cast<float>(corner) - 1.0f) * (0.5f * kPi);
                for (int i = 0; i <= kCornerSegments; ++i) {
                    const float angle = start + (0.5f * kPi * i / kCornerSegments);
                    addPerimeterPoint(cx + a_boundary.rounding * std::cos(angle),
                                      cy + a_boundary.rounding * std::sin(angle));
                }
            }
        } else {
            constexpr int kCircleSegments = 64;
            for (int i = 0; i < kCircleSegments; ++i) {
                const float angle = 2.0f * kPi * i / kCircleSegments;
                addPerimeterPoint(a_boundary.halfSize * std::cos(angle),
                                  a_boundary.halfSize * std::sin(angle));
            }
        }
        if (a_square && Settings::GetSingleton()->minimapSnapshotLog) {
            static std::chrono::steady_clock::time_point lastCornerLog{};
            const auto now = std::chrono::steady_clock::now();
            if (now - lastCornerLog >= std::chrono::seconds(5)) {
                lastCornerLog = now;
                SKSE::log::info(
                    "MinimapSnapshot: Glance square corners start=({:.1f},{:.1f}),({:.1f},{:.1f}),({:.1f},{:.1f}),({:.1f},{:.1f})",
                    perimeter[0].pos.x, perimeter[0].pos.y,
                    perimeter[kCornerSegments + 1].pos.x,
                    perimeter[kCornerSegments + 1].pos.y,
                    perimeter[2 * (kCornerSegments + 1)].pos.x,
                    perimeter[2 * (kCornerSegments + 1)].pos.y,
                    perimeter[3 * (kCornerSegments + 1)].pos.x,
                    perimeter[3 * (kCornerSegments + 1)].pos.y);
            }
        }

        const GlanceTextureVertex center{ a_boundary.center, a_uvCenter };
        std::vector<GlanceTextureVertex> triangles;
        triangles.reserve(perimeter.size() * 9);
        std::vector<GlanceTextureVertex> polygon;
        std::vector<GlanceTextureVertex> clipped;
        polygon.reserve(8);
        clipped.reserve(8);
        for (std::size_t i = 0; i < perimeter.size(); ++i) {
            polygon.clear();
            polygon.push_back(center);
            polygon.push_back(perimeter[i]);
            polygon.push_back(perimeter[(i + 1) % perimeter.size()]);
            for (int edge = 0; edge < 4 && !polygon.empty(); ++edge) {
                const auto coordinate = [edge](const GlanceTextureVertex& a_vertex) {
                    return edge < 2 ? a_vertex.uv.x : a_vertex.uv.y;
                };
                const float limit = edge % 2 == 0 ? 0.0f : 1.0f;
                const auto inside = [&](const GlanceTextureVertex& a_vertex) {
                    return edge % 2 == 0 ? coordinate(a_vertex) >= limit
                                         : coordinate(a_vertex) <= limit;
                };
                clipped.clear();
                auto previous = polygon.back();
                bool previousInside = inside(previous);
                for (const auto& current : polygon) {
                    const bool currentInside = inside(current);
                    if (previousInside != currentInside) {
                        const float t = (limit - coordinate(previous)) /
                                        (coordinate(current) - coordinate(previous));
                        clipped.push_back({
                            ImVec2(previous.pos.x + t * (current.pos.x - previous.pos.x),
                                   previous.pos.y + t * (current.pos.y - previous.pos.y)),
                            ImVec2(previous.uv.x + t * (current.uv.x - previous.uv.x),
                                   previous.uv.y + t * (current.uv.y - previous.uv.y))
                        });
                    }
                    if (currentInside) clipped.push_back(current);
                    previous = current;
                    previousInside = currentInside;
                }
                polygon.swap(clipped);
            }
            for (std::size_t vertex = 1; vertex + 1 < polygon.size(); ++vertex) {
                triangles.push_back(polygon[0]);
                triangles.push_back(polygon[vertex]);
                triangles.push_back(polygon[vertex + 1]);
            }
        }
        if (triangles.empty()) return false;

        a_dl->PushTextureID(reinterpret_cast<ImTextureID>(a_texture));
        a_dl->PrimReserve(static_cast<int>(triangles.size()), static_cast<int>(triangles.size()));
        for (std::size_t i = 0; i < triangles.size(); i += 3) {
            const unsigned int first = a_dl->_VtxCurrentIdx;
            a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(first));
            a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(first + 1));
            a_dl->PrimWriteIdx(static_cast<ImDrawIdx>(first + 2));
            for (std::size_t j = 0; j < 3; ++j) {
                const auto& vertex = triangles[i + j];
                a_dl->PrimWriteVtx(vertex.pos,
                                   ImVec2(std::clamp(vertex.uv.x, 0.0f, 1.0f),
                                          std::clamp(vertex.uv.y, 0.0f, 1.0f)),
                                   a_color);
            }
        }
        a_dl->PopTextureID();
        return true;
    }

    bool TrailHeadNearPlayer(const std::vector<PathTracker::Segment>& a_segments,
                             const RE::NiPoint3& a_player) {
        if (a_segments.empty() || a_segments.back().empty()) return false;
        const auto& head = a_segments.back().back();
        const float dx = a_player.x - head.x;
        const float dy = a_player.y - head.y;
        const float dz = a_player.z - head.z;
        return (dx * dx + dy * dy + dz * dz) < (250.0f * 250.0f);
    }

    // Crossfade helper: scales a color's alpha by a, used to fade every Glance/
    // Inspect draw call during the ~150ms overlay swap.
    ImU32 FadeCol(ImU32 c, float a) {
        const auto ca = static_cast<ImU32>(((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
        return (c & ~IM_COL32_A_MASK) | (ca << IM_COL32_A_SHIFT);
    }

    void DrawEntityIconBadge(ImDrawList* dl, ID3D11ShaderResourceView* icon,
                             const ImVec2& center, float side, float alpha, bool withBadge) {
        const auto* settings = Settings::GetSingleton();
        const float half = side * 0.5f;
        const float padding = withBadge
            ? (std::min)(side * 0.25f, UIScale::Px(settings->navEntityIconPadding)) : 0.0f;
        const float innerHalf = half - padding;
        const float rounding = (std::min)(UIScale::Px(4.0f), side * 0.22f);
        const ImVec2 min(center.x - half, center.y - half);
        const ImVec2 max(center.x + half, center.y + half);
        if (withBadge) {
            dl->AddRectFilled(min, max, FadeCol(IM_COL32(
                settings->navEntityIconBgR, settings->navEntityIconBgG,
                settings->navEntityIconBgB, settings->navEntityIconBgA), alpha), rounding);
        }
        MapDraw::DrawIconFx(dl, icon,
                            ImVec2(center.x - innerHalf, center.y - innerHalf),
                            ImVec2(center.x + innerHalf, center.y + innerHalf),
                            FadeCol(IM_COL32_WHITE, alpha),
                            settings->navEntityIconShadow ? 1 : 0,
                            settings->navEntityIcon3D ? 1 : 0);
        if (withBadge) {
            dl->AddRect(min, max, FadeCol(IM_COL32(
                settings->navEntityIconBorderR, settings->navEntityIconBorderG,
                settings->navEntityIconBorderB, settings->navEntityIconBorderA), alpha),
                        rounding, 0, UIScale::Px(1.0f));
        }
    }

    void DrawQuestTargetIcon(ImDrawList* dl, ID3D11ShaderResourceView* srv,
                             float x, float y, float halfPx, float a_alpha) {
        if (!srv) return;
        dl->AddImage(reinterpret_cast<ImTextureID>(srv),
                     ImVec2(x - halfPx, y - halfPx), ImVec2(x + halfPx, y + halfPx),
                     ImVec2(0, 0), ImVec2(1, 1), FadeCol(IM_COL32_WHITE, a_alpha));
    }

    float SmoothstepLocal(float t) { return t * t * (3.0f - 2.0f * t); }

    constexpr float kViewConeHalf = 0.6108652f; // 35 deg
    // a_alpha: crossfade multiplier, default 1.0 so any other caller is unaffected.
    void DrawViewCone(ImDrawList* dl, const ImVec2& a_origin, float a_mapAngle, float a_radius,
                      float a_alpha = 1.0f) {
        // Screen polar angle of forward(sin a, -cos a) is a - pi/2.
        const float theta = a_mapAngle - 1.5707963f;
        dl->PathLineTo(a_origin);
        dl->PathArcTo(a_origin, a_radius, theta - kViewConeHalf, theta + kViewConeHalf, 16);
        dl->PathFillConvex(FadeCol(IM_COL32(255, 255, 255, 26), a_alpha));
        // Edge rays so the cone stays readable over bright map areas.
        const ImVec2 e1(a_origin.x + std::cos(theta - kViewConeHalf) * a_radius,
                        a_origin.y + std::sin(theta - kViewConeHalf) * a_radius);
        const ImVec2 e2(a_origin.x + std::cos(theta + kViewConeHalf) * a_radius,
                        a_origin.y + std::sin(theta + kViewConeHalf) * a_radius);
        dl->AddLine(a_origin, e1, FadeCol(IM_COL32(255, 255, 255, 60), a_alpha), UIScale::Px(1.0f));
        dl->AddLine(a_origin, e2, FadeCol(IM_COL32(255, 255, 255, 60), a_alpha), UIScale::Px(1.0f));
    }

    void DrawViewCone(ImDrawList* dl, const MapDraw::RoundedRectBoundary& a_boundary, float a_mapAngle,
                      float a_alpha = 1.0f) {
        // Screen polar angle of forward(sin a, -cos a) is a - pi/2.
        const float theta = a_mapAngle - 1.5707963f;
        const ImVec2 origin = a_boundary.center;
        constexpr float kGlanceConeReach = 0.75f;
        constexpr float kFadeStart = 0.55f;
        constexpr int kSamples = 16;
        const ImVec2 uv = ImGui::GetDrawListSharedData()->TexUvWhitePixel;
        const ImU32 fill = FadeCol(IM_COL32(255, 255, 255, 26), a_alpha);
        const ImU32 clear = IM_COL32(255, 255, 255, 0);
        // One vertex-colored mesh fades the fill without another texture or pass.
        dl->PrimReserve(kSamples * 9, 1 + (kSamples + 1) * 2);
        const unsigned int base = dl->_VtxCurrentIdx;
        dl->PrimWriteVtx(origin, uv, fill);
        ImVec2 edges[2][3]{};
        for (int i = 0; i <= kSamples; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kSamples);
            const float angle = (theta - kViewConeHalf) + t * (2.0f * kViewConeHalf);
            const ImVec2 rim = a_boundary.RayIntersection(angle, UIScale::Px(4.0f));
            const ImVec2 outer(origin.x + (rim.x - origin.x) * kGlanceConeReach,
                               origin.y + (rim.y - origin.y) * kGlanceConeReach);
            const ImVec2 inner(origin.x + (outer.x - origin.x) * kFadeStart,
                               origin.y + (outer.y - origin.y) * kFadeStart);
            dl->PrimWriteVtx(inner, uv, fill);
            dl->PrimWriteVtx(outer, uv, clear);
            if (i == 0 || i == kSamples) {
                const int edge = i == 0 ? 0 : 1;
                edges[edge][0] = origin;
                edges[edge][1] = inner;
                edges[edge][2] = outer;
            }
        }
        for (int i = 0; i < kSamples; ++i) {
            const unsigned int innerIdx = base + 1 + i * 2;
            const unsigned int nextInnerIdx = innerIdx + 2;
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(base));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(innerIdx));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(nextInnerIdx));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(innerIdx));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(innerIdx + 1));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(nextInnerIdx + 1));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(innerIdx));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(nextInnerIdx + 1));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(nextInnerIdx));
        }
        const ImU32 edgeColors[3] = {
            FadeCol(IM_COL32(255, 255, 255, 60), a_alpha),
            FadeCol(IM_COL32(255, 255, 255, 60), a_alpha),
            clear
        };
        for (const auto& edge : edges) {
            MapDraw::PolylineVertexColored(dl, edge, edgeColors, 3, UIScale::Px(1.0f));
        }
    }

    using MinimapSnapshotTransition::ProjectWithSnapshot;

    // Engine constants from LocalMapHudSpike
    static constexpr std::size_t kCullingOffset = 0x40;
    static constexpr std::size_t kBlockSize = 0x30500;
    static constexpr std::size_t kCameraOffsetFromMenu = 0x302a0;

    using CtorFn   = void*(*)(void*);                 // SE 16092 / AE 16333  LocalMapCullingProcess ctor
    using RenderFn = void (*)(void*);                 // SE 16094 / AE 16335  the snapshot
    using CamBeginFn  = void (*)(void*);              // SE 16086 / AE 16327
    using CamTargetFn = void (*)(void*, const float*); // SE 16087 / AE 16328
    using CamBoundsFn = void (*)(void*, const float*, const float*); // SE 16090 / AE 16331
    using CamApplyFn  = void (*)(void*, float);       // SE 16089 / AE 16330
    using CamFrustumFn = void (*)(void*, float, float); // SE 16091 / AE 16332  ortho frustum dims
    using InitNodeFn   = void (*)(void*);              // SE 16095 / AE 16336  culling NiNode + flags

    using MenuBlockCtorFn = void (*)(void*);
    static REL::Relocation<MenuBlockCtorFn> g_menuBlockCtor{ REL::RelocationID(52076, 52964) };

    static REL::Relocation<CtorFn>       g_ctor      { REL::RelocationID(16092, 16333) };
    static REL::Relocation<RenderFn>     g_render    { REL::RelocationID(16094, 16335) };
    static REL::Relocation<CamBeginFn>   g_camBegin  { REL::RelocationID(16086, 16327) };
    static REL::Relocation<CamTargetFn>  g_camTarget { REL::RelocationID(16087, 16328) };
    static REL::Relocation<CamBoundsFn>  g_camBounds { REL::RelocationID(16090, 16331) };
    static REL::Relocation<CamApplyFn>   g_camApply  { REL::RelocationID(16089, 16330) };
    static REL::Relocation<CamFrustumFn> g_camFrustum{ REL::RelocationID(16091, 16332) };
    static REL::Relocation<InitNodeFn>   g_initNode  { REL::RelocationID(16095, 16336) };

    class ScopedTreeSwitchState
    {
    public:
        ScopedTreeSwitchState()
        {
            const auto started = std::chrono::steady_clock::now();
            _treeManager = RE::BSTreeManager::GetSingleton();
            if (_treeManager) {
                RE::BSSpinLockGuard locker(_treeManager->treeLock);
                _scanned = _treeManager->loadedTrees.size();
                _savedStates.reserve(_scanned);

                for (const auto& tree : _treeManager->loadedTrees) {
                    if (!tree) {
                        continue;
                    }

                    auto* switchNode = *reinterpret_cast<RE::NiSwitchNode**>(
                        reinterpret_cast<std::uintptr_t>(tree.get()) + 0x180);
                    if (switchNode) {
                        _savedStates.emplace_back(
                            RE::NiPointer<RE::NiSwitchNode>{ switchNode },
                            switchNode->index);
                    }
                }
            }
            _captureMs = ElapsedMs(started);
        }

        ~ScopedTreeSwitchState()
        {
            Restore();
        }

        ScopedTreeSwitchState(const ScopedTreeSwitchState&) = delete;
        ScopedTreeSwitchState& operator=(const ScopedTreeSwitchState&) = delete;

        void Restore()
        {
            if (_restoreAttempted) {
                return;
            }
            _restoreAttempted = true;

            const auto started = std::chrono::steady_clock::now();
            if (_treeManager && !_savedStates.empty()) {
                RE::BSSpinLockGuard locker(_treeManager->treeLock);
                for (const auto& [switchNode, originalIndex] : _savedStates) {
                    if (switchNode) {
                        if (switchNode->index != originalIndex) {
                            ++_changed;
                        }
                        switchNode->index = originalIndex;
                        if (switchNode->index == originalIndex) {
                            ++_restored;
                        }
                    }
                }
            }
            _restoreMs = ElapsedMs(started);
        }

        [[nodiscard]] std::size_t Scanned() const { return _scanned; }
        [[nodiscard]] std::size_t Saved() const { return _savedStates.size(); }
        [[nodiscard]] std::size_t Changed() const { return _changed; }
        [[nodiscard]] std::size_t Restored() const { return _restored; }
        [[nodiscard]] float CaptureMs() const { return _captureMs; }
        [[nodiscard]] float RestoreMs() const { return _restoreMs; }

    private:
        static float ElapsedMs(std::chrono::steady_clock::time_point a_started)
        {
            return std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - a_started).count();
        }

        RE::BSTreeManager* _treeManager{ nullptr };
        std::vector<std::pair<RE::NiPointer<RE::NiSwitchNode>, std::int32_t>> _savedStates;
        std::size_t _scanned{ 0 };
        std::size_t _changed{ 0 };
        std::size_t _restored{ 0 };
        float _captureMs{ 0.0f };
        float _restoreMs{ 0.0f };
        bool _restoreAttempted{ false };
    };

    static std::uint8_t* g_block      = nullptr;  // LocalMapMenu base
    static bool          g_constructed = false;

    static void* CullingBase() { return g_block + kCullingOffset; }
    static void* CameraBase()  { return g_block + kCameraOffsetFromMenu; }

    // PreDisplay only, after the engine render. Never read back from Present:
    // the engine's immediate context belongs to the rendering path here.
    void LogSnapshotPixels(ID3D11ShaderResourceView* srv, std::uint32_t cellID) {
        if (!Settings::GetSingleton()->minimapSnapshotLog || !srv) return;
        static std::chrono::steady_clock::time_point last{};
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(10)) return;
        last = now;
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3D11Resource> resource;
        srv->GetResource(resource.GetAddressOf());
        ComPtr<ID3D11Texture2D> texture;
        if (!resource || FAILED(resource.As(&texture))) {
            SKSE::log::info("MinimapSnapshot: pixels UNKNOWN cell={:08X}, SRV has no Texture2D", cellID);
            return;
        }
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        SKSE::log::info("MinimapSnapshot: texture cell={:08X} srv={} size={}x{} format={} samples={} mips={} array={} bind={:#x}",
            cellID, static_cast<void*>(srv), desc.Width, desc.Height, static_cast<int>(desc.Format),
            desc.SampleDesc.Count, desc.MipLevels, desc.ArraySize, desc.BindFlags);
        const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        if ((!rgba && !bgra) || desc.SampleDesc.Count != 1 || desc.ArraySize != 1) {
            SKSE::log::info("MinimapSnapshot: pixels UNKNOWN, unsupported readback format/layout");
            return;
        }
        ComPtr<ID3D11Device> device;
        texture->GetDevice(device.GetAddressOf());
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(context.GetAddressOf());
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf());
        if (FAILED(hr)) {
            SKSE::log::info("MinimapSnapshot: pixels UNKNOWN, staging creation failed hr={:#x}", static_cast<std::uint32_t>(hr));
            return;
        }
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        // Diagnostic-only synchronous readback: Map waits for the copy so an
        // uncompleted GPU render is never mistaken for a blank texture.
        hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            SKSE::log::info("MinimapSnapshot: pixels UNKNOWN, Map failed hr={:#x} deviceRemoved={:#x}",
                static_cast<std::uint32_t>(hr), static_cast<std::uint32_t>(device->GetDeviceRemovedReason()));
            return;
        }
        unsigned rgbMin = 255, rgbMax = 0, alphaMin = 255, alphaMax = 0;
        std::uint64_t black = 0, transparent = 0, different = 0;
        const auto* first = static_cast<const unsigned char*>(mapped.pData);
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* row = first + static_cast<size_t>(y) * mapped.RowPitch;
            for (UINT x = 0; x < desc.Width; ++x) {
                const auto* pixel = row + static_cast<size_t>(x) * 4;
                for (int c = 0; c < 3; ++c) {
                    rgbMin = std::min(rgbMin, static_cast<unsigned>(pixel[c]));
                    rgbMax = std::max(rgbMax, static_cast<unsigned>(pixel[c]));
                }
                alphaMin = std::min(alphaMin, static_cast<unsigned>(pixel[3]));
                alphaMax = std::max(alphaMax, static_cast<unsigned>(pixel[3]));
                black += pixel[0] <= 3 && pixel[1] <= 3 && pixel[2] <= 3;
                transparent += pixel[3] <= 3;
                different += std::memcmp(pixel, first, 4) != 0;
            }
        }
        context->Unmap(staging.Get(), 0);
        const double total = static_cast<double>(desc.Width) * desc.Height;
        SKSE::log::info("MinimapSnapshot: pixels cell={:08X} rgbRange={}-{} alphaRange={}-{} nearBlack={:.2f}% nearTransparent={:.2f}% uniform={} readbackMs={:.2f}; content statistics do not prove geometry or on-screen visibility",
            cellID, rgbMin, rgbMax, alphaMin, alphaMax, 100.0 * black / total,
            100.0 * transparent / total, different == 0,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count());
    }

    // Texture allocation metadata alone cannot establish whether pixels were written.
    void LogTextureDesc(ID3D11ShaderResourceView* a_srv) {
        if (!a_srv) {
            SKSE::log::warn("MinimapRenderer: DIAG srv null, no texture desc");
            return;
        }
        ID3D11Resource* res = nullptr;
        a_srv->GetResource(&res);
        if (!res) {
            SKSE::log::warn("MinimapRenderer: DIAG srv has no resource");
            return;
        }
        ID3D11Texture2D* tex = nullptr;
        res->QueryInterface(IID_PPV_ARGS(&tex));
        if (tex) {
            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);
            SKSE::log::debug("MinimapRenderer: DIAG texture {}x{} format={} usage={} bind={:#x}",
                desc.Width, desc.Height, static_cast<int>(desc.Format),
                static_cast<int>(desc.Usage), desc.BindFlags);
            tex->Release();
        }
        res->Release();
    }

    static void EnsureConstructed() {
        if (g_constructed) return;
        g_block = static_cast<std::uint8_t*>(RE::malloc(kBlockSize));
        if (!g_block) {
            SKSE::log::error("MinimapRenderer: malloc({:#x}) failed", kBlockSize);
            return;
        }
        std::memset(g_block, 0, kBlockSize);
        g_menuBlockCtor(g_block);
        g_initNode(CullingBase());
        g_camFrustum(CameraBase(), 4000.0f, 2250.0f);  // vanilla constants, 16:9
        g_constructed = true;
        SKSE::log::info("MinimapRenderer: menu block constructed + node/frustum init");
    }
}

bool MinimapRenderer::CanOpenControllerInspect() {
    if (MenuUtils::ShouldBlockOverlay() || PartySheetState::ShouldHideOverlay() ||
        SettingsMenu::IsVisible()) {
        return false;
    }

    auto* settings = Settings::GetSingleton();
    if (settings->hideInBeastForm && BeastForm::IsBeastForm()) {
        return false;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* cell = player ? player->GetParentCell() : nullptr;
    if (!cell) {
        return false;
    }

    return cell->IsExteriorCell() ? settings->navInspectExteriorVisible
                                  : settings->navInspectInteriorVisible;
}

MinimapRenderer::NavigationFrame MinimapRenderer::BuildFrame() {
    NavigationFrame f;
    if (MenuUtils::ShouldBlockOverlay()) {
        _inspectToggleActive.store(false);
        CloseControllerInspect();
        return f;
    }

    if (Settings::GetSingleton()->hideInBeastForm && BeastForm::IsBeastForm()) {
        _inspectToggleActive.store(false);
        CloseControllerInspect();
        return f;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return f;

    auto* cell = player->GetParentCell();
    if (!cell) return f;

    auto* ws = player->GetWorldspace();
    f.isExterior = cell->IsExteriorCell();
    auto* navSettings = Settings::GetSingleton();
    const bool anyVisible = f.isExterior
        ? ((navSettings->navGlanceExteriorVisible && _glanceVisible.load()) || navSettings->navInspectExteriorVisible)
        : ((navSettings->navGlanceInteriorVisible && _glanceVisible.load()) || navSettings->navInspectInteriorVisible);
    if (f.isExterior && !ws) return f;
    if (!anyVisible) return f;
    f.worldSpaceID = f.isExterior ? ws->GetFormID() : 0;

    f.valid = true;
    f.cellID = cell->GetFormID();
    // Title text for the Inspect overlay. Resolved here, not in the draw path,
    // so the Present thread never walks forms.
    if (f.isExterior) {
        const char* n = nullptr;
        if (auto* loc = player->GetCurrentLocation()) n = loc->GetFullName();
        if ((!n || !*n) && ws) n = ws->GetFullName();
        f.locationName = (n && *n) ? n : "";
    } else {
        const char* n = cell->GetFullName();
        f.locationName = (n && *n) ? n : "";
    }
    f.playerPos = player->GetPosition();
    f.playerAngle = player->data.angle.z;
    f.cameraAngle = f.playerAngle;
    if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
        const auto& r = cam->cameraRoot->world.rotate;
        f.cameraAngle = std::atan2(r.entry[0][1], r.entry[1][1]);
    }
    f.segments = (!f.isExterior && navSettings->enablePathTracking)
        ? PathTracker::GetSingleton()->GetSegments(f.cellID)
        : decltype(f.segments){};
    f.markers = MarkerManager::GetSingleton()->GetMarkers();

    f.questTargets = HUDMarkerHook::GetQuestMarkers();

    const bool inspectVisibleHere = f.isExterior ? navSettings->navInspectExteriorVisible : navSettings->navInspectInteriorVisible;
    bool keyboardInspectActive = false;
    if (navSettings->navInspectActivation == 1) {
        keyboardInspectActive = _inspectToggleActive.load();
    } else {
        const bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        // Hotkeys store DIK; GetAsyncKeyState needs a VK. 0 back = unbound, or a
        // mouse/gamepad bind this poll cannot see, so treat it as not pressed.
        const int inspectVk = InputHandler::DIKToVK(Settings::GetSingleton()->navInspectKey);
        keyboardInspectActive = !ctrlHeld && inspectVk != 0 &&
                                (GetAsyncKeyState(inspectVk) & 0x8000) != 0;
    }
    f.inspectHeld = !SettingsMenu::IsVisible() && inspectVisibleHere &&
                    (keyboardInspectActive || _controllerInspectActive.load());
    return f;
}

void MinimapRenderer::Draw() {
    MinimapSnapshotTransition::BeginFrame();
    if (PartySheetState::ShouldHideOverlay()) {
        _inspectShown.store(false);
        _interiorInspectShown.store(false);
        _inspectToggleActive.store(false);
        CloseControllerInspect();
        _inspectWasHeld = false;
        _toggleWasDown = false;
        return;
    }

    const auto frame = BuildFrame();
    if (Settings::GetSingleton()->minimapSnapshotLog) {
        static std::chrono::steady_clock::time_point lastDiag{};
        const auto now = std::chrono::steady_clock::now();
        if (now - lastDiag >= std::chrono::seconds(5)) {
            lastDiag = now;
            auto ui = RE::UI::GetSingleton();
            bool hudPresent = false, offscreen = false;
            if (ui) {
                if (auto hud = ui->GetMenu(RE::HUDMenu::MENU_NAME)) {
                    hudPresent = true;
                    offscreen = hud->RendersOffscreenTargets();
                }
            }
            SKSE::log::info("MinimapSnapshot: Draw heartbeat frameValid={} cell={:08X} pending={} hud={} offscreenFlag={} refreshEnabled={}",
                frame.valid, frame.cellID, _refreshPending.load(), hudPresent, offscreen,
                Settings::GetSingleton()->navRefreshOnInspect);
        }
    }
    _inspectShown.store(frame.valid && frame.inspectHeld);
    _interiorInspectShown.store(frame.valid && frame.inspectHeld && !frame.isExterior);
    if (!frame.valid) {
        _inspectWasHeld = false;
        return;
    }

    {
        const std::uint32_t space = frame.isExterior ? frame.worldSpaceID : frame.cellID;
        if (space != _lastDrawnSpace || frame.isExterior != _lastDrawnExterior) {
            // Test BEFORE assigning: _lastDrawnSpace == 0 means nothing has been drawn
            // yet, i.e. this is the first draw rather than a real transition.
            const bool firstEverDraw = _lastDrawnSpace == 0;
            _lastDrawnSpace = space;
            _lastDrawnExterior = frame.isExterior;
            _spaceEnteredAt = std::chrono::steady_clock::now();
            if (!firstEverDraw) {
                _glanceFade = 0.0f;
                _spaceFadeHold = true;
            }
        }
    }

    const bool inspectEdge = frame.inspectHeld && !_inspectWasHeld;
    const bool inspectReleased = !frame.inspectHeld && _inspectWasHeld;
    _inspectWasHeld = frame.inspectHeld;

    const bool glanceVisibleHere = _glanceVisible.load() &&
        (frame.isExterior ? Settings::GetSingleton()->navGlanceExteriorVisible
                          : Settings::GetSingleton()->navGlanceInteriorVisible);
    if (inspectReleased && glanceVisibleHere &&
        Settings::GetSingleton()->navRefreshOnInspect) {
        RequestSnapshotRefresh(frame, "glance reframe after inspect");
    }

    if (!_inspectModeInited) {
        _inspectMode.store(Settings::GetSingleton()->navInspectDefaultMode == 1
                               ? InspectViewMode::Radar
                               : InspectViewMode::Cover);
        _inspectModeInited = true;
    }

    const bool controllerModeToggleRequested = _controllerModeToggleRequested.exchange(false);
    if (frame.isExterior) {
        if (frame.inspectHeld && _inspectMode.load() != InspectViewMode::Radar) {
            _inspectMode.store(InspectViewMode::Radar);
            RequestSnapshotRefresh(frame, "exterior forces radar mode");
        }
        _toggleWasDown = false;
    } else if (frame.inspectHeld) {
        const int toggleVk =
            InputHandler::DIKToVK(Settings::GetSingleton()->navInspectModeToggleKey);
        const bool toggleDown = toggleVk != 0 && (GetAsyncKeyState(toggleVk) & 0x8000) != 0;
        if ((toggleDown && !_toggleWasDown) || controllerModeToggleRequested) {
            const auto cur = _inspectMode.load();
            _inspectMode.store(cur == InspectViewMode::Cover ? InspectViewMode::Radar
                                                             : InspectViewMode::Cover);
            RequestSnapshotRefresh(frame, "inspect mode toggle");
        }
        _toggleWasDown = toggleDown;
    } else {
        _toggleWasDown = false;
    }

    if (frame.inspectHeld) {
        const int markVk = InputHandler::DIKToVK(Settings::GetSingleton()->navQuickMarkKey);
        const bool markDown = markVk != 0 && (GetAsyncKeyState(markVk) & 0x8000) != 0;
        const bool controllerQuickMarkRequested = _controllerQuickMarkRequested.exchange(false);
        if ((markDown && !_quickMarkWasDown) || controllerQuickMarkRequested) {
            const auto now = std::chrono::steady_clock::now();
            const auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - _lastQuickMarkTime).count();
            if (sinceLast > 500) {
                if (frame.isExterior) {
                    const std::uint32_t assignedId = MarkerManager::GetSingleton()->AddAutoNamedMarker(
                        frame.worldSpaceID, frame.cellID, frame.playerPos,
                        0, AutoName::Custom, 0, MarkerOrigin::InspectQuickMark);
                    if (assignedId == 0) {
                        SKSE::log::warn("MinimapRenderer: quick mark placement rejected by MarkerManager");
                    } else {
                        _lastQuickMarkTime = now;
                        // Toast text is built here, not in the draw path: exterior
                        // marks carry no number, interior ones do.
                        _quickMarkToastText = LocalizationManager::Get("Navigation.MarkerAdded");
                        _quickMarkToastTime = now;
                        SKSE::log::info("MinimapRenderer: quick mark placed (exterior, default pin) id={} worldSpace={:08X} cell={:08X}",
                            assignedId, frame.worldSpaceID, frame.cellID);
                    }
                } else {
                    std::uint32_t maxN = 0;
                    for (const auto& m : MarkerManager::GetSingleton()->GetUserMarkers()) {
                        const bool sameScope = m.worldSpaceID == 0 && m.cellID == frame.cellID;
                        if (sameScope && m.iconType >= MapDraw::kQuickMarkBase) {
                            maxN = std::max(maxN, m.iconType - MapDraw::kQuickMarkBase + 1);
                        }
                    }
                    const std::uint32_t number = maxN + 1;
                    const std::uint32_t assignedId = MarkerManager::GetSingleton()->AddAutoNamedMarker(
                        frame.worldSpaceID, frame.cellID, frame.playerPos,
                        MapDraw::kQuickMarkBase + number - 1,
                        AutoName::QuickMark, number, MarkerOrigin::InspectQuickMark);
                    if (assignedId == 0) {
                        SKSE::log::warn("MinimapRenderer: numbered quick mark placement rejected by MarkerManager");
                    } else {
                        _lastQuickMarkTime = now;
                        _quickMarkToastText = LocalizationManager::Format(
                            "Navigation.MarkerAddedNumbered", std::to_string(number));
                        _quickMarkToastTime = now;
                        SKSE::log::info("MinimapRenderer: quick mark placed number={} id={} worldSpace={:08X} cell={:08X}",
                            number, assignedId, frame.worldSpaceID, frame.cellID);
                    }
                }
            }
        }
        _quickMarkWasDown = markDown;
    } else {
        _quickMarkWasDown = false;
    }

    if (!Settings::GetSingleton()->navRefreshOnInspect) {
        if (inspectEdge) {
            std::lock_guard<std::mutex> lock(_snapshotMutex);
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (renderer) {
                _snapshot.srv = reinterpret_cast<ID3D11ShaderResourceView*>(
                    renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kLOCAL_MAP].SRV);
                _snapshot.valid = _snapshot.srv != nullptr;
                _snapshot.image.reset();
                _snapshot.stale = false;
                _snapshot.cellID = frame.cellID;
                _snapshot.takenAt = std::chrono::steady_clock::now();
            }
            SKSE::log::debug("MinimapRenderer: DIAG frozen-texture mode, srv={}",
                static_cast<void*>(_snapshot.srv));
            LogTextureDesc(_snapshot.srv);
        }
    } else if (inspectEdge) {
        RequestSnapshotRefresh(frame, "inspect edge");
    } else if (frame.inspectHeld) {
        SnapshotProjection snap;
        {
            std::lock_guard<std::mutex> lock(_snapshotMutex);
            snap = _snapshot;
        }
        if (SnapshotIsStale(snap, frame, _spaceEnteredAt)) {
            RequestSnapshotRefresh(frame, "inspect stale");
        }
    } else if (glanceVisibleHere) {
        SnapshotProjection snap;
        {
            std::lock_guard<std::mutex> lock(_snapshotMutex);
            snap = _snapshot;
        }
        if (SnapshotIsStale(snap, frame, _spaceEnteredAt)) {
            RequestSnapshotRefresh(frame, "glance stale");
        }
    }

    {
        if (_spaceFadeHold) {
            SnapshotProjection snap;
            {
                std::lock_guard<std::mutex> lock(_snapshotMutex);
                snap = _snapshot;
            }
            // Same space test the backdrop gate uses. Release only when the new
            // snapshot has actually arrived for THIS space.
            const bool sameSpace = frame.isExterior
                ? (snap.isExterior && snap.worldSpaceID == frame.worldSpaceID)
                : (!snap.isExterior && snap.cellID == frame.cellID);
            if (snap.valid && snap.srv && sameSpace) {
                _spaceFadeHold = false;
            } else {
                const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - _spaceEnteredAt).count();
                if (held > kSpaceFadeMaxHoldMs) _spaceFadeHold = false;
            }
        }
    }

    constexpr float kFadeSeconds = 0.15f;
    constexpr float kSpaceFadeSeconds = 0.35f;
    const float dt = ImGui::GetIO().DeltaTime;
    const float rate = std::clamp((kFadeSeconds > 1e-4f) ? (dt / kFadeSeconds) : 1.0f, 0.0f, 1.0f);
    const float glanceRate = _glanceFade < 1.0f && !frame.inspectHeld && !_spaceFadeHold
        ? std::clamp(dt / kSpaceFadeSeconds, 0.0f, 1.0f)
        : rate;
    // Held at 0 until the new space's snapshot lands (see the transition block above).
    const float glanceTarget = (frame.inspectHeld || _spaceFadeHold) ? 0.0f : 1.0f;
    const float inspectTarget = frame.inspectHeld ? 1.0f : 0.0f;
    const float glanceStep = std::clamp(glanceTarget - _glanceFade, -glanceRate, glanceRate);
    const float inspectStep = std::clamp(inspectTarget - _inspectFade, -rate, rate);
    _glanceFade = std::clamp(_glanceFade + glanceStep, 0.0f, 1.0f);
    _inspectFade = std::clamp(_inspectFade + inspectStep, 0.0f, 1.0f);

    const float glanceAlpha = SmoothstepLocal(_glanceFade);
    const float inspectAlpha = SmoothstepLocal(_inspectFade);

    if (!Settings::GetSingleton()->enablePathTracking) {
        _retraceFade = 0.0f;
    } else {
        constexpr float kRetraceFadeSeconds = 0.25f;
        const float retraceRate = std::clamp((kRetraceFadeSeconds > 1e-4f) ? (dt / kRetraceFadeSeconds) : 1.0f, 0.0f, 1.0f);
        const float retraceTarget = PathTracker::GetSingleton()->GetRecentRevisits(0.6f).empty() ? 0.0f : 1.0f;
        const float retraceStep = std::clamp(retraceTarget - _retraceFade, -retraceRate, retraceRate);
        _retraceFade = std::clamp(_retraceFade + retraceStep, 0.0f, 1.0f);
    }

    {
        const float now = static_cast<float>(ImGui::GetTime());
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player && player->IsInCombat()) _lastCombatTime = now;
        const float combatTarget = (now - _lastCombatTime < 1.5f) ? 1.0f : 0.0f;
        constexpr float kCombatFadeSeconds = 0.5f;
        const float combatRate = std::clamp(dt / kCombatFadeSeconds, 0.0f, 1.0f);
        const float combatStep = std::clamp(combatTarget - _combatFade, -combatRate, combatRate);
        _combatFade = std::clamp(_combatFade + combatStep, 0.0f, 1.0f);
    }

    CollectActorsIfDue(frame);
    MergeActorGhosts();

    const unsigned snapshotViews = ((glanceVisibleHere && glanceAlpha > 0.0f) ? 1u : 0u) |
        ((inspectAlpha > 0.0f) ? 2u : 0u);
    {
        std::lock_guard<std::mutex> lock(_snapshotMutex);
        MinimapSnapshotTransition::Prepare(_snapshot, frame, snapshotViews);
    }
    if (glanceVisibleHere && glanceAlpha > 0.0f) {
        DrawGlance(frame, glanceAlpha);
    }
    if (inspectAlpha > 0.0f) {
        DrawInspect(frame, inspectAlpha);
    }
}

void MinimapRenderer::MergeActorGhosts() {
    const auto now = std::chrono::steady_clock::now();

    const bool newScan = _lastActorScanAt != _lastMergedScanAt;
    if (newScan) {
        _lastMergedScanAt = _lastActorScanAt;
        for (const auto& a : _lastActorScan) {
            auto& ghost = _actorGhosts[a.formID];   // inserts default if new
            ghost.position   = a.position;
            ghost.allegiance = a.allegiance;
            ghost.lastSeen   = now;
            ghost.frozen     = false;
        }
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    const RE::NiPoint3 playerPos = player ? player->GetPosition() : RE::NiPoint3();
    // Radius hoisted out of the loop: Settings lookup + r*r once per frame,
    // not once per ghost.
    auto* st = Settings::GetSingleton();
    const float r = st ? st->navActorProximityUnits : ActorMarkerSource::kActorProximityRadius;
    const float r2 = r * r;
    for (auto& [formID, ghost] : _actorGhosts) {
        if (ghost.frozen) continue;
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);
        if (actor && actor->Is3DLoaded() && !actor->IsDead()) {
            const auto pos = actor->GetPosition();
            if (player) {
                const float dx = pos.x - playerPos.x;
                const float dy = pos.y - playerPos.y;
                if (dx * dx + dy * dy > r2) {
                    ghost.frozen   = true;
                    ghost.lastSeen = now;
                    continue;
                }
            }
            ghost.position = pos;
        } else {
            ghost.frozen   = true;
            ghost.lastSeen = now;
        }
    }

    if (newScan) {
        std::unordered_set<RE::FormID> seenThisScan;
        seenThisScan.reserve(_lastActorScan.size());
        for (const auto& a : _lastActorScan) seenThisScan.insert(a.formID);
        for (auto& [formID, ghost] : _actorGhosts) {
            if (seenThisScan.count(formID)) continue;
            if (!ghost.frozen) {
                ghost.frozen   = true;
                ghost.lastSeen = now;
            }
        }
    }

    constexpr float kActorFadeInSeconds  = 0.3f;
    constexpr float kActorFadeOutSeconds = 1.0f;
    float dt = std::chrono::duration<float>(now - _lastGhostTick).count();
    _lastGhostTick = now;
    dt = std::clamp(dt, 0.0f, 0.1f);  // first call / menu stall: don't jump the fade
    for (auto it = _actorGhosts.begin(); it != _actorGhosts.end(); ) {
        auto& ghost = it->second;
        if (ghost.frozen) {
            ghost.alpha -= dt / kActorFadeOutSeconds;
            if (ghost.alpha <= 0.0f) { it = _actorGhosts.erase(it); continue; }
        } else {
            ghost.alpha = std::min(ghost.alpha + dt / kActorFadeInSeconds, 1.0f);
        }
        ++it;
    }

}

void MinimapRenderer::CollectActorsIfDue(const NavigationFrame& a_frame) {
    auto* navSettings = Settings::GetSingleton();
    if (!navSettings->navActorAnyVisible()) return;

    const auto now = std::chrono::steady_clock::now();
    if (_lastActorScanAt.time_since_epoch().count() != 0 &&
        std::chrono::duration<float>(now - _lastActorScanAt).count() < kActorScanSeconds) {
        return;
    }

    if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
        return;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player || !player->GetParentCell()) return;

    if (a_frame.isExterior) {
        _lastActorScan = ActorMarkerSource::Collect(
            player, navSettings->navInspectRadarRangeUnits, a_frame.playerPos);
    } else if (auto* cell = player->GetParentCell()) {
        _lastActorScan = ActorMarkerSource::Collect(cell, a_frame.playerPos);
    } else {
        _lastActorScan.clear();
    }
    _lastActorScanAt = now;
}

void MinimapRenderer::RequestSnapshotRefresh(const NavigationFrame& a_frame, const char* a_reason) {
    // Hard gate here, not only in Draw(). Exterior Inspect mode coercion and future
    // callers can request before Draw reaches the frozen-texture branch.
    if (!Settings::GetSingleton()->navRefreshOnInspect) {
        _refreshPending.store(false);
        return;
    }

    // Log only on the false->true edge so a stuck pending flag doesn't spam per frame.
    if (!_refreshPending.exchange(true)) {
        if (Settings::GetSingleton()->minimapSnapshotLog)
            SKSE::log::info("MinimapSnapshot: refresh requested reason={} cell={:08X}", a_reason, a_frame.cellID);
    }

    if (auto ui = RE::UI::GetSingleton()) {
        if (auto hud = ui->GetMenu(RE::HUDMenu::MENU_NAME)) {
            hud->menuFlags.set(RE::UI_MENU_FLAGS::kRendersOffscreenTargets);
        }
    }
}

// Small HUD radar for Glance mode.
void MinimapRenderer::DrawGlance(const NavigationFrame& f, float a_alpha) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    auto* settings = Settings::GetSingleton();
    // Live radius, not a fixed constant: driven by the Glance Size slider (navGlanceSize).
    const float kGlanceRadius = UIScale::Px(settings->navGlanceSize * 0.5f);
    const float glanceScale = kGlanceRadius / settings->navGlanceRangeUnits;

    const char* cardinalText[4] = {};
    float cardinalFontSize = 0.0f;
    float cardinalHalfDiag = 0.0f;
    float cardinalRing = 0.0f;
    const float cardinalGap = UIScale::Px(3.0f);
    if (settings->navGlanceShowCardinals) {
        // Static: Get() returns by value and the draw needs stable c_str()s.
        static std::string labels[4];
        labels[0] = LocalizationManager::Get("Navigation.CardinalN");
        labels[1] = LocalizationManager::Get("Navigation.CardinalE");
        labels[2] = LocalizationManager::Get("Navigation.CardinalS");
        labels[3] = LocalizationManager::Get("Navigation.CardinalW");
        cardinalFontSize = ImGui::GetFont()->FontSize * UIScale::Font() *
            std::clamp(settings->bodyScale, 0.5f, 3.0f);
        for (int i = 0; i < 4; ++i) {
            cardinalText[i] = labels[i].c_str();
            const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(cardinalFontSize, FLT_MAX, 0.0f, cardinalText[i]);
            cardinalHalfDiag = (std::max)(cardinalHalfDiag, 0.5f * std::hypot(sz.x, sz.y));
        }
        cardinalRing = cardinalGap + 2.0f * cardinalHalfDiag;
    }

    const auto axisMax = [&](float a_disp) {
        return (std::max)(cardinalRing, a_disp - 2.0f * kGlanceRadius - cardinalRing);
    };
    const float maxX = axisMax(disp.x);
    const float maxY = axisMax(disp.y);
    // Settings sliders read these as their max (see GlanceBounds.h).
    GlanceBounds::maxPosX.store(static_cast<int>(std::floor(maxX)));
    GlanceBounds::maxPosY.store(static_cast<int>(std::floor(maxY)));
    const float posX = std::clamp(static_cast<float>(settings->navGlancePosX), cardinalRing, maxX);
    const float posY = std::clamp(static_cast<float>(settings->navGlancePosY), cardinalRing, maxY);
    const ImVec2 center(posX + kGlanceRadius, posY + kGlanceRadius);


    const float kGlanceSquareRounding = UIScale::Px(10.0f);
    const float kGlanceRimMarkerInset = UIScale::Px(8.0f);
    const float halfSize = kGlanceRadius;
    const bool square = settings->navGlanceShape == 1;
    const bool followCamera = settings->navGlanceOrientation == 1;
    const MapDraw::RoundedRectBoundary boundary{
        center,
        halfSize,
        square ? kGlanceSquareRounding : halfSize
    };

    // The engine snapshot contains transparent pixels. Keep the same strong
    // backfill opacity as Inspect so both surfaces composite consistently.
    const ImU32 bgFill = FadeCol(IM_COL32(10, 12, 14, 230), a_alpha);
    if (square) {
        dl->AddRectFilled(boundary.Min(), boundary.Max(), bgFill, boundary.rounding);
    } else {
        dl->AddCircleFilled(center, halfSize, bgFill, 64);
    }

    SnapshotProjection snap;
    {
        std::lock_guard<std::mutex> lock(_snapshotMutex);
        snap = _snapshot;
    }
    {
        const char* backdropResult = "invalid snapshot";
        ImVec2 diagUV0{}, diagUV1{};
        const bool snapSameSpace = f.isExterior
            ? (snap.isExterior && snap.worldSpaceID == f.worldSpaceID)
            : (!snap.isExterior && snap.cellID == f.cellID);
        if (snap.valid) backdropResult = !snap.srv ? "missing SRV" : !snapSameSpace ? "wrong space" : "projection failed";
        if (snap.valid && snap.srv && snapSameSpace) {
            const float glanceWorldRadius = settings->navGlanceRangeUnits;

            // UV center = player world pos projected into the snapshot's 0..1 UV box.
            ImVec2 uvCenter;
            const bool haveCenter = ProjectWithSnapshot(
                snap, f.playerPos, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), uvCenter);
            RE::NiPoint3 edgeWorldX = f.playerPos;
            edgeWorldX.x += glanceWorldRadius;
            RE::NiPoint3 edgeWorldY = f.playerPos;
            edgeWorldY.y += glanceWorldRadius;
            ImVec2 uvEdgeX, uvEdgeY;
            const bool haveEdgeX = ProjectWithSnapshot(
                snap, edgeWorldX, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), uvEdgeX);
            const bool haveEdgeY = ProjectWithSnapshot(
                snap, edgeWorldY, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), uvEdgeY);

            if (haveCenter && haveEdgeX && haveEdgeY) {
                backdropResult = "degenerate or nonfinite crop extent";
                // A, B = UV images of the two equal-length orthogonal world
                // vectors = columns of the world->UV rotation+scale map.
                const float Ax = uvEdgeX.x - uvCenter.x;
                const float Ay = uvEdgeX.y - uvCenter.y;
                const float Bx = uvEdgeY.x - uvCenter.x;
                const float By = uvEdgeY.y - uvCenter.y;
                // Per-axis half-extent that covers glanceWorldRadius in every
                // world direction. Zero-north reduces to |Ax| / |By|.
                const float uHalf = std::sqrt(Ax * Ax + Bx * Bx);
                const float vHalf = std::sqrt(Ay * Ay + By * By);
                if (uHalf > 1e-4f && vHalf > 1e-4f) {
                    // Clamp the crop to [0,1] per axis. Near a cell edge this leaves
                    // the player off-center rather than sampling outside the texture.
                    ImVec2 uv0(uvCenter.x - uHalf, uvCenter.y - vHalf);
                    ImVec2 uv1(uvCenter.x + uHalf, uvCenter.y + vHalf);
                    uv0.x = std::clamp(uv0.x, 0.0f, 1.0f);
                    uv0.y = std::clamp(uv0.y, 0.0f, 1.0f);
                    uv1.x = std::clamp(uv1.x, 0.0f, 1.0f);
                    uv1.y = std::clamp(uv1.y, 0.0f, 1.0f);
                    diagUV0 = uv0;
                    diagUV1 = uv1;
                    backdropResult = (uv1.x > uv0.x && uv1.y > uv0.y)
                        ? "image submitted" : "image submitted with empty or nonfinite crop";

                    auto* easeDraw = MinimapSnapshotTransition::BeginDraw(dl, snap);
                    auto* imageSRV = MinimapSnapshotTransition::DrawSRV(snap);
                    if (followCamera) {
                        const float c = std::cos(f.cameraAngle);
                        const float s = std::sin(f.cameraAngle);
                        const ImVec2 uvRight((c * Ax - s * Bx) / kGlanceRadius,
                                             (c * Ay - s * By) / kGlanceRadius);
                        const ImVec2 uvDown((-s * Ax - c * Bx) / kGlanceRadius,
                                            (-s * Ay - c * By) / kGlanceRadius);
                        if (!DrawRotatedGlanceBackdrop(
                                dl, boundary, square, uvCenter, uvRight, uvDown,
                                imageSRV, FadeCol(MapDraw::kGlanceBackdropTint, a_alpha))) {
                            backdropResult = "outside snapshot UV";
                        }
                    } else {
                        dl->AddImageRounded(reinterpret_cast<ImTextureID>(imageSRV),
                                            boundary.Min(), boundary.Max(), uv0, uv1,
                                            FadeCol(MapDraw::kGlanceBackdropTint, a_alpha),
                                             boundary.rounding, ImDrawFlags_RoundCornersAll);
                    }
                    MinimapSnapshotTransition::EndDraw(dl, easeDraw);

                    if (!_glanceBackdropActive) {
                        _glanceBackdropActive = true;
                        SKSE::log::info(
                            "MinimapRenderer: Glance backdrop active. worldRadius={} uvCenter=({:.3f},{:.3f}) uHalf={:.3f} vHalf={:.3f} uv=({:.3f},{:.3f})-({:.3f},{:.3f})",
                            glanceWorldRadius, uvCenter.x, uvCenter.y, uHalf, vHalf,
                            uv0.x, uv0.y, uv1.x, uv1.y);
                    }
                }
            }
        } else {
            _glanceBackdropActive = false;
        }
        if (settings->minimapSnapshotLog) {
            static std::chrono::steady_clock::time_point lastDiag{};
            const auto now = std::chrono::steady_clock::now();
            if (now - lastDiag >= std::chrono::seconds(5)) {
                lastDiag = now;
                SKSE::log::info("MinimapSnapshot: Glance result={} currentCell={:08X} snapshotCell={:08X} currentWorld={:08X} snapshotWorld={:08X} srv={} alpha={} uv=({},{})-({},{}) pending={}",
                    backdropResult, f.cellID, snap.cellID, f.worldSpaceID, snap.worldSpaceID,
                    static_cast<void*>(snap.srv), a_alpha, diagUV0.x, diagUV0.y, diagUV1.x, diagUV1.y, _refreshPending.load());
            }
        }
    }

    const bool glanceBackdropShown = snap.valid && snap.srv &&
        (f.isExterior ? (snap.isExterior && snap.worldSpaceID == f.worldSpaceID)
                      : (!snap.isExterior && snap.cellID == f.cellID));
    const float glanceNorthRot = glanceBackdropShown ? snap.northRot : 0.0f;
    const float glanceRotation = followCamera ? f.cameraAngle : glanceNorthRot;
    AutomationTelemetry::Radar(f.playerPos, a_alpha, center, kGlanceRadius, glanceScale, glanceRotation);

    // Combat pulse: rim lerps gold->red on _combatFade, red breathes with a
    // ~1.4s sine and thickens slightly so it reads without being loud.
    {
        const float combatT = SmoothstepLocal(_combatFade);
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.5f);
        const float redT = combatT * (0.45f + 0.55f * pulse);
        // User border color is the BASE of the lerp; combat still drives it to red.
        const ImU32 rimCol = MapDraw::LerpColor(Settings::GetSingleton()->BorderCol(),
                                                MapTheme::kCombatBorder, redT);
        const float rimW = UIScale::Px(1.5f + 1.0f * combatT * pulse);
        if (square) {
            dl->AddRect(boundary.Min(), boundary.Max(), FadeCol(rimCol, a_alpha), boundary.rounding, 0, rimW);
        } else {
            dl->AddCircle(center, halfSize, FadeCol(rimCol, a_alpha), 64, rimW);
        }
    }

    std::vector<std::vector<ImVec2>> visibleSegments;
    std::vector<std::vector<float>> visibleSegmentZ;
    const bool showTrail = settings->enablePathTracking && settings->navGlanceShowTrail;
    if (showTrail) {
        visibleSegments.reserve(f.segments.size());
        visibleSegmentZ.reserve(f.segments.size());
        for (const auto& seg : f.segments) {
            std::vector<ImVec2> pts;
            std::vector<float>  ptsZ;
            pts.reserve(seg.size());
            ptsZ.reserve(seg.size());
            for (const auto& p : seg) {
                ImVec2 rp = WorldToGlance(p, f.playerPos, glanceScale, glanceRotation);
                pts.emplace_back(center.x + rp.x, center.y + rp.y);
                ptsZ.push_back(p.z);
            }
            if (!pts.empty()) {
                visibleSegments.push_back(std::move(pts));
                visibleSegmentZ.push_back(std::move(ptsZ));
            }
        }
    }
    if (showTrail) {
        MapDraw::DrawPathTrail(dl, visibleSegments,
                               TrailHeadNearPlayer(f.segments, f.playerPos),
                               PathTracker::GetSingleton()->IsTracking(),
                               center, halfSize,
                               0.0f, true, nullptr,
                               0,
                               a_alpha,
                               &visibleSegmentZ, f.playerPos.z,
                               &boundary);
    }

    const bool glanceSnapSameSpace = f.isExterior
        ? (snap.isExterior && snap.worldSpaceID == f.worldSpaceID)
        : (!snap.isExterior && snap.cellID == f.cellID);
    const auto& entityVisuals = MinimapEntityTransition::Update(snap, f);
    if (snap.valid && glanceSnapSameSpace) {
        auto* s = Settings::GetSingleton();
        if (s->GlanceAnyEntity()) {
            const float iconSz = UIScale::Px(static_cast<float>(s->navGlanceEntityIconSize));
            const float sizeScale = iconSz / 7.0f;
            const float half = iconSz * 0.5f;
            const float fxPad = MapDraw::IconFxBoundaryExtent(iconSz, iconSz);
            const float entityExtent = std::hypot(half + fxPad, half + fxPad);
            for (const auto& visual : entityVisuals) {
                const auto& e = visual.marker;
                if (!e.discovered) continue;  // proximity fog: never seen it yet
                if (!s->GlanceSubEnabled(e.category, e.subCategory)) continue;  // flat per-view subtype gate

                ImVec2 rp = WorldToGlance(e.position, f.playerPos, glanceScale, glanceRotation);
                const ImVec2 projected(center.x + rp.x, center.y + rp.y);
                if (!boundary.Contains(projected, entityExtent)) continue;  // off-radar: don't draw
                const float edgeAlpha = e.category == EntityMarkerSource::Category::Door ? 1.0f :
                    MinimapEntityTransition::EdgeAlpha(boundary, projected, entityExtent, UIScale::Px(12.0f));
                const float entityAlpha = a_alpha * visual.alpha * edgeAlpha;
                if (entityAlpha <= 0.0f) continue;
                ImVec2 g = projected;

                // Subcategory PNG first (e.g. "barrel" over generic "container"),
                // then the category fallback PNG, then the vector glyph below.
                ID3D11ShaderResourceView* icon = nullptr;
                if (const char* sub = EntityMarkerSource::IconNameFor(e.category, e.subCategory))
                    icon = TextureManager::GetIconByName(sub);
                if (!icon)
                    icon = TextureManager::GetIconByName(EntityMarkerSource::CategoryIconName(e.category));
                if (icon) {
                    // Entity-specific 3D and shadow controls leave map pins alone.
                    DrawEntityIconBadge(dl, icon, g, iconSz, entityAlpha,
                                        e.category != EntityMarkerSource::Category::Door);
                    continue;
                }

                // Fallback: PNG missing/failed to load, draw the old vector glyph.
                using Cat = EntityMarkerSource::Category;
                switch (e.category) {
                    case Cat::Corpse: {
                        // White X, ~70% of Inspect (arm 4 -> 3, stroke 2 -> 1.5).
                        const float arm = 3.0f * sizeScale;
                        dl->AddLine(ImVec2(g.x - arm, g.y - arm), ImVec2(g.x + arm, g.y + arm), FadeCol(MapTheme::kEntityCorpse, entityAlpha), UIScale::Px(1.5f));
                        dl->AddLine(ImVec2(g.x - arm, g.y + arm), ImVec2(g.x + arm, g.y - arm), FadeCol(MapTheme::kEntityCorpse, entityAlpha), UIScale::Px(1.5f));
                        break;
                    }
                    case Cat::Door: {
                        // Tan vertical rounded rect, ~70% (5x9 -> 3.5x6.5).
                        const float hw = 1.75f * sizeScale, hh = 3.25f * sizeScale;
                        dl->AddRectFilled(ImVec2(g.x - hw, g.y - hh), ImVec2(g.x + hw, g.y + hh),
                                          FadeCol(MapTheme::kEntityDoor, entityAlpha), UIScale::Px(1.0f));
                        break;
                    }
                    case Cat::Container: {
                        // Orange square, ~70% (7 -> 5), filled + outline.
                        const float hs = 2.5f * sizeScale;
                        dl->AddRectFilled(ImVec2(g.x - hs, g.y - hs), ImVec2(g.x + hs, g.y + hs),
                                          FadeCol(MapTheme::kEntityContainer, entityAlpha));
                        dl->AddRect(ImVec2(g.x - hs, g.y - hs), ImVec2(g.x + hs, g.y + hs),
                                    FadeCol(IM_COL32(20, 15, 5, 235), entityAlpha));
                        break;
                    }
                    case Cat::LooseLoot: {
                        // Orange dot, ~70% (4 -> 3).
                        MapDraw::DrawDotFx(dl, g, 3.0f * sizeScale, FadeCol(MapTheme::kEntityLoot, entityAlpha));
                        break;
                    }
                    case Cat::Ingredient: {
                        // Green dot, matches the loot dot's ~70% scale.
                        MapDraw::DrawDotFx(dl, g, 3.0f * sizeScale, FadeCol(MapTheme::kEntityIngredient, entityAlpha));
                        break;
                    }
                }
            }
        }
    }

    if (Settings::GetSingleton()->navActorAnyVisible()) {
        auto* s = Settings::GetSingleton();
        const float actorDotRadius = UIScale::Px(static_cast<float>(s->navGlanceActorDotSize));
        const float actorExtent = actorDotRadius + UIScale::Px(1.5f);
        for (const auto& [formID, ghost] : _actorGhosts) {
            using Al = ActorMarkerSource::ActorMarker::Allegiance;
            if (Al::Ally == ghost.allegiance && !s->navActorShowAlly) continue;
            if (Al::Hostile == ghost.allegiance && !s->navActorShowHostile) continue;
            if (Al::Neutral == ghost.allegiance && !s->navActorShowNeutral) continue;
            if (Al::Animal == ghost.allegiance && !s->navActorShowAnimal) continue;

            ImVec2 rp = WorldToGlance(ghost.position, f.playerPos, glanceScale, glanceRotation);
            const ImVec2 projected(center.x + rp.x, center.y + rp.y);
            if (!boundary.Contains(projected, actorExtent)) continue;  // off-radar: don't draw
            ImVec2 g = projected;

            const ImU32 col = Al::Ally == ghost.allegiance ? MapTheme::kActorAlly
                             : Al::Hostile == ghost.allegiance ? MapTheme::kActorHostile
                             : Al::Animal == ghost.allegiance ? MapTheme::kActorAnimal
                             : MapTheme::kActorNeutral;
            // ghost.alpha carries fade-in AND fade-out (merge fade machine).
            MapDraw::DrawDotFx(dl, g, actorDotRadius, FadeCol(col, a_alpha * ghost.alpha));
        }
    }

    for (const MapMarker* mp : CollectVisibleMarkers(f.markers, f.isExterior,
                                                     f.worldSpaceID, f.cellID, f.playerPos,
                                                     true)) {
        const MapMarker& m = *mp;
        ImVec2 rp = WorldToGlance(m.position, f.playerPos, glanceScale, glanceRotation);
        const float pinDiameter = UIScale::Px(static_cast<float>(Settings::GetSingleton()->navGlancePinSize));
        const float pinScale = pinDiameter / (2.0f * MapDraw::kPinRadius);
        const std::uint32_t drawIconType = m.isDestination ? 0u : m.iconType;
        auto* pinSrv = m.isDestination ? TextureManager::GetIconByName("destination")
                                       : TextureManager::GetCategoryIcon(m.iconType);

        const ImVec2 projected(center.x + rp.x, center.y + rp.y);
        const ImVec2 drawPos = boundary.Contains(projected)
            ? projected
            : boundary.ClampFromCenter(projected, kGlanceRimMarkerInset);

        AutomationTelemetry::RadarMarker(m.id, m.position, projected, drawPos);
        MapDraw::DrawPin(dl, drawPos.x, drawPos.y, 0.0f, drawIconType, pinSrv,
                         pinScale, Settings::GetSingleton()->PinTintCol(), a_alpha, UIScale::Master());
        if (m.isDestination) {
            const float timeSec = std::chrono::duration<float>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            MapDraw::DrawDestinationRing(dl, drawPos.x, drawPos.y, timeSec, false, UIScale::Master());
        }
    }

    // The engine frame distinguishes direct targets from retargeted load doors.
    // Same WorldToGlance + edge-clamp as custom markers.
    auto* directQuestSrv = TextureManager::GetIconByName("activemarker");
    auto* doorQuestSrv = TextureManager::GetIconByName("active");
    if (directQuestSrv || doorQuestSrv) {
        const float questHalf = UIScale::Px(20.0f * settings->navGlancePinSize / (2.0f * MapDraw::kPinRadius));
        // On-radar targets take the limited slots before rim-clamped targets.
        // Within each group, the nearest targets win; ties keep compass order.
        const int maxQuest = settings->navMaxQuestMarkers;
        std::vector<QuestDrawCandidate> candidates;
        if (maxQuest > 0) candidates.reserve(f.questTargets.size());
        for (std::size_t i = 0; maxQuest > 0 && i < f.questTargets.size(); ++i) {
            const auto& quest = f.questTargets[i];
            auto* questSrv = quest.isDoor ? doorQuestSrv : (directQuestSrv ? directQuestSrv : doorQuestSrv);
            if (!questSrv) continue;
            const ImVec2 rp = WorldToGlance(quest.position, f.playerPos, glanceScale, glanceRotation);
            const ImVec2 projected(center.x + rp.x, center.y + rp.y);
            const bool inView = boundary.Contains(projected);
            const ImVec2 drawPos = inView
                ? projected
                : boundary.ClampFromCenter(projected, kGlanceRimMarkerInset);
            const float dx = quest.position.x - f.playerPos.x;
            const float dy = quest.position.y - f.playerPos.y;
            candidates.push_back({ &quest, drawPos, dx * dx + dy * dy, i, inView });
        }
        std::sort(candidates.begin(), candidates.end());
        const auto drawCount = (std::min)(candidates.size(), static_cast<std::size_t>(maxQuest));
        for (std::size_t i = drawCount; i > 0; --i) {
            const auto& candidate = candidates[i - 1];
            auto* questSrv = candidate.marker->isDoor ? doorQuestSrv
                : (directQuestSrv ? directQuestSrv : doorQuestSrv);
            DrawQuestTargetIcon(dl, questSrv, candidate.screenPos.x, candidate.screenPos.y, questHalf, a_alpha);
        }
    }

    // Camera view cone under the arrow. Follow Camera keeps it at 12 o'clock.
    const float coneAngle = followCamera ? 0.0f : f.cameraAngle - glanceNorthRot;
    DrawViewCone(dl, boundary, coneAngle, a_alpha);

    // Draw player arrow using existing MinimapRenderer player-angle math.
    const float arrowAngle = followCamera
        ? f.playerAngle - f.cameraAngle
        : f.playerAngle - glanceNorthRot;
    float c = std::cos(arrowAngle);
    float s = std::sin(arrowAngle);

    ImVec2 forward(s, -c);
    ImVec2 right(c, s);

    const float arrF12 = UIScale::Px(12.0f);
    const float arrF10 = UIScale::Px(10.0f);
    const float arrR8  = UIScale::Px(8.0f);
    const float arrF4  = UIScale::Px(4.0f);

    ImVec2 p1(center.x + forward.x * arrF12, center.y + forward.y * arrF12);
    ImVec2 p2(center.x - forward.x * arrF10 - right.x * arrR8, center.y - forward.y * arrF10 - right.y * arrR8);
    ImVec2 p3(center.x - forward.x * arrF4, center.y - forward.y * arrF4);
    ImVec2 p4(center.x - forward.x * arrF10 + right.x * arrR8, center.y - forward.y * arrF10 + right.y * arrR8);

    // Yellow while retracing already-tracked ground (_retraceFade ramped in Draw()).
    const float retraceT = SmoothstepLocal(_retraceFade);
    const ImU32 arrowFill = MapDraw::LerpColor(IM_COL32(255, 255, 255, 255), MapTheme::kRetraceArrow, retraceT);
    dl->AddQuadFilled(p1, p2, p3, p4, FadeCol(arrowFill, a_alpha));
    dl->AddQuad(p1, p2, p3, p4, FadeCol(IM_COL32(0, 0, 0, 255), a_alpha), UIScale::Px(1.5f));

    if (settings->navGlanceShowCardinals) {
        constexpr float kDirs[4][2] = { { 0.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, -1.0f }, { -1.0f, 0.0f } };
        for (int i = 0; i < 4; ++i) {
            RE::NiPoint3 probe = f.playerPos;
            probe.x += kDirs[i][0];
            probe.y += kDirs[i][1];
            const ImVec2 v = WorldToGlance(probe, f.playerPos, 1.0f, glanceRotation);
            const ImVec2 at = boundary.RayIntersection(std::atan2(v.y, v.x),
                                                       -(cardinalGap + cardinalHalfDiag));
            const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(cardinalFontSize, FLT_MAX, 0.0f, cardinalText[i]);
            const ImVec2 tpos(at.x - sz.x * 0.5f, at.y - sz.y * 0.5f);
            dl->AddText(ImGui::GetFont(), cardinalFontSize,
                        ImVec2(tpos.x + UIScale::Px(1.0f), tpos.y + UIScale::Px(1.0f)),
                        FadeCol(IM_COL32(0, 0, 0, 200), a_alpha), cardinalText[i]);
            // North keeps the title colour so it reads first at a glance.
            dl->AddText(ImGui::GetFont(), cardinalFontSize, tpos,
                        FadeCol(i == 0 ? MapTheme::kHeaderText : MapTheme::kBodyText, a_alpha),
                        cardinalText[i]);
        }
    }

    if (settings->navGlanceShowLocation && !f.locationName.empty()) {
        ImFont* titleFont = FontManager::GetSingleton()->GetHeadingFont();
        if (!titleFont) titleFont = ImGui::GetFont();
        const float titleSize = titleFont->FontSize * UIScale::Font() *
            std::clamp(settings->titleScale, 0.5f, 3.0f);
        const char* title = f.locationName.c_str();
        const ImVec2 tsz = titleFont->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, title);
        const float gap = UIScale::Px(4.0f);
        float ty = center.y + halfSize + cardinalRing + gap;
        if (ty + tsz.y > disp.y) {
            ty = center.y - halfSize - cardinalRing - gap - tsz.y;
        }
        const float tx = std::clamp(center.x - tsz.x * 0.5f, 0.0f, (std::max)(0.0f, disp.x - tsz.x));
        dl->AddText(titleFont, titleSize, ImVec2(tx + UIScale::Px(1.0f), ty + UIScale::Px(1.0f)),
                    FadeCol(IM_COL32(0, 0, 0, 200), a_alpha), title);
        dl->AddText(titleFont, titleSize, ImVec2(tx, ty),
                    FadeCol(MapTheme::kHeaderText, a_alpha), title);
    }
}

// Large local-map overlay for Inspect mode.
void MinimapRenderer::DrawInspect(const NavigationFrame& f, float a_alpha) {
    SnapshotProjection snap;
    {
        std::lock_guard<std::mutex> lock(_snapshotMutex);
        snap = _snapshot;
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    float pct = Settings::GetSingleton()->navInspectSizePct / 100.0f;

    const float boxW = disp.x * pct;
    const float boxH = disp.y * pct;
    const float overlayAspect = (boxH > 1e-3f) ? (boxW / boxH) : (16.0f / 9.0f);
    _overlayAspect.store(overlayAspect);

    const float cropW = 1.0f;  // full texture; kept so the virtual-rect math below is unchanged
    const float cropH = 1.0f;
    ImVec2 uv0(0.0f, 0.0f);
    ImVec2 uv1(1.0f, 1.0f);

    const ImVec2 min((disp.x - boxW) * 0.5f, (disp.y - boxH) * 0.5f);
    const ImVec2 max(min.x + boxW, min.y + boxH);

    // The engine snapshot contains transparent pixels. Match the Radar backfill
    // opacity so both surfaces composite consistently.
    dl->AddRectFilled(min, max, FadeCol(IM_COL32(8, 9, 10, 230), a_alpha), UIScale::Px(MapTheme::kPanelRounding));
    dl->AddRect(min, max, FadeCol(Settings::GetSingleton()->BorderCol(), a_alpha), UIScale::Px(MapTheme::kPanelRounding), 0, UIScale::Px(MapTheme::kPanelBorderW));

    if (snap.valid && snap.srv) {
        const ImU32 tint = snap.stale ? IM_COL32(255, 255, 255, 150) : IM_COL32_WHITE;
        auto* easeDraw = MinimapSnapshotTransition::BeginDraw(dl, snap);
        dl->AddImage(reinterpret_cast<ImTextureID>(MinimapSnapshotTransition::DrawSRV(snap)), min, max, uv0, uv1, FadeCol(tint, a_alpha));
        MinimapSnapshotTransition::EndDraw(dl, easeDraw);
    }
    if (Settings::GetSingleton()->minimapSnapshotLog) {
        static std::chrono::steady_clock::time_point lastDiag{};
        const auto now = std::chrono::steady_clock::now();
        if (now - lastDiag >= std::chrono::seconds(5)) {
            lastDiag = now;
            SKSE::log::info("MinimapSnapshot: Inspect result={} cell={:08X} srv={} stale={} alpha={} uv=({},{})-({},{})",
                !snap.valid ? "invalid snapshot" : !snap.srv ? "missing SRV" : "image submitted",
                snap.cellID, static_cast<void*>(snap.srv), snap.stale, a_alpha, uv0.x, uv0.y, uv1.x, uv1.y);
        }
    }

    float vW = boxW / cropW;
    float vH = boxH / cropH;
    const ImVec2 vMin(min.x - uv0.x * vW, min.y - uv0.y * vH);
    const ImVec2 vSize(vW, vH);

    if (snap.valid) {
        std::vector<std::vector<ImVec2>> visibleSegments;
        std::vector<std::vector<float>> visibleSegmentZ;
        const bool showTrail = Settings::GetSingleton()->enablePathTracking &&
                               Settings::GetSingleton()->navInspectShowTrail;
        if (showTrail) {
            visibleSegments.reserve(f.segments.size());
            visibleSegmentZ.reserve(f.segments.size());
            for (const auto& seg : f.segments) {
                std::vector<ImVec2> pts;
                std::vector<float>  ptsZ;
                pts.reserve(seg.size());
                ptsZ.reserve(seg.size());
                for (const auto& p : seg) {
                    ImVec2 screen;
                    if (ProjectWithSnapshot(snap, p, vMin, vSize, screen)) {
                        pts.push_back(screen);
                        ptsZ.push_back(p.z);
                    }
                }
                if (!pts.empty()) {
                    visibleSegments.push_back(std::move(pts));
                    visibleSegmentZ.push_back(std::move(ptsZ));
                }
            }
        }
        if (showTrail) {
            dl->PushClipRect(min, max, true);
            MapDraw::DrawPathTrail(dl, visibleSegments,
                                   TrailHeadNearPlayer(f.segments, f.playerPos),
                                   PathTracker::GetSingleton()->IsTracking(),
                                   ImVec2(0.0f, 0.0f), 0.0f,
                                   f.playerAngle - snap.northRot, true,
                                   nullptr, 0, a_alpha,
                                   &visibleSegmentZ, f.playerPos.z);
            dl->PopClipRect();
        }

        {
            auto* s = Settings::GetSingleton();
            if (s->InspectAnyEntity()) {
                // Icon box side length in screen px, user-configurable
                // (Settings::navInspectEntityIconSize, 10-80 in SettingsMenu).
                const float half = UIScale::Px(static_cast<float>(s->navInspectEntityIconSize) * 0.5f);

                dl->PushClipRect(min, max, true);
                for (const auto& e : snap.entities) {
                    if (!e.discovered) continue;  // proximity fog: never seen it yet
                    using Cat = EntityMarkerSource::Category;
                    if (!s->InspectSubEnabled(e.category, e.subCategory)) continue;  // flat per-view subtype gate

                    ImVec2 g;
                    if (!ProjectWithSnapshot(snap, e.position, vMin, vSize, g)) continue;

                    // Subcategory PNG first, then category fallback, then vector glyph.
                    ID3D11ShaderResourceView* icon = nullptr;
                    if (const char* sub = EntityMarkerSource::IconNameFor(e.category, e.subCategory))
                        icon = TextureManager::GetIconByName(sub);
                    if (!icon)
                        icon = TextureManager::GetIconByName(EntityMarkerSource::CategoryIconName(e.category));
                    if (icon) {
                        // Same badge style as Glance, using Inspect's own size.
                        DrawEntityIconBadge(dl, icon, g, half * 2.0f, a_alpha,
                                            e.category != Cat::Door);
                        continue;
                    }

                    // Fallback: PNG missing/failed to load, draw the old vector glyph.
                    switch (e.category) {
                        case Cat::Corpse: {
                            // White X, two 2px strokes.
                            const float arm = UIScale::Px(4.0f);
                            const float stroke = UIScale::Px(2.0f);
                            dl->AddLine(ImVec2(g.x - arm, g.y - arm), ImVec2(g.x + arm, g.y + arm), FadeCol(MapTheme::kEntityCorpse, a_alpha), stroke);
                            dl->AddLine(ImVec2(g.x - arm, g.y + arm), ImVec2(g.x + arm, g.y - arm), FadeCol(MapTheme::kEntityCorpse, a_alpha), stroke);
                            break;
                        }
                        case Cat::Door: {
                            // Tan vertical rounded rect 5x9.
                            const float hw = UIScale::Px(2.5f), hh = UIScale::Px(4.5f);
                            dl->AddRectFilled(ImVec2(g.x - hw, g.y - hh), ImVec2(g.x + hw, g.y + hh),
                                              FadeCol(MapTheme::kEntityDoor, a_alpha), UIScale::Px(1.5f));
                            break;
                        }
                        case Cat::Container: {
                            // Orange 7px square, filled + outline.
                            const float hs = UIScale::Px(3.5f);
                            dl->AddRectFilled(ImVec2(g.x - hs, g.y - hs), ImVec2(g.x + hs, g.y + hs),
                                              FadeCol(MapTheme::kEntityContainer, a_alpha));
                            dl->AddRect(ImVec2(g.x - hs, g.y - hs), ImVec2(g.x + hs, g.y + hs),
                                        FadeCol(IM_COL32(20, 15, 5, 235), a_alpha));
                            break;
                        }
                        case Cat::LooseLoot: {
                            // Orange 4px filled circle.
                            MapDraw::DrawDotFx(dl, g, UIScale::Px(4.0f), FadeCol(MapTheme::kEntityLoot, a_alpha));
                            break;
                        }
                        case Cat::Ingredient: {
                            // Green 4px filled circle, mirrors the loot dot.
                            MapDraw::DrawDotFx(dl, g, UIScale::Px(4.0f), FadeCol(MapTheme::kEntityIngredient, a_alpha));
                            break;
                        }
                    }
                }
                dl->PopClipRect();
            }
        }

        if (Settings::GetSingleton()->navActorAnyVisible()) {
            auto* s = Settings::GetSingleton();
            const float actorDotRadius = UIScale::Px(static_cast<float>(s->navInspectActorDotSize));
            dl->PushClipRect(min, max, true);
            for (const auto& [formID, ghost] : _actorGhosts) {
                using Al = ActorMarkerSource::ActorMarker::Allegiance;
                if (Al::Ally == ghost.allegiance && !s->navActorShowAlly) continue;
                if (Al::Hostile == ghost.allegiance && !s->navActorShowHostile) continue;
                if (Al::Neutral == ghost.allegiance && !s->navActorShowNeutral) continue;
                if (Al::Animal == ghost.allegiance && !s->navActorShowAnimal) continue;

                ImVec2 g;
                if (!ProjectWithSnapshot(snap, ghost.position, vMin, vSize, g)) continue;

                const ImU32 col = Al::Ally == ghost.allegiance ? MapTheme::kActorAlly
                                 : Al::Hostile == ghost.allegiance ? MapTheme::kActorHostile
                                 : Al::Animal == ghost.allegiance ? MapTheme::kActorAnimal
                                 : MapTheme::kActorNeutral;
                // ghost.alpha carries fade-in AND fade-out (merge fade machine).
                MapDraw::DrawDotFx(dl, g, actorDotRadius, FadeCol(col, a_alpha * ghost.alpha));
            }
            dl->PopClipRect();
        }

        dl->PushClipRect(min, max, true);
        ImVec2 pScreen;
        if (ProjectWithSnapshot(snap, f.playerPos, vMin, vSize, pScreen)) {
            // Camera view cone under the arrow, same north-relative correction as the arrow.
            DrawViewCone(dl, pScreen, f.cameraAngle - snap.northRot, UIScale::Px(48.0f), a_alpha);
            // Draw player arrow
            float c = std::cos(f.playerAngle - snap.northRot); // relative to snapshot north
            float s = std::sin(f.playerAngle - snap.northRot);
            ImVec2 forward(s, -c);
            ImVec2 right(c, s);
            const float arrF12 = UIScale::Px(12.0f);
            const float arrF10 = UIScale::Px(10.0f);
            const float arrR8  = UIScale::Px(8.0f);
            const float arrF4  = UIScale::Px(4.0f);
            ImVec2 p1(pScreen.x + forward.x * arrF12, pScreen.y + forward.y * arrF12);
            ImVec2 p2(pScreen.x - forward.x * arrF10 - right.x * arrR8, pScreen.y - forward.y * arrF10 - right.y * arrR8);
            ImVec2 p3(pScreen.x - forward.x * arrF4, pScreen.y - forward.y * arrF4);
            ImVec2 p4(pScreen.x - forward.x * arrF10 + right.x * arrR8, pScreen.y - forward.y * arrF10 + right.y * arrR8);
            const float retraceT = SmoothstepLocal(_retraceFade);
            const ImU32 arrowFill = MapDraw::LerpColor(IM_COL32(255, 255, 255, 255), MapTheme::kRetraceArrow, retraceT);
            dl->AddQuadFilled(p1, p2, p3, p4, FadeCol(arrowFill, a_alpha));
            dl->AddQuad(p1, p2, p3, p4, FadeCol(IM_COL32(0, 0, 0, 255), a_alpha), UIScale::Px(1.5f));
        }

        for (const MapMarker* mp : CollectVisibleMarkers(f.markers, f.isExterior,
                                                         f.worldSpaceID, f.cellID, f.playerPos,
                                                         false)) {
            const MapMarker& m = *mp;
            RE::NiPoint3 projPos{ m.position.x, m.position.y, f.playerPos.z };
            ImVec2 mScreen;
            if (ProjectWithSnapshot(snap, projPos, vMin, vSize, mScreen)) {
                const std::uint32_t drawIconType = m.isDestination ? 0u : m.iconType;
                auto* pinSrv = m.isDestination ? TextureManager::GetIconByName("destination")
                                               : TextureManager::GetCategoryIcon(m.iconType);
                const float pinDiameter = UIScale::Px(static_cast<float>(Settings::GetSingleton()->navInspectPinSize));
                const float pinScale = pinDiameter / (2.0f * MapDraw::kPinRadius);
                // Shared pin tint, same reasoning as the Glance loop above.
                MapDraw::DrawPin(dl, mScreen.x, mScreen.y, 0.0f, drawIconType, pinSrv,
                                 pinScale,
                                 Settings::GetSingleton()->PinTintCol(), a_alpha, UIScale::Master());
                if (m.isDestination) {
                    const float timeSec = std::chrono::duration<float>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    MapDraw::DrawDestinationRing(dl, mScreen.x, mScreen.y, timeSec, false, UIScale::Master());
                }
            }
        }

        // Match the engine's direct-target or load-door frame. Same snapshot
        // projection as custom markers, so they land in the frozen cell frame.
        auto* directQuestSrv = TextureManager::GetIconByName("activemarker");
        auto* doorQuestSrv = TextureManager::GetIconByName("active");
        if (directQuestSrv || doorQuestSrv) {
            const float questHalf = UIScale::Px(20.0f * Settings::GetSingleton()->navInspectPinSize / (2.0f * MapDraw::kPinRadius));
            // Only targets on the visible map use a slot. Prioritize nearby
            // targets, keeping compass order when distances tie.
            const int maxQuest = Settings::GetSingleton()->navMaxQuestMarkers;
            std::vector<QuestDrawCandidate> candidates;
            if (maxQuest > 0) candidates.reserve(f.questTargets.size());
            for (std::size_t i = 0; maxQuest > 0 && i < f.questTargets.size(); ++i) {
                const auto& quest = f.questTargets[i];
                auto* questSrv = quest.isDoor ? doorQuestSrv : (directQuestSrv ? directQuestSrv : doorQuestSrv);
                if (!questSrv) continue;
                ImVec2 qScreen;
                if (!ProjectWithSnapshot(snap, quest.position, vMin, vSize, qScreen)) continue;
                if (qScreen.x < min.x || qScreen.x > max.x ||
                    qScreen.y < min.y || qScreen.y > max.y) continue;
                const float dx = quest.position.x - f.playerPos.x;
                const float dy = quest.position.y - f.playerPos.y;
                candidates.push_back({ &quest, qScreen, dx * dx + dy * dy, i, true });
            }
            std::sort(candidates.begin(), candidates.end());
            const auto drawCount = (std::min)(candidates.size(), static_cast<std::size_t>(maxQuest));
            for (std::size_t i = drawCount; i > 0; --i) {
                const auto& candidate = candidates[i - 1];
                auto* questSrv = candidate.marker->isDoor ? doorQuestSrv
                    : (directQuestSrv ? directQuestSrv : doorQuestSrv);
                DrawQuestTargetIcon(dl, questSrv, candidate.screenPos.x, candidate.screenPos.y, questHalf, a_alpha);
            }
        }
        dl->PopClipRect();
    } else {
        // Fallback
        const float unavailableSize = ImGui::GetFont()->FontSize * UIScale::Font();
        dl->AddText(ImGui::GetFont(), unavailableSize, ImVec2(min.x + UIScale::Px(10.0f), min.y + UIScale::Px(10.0f)),
                    FadeCol(IM_COL32_WHITE, a_alpha), "Local map unavailable (waiting for snapshot)");
    }

    {
        const char* keyName =
            InputHandler::DIKToKeyName(Settings::GetSingleton()->navInspectModeToggleKey);
        const bool radarOn = _inspectMode.load() == InspectViewMode::Radar;
        // On/Off are separate keys rather than one pattern with two slots: some
        // languages inflect the surrounding words differently for each state.
        const std::string badgeStr = LocalizationManager::Format(
            radarOn ? "Navigation.RadarModeOn" : "Navigation.RadarModeOff", keyName);
        const char* badge = badgeStr.c_str();

        // Badge honors the global Title Scale, hint below the Body Scale, so
        // the Appearance sliders drive the overlay text like every other panel.
        const float badgeSize = ImGui::GetFont()->FontSize * UIScale::Font() * Settings::GetSingleton()->titleScale;
        const ImVec2 pad(UIScale::Px(8.0f), UIScale::Px(4.0f));
        const ImVec2 pos(min.x + UIScale::Px(10.0f), min.y + UIScale::Px(10.0f));
        const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(badgeSize, FLT_MAX, 0.0f, badge);
        dl->AddRectFilled(ImVec2(pos.x - pad.x, pos.y - pad.y),
                          ImVec2(pos.x + sz.x + pad.x, pos.y + sz.y + pad.y),
                          FadeCol(MapTheme::kPanelBg, a_alpha), UIScale::Px(MapTheme::kPanelRounding));
        dl->AddText(ImGui::GetFont(), badgeSize, pos,
                    FadeCol(radarOn ? MapTheme::kBodyTextHover : MapTheme::kMutedText, a_alpha), badge);

        const char* markKeyName =
            InputHandler::DIKToKeyName(Settings::GetSingleton()->navQuickMarkKey);
        const std::string hintStr = LocalizationManager::Format("Navigation.AddMarkerHint", markKeyName);
        const char* hint = hintStr.c_str();

        const float bodySize = ImGui::GetFont()->FontSize * UIScale::Font() * Settings::GetSingleton()->bodyScale;
        const ImVec2 hintPos(pos.x, pos.y + sz.y + pad.y + UIScale::Px(4.0f));
        dl->AddText(ImGui::GetFont(), bodySize, hintPos,
                   FadeCol(MapTheme::kMutedText, a_alpha), hint);

        const ImVec2 hintSize = ImGui::GetFont()->CalcTextSizeA(bodySize, FLT_MAX, 0.0f, hint);

        if (!f.isExterior && Settings::GetSingleton()->enablePathTracking) {
            const std::string trackBind = InputHandler::FormatHotkeyWithModifier(
                Settings::GetSingleton()->pathTrackKey,
                Settings::GetSingleton()->pathTrackModifier);
            const bool tracking = PathTracker::GetSingleton()->IsTracking();
            const std::string trackingText = LocalizationManager::Format(
                tracking ? "PathTracking.LegendOn" : "PathTracking.LegendPaused",
                trackBind);
            const ImVec2 trackingPos(hintPos.x, hintPos.y + hintSize.y + UIScale::Px(4.0f));
            const ImU32 trackingColor = tracking
                ? IM_COL32(100, 255, 100, 255)
                : IM_COL32(200, 200, 200, 255);
            dl->AddText(ImGui::GetFont(), bodySize, trackingPos,
                        FadeCol(trackingColor, a_alpha), trackingText.c_str());
        }
    }

    if (!f.locationName.empty()) {
        ImFont* titleFont = FontManager::GetSingleton()->GetHeadingFont();
        if (!titleFont) titleFont = ImGui::GetFont();
        const float titleSize = titleFont->FontSize * UIScale::Font() *
            std::clamp(Settings::GetSingleton()->titleScale, 0.5f, 3.0f);
        const char* title = f.locationName.c_str();
        const ImVec2 tsz = titleFont->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, title);
        // Scaled gap between the text baseline box and the border.
        const ImVec2 tpos((min.x + max.x) * 0.5f - tsz.x * 0.5f, min.y - tsz.y - UIScale::Px(8.0f));
        dl->AddText(titleFont, titleSize, ImVec2(tpos.x + UIScale::Px(1.0f), tpos.y + UIScale::Px(1.0f)),
                    FadeCol(IM_COL32(0, 0, 0, 200), a_alpha), title);
        dl->AddText(titleFont, titleSize, tpos,
                    FadeCol(MapTheme::kHeaderText, a_alpha), title);
    }

    if (!_quickMarkToastText.empty()) {
        const float age = std::chrono::duration<float>(
            std::chrono::steady_clock::now() - _quickMarkToastTime).count();
        constexpr float kPopT = 0.15f, kHoldT = 1.25f, kEndT = 1.8f;
        if (age < kEndT) {
            // Overshoot ease on the way in (1.0 -> 1.12 -> 1.0) so the eye
            // catches it; linear fade out, no scale change while leaving.
            float scale = 1.0f;
            float fade = 1.0f;
            if (age < kPopT) {
                const float t = age / kPopT;
                scale = 0.75f + 0.37f * t - 0.12f * t * t;
                fade = t;
            } else if (age > kHoldT) {
                fade = 1.0f - (age - kHoldT) / (kEndT - kHoldT);
            }
            const float toastAlpha = a_alpha * fade;
            const float toastSize =
                ImGui::GetFont()->FontSize * UIScale::Font() * Settings::GetSingleton()->titleScale * scale;
            const char* toast = _quickMarkToastText.c_str();
            const ImVec2 tsz = ImGui::GetFont()->CalcTextSizeA(toastSize, FLT_MAX, 0.0f, toast);
            const ImVec2 tpad(UIScale::Px(10.0f), UIScale::Px(5.0f));
            // Centered on the overlay rect, 16px below its top edge. Recentred
            // per frame because the pop scale changes the measured width.
            const ImVec2 tpos((min.x + max.x) * 0.5f - tsz.x * 0.5f, min.y + UIScale::Px(16.0f));
            dl->AddRectFilled(ImVec2(tpos.x - tpad.x, tpos.y - tpad.y),
                              ImVec2(tpos.x + tsz.x + tpad.x, tpos.y + tsz.y + tpad.y),
                              FadeCol(MapTheme::kPanelBg, toastAlpha), UIScale::Px(MapTheme::kPanelRounding));
            dl->AddRect(ImVec2(tpos.x - tpad.x, tpos.y - tpad.y),
                        ImVec2(tpos.x + tsz.x + tpad.x, tpos.y + tsz.y + tpad.y),
                        FadeCol(MapTheme::kPanelBorder, toastAlpha),
                        UIScale::Px(MapTheme::kPanelRounding), 0, UIScale::Px(MapTheme::kPanelBorderW));
            dl->AddText(ImGui::GetFont(), toastSize, tpos,
                        FadeCol(MapTheme::kBodyTextHover, toastAlpha), toast);
        } else {
            _quickMarkToastText.clear();
        }
    }
}

void MinimapRenderer::Install() {
    REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_HUDMenu[0] };
    _origPreDisplay = reinterpret_cast<PreDisplayFn>(vtbl.write_vfunc(0x7, &HookedPreDisplay));
    SKSE::log::info("MinimapRenderer: hooked HUDMenu::PreDisplay (IMenu vtable idx 7)");
}

void MinimapRenderer::HookedPreDisplay(RE::IMenu* a_this) {
    auto* self = MinimapRenderer::GetSingleton();
    self->_origPreDisplay(a_this);

    if (!Settings::GetSingleton()->navRefreshOnInspect) {
        self->_refreshPending.store(false);
        return;
    }

    if (!self->_refreshPending.exchange(false)) return;
    if (Settings::GetSingleton()->minimapSnapshotLog)
        SKSE::log::info("MinimapSnapshot: PreDisplay consumed refresh");
    self->RenderOneSnapshot();
}

MinimapRenderer::FrustumRect MinimapRenderer::ComputeInspectFrustum(
    InspectViewMode a_mode,
    float a_cellExtentX, float a_cellExtentY,
    float a_cellCenterX, float a_cellCenterY,
    const RE::NiPoint3& a_playerPos,
    float a_overlayAspect) const {
    FrustumRect r{};
    // Guard a degenerate overlay aspect (0-height rect etc.).
    if (a_overlayAspect < 1e-3f) a_overlayAspect = 16.0f / 9.0f;

    const char* modeName = "Cover";
    switch (a_mode) {
    case InspectViewMode::Radar: {
        modeName = "Radar";
        const float w = Settings::GetSingleton()->navInspectRadarRangeUnits;
        r.centerX = a_playerPos.x;
        r.centerY = a_playerPos.y;
        r.width = w;
        r.height = w / a_overlayAspect;
        break;
    }
    case InspectViewMode::Cover:
    default: {
        const float cellAspect = (a_cellExtentY > 1e-3f) ? (a_cellExtentX / a_cellExtentY)
                                                         : a_overlayAspect;
        float w, h;
        if (cellAspect > a_overlayAspect) {
            // Cell is wider than the overlay: width bound, grow height to fit aspect.
            w = a_cellExtentX;
            h = w / a_overlayAspect;
        } else {
            // Cell is taller: height bound, grow width.
            h = a_cellExtentY;
            w = h * a_overlayAspect;
        }
        r.centerX = a_cellCenterX;
        r.centerY = a_cellCenterY;
        r.width = w * 1.05f;
        r.height = h * 1.05f;
        break;
    }
    }

    // Muted (per-snapshot). Interior twin of the exterior frustum line below.
    (void)modeName;
    return r;
}

void MinimapRenderer::RenderOneSnapshot() {
    const auto perfStart = std::chrono::steady_clock::now();

    RenderTargetUpscaler::EnsureApplied();

    EnsureConstructed();
    if (!g_constructed) {
        SKSE::log::warn("MinimapRenderer: culling struct not constructed, skipping render");
        return;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* tes    = RE::TES::GetSingleton();
    if (!player || !tes) {
        SKSE::log::warn("MinimapRenderer: no player/TES, skipping render");
        return;
    }
    void* cam = CameraBase();

    // Cell classification, used by the framing code below and the snapshot metadata.
    auto* parentCell = player->GetParentCell();
    const bool isInterior = parentCell && parentCell->IsInteriorCell();
    const bool isExterior = parentCell && parentCell->IsExteriorCell();

    g_initNode(CullingBase());

    const float* playerPos = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(player) + 0x54);
    static const std::ptrdiff_t kTesBoundsBlockOffset =
        REL::Module::get().version().compare(SKSE::RUNTIME_SSE_1_6_1130) == std::strong_ordering::less ?
            0x2b0 : 0x2b8;
    static bool loggedTesBoundsLayout = false;
    if (Settings::GetSingleton()->minimapSnapshotLog && !loggedTesBoundsLayout) {
        loggedTesBoundsLayout = true;
        SKSE::log::info("MinimapSnapshot: TES bounds layout runtime={} offset=0x{:X}",
                        REL::Module::get().version().string("."), kTesBoundsBlockOffset);
    }
    auto* tesBlock = *reinterpret_cast<std::uint8_t**>(
        reinterpret_cast<std::uint8_t*>(tes) + kTesBoundsBlockOffset);

    g_camBegin(cam);
    g_camTarget(cam, playerPos);
    if (tesBlock) {
        g_camBounds(cam, reinterpret_cast<const float*>(tesBlock + 0x78), reinterpret_cast<const float*>(tesBlock + 0x84));
    }

    const float northRot = parentCell ? -parentCell->GetNorthRotation() : 0.0f;
    g_camApply(cam, northRot);

    const float* boundsMax = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(cam) + 0x38);
    const float* boundsMin = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(cam) + 0x44);
    const float trueExtentX = boundsMax[0] - boundsMin[0];
    const float trueExtentY = boundsMax[1] - boundsMin[1];
    const float cellCenterX = (boundsMax[0] + boundsMin[0]) * 0.5f;
    const float cellCenterY = (boundsMax[1] + boundsMin[1]) * 0.5f;

    if (isInterior) {
        const float pad = 2000.0f;
        float paddedMax[3] = { boundsMax[0] + pad, boundsMax[1] + pad, boundsMax[2] };
        float paddedMin[3] = { boundsMin[0] - pad, boundsMin[1] - pad, boundsMin[2] };
        g_camBounds(cam, paddedMax, paddedMin);
    }

    ImVec2 snapCropMin(0.0f, 0.0f);
    ImVec2 snapCropMax(1.0f, 1.0f);
    float snapCellX = trueExtentX;
    float snapCellY = trueExtentY;
    float snapFrustumHalfWidth = 0.0f;
    int snapshotFraming = 0;
    if (isExterior) {
        FrustumRect fr{};
        RE::NiPoint3 playerNi{ playerPos[0], playerPos[1], playerPos[2] };
        if (_inspectShown.load()) {
            snapshotFraming = 3;
            fr.width = Settings::GetSingleton()->navInspectRadarRangeUnits;
            fr.height = fr.width / _overlayAspect.load();
        } else {
            snapshotFraming = 1;
            // Glance patch (decision 2): isotropic 16:9, 1.5x pad over 2x range so
            // the UV-clamp crop in DrawGlance never clips before the next refresh.
            const float w = std::max(2.0f * Settings::GetSingleton()->navGlanceRangeUnits * 1.5f, 4000.0f);
            fr.width = w;
            fr.height = w / (1920.0f / 1080.0f);
        }
        fr.centerX = playerNi.x;
        fr.centerY = playerNi.y;
        g_camFrustum(cam, fr.width, fr.height);
        float center[3] = { fr.centerX, fr.centerY, playerPos[2] };
        g_camTarget(cam, center);
        snapCellX = fr.width;
        snapCellY = fr.height;
        snapFrustumHalfWidth = fr.width * 0.5f;
    } else if (trueExtentX > 0.0f && trueExtentY > 0.0f) {
        FrustumRect fr{};
        if (_inspectShown.load()) {
            // Inspect is on screen: frame for it (Cover/Radar at the overlay's aspect).
            RE::NiPoint3 playerNi{ playerPos[0], playerPos[1], playerPos[2] };
            const auto captureMode = _inspectMode.load();
            snapshotFraming = captureMode == InspectViewMode::Cover ? 2 : 3;
            fr = ComputeInspectFrustum(
                captureMode, trueExtentX, trueExtentY,
                cellCenterX, cellCenterY, playerNi, _overlayAspect.load());
        } else {
            snapshotFraming = 1;
            const float glanceWorldRadius = Settings::GetSingleton()->navGlanceRangeUnits;
            const float needW = trueExtentX + 2.0f * glanceWorldRadius;
            const float needH = trueExtentY + 2.0f * glanceWorldRadius;
            constexpr float kTexAspect = 1920.0f / 1080.0f;
            fr.centerX = cellCenterX;
            fr.centerY = cellCenterY;
            fr.width = std::max(needW, needH * kTexAspect);
            fr.height = fr.width / kTexAspect;
        }
        g_camFrustum(cam, fr.width, fr.height);
        // Center the frustum where the framing chose (cell center, or player for Radar).
        float center[3] = { fr.centerX, fr.centerY, playerPos[2] };
        g_camTarget(cam, center);
    }

    auto* lmCamera = reinterpret_cast<RE::LocalMapCamera*>(cam);
    lmCamera->enabled = true;
    lmCamera->Update();

    float w2c[4][4]{};
    RE::NiRect<float> port;
    if (auto* niCam = lmCamera->camera.get()) {
        std::memcpy(w2c, niCam->GetRuntimeData().worldToCam, sizeof(w2c));
        port = niCam->GetRuntimeData2().port;
        const auto& fr = niCam->GetRuntimeData2().viewFrustum;
        // NiRect members are protected; raw-view the 4 floats (layout l,r,t,b per NiRect.h).
        const float* pf = reinterpret_cast<const float*>(&port);
        // A sub-rect port can explain a blank or stretched full-texture blit.
        if (Settings::GetSingleton()->minimapSnapshotLog)
            SKSE::log::info("MinimapSnapshot: camera frustum=({},{},{},{}) ortho={} port=({},{},{},{})",
                fr.fLeft, fr.fRight, fr.fTop, fr.fBottom, fr.bOrtho, pf[0], pf[1], pf[2], pf[3]);
    }

    auto* previousAccumulator = RE::BSShaderAccumulator::GetCurrentAccumulator();
    const bool logRenderState = Settings::GetSingleton()->minimapSnapshotLog;
    ScopedTreeSwitchState treeSwitchState;

    g_render(CullingBase());

    auto* snapshotAccumulator = logRenderState ?
        RE::BSShaderAccumulator::GetCurrentAccumulator() : nullptr;

    RE::BSShaderManager::SetRenderMode(
        RE::BSShaderAccumulator::RENDER_MODE::kNormal);
    RE::BSShaderAccumulator::SetCurrentAccumulator(previousAccumulator);
    treeSwitchState.Restore();

    if (logRenderState) {
        auto* restoredAccumulator = RE::BSShaderAccumulator::GetCurrentAccumulator();
        SKSE::log::info(
            "MinimapSnapshot: render state previousAccumulator={} snapshotAccumulator={} restoredAccumulator={} accumulatorRestored={} renderModeAction=kNormal",
            static_cast<void*>(previousAccumulator),
            static_cast<void*>(snapshotAccumulator),
            static_cast<void*>(restoredAccumulator),
            restoredAccumulator == previousAccumulator);
        SKSE::log::info(
            "MinimapSnapshot: tree state exterior={} scanned={} saved={} changed={} restored={} allRestored={} captureMs={:.3f} restoreMs={:.3f}",
            isExterior,
            treeSwitchState.Scanned(),
            treeSwitchState.Saved(),
            treeSwitchState.Changed(),
            treeSwitchState.Restored(),
            treeSwitchState.Restored() == treeSwitchState.Saved(),
            treeSwitchState.CaptureMs(),
            treeSwitchState.RestoreMs());
    }

    // Store snapshot
    auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
    ID3D11ShaderResourceView* srv = nullptr;
    if (renderer) {
        auto& rt = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kLOCAL_MAP];
        srv = reinterpret_cast<ID3D11ShaderResourceView*>(rt.SRV);
        if (Settings::GetSingleton()->minimapSnapshotLog) {
            for (auto target : { RE::RENDER_TARGET::kLOCAL_MAP, RE::RENDER_TARGET::kLOCAL_MAP_SWAP }) {
                const auto& slot = renderer->GetRuntimeData().renderTargets[target];
                D3D11_TEXTURE2D_DESC desc{};
                if (slot.texture)
                    reinterpret_cast<ID3D11Texture2D*>(slot.texture)->GetDesc(&desc);
                SKSE::log::info("MinimapSnapshot: target={} texture={} srv={} rtv={} size={}x{} samples={} format={} stage=after-render",
                    static_cast<int>(target), static_cast<const void*>(slot.texture),
                    static_cast<const void*>(slot.SRV), static_cast<const void*>(slot.RTV),
                    desc.Width, desc.Height, desc.SampleDesc.Count, static_cast<int>(desc.Format));
            }
        }
    }

    SnapshotProjection snap;
    snap.valid = true;
    snap.stale = false;
    snap.cellID = parentCell ? parentCell->GetFormID() : 0;
    snap.isExterior = isExterior;
    snap.worldSpaceID = isExterior && player->GetWorldspace() ? player->GetWorldspace()->GetFormID() : 0;
    snap.frustumHalfWidth = snapFrustumHalfWidth;
    snap.playerPos = player->GetPosition();
    snap.northRot = northRot;
    std::memcpy(snap.worldToCam, w2c, sizeof(snap.worldToCam));
    snap.port = port;
    snap.uvCropMin = snapCropMin;
    snap.uvCropMax = snapCropMax;
    snap.cellExtentX = snapCellX;
    snap.cellExtentY = snapCellY;
    snap.srv = srv;
    snap.takenAt = std::chrono::steady_clock::now();

    auto* navSettings = Settings::GetSingleton();
    const bool wantGlanceEntities = navSettings->GlanceAnyEntity();
    const bool wantInspectEntities = navSettings->InspectAnyEntity();
    if (wantGlanceEntities || wantInspectEntities) {
        const bool needLOS = navSettings->navEntityRequireLOS;
        if (isExterior) {
            snap.entities = EntityMarkerSource::Collect(
                player, navSettings->navInspectRadarRangeUnits, snap.playerPos,
                EntityMarkerSource::kEntityRevealRadius, needLOS);
        } else if (parentCell) {
            snap.entities = EntityMarkerSource::Collect(
                parentCell, snap.playerPos,
                EntityMarkerSource::kEntityRevealRadius, needLOS);
        }

    }

    {
        std::lock_guard<std::mutex> lock(_snapshotMutex);
        snap.generation = _snapshot.generation + 1;
        snap.framing = snapshotFraming;
        snap.image = MinimapSnapshotTransition::CopyImage(srv, snap.generation);
        _snapshot = snap;
    }
    const float perfMs = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - perfStart).count();
    if (!srv) {
        static bool s_warnedSrvNull = false;
        if (!s_warnedSrvNull) {
            s_warnedSrvNull = true;
            SKSE::log::warn("MinimapRenderer: kLOCAL_MAP render target SRV is null, snapshot backdrop will not render");
        }
    }
    SKSE::log::debug("MinimapRenderer: snapshot render took {:.2f}ms (cell={:08X}, srv={})",
        perfMs, snap.cellID, static_cast<void*>(srv));
    if (Settings::GetSingleton()->minimapSnapshotLog)
        SKSE::log::info("MinimapSnapshot: completed cell={:08X} world={:08X} exterior={} srv={} camera={} renderMs={:.2f}",
            snap.cellID, snap.worldSpaceID, snap.isExterior, static_cast<void*>(srv),
            static_cast<void*>(lmCamera->camera.get()), perfMs);
    LogSnapshotPixels(srv, snap.cellID);
}
