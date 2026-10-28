// ============================================================================
// MinimapRenderer — Navigation Overlay (Glance Radar & Inspect Overlay)
// ----------------------------------------------------------------------------
// STATUS / TERMINOLOGY:
//   - "Glance mode" / "Glance radar": The HUD mini radar drawn during gameplay
//     (DrawGlance()). Supports Circle (default) and Square shapes (navGlanceShape),
//     with fixed 10px rounding for Square. The selected boundary owns background
//     fill, snapshot mask, combat rim, trail containment, entity/actor visibility,
//     bearing-preserving custom pin/quest rim clamping, and view-cone edge. Rim
//     markers retain the established 8px anchor inset so they sit on the border.
//   - "Inspect mode" / "Inspect overlay": The large local-map view held via N
//     (DrawInspect()). Shows path tracking ON/Pause or PAUSED/Resume legend in
//     interior cells when enablePathTracking is enabled; publishes
//     _interiorInspectShown atomic for InputHook hotkey consumption.
//   - Path snapshots and per-view projection are skipped when their settings
//     disable them. Stored PathTracker data remains intact for re-enable.
//   - Offscreen snapshots restore kNormal render mode and the previously current
//     BSShaderAccumulator before the gameplay world draw. Static RE complete;
//     affected-user flicker confirmation pending.
//   - [Logging] MinimapSnapshotLog=1 enables pipeline/draw diagnostics and a
//     throttled GPU readback. Pixel statistics flag blank-looking textures, not
//     successful geometry rendering. Pending Visual Studio and in-game validation.
// ============================================================================

#include "MinimapRenderer.h"
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
#include "PartySheetState.h"
#include "FontManager.h"

#include <imgui.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <utility>
#include <unordered_set>

#include <SKSE/SKSE.h>

namespace {
    // Zoom (world units from disc center to rim) is Settings::navGlanceRangeUnits,
    // not a fixed constant here. Scale is derived per-call: kGlanceRadius / range.
    constexpr float kInspectMoveRefreshDist = 600.0f;
    constexpr float kInspectRefreshSeconds  = 2.0f;
    // Post-transition settling window: how long after entering a space we keep
    // re-rendering on a short cadence because early snapshots catch a half-streamed
    // cell. 5s covers a slow interior load; each retry is one ~3-6ms render, and they
    // stop as soon as the window closes. See the settling block in SnapshotIsStale.
    constexpr long long kSpaceSettleMs = 5000;
    constexpr float kSpaceSettleRetrySeconds = 0.5f;
    // Upper bound on the space-transition fade hold (see the transition block in
    // Draw()). Measured exterior snapshots run 4-6ms once PreDisplay fires, so this
    // is a stuck-state escape hatch, not the expected path. Matches the settling
    // window: releasing the fade earlier would show the radar while the settling
    // retries are still replacing a half-streamed snapshot.
    constexpr long long kSpaceFadeMaxHoldMs = 5000;

    // Shared same-space + range + count gate for the custom-marker layer, used by
    // BOTH Glance and Inspect so the two views never disagree about which markers
    // exist. Returns pointers into a_markers (caller owns the storage; the frame
    // marker vector outlives the draw).
    //
    // Order matters: same-space, then range, then count. The count cap
    // (Settings::navMaxCustomMarkers, 0 = uncapped) keeps the N NEAREST by XY
    // distance -- the marker list is cosave order, so an unsorted "first N" would
    // drop markers arbitrarily as the player walks. a_applyCountCap is FALSE for
    // Inspect: the cap exists because the Glance disc rim-clamps out-of-view pins
    // into a pile, which Inspect (full-cell, every pin at its real spot) never does.
    //
    // XY only, never 3D: a world-map-placed marker stores the map-mesh plane Z
    // (~180000, kMapPlaneZ), not a real Z, so any Z term swamps the compare. Same
    // reasoning as the range gate this replaces.
    //
    // Destinations are EXEMPT from both gates and do not consume a cap slot: only
    // one is active at a time and it is what the player is navigating to.
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
            // Exterior: share the compass's WorldMatches, which compares worldspace
            // ROOTS. A bare == here hid every marker stored in a child worldspace
            // (Whiterun 0x1A26F, any walled city) the moment the player stepped out
            // into Tamriel -- setting the Whiterun or Dragonsreach map icon as a
            // destination showed nothing on the radar OR the compass. Confirmed
            // 2026-08-27: markerWs=0001A26F vs playerWs=0000003C.
            // One shared test on purpose: the two surfaces must never disagree
            // about which markers exist.
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

    // Shared snapshot-staleness test for both the Inspect and Glance refresh
    // drivers. True = the current snapshot no longer represents the player's
    // situation and a re-render is warranted: invalid, wrong cell, moved too
    // far, or too old. Both drivers request a fresh snapshot on the same
    // conditions so the Glance backdrop stays live even if Inspect is never
    // opened.
    // a_spaceEnteredAt: when the player entered the current cell/worldspace, used by
    // the settling window below. Callers pass MinimapRenderer::_spaceEnteredAt.
    bool SnapshotIsStale(const MinimapRenderer::SnapshotProjection& snap,
                         const MinimapRenderer::NavigationFrame& frame,
                         std::chrono::steady_clock::time_point a_spaceEnteredAt) {
        if (!snap.valid) return true;
        // Exterior (spec decision 4): compare worldSpaceID, not cellID. The
        // exterior grid cellID changes every 4096 units, which would force a
        // refresh on every grid-cell crossing; worldSpaceID only changes on a
        // real transition (load door, fast travel to another worldspace).
        if (frame.isExterior || snap.isExterior) {
            if (snap.isExterior != frame.isExterior || snap.worldSpaceID != frame.worldSpaceID) return true;
        } else if (snap.cellID != frame.cellID) {
            return true;
        }
        const float distSq = frame.playerPos.GetSquaredDistance(snap.playerPos);
        // Exterior move threshold scales with the current patch (25% of its half-
        // width): a fixed 600-unit trigger is too tight at a large Glance patch
        // and too loose at a tight Radar range. Pick whichever is smaller so the
        // player never outruns the refresh (interior keeps the fixed constant).
        float moveThreshold = kInspectMoveRefreshDist;
        if (frame.isExterior && snap.frustumHalfWidth > 0.0f) {
            moveThreshold = std::min(moveThreshold, snap.frustumHalfWidth * 0.25f);
        }
        if (distSq > moveThreshold * moveThreshold) return true;
        // Settling window (2026-08-15). The first snapshot after entering a space
        // renders before the cell's geometry has finished streaming: the render target
        // is handed back non-null with empty pixels, so there is NO field on the
        // snapshot that says "this one came out blank" (g_render is void, and srv is
        // the engine's persistent target - always non-null, carries no success info).
        // Since the bad snapshot can't be detected, don't trust the early ones: for
        // kSpaceSettleSeconds after a transition, keep refreshing on a short cadence
        // regardless of movement. Was: radar sat blank until the player physically
        // moved (measured 8.5s standing still in an interior), because the stationary
        // skip below pinned that first empty snapshot indefinitely.
        const auto now = std::chrono::steady_clock::now();
        const auto sinceSpaceEntry = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         now - a_spaceEnteredAt).count();
        const bool settling = sinceSpaceEntry < kSpaceSettleMs;
        // Stationary skip: fog can't change without movement, so the periodic
        // timer only fires once the player has actually moved since the last
        // snapshot. Known edge: a door opened while standing still shows one
        // refresh late (cell-change and move-threshold triggers still apply).
        // Suspended while settling (see above).
        if (!settling && distSq < 16.0f * 16.0f) return false;
        const auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - snap.takenAt).count();
        const float refreshSeconds = settling ? kSpaceSettleRetrySeconds
                                              : kInspectRefreshSeconds;
        return diff > refreshSeconds * 1000.0f;
    }

    // a_northRot: cell north rotation baked into the snapshot backdrop
    // (RenderOneSnapshot applies -GetNorthRotation() to the snapshot camera). The
    // backdrop picture is rotated by that angle but AddImageRounded can't rotate a
    // UV blit, so to keep glyphs aligned with the rotated map we rotate the
    // player-relative delta by the SAME angle here. Zero for vanilla/near-zero-north
    // cells (and callers pass 0 when there's no valid backdrop), which collapses this
    // to the old north-up delta. Without it, glyphs sit north-up while the picture is
    // rotated -> everything drifts/floats in cells a mod gave a non-zero north (e.g.
    // BleakFallsBarrow.esp). Inspect already does the equivalent (- snap.northRot on
    // its angle math); this brings Glance to the same coordinate system.
    ImVec2 WorldToGlance(const RE::NiPoint3& a_world,
                         const RE::NiPoint3& a_player,
                         float a_scale,
                         float a_northRot = 0.0f) {
        const float dx = a_world.x - a_player.x;
        const float dy = a_world.y - a_player.y;
        // Screen y is down and WorldToGlance already flips y (-dy). Match the sign
        // convention used by the arrow/cone (angle - northRot) so glyphs and the
        // player arrow rotate together with the backdrop.
        const float ca = std::cos(a_northRot);
        const float sa = std::sin(a_northRot);
        const float rx = dx * ca - dy * sa;
        const float ry = dx * sa + dy * ca;
        return ImVec2(rx * a_scale, -ry * a_scale);
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

    // active.png quest-target icon, centred on (x,y). Null SRV = skip (file
    // missing, no fallback glyph). halfPx is the glyph half-extent, sized off
    // the caller's pin knob so Glance/Inspect match their own marker pins.
    void DrawQuestTargetIcon(ImDrawList* dl, ID3D11ShaderResourceView* srv,
                             float x, float y, float halfPx, float a_alpha) {
        if (!srv) return;
        dl->AddImage(reinterpret_cast<ImTextureID>(srv),
                     ImVec2(x - halfPx, y - halfPx), ImVec2(x + halfPx, y + halfPx),
                     ImVec2(0, 0), ImVec2(1, 1), FadeCol(IM_COL32_WHITE, a_alpha));
    }

    // Shared with Draw()'s glance/inspect crossfade easing; pulled out to file
    // scope so DrawGlance/DrawInspect can ease the retrace arrow tint the same way.
    float SmoothstepLocal(float t) { return t * t * (3.0f - 2.0f * t); }

    // Camera view cone (GTA-style). Drawn under the player arrow: arrow = body heading,
    // cone = where the camera looks. a_mapAngle is a map-space yaw (0 = up/north),
    // same convention as the arrow math. Half-angle is fixed, not tied to worldFOV;
    // 35 deg reads well at radar size.
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
        dl->AddLine(a_origin, e1, FadeCol(IM_COL32(255, 255, 255, 60), a_alpha), 1.0f);
        dl->AddLine(a_origin, e2, FadeCol(IM_COL32(255, 255, 255, 60), a_alpha), 1.0f);
    }

    void DrawViewCone(ImDrawList* dl, const MapDraw::RoundedRectBoundary& a_boundary, float a_mapAngle,
                      float a_alpha = 1.0f) {
        // Screen polar angle of forward(sin a, -cos a) is a - pi/2.
        const float theta = a_mapAngle - 1.5707963f;
        const ImVec2 origin = a_boundary.center;
        dl->PathLineTo(origin);
        constexpr int kSamples = 16;
        ImVec2 e1{}, e2{};
        for (int i = 0; i <= kSamples; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kSamples);
            const float angle = (theta - kViewConeHalf) + t * (2.0f * kViewConeHalf);
            const ImVec2 pt = a_boundary.RayIntersection(angle, 4.0f);
            if (i == 0) e1 = pt;
            if (i == kSamples) e2 = pt;
            dl->PathLineTo(pt);
        }
        dl->PathFillConvex(FadeCol(IM_COL32(255, 255, 255, 26), a_alpha));
        // Edge rays so the cone stays readable over bright map areas.
        dl->AddLine(origin, e1, FadeCol(IM_COL32(255, 255, 255, 60), a_alpha), 1.0f);
        dl->AddLine(origin, e2, FadeCol(IM_COL32(255, 255, 255, 60), a_alpha), 1.0f);
    }

    bool ProjectWithSnapshot(const MinimapRenderer::SnapshotProjection& s,
                             const RE::NiPoint3& world,
                             const ImVec2& imageMin,
                             const ImVec2& imageSize,
                             ImVec2& out) {
        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
        const bool ok = RE::NiCamera::WorldPtToScreenPt3(
            s.worldToCam, s.port, world, sx, sy, sz, 1e-5f);
        if (!ok || !MapDraw::ProjectionDepthValid(sz)) return false;
        out = ImVec2(imageMin.x + sx * imageSize.x,
                     imageMin.y + (1.0f - sy) * imageSize.y);
        return true;
    }
    
    // Engine constants from LocalMapHudSpike
    static constexpr std::size_t kCullingOffset = 0x40;
    static constexpr std::size_t kBlockSize = 0x30500;
    static constexpr std::size_t kCameraOffsetFromMenu = 0x302a0;

    // SE/AE id pairs from findings-LocalMap-LiveRender-Blank.md section 4
    // (SE ids by cluster alignment in offsets-se.txt, unverified on SE runtime).
    using CtorFn   = void*(*)(void*);                 // SE 16092 / AE 16333  LocalMapCullingProcess ctor
    using RenderFn = void (*)(void*);                 // SE 16094 / AE 16335  the snapshot
    using CamBeginFn  = void (*)(void*);              // SE 16086 / AE 16327
    using CamTargetFn = void (*)(void*, const float*);// SE 16087 / AE 16328
    using CamBoundsFn = void (*)(void*, const float*, const float*); // SE 16090 / AE 16331
    using CamApplyFn  = void (*)(void*, float);       // SE 16089 / AE 16330
    // The two ctor-time steps vanilla's LocalMapMenu ctor does that 16333 alone skips.
    // Missing frustum = 0x0 ortho viewport, missing node = empty render lists; both
    // render blank with no crash. findings-LocalMap-LiveRender-Blank.md sections 1-3.
    using CamFrustumFn = void (*)(void*, float, float); // SE 16091 / AE 16332  ortho frustum dims
    using InitNodeFn   = void (*)(void*);              // SE 16095 / AE 16336  culling NiNode + flags

    // Full LocalMapMenu-block ctor. MiniMap Menu DLL calls this on a bare heap block
    // (no Scaleform context needed) instead of hand-initializing pieces; it builds the
    // culling process, accumulator and camera in one shot.
    // findings-MiniMapMenu-RenderPipeline.md section 1.
    using MenuBlockCtorFn = void (*)(void*);           // SE 52076 / AE 52964
    static REL::Relocation<MenuBlockCtorFn> g_menuBlockCtor{ REL::RelocationID(52076, 52964) };

    // NOTE: the two "render-gate globals" that used to live here (527793/414746 and
    // 513141/390865, from findings-MiniMapMenu-RenderPipeline.md section 2) were REMOVED
    // 2026-07-21. Both ids were misresolved and pointed at unrelated engine data, not at
    // bools -- see the long note at the g_render call site in RenderOneSnapshot.

    static REL::Relocation<CtorFn>       g_ctor      { REL::RelocationID(16092, 16333) };
    static REL::Relocation<RenderFn>     g_render    { REL::RelocationID(16094, 16335) };
    static REL::Relocation<CamBeginFn>   g_camBegin  { REL::RelocationID(16086, 16327) };
    static REL::Relocation<CamTargetFn>  g_camTarget { REL::RelocationID(16087, 16328) };
    static REL::Relocation<CamBoundsFn>  g_camBounds { REL::RelocationID(16090, 16331) };
    static REL::Relocation<CamApplyFn>   g_camApply  { REL::RelocationID(16089, 16330) };
    static REL::Relocation<CamFrustumFn> g_camFrustum{ REL::RelocationID(16091, 16332) };
    static REL::Relocation<InitNodeFn>   g_initNode  { REL::RelocationID(16095, 16336) };

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
        // Full menu-block ctor (52964) replaces the old piecewise 16333-only init; the
        // MiniMap Menu DLL proves it runs fine on a bare heap block. Keep the lazy 16336
        // node init and 16332 frustum after it: 52964's coverage of those is unproven and
        // both are idempotent-safe (16336 smart-ptr-replaces the node, 16332 is a setter).
        g_menuBlockCtor(g_block);
        g_initNode(CullingBase());
        g_camFrustum(CameraBase(), 4000.0f, 2250.0f);  // vanilla constants, 16:9
        g_constructed = true;
        // Ids are logged as SE/AE pairs in the type aliases above, not inlined here: the
        // old message hardcoded "(52964)" on both runtimes, which reads as an AE-id-on-SE
        // bug that isn't real (the RelocationIDs are correct; verified 2026-07-21).
        SKSE::log::info("MinimapRenderer: menu block constructed + node/frustum init");
    }
}

MinimapRenderer::NavigationFrame MinimapRenderer::BuildFrame() {
    NavigationFrame f;
    if (MenuUtils::ShouldBlockOverlay()) {
        _inspectToggleActive.store(false);
        return f;
    }

    // Beast form: Werewolf/Werebear have no map or fast travel, so the radar
    // has nothing actionable on it. Vampire Lord is exempt (include/BeastForm.h).
    if (Settings::GetSingleton()->hideInBeastForm && BeastForm::IsBeastForm()) {
        _inspectToggleActive.store(false);
        return f;
    }

    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return f;

    auto* cell = player->GetParentCell();
    if (!cell) return f;

    // Exterior branch (spec-findings-Exterior-Navigation-Overlay.md, live-test
    // gate PASSED 2026-07-17: terrain+fog render fine outdoors through the same
    // menu-block pipeline). Four independent visibility toggles (2026-07-17
    // restructure) replace the old GlanceExteriorVisible master, which bailed
    // the WHOLE frame build outdoors and wrongly killed Inspect too. Bail here
    // only if NOTHING wants to draw in this environment -- Glance AND Inspect
    // both off for the current interior/exterior state -- so e.g. Inspect can
    // still work outdoors with the Glance radar disabled outdoors.
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
    // Camera yaw for the view cone. cameraRoot rotation column 1 is the camera forward
    // vector; atan2(x, y) lands in the same clockwise-from-north convention as
    // data.angle.z (verified live 2026-07-16: swing right raised yaw, matched body yaw
    // when aligned). Works in every camera mode, no ThirdPersonState::freeRotation
    // sign guessing needed.
    f.cameraAngle = f.playerAngle;
    if (auto* cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
        const auto& r = cam->cameraRoot->world.rotate;
        f.cameraAngle = std::atan2(r.entry[0][1], r.entry[1][1]);
    }
    // Path trail stays interior-only in v1 (spec decision 6): PathTracker is
    // keyed per interior cellID and framed as a "local map" feature; an
    // exterior variant (worldspace-keyed segments) is future work. Skip the
    // copy entirely while the master switch is off; stored paths stay intact.
    f.segments = (!f.isExterior && navSettings->enablePathTracking)
        ? PathTracker::GetSingleton()->GetSegments(f.cellID)
        : decltype(f.segments){};
    f.markers = MarkerManager::GetSingleton()->GetMarkers();

    // Active quest markers: the engine already resolves quest targets to live
    // refs, tracked-filtered, door-retargeted, NPC positions live. HUDMarkerHook
    // taps the engine's per-marker AddMarker (RELOCATION_ID 50851/51728) and
    // buffers the world position of every quest/questDoor marker each compass
    // update. We just read that buffer. See findings-HUDMarker-Pipeline-
    // QuestMarkers.md; replaced the old DataHandler walk (kActive was NOT the
    // tracked flag, and most targets are NPCs the static filter dropped).
    f.questTargets = HUDMarkerHook::GetQuestMarkerPositions();

    // Inspect-visible gates the whole Inspect cascade here, not just the draw: with it
    // off for this environment, the key does nothing downstream either (no edge detection,
    // no refresh-on-inspect, no mode-toggle key poll), so toggling it off is a true
    // "nothing happens" state. Picks the Interior/Exterior toggle per f.isExterior.
    const bool inspectVisibleHere = f.isExterior ? navSettings->navInspectExteriorVisible : navSettings->navInspectInteriorVisible;
    if (navSettings->navInspectActivation == 1) {
        f.inspectHeld = !SettingsMenu::IsVisible() && inspectVisibleHere && _inspectToggleActive.load();
    } else {
        // Ctrl must be UP: Look & Pin is Ctrl+N and Inspect defaults to bare N, so
        // without this the same chord drives both (Inspect flickers on every Look &
        // Pin toggle). GetAsyncKeyState does not consume, so InputHook's swallow does
        // not separate them either -- both features poll the raw key state
        // independently and both see the press. Bare key = Inspect, modified = not.
        const bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        // Hotkeys store DIK; GetAsyncKeyState needs a VK. 0 back = unbound, or a
        // mouse/gamepad bind this poll cannot see, so treat it as not pressed.
        const int inspectVk = InputHandler::DIKToVK(Settings::GetSingleton()->navInspectKey);
        f.inspectHeld = !SettingsMenu::IsVisible() && inspectVisibleHere && !ctrlHeld && inspectVk != 0 &&
                         (GetAsyncKeyState(inspectVk) & 0x8000) != 0;
    }
    return f;
}

void MinimapRenderer::Draw() {
    // Party Sheet owns the screen while its HUD is hidden or a fullscreen panel
    // is up - our radar would draw on top of its widgets. Bail before BuildFrame
    // so the snapshot work is skipped too. Also clear the held-state latches, or
    // an Inspect key released behind a Party Sheet panel reads as still held on
    // the frame we come back. See include/PartySheetState.h.
    if (PartySheetState::ShouldHideOverlay()) {
        _inspectShown.store(false);
        _interiorInspectShown.store(false);
        _inspectToggleActive.store(false);
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

    // Space-transition detect. Must run BEFORE the staleness checks below: they read
    // _spaceEnteredAt for the settling window, so on the transition frame itself a
    // later update would leave them looking at the previous space's timestamp.
    // Blanks the radar (fade held at 0 until a same-space snapshot lands) and opens
    // the settling window that re-renders early half-streamed snapshots.
    {
        const std::uint32_t space = frame.isExterior ? frame.worldSpaceID : frame.cellID;
        if (space != _lastDrawnSpace || frame.isExterior != _lastDrawnExterior) {
            // Test BEFORE assigning: _lastDrawnSpace == 0 means nothing has been drawn
            // yet, i.e. this is the first draw rather than a real transition.
            const bool firstEverDraw = _lastDrawnSpace == 0;
            _lastDrawnSpace = space;
            _lastDrawnExterior = frame.isExterior;
            _spaceEnteredAt = std::chrono::steady_clock::now();
            // Skip the blank on the first draw: there is nothing on screen to fade out,
            // and holding would only delay the very first radar. The settling window
            // still applies, so an early half-streamed snapshot is still re-rendered.
            if (!firstEverDraw) {
                _glanceFade = 0.0f;
                _spaceFadeHold = true;
            }
        }
    }

    const bool inspectEdge = frame.inspectHeld && !_inspectWasHeld;
    const bool inspectReleased = !frame.inspectHeld && _inspectWasHeld;
    _inspectWasHeld = frame.inspectHeld;

    // Falling edge: the last snapshot was inspect-framed (overlay aspect / radar crop),
    // which Glance can't use (see RenderOneSnapshot). Re-render full-cell now instead of
    // letting the radar sit on a skewed backdrop until the 2s staleness cycle.
    const bool glanceVisibleHere = _glanceVisible.load() &&
        (frame.isExterior ? Settings::GetSingleton()->navGlanceExteriorVisible
                          : Settings::GetSingleton()->navGlanceInteriorVisible);
    if (inspectReleased && glanceVisibleHere &&
        Settings::GetSingleton()->navRefreshOnInspect) {
        RequestSnapshotRefresh(frame, "glance reframe after inspect");
    }

    // Seed the runtime view mode from InspectDefaultMode once per session (first
    // Inspect). Toggling after that stays in memory; the INI is never written here.
    if (!_inspectModeInited) {
        _inspectMode.store(Settings::GetSingleton()->navInspectDefaultMode == 1
                               ? InspectViewMode::Radar
                               : InspectViewMode::Cover);
        _inspectModeInited = true;
    }

    // View-mode toggle: only while the overlay is shown, edge-triggered (fire once per
    // press), mirroring the GetAsyncKeyState poll used for the Inspect key itself. On
    // toggle: flip the mode and force an immediate snapshot re-render so the framing
    // switches without waiting for the 2s cycle.
    // Exterior Inspect is Radar-only (spec decision 1: Cover/cell-fit is meaningless
    // with no cell to contain, the loaded area is a ~5x5 grid). Skip the poll entirely
    // outdoors so the key is a true no-op, and force Radar once per outdoor Inspect
    // session (mirrors the _inspectModeInited seed above).
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
        if (toggleDown && !_toggleWasDown) {
            const auto cur = _inspectMode.load();
            _inspectMode.store(cur == InspectViewMode::Cover ? InspectViewMode::Radar
                                                             : InspectViewMode::Cover);
            // Force an immediate re-render so the framing switches now, not on the 2s cycle.
            RequestSnapshotRefresh(frame, "inspect mode toggle");
        }
        _toggleWasDown = toggleDown;
    } else {
        _toggleWasDown = false;
    }

    // Quick-mark: drop a numbered breadcrumb at the player's current position
    // while Inspect is held. Edge-triggered (same pattern as the mode toggle
    // above) + a short cooldown so a held key can't spam markers if the edge
    // state ever desyncs from InputHook's independent scan-code consume.
    // Works both indoors and outdoors (spec decision 7). Indoors:
    // worldSpaceID=0 sentinel matches the interior placement path in
    // LocalMapClickHook::TryPlaceAtCursor, numbering is per-cell. Outdoors:
    // stores worldSpaceID=frame.worldSpaceID (matches the world-map placement
    // convention) + cellID=current exterior cell FormID. Exterior quick marks
    // are UNNUMBERED (iconType 0, default pin): worldspace numbering never
    // resets across a save, so it would creep into unreadable 3-digit glyphs.
    //
    // Interior numbering is per-cell max+1: iconType = kQuickMarkBase + N (N
    // starts at 1). Uses max-existing-N+1 rather than a running counter so a
    // deleted mid-sequence number leaves a gap instead of being reused (e.g.
    // delete #3, next press is #5, not #3 again).
    //
    // kQuickMarkBase (200) is a separate range from the note dialog's manual
    // waypoint picker (100-104 numbered, 110-113 color dots, 120 star, see
    // MapMarkerRenderer's kInteriorCategoryValues) so auto-numbered breadcrumbs
    // never collide with or get relabeled by that fixed picker set.
    if (frame.inspectHeld) {
        const int markVk = InputHandler::DIKToVK(Settings::GetSingleton()->navQuickMarkKey);
        const bool markDown = markVk != 0 && (GetAsyncKeyState(markVk) & 0x8000) != 0;
        if (markDown && !_quickMarkWasDown) {
            const auto now = std::chrono::steady_clock::now();
            const auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - _lastQuickMarkTime).count();
            if (sinceLast > 500) {
                if (frame.isExterior) {
                    // Outdoors: plain default custom marker, NO breadcrumb number.
                    // Worldspace-scoped numbering never resets, so a long save
                    // marches into 3-digit glyphs that are unreadable at pin size
                    // (decided 2026-07-17). Numbered trails stay interior-only.
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
                    // Number comes from the iconType glyph scan above, not from
                    // the name, so it is passed through explicitly rather than
                    // letting the scheme pick its own.
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

    // DIAG gate: RefreshOnInspect=0 skips our 16335 driver entirely so inspect shows
    // whatever vanilla last wrote to kLOCAL_MAP (open local map once first). Separates
    // "our render writes blank" from "display path broken".
    if (!Settings::GetSingleton()->navRefreshOnInspect) {
        if (inspectEdge) {
            std::lock_guard<std::mutex> lock(_snapshotMutex);
            // Borrow vanilla's texture with no projection metadata; background only.
            // Re-borrow every press so a local-map view opened after the first N still lands.
            auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (renderer) {
                _snapshot.srv = reinterpret_cast<ID3D11ShaderResourceView*>(
                    renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kLOCAL_MAP].SRV);
                _snapshot.valid = _snapshot.srv != nullptr;
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
        // Glance is visible and Inspect is not held: keep the radar backdrop
        // live on the same staleness rules. Without this the backdrop only
        // ever appeared after Inspect had been opened once (the only other
        // path that requests a snapshot). Costs one ~3-4ms snapshot render at
        // most every kInspectRefreshSeconds, accepted.
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
                // Safety release. If a snapshot never lands for this space (render
                // failed, or a space the snapshot path can't cover) the hold would
                // keep the radar dark forever. Give up after kSpaceFadeMaxHold and
                // fade in the glyph layers anyway - they don't need the backdrop.
                const auto held = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - _spaceEnteredAt).count();
                if (held > kSpaceFadeMaxHoldMs) _spaceFadeHold = false;
            }
        }
    }

    // Crossfade the two overlays instead of cutting on the same frame the key
    // state flips (felt abrupt in testing). Both fades ramp linearly toward
    // their target over ~150ms; smoothstep is applied only at the draw-alpha
    // conversion so the stored state stays linear and easy to reason about.
    constexpr float kFadeSeconds = 0.15f;
    // Cell entry gets a slower ramp than the keypress crossfade: 150ms is tuned for
    // an input the player made and expects to be instant, while a load-door fade-in
    // reads as a pop at that speed.
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

    // Retracing tint: non-empty recent revisit hits mean the player is walking
    // over already-tracked ground. Master-disable bypasses the tracker read and
    // removes this path-derived visual immediately.
    if (!Settings::GetSingleton()->enablePathTracking) {
        _retraceFade = 0.0f;
    } else {
        constexpr float kRetraceFadeSeconds = 0.25f;
        const float retraceRate = std::clamp((kRetraceFadeSeconds > 1e-4f) ? (dt / kRetraceFadeSeconds) : 1.0f, 0.0f, 1.0f);
        const float retraceTarget = PathTracker::GetSingleton()->GetRecentRevisits(0.6f).empty() ? 0.0f : 1.0f;
        const float retraceStep = std::clamp(retraceTarget - _retraceFade, -retraceRate, retraceRate);
        _retraceFade = std::clamp(_retraceFade + retraceStep, 0.0f, 1.0f);
    }

    // Combat rim: debounce raw IsInCombat() (flaps false for a few frames when
    // sheathing mid-fight, pattern from PartySheet PlayerHUDDisease), then ramp
    // ~0.5s so the red eases in/out instead of snapping.
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

    // Actor layer: collect on its own 2s cadence (snapshot-independent, so a
    // standing player keeps discovering approaching NPCs), then merge into the
    // ghost map. One merge feeds both DrawGlance and DrawInspect.
    CollectActorsIfDue(frame);
    MergeActorGhosts();

    if (glanceVisibleHere && glanceAlpha > 0.0f) {
        DrawGlance(frame, glanceAlpha);
    }
    if (inspectAlpha > 0.0f) {
        DrawInspect(frame, inspectAlpha);
    }
}

// Actor ghost merge (spec-findings-Actor-Radar-Layer.md decision 5). Applies
// each fresh actor scan (discovery/classification/unfreeze), freezes anything
// missing from a fresh scan, refreshes live positions per frame, and runs the
// fade-in/out machine. Consumes _lastActorScan (CollectActorsIfDue), NOT the
// snapshot, so discovery survives the snapshot stationary skip.
void MinimapRenderer::MergeActorGhosts() {
    const auto now = std::chrono::steady_clock::now();
    // Perf note (instrumentation removed 2026-07-21 after sign-off). This runs EVERY
    // frame and does a LookupByID per ghost, so it scales with crowd size. Measured
    // avg 1.5-5.1us at 0-5 ghosts, 2-7us @8, 12us @34 (0.05% of a 25ms frame), with
    // rare ~340us cold lookup spikes. Not a frame-drop source at any crowd tested.

    // Apply the scan list only when a NEW scan arrived (compare collect time).
    // The merge runs per frame, but re-applying a stale list every frame stomped
    // ghost.position back to the old scan position and un-froze departed actors,
    // so a leaving NPC faded at the spot he was first seen instead of where he
    // left (2026-07-17 report). Scan apply = discovery/classification/unfreeze;
    // everything per-frame (live position, leave-freeze, fade) runs below.
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

    // Live position refresh (2026-07-17): the 2s scan owns discovery,
    // classification and leave-detection, but a dot that only moves per scan
    // teleports (measured ~237 units/hop walking; a draw lerp still read as
    // laggy). While a ghost is live we re-read its position every frame via a
    // FormID round-trip: LookupByID validates the form still exists BEFORE any
    // vtable call (Party Sheet IsActorValid precedent), so no Actor* is ever
    // held across frames. Frozen ghosts keep their last-seen position.
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
            // Leave-detection per frame, not per scan: past the proximity
            // radius the ghost freezes immediately (dot stops at the rim of
            // knowledge and fades) instead of live-tracking into radar fog
            // for up to a scan interval.
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
            // Form gone, unloaded, or died. The scan can't catch this while the
            // stationary skip holds snapshots back, so freeze here: the dot
            // fades at its last live position (a death becomes the Corpse
            // layer's job on the next real scan).
            ghost.frozen   = true;
            ghost.lastSeen = now;
        }
    }

    // Freeze ghosts missing from a fresh scan (died/despawned/left the walk).
    // Only meaningful against a NEW scan list; comparing against a stale one
    // would re-freeze everyone the moment the stationary skip kicks in.
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

    // Fade state machine (2026-07-17, replaces the 4s frozen-ghost model):
    // alpha ramps toward 1 while live (fade-in ~0.3s, covers first sighting
    // AND re-entry) and toward 0 while frozen (fade-out ~1s at last position:
    // "signal lost", not a lingering tactical ghost; the actor may re-appear
    // anywhere so a stuck dot misleads). Ghost erased once fully faded.
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

// Self-paced actor collect, decoupled from the snapshot render (2026-07-17).
// Walks refs at most every kActorScanSeconds; discovery no longer waits on a
// snapshot, which the stationary skip can suppress for a minute while a player
// stands still. Cheap (~55us measured) so a fixed cadence is fine. Runs on the
// Present thread, same as the merge that consumes _lastActorScan.
void MinimapRenderer::CollectActorsIfDue(const NavigationFrame& a_frame) {
    auto* navSettings = Settings::GetSingleton();
    if (!navSettings->navActorAnyVisible()) return;

    const auto now = std::chrono::steady_clock::now();
    if (_lastActorScanAt.time_since_epoch().count() != 0 &&
        std::chrono::duration<float>(now - _lastActorScanAt).count() < kActorScanSeconds) {
        return;
    }

    // Loading guard: this walk runs on the Present thread, concurrent with the
    // game thread. During a load the exterior gridCells array is torn down and
    // rebuilt mid-transition, so iterating it here can race a detach and CTD.
    // The cell spinlock covers ref-list mutation, NOT grid reshuffles.
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
    // Perf note (instrumentation removed 2026-07-21 after sign-off). The ref walk +
    // classify measured 22-124us at 0-5 actors (22-34us with nothing in range) and
    // 70-150us across 6-34 actors in the 2026-07-18 crowd test, scaling sub-linearly
    // (~110us @13, ~150us @34), with a ~350us cold first-of-batch outlier.
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
    
    // The UI manager only calls IMenu::PreDisplay on menus with kRendersOffscreenTargets
    // (IMenu.h vfunc 07 comment; verified live: HUDMenu flags 0x18942 lack bit 12 and the
    // hook never fired). Vanilla HUDMenu::PreDisplay is an empty inherited stub, so setting
    // the flag only makes the engine reach our vtable hook — no vanilla behavior added.
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
    const float kGlanceRadius = settings->navGlanceSize * 0.5f;
    // Derived px-per-world-unit scale for this call. GlanceRangeUnits is the world
    // distance shown center-to-rim; kGlanceRadius is disc geometry for this call.
    const float glanceScale = kGlanceRadius / settings->navGlanceRangeUnits;

    // Position: explicit top-left origin from settings (no auto sentinel since
    // 2026-07-17). Clamped so a stale INI from a bigger display can't push the
    // disc fully off-screen.
    const float posX = std::clamp(static_cast<float>(settings->navGlancePosX), 0.0f, disp.x - 2.0f * kGlanceRadius);
    const float posY = std::clamp(static_cast<float>(settings->navGlancePosY), 0.0f, disp.y - 2.0f * kGlanceRadius);
    const ImVec2 center(posX + kGlanceRadius, posY + kGlanceRadius);

    constexpr float kGlanceSquareRounding = 10.0f;
    constexpr float kGlanceRimMarkerInset = 8.0f;
    const float halfSize = kGlanceRadius;
    const bool square = settings->navGlanceShape == 1;
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

    // Local-map snapshot backdrop: crop the SAME kLOCAL_MAP texture Inspect renders
    // to a sub-rect around the player, masked to the radar circle. No second render,
    // no new pipeline. When the snapshot is invalid the plain disc above shows through.
    //
    // ALIGNMENT NOTE: the trail/marker glyphs below use WorldToGlance (north-up in
    // world axes). The snapshot texture is baked with the engine's north rotation
    // (RenderOneSnapshot: g_camApply(cam, -GetNorthRotation())), so the backdrop
    // pixels are rotated by that angle. AddImageRounded cannot rotate a UV blit, so
    // the backdrop is exact only in cells with zero north rotation and drifts by the
    // north angle otherwise. The glyphs stay correct; only the map picture rotates.
    //
    // CROP SIZING (per-axis): the snapshot frustum is now anisotropic (matches the cell,
    // 2026-07-16), so one u-unit and one v-unit span different world distances. This code
    // already measured the local world->UV scale empirically, so it needs NO change for the
    // anisotropic frustum: it never assumed square UV scaling.
    // Project the player and two orthogonal world vectors of equal length
    // (glanceWorldRadius along +X and +Y); their UV images A and B are the columns
    // of the world->UV rotation+anisotropic-scale matrix. The u-extent that covers
    // glanceWorldRadius in every world direction is sqrt(A.x^2 + B.x^2), the
    // v-extent is sqrt(A.y^2 + B.y^2). In the zero-north case this reduces to the
    // plain aspect fix (uHalf=|A.x|, vHalf=|B.y|). The crop then covers the same
    // world radius on both screen axes and the picture is un-squished.
    // One snapshot copy, reused by the backdrop crop below AND the entity layer
    // further down. Lock once, not twice.
    SnapshotProjection snap;
    {
        std::lock_guard<std::mutex> lock(_snapshotMutex);
        snap = _snapshot;
    }
    {
        const char* backdropResult = "invalid snapshot";
        ImVec2 diagUV0{}, diagUV1{};
        // Same validity gate Inspect uses; also require the snapshot to match this
        // space. Exterior compares worldSpaceID (see SnapshotIsStale) since the
        // grid cellID changes every 4096 units and would drop the backdrop on
        // every crossing.
        const bool snapSameSpace = f.isExterior
            ? (snap.isExterior && snap.worldSpaceID == f.worldSpaceID)
            : (!snap.isExterior && snap.cellID == f.cellID);
        if (snap.valid) backdropResult = !snap.srv ? "missing SRV" : !snapSameSpace ? "wrong space" : "projection failed";
        if (snap.valid && snap.srv && snapSameSpace) {
            // Glance world radius: the world distance that maps to kGlanceRadius pixels
            // under WorldToGlance (dx * glanceScale), i.e. Settings::navGlanceRangeUnits.
            // Matching this to the crop half-extent is what aligns the backdrop to the
            // trail/marker projection.
            const float glanceWorldRadius = settings->navGlanceRangeUnits;

            // UV center = player world pos projected into the snapshot's 0..1 UV box.
            ImVec2 uvCenter;
            const bool haveCenter = ProjectWithSnapshot(
                snap, f.playerPos, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), uvCenter);
            // UV-per-world scale at the player: project two orthogonal world
            // vectors of equal length (glanceWorldRadius along +X and +Y). The
            // snapshot frustum is fitted per cell, so measuring the local scale is
            // the only reliable way to size the crop; a fixed UV extent would
            // mis-scale on every cell.
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

                    dl->AddImageRounded(reinterpret_cast<ImTextureID>(snap.srv),
                                        boundary.Min(), boundary.Max(), uv0, uv1,
                                        FadeCol(MapDraw::kGlanceBackdropTint, a_alpha),
                                        boundary.rounding, ImDrawFlags_RoundCornersAll);

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

    // North-rotation alignment for ALL glyphs (trail, entities, actors, markers,
    // player arrow, view cone). Only rotate when a valid same-space backdrop is
    // actually painted; with the plain disc there's no rotated picture to match, so
    // glyphs stay north-up (northRot=0). See WorldToGlance comment. This is the fix
    // for markers/arrow "floating"/"backwards" in cells a mod gave a non-zero north
    // rotation (Inspect never had the bug because it already subtracts snap.northRot).
    const bool glanceBackdropShown = snap.valid && snap.srv &&
        (f.isExterior ? (snap.isExterior && snap.worldSpaceID == f.worldSpaceID)
                      : (!snap.isExterior && snap.cellID == f.cellID));
    const float glanceNorthRot = glanceBackdropShown ? snap.northRot : 0.0f;

    // Combat pulse: rim lerps gold->red on _combatFade, red breathes with a
    // ~1.4s sine and thickens slightly so it reads without being loud.
    {
        const float combatT = SmoothstepLocal(_combatFade);
        const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.5f);
        const float redT = combatT * (0.45f + 0.55f * pulse);
        // User border color is the BASE of the lerp; combat still drives it to red.
        const ImU32 rimCol = MapDraw::LerpColor(Settings::GetSingleton()->BorderCol(),
                                                MapTheme::kCombatBorder, redT);
        const float rimW = 1.5f + 1.0f * combatT * pulse;
        if (square) {
            dl->AddRect(boundary.Min(), boundary.Max(), FadeCol(rimCol, a_alpha), boundary.rounding, 0, rimW);
        } else {
            dl->AddCircle(center, halfSize, FadeCol(rimCol, a_alpha), 64, rimW);
        }
    }

    // Keep the radar on the same breadcrumb/diamond/cone/pulse trail design as
    // the interior local map. Convert EVERY point of each segment and let
    // DrawPathTrail's circular clip (clipCenter/clipRadius below) discard the
    // outside glyphs. Do NOT pre-cull points here: glyphs emit at fixed
    // arc-length from a segment's start, so dropping a point moves the start
    // and re-phases all downstream dot/arrow spacing, which reads as the whole
    // trail wobbling as the player walks. DrawPathTrail only draws per-glyph
    // primitives (dots, diamonds, caps), each gated on insideClip; it draws no
    // unclipped connecting polyline, so the circle mask is complete.
    std::vector<std::vector<ImVec2>> visibleSegments;
    // Real world Z per point, parallel to visibleSegments, for the elevation
    // colour ramp. Built in the SAME loop and pushed under the SAME conditions
    // as the ImVec2s so the two arrays cannot desync.
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
                ImVec2 rp = WorldToGlance(p, f.playerPos, glanceScale, glanceNorthRot);
                pts.emplace_back(center.x + rp.x, center.y + rp.y);
                ptsZ.push_back(p.z);
            }
            if (!pts.empty()) {
                visibleSegments.push_back(std::move(pts));
                visibleSegmentZ.push_back(std::move(ptsZ));
            }
        }
    }
    // Revisit glow removed 2026-07-17 (read as noise); hits now drive the
    // retracing arrow tint instead.
    // Per-mode preparation/draw gate (Glance). Stored segments are untouched,
    // so re-enabling mid-session shows the full trail.
    if (showTrail) {
        MapDraw::DrawPathTrail(dl, visibleSegments,
                               TrailHeadNearPlayer(f.segments, f.playerPos),
                               PathTracker::GetSingleton()->IsTracking(),
                               center, halfSize,
                               0.0f, true, nullptr,
                               // Tail window forced to 0 (full trail) 2026-07-20.
                               // Radar used to fade to the last N dots; that partial
                               // reveal fought Inspect/local (already full) and left
                               // a useless 4-dot stub. Show the whole path or nothing.
                               // pathTrailRadarTailDots setting + slider now dead.
                               0,
                               a_alpha,
                               &visibleSegmentZ, f.playerPos.z,
                               &boundary);
    }

    // Entity layer: same snapshot-collected entities as Inspect (Corpse/Door/
    // Container/LooseLoot), drawn under our custom markers/cone/arrow. Reuses the
    // snapshot copied above; gate on this-space validity so a stale/wrong-space walk
    // can't leak entities onto the radar (exterior compares worldSpaceID, see the
    // backdrop crop above). Positioned player-centred via WorldToGlance
    // (NOT the snapshot UV projection) so entities track the trail/markers, not the
    // rotated backdrop. Off-radar entities simply don't draw: no rim-clamp, unlike
    // custom markers. PNG icons boxed to navGlanceEntityIconSize, same as Inspect;
    // falls back to the old vector glyph per-category if the icon SRV is null.
    // Interior/exterior no longer factors into entity filtering (2026-07-17
    // restructure #2: flat per-view subtype model, GlanceSubEnabled below).
    const bool glanceSnapSameSpace = f.isExterior
        ? (snap.isExterior && snap.worldSpaceID == f.worldSpaceID)
        : (!snap.isExterior && snap.cellID == f.cellID);
    if (snap.valid && glanceSnapSameSpace) {
        auto* s = Settings::GetSingleton();
        if (s->GlanceAnyEntity()) {
            const float iconSz = static_cast<float>(s->navGlanceEntityIconSize);
            const float sizeScale = iconSz / 7.0f;
            const float half = iconSz * 0.5f;
            const float fxPad = MapDraw::IconFxBoundaryExtent(iconSz, iconSz);
            const float entityExtent = std::hypot(half + fxPad, half + fxPad);
            for (const auto& e : snap.entities) {
                if (!e.discovered) continue;  // proximity fog: never seen it yet
                if (!s->GlanceSubEnabled(e.category, e.subCategory)) continue;  // flat per-view subtype gate

                ImVec2 rp = WorldToGlance(e.position, f.playerPos, glanceScale, glanceNorthRot);
                const ImVec2 projected(center.x + rp.x, center.y + rp.y);
                if (!boundary.Contains(projected, entityExtent)) continue;  // off-radar: don't draw
                ImVec2 g = projected;

                // Subcategory PNG first (e.g. "barrel" over generic "container"),
                // then the category fallback PNG, then the vector glyph below.
                ID3D11ShaderResourceView* icon = nullptr;
                if (const char* sub = EntityMarkerSource::IconNameFor(e.category, e.subCategory))
                    icon = TextureManager::GetIconByName(sub);
                if (!icon)
                    icon = TextureManager::GetIconByName(EntityMarkerSource::CategoryIconName(e.category));
                if (icon) {
                    // Same 3D treatment the custom pins get (MapDraw::DrawPin routes
                    // here too), so entity icons don't read as flat stickers next to a
                    // lifted pin on the same radar.
                    MapDraw::DrawIconFx(dl, icon,
                                        ImVec2(g.x - half, g.y - half),
                                        ImVec2(g.x + half, g.y + half),
                                        FadeCol(IM_COL32_WHITE, a_alpha));
                    continue;
                }

                // Fallback: PNG missing/failed to load, draw the old vector glyph.
                using Cat = EntityMarkerSource::Category;
                switch (e.category) {
                    case Cat::Corpse: {
                        // White X, ~70% of Inspect (arm 4 -> 3, stroke 2 -> 1.5).
                        const float arm = 3.0f * sizeScale;
                        dl->AddLine(ImVec2(g.x - arm, g.y - arm), ImVec2(g.x + arm, g.y + arm), FadeCol(MapTheme::kEntityCorpse, a_alpha), 1.5f);
                        dl->AddLine(ImVec2(g.x - arm, g.y + arm), ImVec2(g.x + arm, g.y - arm), FadeCol(MapTheme::kEntityCorpse, a_alpha), 1.5f);
                        break;
                    }
                    case Cat::Door: {
                        // Tan vertical rounded rect, ~70% (5x9 -> 3.5x6.5).
                        const float hw = 1.75f * sizeScale, hh = 3.25f * sizeScale;
                        dl->AddRectFilled(ImVec2(g.x - hw, g.y - hh), ImVec2(g.x + hw, g.y + hh),
                                          FadeCol(MapTheme::kEntityDoor, a_alpha), 1.0f);
                        break;
                    }
                    case Cat::Container: {
                        // Orange square, ~70% (7 -> 5), filled + outline.
                        const float hs = 2.5f * sizeScale;
                        dl->AddRectFilled(ImVec2(g.x - hs, g.y - hs), ImVec2(g.x + hs, g.y + hs),
                                          FadeCol(MapTheme::kEntityContainer, a_alpha));
                        dl->AddRect(ImVec2(g.x - hs, g.y - hs), ImVec2(g.x + hs, g.y + hs),
                                    FadeCol(IM_COL32(20, 15, 5, 235), a_alpha));
                        break;
                    }
                    case Cat::LooseLoot: {
                        // Orange dot, ~70% (4 -> 3).
                        MapDraw::DrawDotFx(dl, g, 3.0f * sizeScale, FadeCol(MapTheme::kEntityLoot, a_alpha));
                        break;
                    }
                    case Cat::Ingredient: {
                        // Green dot, matches the loot dot's ~70% scale.
                        MapDraw::DrawDotFx(dl, g, 3.0f * sizeScale, FadeCol(MapTheme::kEntityIngredient, a_alpha));
                        break;
                    }
                }
            }
        }
    }

    // Actor ghost layer: transient, drawn above entity glyphs, below custom
    // pins/player arrow (spec decision 6). _actorGhosts already merged once
    // in Draw(); iterate directly, no snapshot re-copy needed. No space/cell
    // gate: a ghost is only ever populated from a scan of the current space,
    // so there's nothing stale to filter (unlike snap.entities, which persists
    // across a stale snapshot until the next refresh).
    if (Settings::GetSingleton()->navActorAnyVisible()) {
        auto* s = Settings::GetSingleton();
        const float actorExtent = static_cast<float>(s->navGlanceActorDotSize) + 1.5f;
        for (const auto& [formID, ghost] : _actorGhosts) {
            using Al = ActorMarkerSource::ActorMarker::Allegiance;
            if (Al::Ally == ghost.allegiance && !s->navActorShowAlly) continue;
            if (Al::Hostile == ghost.allegiance && !s->navActorShowHostile) continue;
            if (Al::Neutral == ghost.allegiance && !s->navActorShowNeutral) continue;
            if (Al::Animal == ghost.allegiance && !s->navActorShowAnimal) continue;

            ImVec2 rp = WorldToGlance(ghost.position, f.playerPos, glanceScale, glanceNorthRot);
            const ImVec2 projected(center.x + rp.x, center.y + rp.y);
            if (!boundary.Contains(projected, actorExtent)) continue;  // off-radar: don't draw
            ImVec2 g = projected;

            const ImU32 col = Al::Ally == ghost.allegiance ? MapTheme::kActorAlly
                             : Al::Hostile == ghost.allegiance ? MapTheme::kActorHostile
                             : Al::Animal == ghost.allegiance ? MapTheme::kActorAnimal
                             : MapTheme::kActorNeutral;
            // ghost.alpha carries fade-in AND fade-out (merge fade machine).
            MapDraw::DrawDotFx(dl, g, static_cast<float>(s->navGlanceActorDotSize), FadeCol(col, a_alpha * ghost.alpha));
        }
    }

    // Draw current-cell custom markers and destination/exit direction.
    // sameSpace (spec decision 5): exterior compares worldSpaceID only (the
    // grid cellID is meaningless as a scope outdoors); interior keeps the
    // strict worldSpaceID==0 (interior sentinel) + cellID match.
    // Same-space + range + nearest-N count cap, shared with Inspect. Markers past
    // navMarkerRange are hidden, not rim-clamped; destinations are exempt.
    for (const MapMarker* mp : CollectVisibleMarkers(f.markers, f.isExterior,
                                                     f.worldSpaceID, f.cellID, f.playerPos,
                                                     true)) {
        const MapMarker& m = *mp;
        ImVec2 rp = WorldToGlance(m.position, f.playerPos, glanceScale, glanceNorthRot);
        const float pinScale = Settings::GetSingleton()->navGlancePinSize
                               / (2.0f * MapDraw::kPinRadius);
        const std::uint32_t drawIconType = m.isDestination ? 0u : m.iconType;
        auto* pinSrv = m.isDestination ? TextureManager::GetIconByName("destination")
                                       : TextureManager::GetCategoryIcon(m.iconType);

        const ImVec2 projected(center.x + rp.x, center.y + rp.y);
        const ImVec2 drawPos = boundary.Contains(projected)
            ? projected
            : boundary.ClampFromCenter(projected, kGlanceRimMarkerInset);

        // Draw the real category pin always; a destination gets the breathing
        // ring on top, same as the world map. (Was a red triangle that REPLACED
        // the pin, hiding which marker the destination even is. 2026-07-20.)
        // Same pin renderer as Inspect/local map so waypoint glyphs
        // (iconType >= 100, drawn in C++, no PNG) render too. Scale maps
        // navGlancePinSize (own knob, separate from markerIconScale and
        // from the entity icon size) onto DrawPin's base diameter
        // (2 * kPinRadius).
        // Same tint as the world/local map pins (Settings::PinTintCol). The
        // overlays never draw vanilla markers, so there is nothing here to
        // distinguish OUR pins from — which is exactly why they should match the
        // map rather than carry a second color. Was hardcoded alpha-0 (art true).
        MapDraw::DrawPin(dl, drawPos.x, drawPos.y, 0.0f, drawIconType, pinSrv,
                         pinScale, Settings::GetSingleton()->PinTintCol(), a_alpha);
        if (m.isDestination) {
            const float timeSec = std::chrono::duration<float>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            MapDraw::DrawDestinationRing(dl, drawPos.x, drawPos.y, timeSec);
        }
    }

    // Active quest targets (active.png). Same WorldToGlance + edge-clamp as the
    // marker loop so they inherit the backdrop rotation. SRV resolved once.
    if (auto* questSrv = TextureManager::GetIconByName("active")) {
        const float questHalf = 20.0f * settings->navGlancePinSize / (2.0f * MapDraw::kPinRadius);
        // Count cap: draw at most navMaxQuestMarkers, first N in compass priority
        // order. 0 = none (guard at loop top catches it). Glance always draws
        // (rim-clamps), so drawn count == index here.
        const int maxQuest = settings->navMaxQuestMarkers;
        int drawnQuest = 0;
        for (const auto& qpos : f.questTargets) {
            if (drawnQuest >= maxQuest) break;
            ImVec2 rp = WorldToGlance(qpos, f.playerPos, glanceScale, glanceNorthRot);
            const ImVec2 projected(center.x + rp.x, center.y + rp.y);
            const ImVec2 drawPos = boundary.Contains(projected)
                ? projected
                : boundary.ClampFromCenter(projected, kGlanceRimMarkerInset);
            DrawQuestTargetIcon(dl, questSrv, drawPos.x, drawPos.y, questHalf, a_alpha);
            ++drawnQuest;
        }
    }

    // Camera view cone under the arrow: shows look direction in third person,
    // collapses onto the body heading in first person.
    // - glanceNorthRot so the cone rotates with the backdrop, same as Inspect
    // (DrawInspect passes f.cameraAngle - snap.northRot).
    DrawViewCone(dl, boundary, f.cameraAngle - glanceNorthRot, a_alpha);

    // Draw player arrow using existing MinimapRenderer player-angle math.
    // - glanceNorthRot aligns the arrow with the rotated backdrop (matches Inspect).
    float c = std::cos(f.playerAngle - glanceNorthRot);
    float s = std::sin(f.playerAngle - glanceNorthRot);

    ImVec2 forward(s, -c);
    ImVec2 right(c, s);

    ImVec2 p1(center.x + forward.x * 12.0f, center.y + forward.y * 12.0f);
    ImVec2 p2(center.x - forward.x * 10.0f - right.x * 8.0f, center.y - forward.y * 10.0f - right.y * 8.0f);
    ImVec2 p3(center.x - forward.x * 4.0f, center.y - forward.y * 4.0f);
    ImVec2 p4(center.x - forward.x * 10.0f + right.x * 8.0f, center.y - forward.y * 10.0f + right.y * 8.0f);

    // Yellow while retracing already-tracked ground (_retraceFade ramped in Draw()).
    const float retraceT = SmoothstepLocal(_retraceFade);
    const ImU32 arrowFill = MapDraw::LerpColor(IM_COL32(255, 255, 255, 255), MapTheme::kRetraceArrow, retraceT);
    dl->AddQuadFilled(p1, p2, p3, p4, FadeCol(arrowFill, a_alpha));
    dl->AddQuad(p1, p2, p3, p4, FadeCol(IM_COL32(0, 0, 0, 255), a_alpha), 1.5f);
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

    // View modes (2026-07-17): the frustum is framed at the OVERLAY's aspect
    // (ComputeInspectFrustum, both Cover and Radar), so the whole texture fills the
    // overlay rect edge to edge with no letterbox and no un-stretch. The overlay is a
    // screen-aspect box scaled by InspectSizePct. Report that aspect back to the render
    // thread so the frustum matches. Legacy/frozen snapshots (extents 0, or an old
    // snapshot rendered before this logic) fall back to just filling the box.
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
    dl->AddRectFilled(min, max, FadeCol(IM_COL32(8, 9, 10, 230), a_alpha), MapTheme::kPanelRounding);
    dl->AddRect(min, max, FadeCol(Settings::GetSingleton()->BorderCol(), a_alpha), MapTheme::kPanelRounding, 0, MapTheme::kPanelBorderW);

    if (snap.valid && snap.srv) {
        const ImU32 tint = snap.stale ? IM_COL32(255, 255, 255, 150) : IM_COL32_WHITE;
        dl->AddImage(reinterpret_cast<ImTextureID>(snap.srv), min, max, uv0, uv1, FadeCol(tint, a_alpha));
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

    // Virtual rect: ProjectWithSnapshot maps FULL-texture UV (0..1) linearly onto the rect
    // it's given. The whole texture now fills the box (uv0=0, uv1=1), so the virtual rect IS
    // the box. (cropW/cropH kept at 1.0 for readability; the general form was needed back when
    // Inspect blitted a UV sub-rect.)
    float vW = boxW / cropW;
    float vH = boxH / cropH;
    const ImVec2 vMin(min.x - uv0.x * vW, min.y - uv0.y * vH);
    const ImVec2 vSize(vW, vH);

    if (snap.valid) {
        std::vector<std::vector<ImVec2>> visibleSegments;
        // Real world Z per point, parallel to visibleSegments, for the elevation
        // colour ramp. ProjectWithSnapshot DROPS points that fail to project, so
        // the Z must be pushed inside the same success branch — a separately
        // built array would silently desync and colour by the wrong points.
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
        // Revisit glow removed 2026-07-17 (read as noise); hits now drive the
        // retracing arrow tint instead.
        // Per-mode preparation/draw gate (Inspect). Stored segments are
        // untouched, so re-enabling mid-session shows the full trail.
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

        // Entity layer: under the custom pins and player arrow (drawn next), over
        // the trail. PNG icons (clearer at Inspect scale than Glance's tiny
        // glyphs); falls back to the old vector glyph if a PNG failed to load
        // so a missing/corrupt icon file never means the entity draws nothing.
        // Per-category toggles + the discovered gate apply here (walk always
        // collects all).
        {
            auto* s = Settings::GetSingleton();
            // Interior/exterior no longer factors into entity filtering
            // (2026-07-17 restructure #2: flat per-view subtype model,
            // InspectSubEnabled below).
            if (s->InspectAnyEntity()) {
                // Icon box side length in screen px, user-configurable
                // (Settings::navInspectEntityIconSize, 10-40 in SettingsMenu).
                const float half = static_cast<float>(s->navInspectEntityIconSize) * 0.5f;

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
                        // Shared 3D treatment, same as the Glance entity layer above.
                        MapDraw::DrawIconFx(dl, icon,
                                            ImVec2(g.x - half, g.y - half),
                                            ImVec2(g.x + half, g.y + half),
                                            FadeCol(IM_COL32_WHITE, a_alpha));
                        continue;
                    }

                    // Fallback: PNG missing/failed to load, draw the old vector glyph.
                    switch (e.category) {
                        case Cat::Corpse: {
                            // White X, two 2px strokes.
                            dl->AddLine(ImVec2(g.x - 4, g.y - 4), ImVec2(g.x + 4, g.y + 4), FadeCol(MapTheme::kEntityCorpse, a_alpha), 2.0f);
                            dl->AddLine(ImVec2(g.x - 4, g.y + 4), ImVec2(g.x + 4, g.y - 4), FadeCol(MapTheme::kEntityCorpse, a_alpha), 2.0f);
                            break;
                        }
                        case Cat::Door: {
                            // Tan vertical rounded rect 5x9.
                            dl->AddRectFilled(ImVec2(g.x - 2.5f, g.y - 4.5f), ImVec2(g.x + 2.5f, g.y + 4.5f),
                                              FadeCol(MapTheme::kEntityDoor, a_alpha), 1.5f);
                            break;
                        }
                        case Cat::Container: {
                            // Orange 7px square, filled + outline.
                            dl->AddRectFilled(ImVec2(g.x - 3.5f, g.y - 3.5f), ImVec2(g.x + 3.5f, g.y + 3.5f),
                                              FadeCol(MapTheme::kEntityContainer, a_alpha));
                            dl->AddRect(ImVec2(g.x - 3.5f, g.y - 3.5f), ImVec2(g.x + 3.5f, g.y + 3.5f),
                                        FadeCol(IM_COL32(20, 15, 5, 235), a_alpha));
                            break;
                        }
                        case Cat::LooseLoot: {
                            // Orange 4px filled circle.
                            MapDraw::DrawDotFx(dl, g, 4.0f, FadeCol(MapTheme::kEntityLoot, a_alpha));
                            break;
                        }
                        case Cat::Ingredient: {
                            // Green 4px filled circle, mirrors the loot dot.
                            MapDraw::DrawDotFx(dl, g, 4.0f, FadeCol(MapTheme::kEntityIngredient, a_alpha));
                            break;
                        }
                    }
                }
                dl->PopClipRect();
            }
        }

        // Actor ghost layer: transient, drawn above entity glyphs, below custom
        // pins/player arrow (spec decision 6). Same _actorGhosts map DrawGlance
        // reads, merged once in Draw(). Inspect scale is ~2x Glance (radius 5-6
        // vs 3), matching the ratio the entity glyphs use.
        if (Settings::GetSingleton()->navActorAnyVisible()) {
            auto* s = Settings::GetSingleton();
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
                MapDraw::DrawDotFx(dl, g, static_cast<float>(s->navInspectActorDotSize), FadeCol(col, a_alpha * ghost.alpha));
            }
            dl->PopClipRect();
        }

        // Clip the arrow/cone and custom markers to the box: with the cell-cropped blit the
        // virtual projection rect extends past the panel, so a point near a cell edge (or a
        // marker just outside the crop) can project outside the box. The trail and entity
        // blocks above already clip; do the same here so nothing spills onto the panel border.
        dl->PushClipRect(min, max, true);
        ImVec2 pScreen;
        if (ProjectWithSnapshot(snap, f.playerPos, vMin, vSize, pScreen)) {
            // Camera view cone under the arrow, same north-relative correction as the arrow.
            DrawViewCone(dl, pScreen, f.cameraAngle - snap.northRot, 48.0f, a_alpha);
            // Draw player arrow
            float c = std::cos(f.playerAngle - snap.northRot); // relative to snapshot north
            float s = std::sin(f.playerAngle - snap.northRot);
            ImVec2 forward(s, -c);
            ImVec2 right(c, s);
            ImVec2 p1(pScreen.x + forward.x * 12.0f, pScreen.y + forward.y * 12.0f);
            ImVec2 p2(pScreen.x - forward.x * 10.0f - right.x * 8.0f, pScreen.y - forward.y * 10.0f - right.y * 8.0f);
            ImVec2 p3(pScreen.x - forward.x * 4.0f, pScreen.y - forward.y * 4.0f);
            ImVec2 p4(pScreen.x - forward.x * 10.0f + right.x * 8.0f, pScreen.y - forward.y * 10.0f + right.y * 8.0f);
            const float retraceT = SmoothstepLocal(_retraceFade);
            const ImU32 arrowFill = MapDraw::LerpColor(IM_COL32(255, 255, 255, 255), MapTheme::kRetraceArrow, retraceT);
            dl->AddQuadFilled(p1, p2, p3, p4, FadeCol(arrowFill, a_alpha));
            dl->AddQuad(p1, p2, p3, p4, FadeCol(IM_COL32(0, 0, 0, 255), a_alpha), 1.5f);
        }

        // Same gate helper as Glance for same-space + range, but the count cap is
        // OFF here (last arg): Inspect has room for every in-range marker at its
        // real position, so capping would hide information for no readability win.
        for (const MapMarker* mp : CollectVisibleMarkers(f.markers, f.isExterior,
                                                         f.worldSpaceID, f.cellID, f.playerPos,
                                                         false)) {
            const MapMarker& m = *mp;
            // World-map-placed markers store the map-mesh plane Z (~180000,
            // kMapPlaneZ), not a real Z. The snapshot projection is a top-down
            // ortho of the cell at ground level, so a Z=180000 point projects
            // off-frame and the marker silently vanishes on Inspect (it shows on
            // Glance, which ignores Z). Exterior-local-placed and quick-mark
            // markers store real ground Z and are fine. Substitute the player's Z
            // so every marker projects at ground level; XY is what positions the
            // pin anyway. (Same class of fix as the compass anchor Z override.)
            RE::NiPoint3 projPos{ m.position.x, m.position.y, f.playerPos.z };
            ImVec2 mScreen;
            if (ProjectWithSnapshot(snap, projPos, vMin, vSize, mScreen)) {
                // Draw the real category pin always; a destination gets the
                // breathing ring on top, same as the world map. (Was a red
                // triangle that REPLACED the pin. 2026-07-20.)
                // Same category pin as the vanilla local map (LocalMapRenderer),
                // not a placeholder dot; icon only, no name, to keep dense cells
                // readable. navInspectPinSize is Inspect's own pin-diameter knob
                // (2026-07-17), separate from markerIconScale (world/local map).
                // Destination markers draw destination.png regardless of their
                // category/waypoint icon (see the Glance loop for why).
                const std::uint32_t drawIconType = m.isDestination ? 0u : m.iconType;
                auto* pinSrv = m.isDestination ? TextureManager::GetIconByName("destination")
                                               : TextureManager::GetCategoryIcon(m.iconType);
                const float pinScale = Settings::GetSingleton()->navInspectPinSize
                                       / (2.0f * MapDraw::kPinRadius);
                // Shared pin tint, same reasoning as the Glance loop above.
                MapDraw::DrawPin(dl, mScreen.x, mScreen.y, 0.0f, drawIconType, pinSrv,
                                 pinScale,
                                 Settings::GetSingleton()->PinTintCol(), a_alpha);
                if (m.isDestination) {
                    const float timeSec = std::chrono::duration<float>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    MapDraw::DrawDestinationRing(dl, mScreen.x, mScreen.y, timeSec);
                }
            }
        }

        // Active quest targets (active.png). Same snapshot projection as markers,
        // so they land in the frozen cell frame. SRV resolved once.
        if (auto* questSrv = TextureManager::GetIconByName("active")) {
            const float questHalf = 20.0f * Settings::GetSingleton()->navInspectPinSize / (2.0f * MapDraw::kPinRadius);
            // Count cap: at most navMaxQuestMarkers DRAWN. Count only successful
            // projections: an off-frame target must not consume a slot, else fewer
            // than N visible ones show. 0 = none.
            const int maxQuest = Settings::GetSingleton()->navMaxQuestMarkers;
            int drawnQuest = 0;
            for (const auto& qpos : f.questTargets) {
                if (drawnQuest >= maxQuest) break;
                ImVec2 qScreen;
                if (ProjectWithSnapshot(snap, qpos, vMin, vSize, qScreen)) {
                    DrawQuestTargetIcon(dl, questSrv, qScreen.x, qScreen.y, questHalf, a_alpha);
                    ++drawnQuest;
                }
            }
        }
        dl->PopClipRect();
    } else {
        // Fallback
        dl->AddText(ImVec2(min.x + 10, min.y + 10), FadeCol(IM_COL32_WHITE, a_alpha), "Local map unavailable (waiting for snapshot)");
    }

    // Mode badge, top-left, drawn last so markers/trail never cover it.
    // Key label comes from the configurable bind. DIKToKeyName covers every key,
    // not just A-Z/0-9, so a rebind to F5 or Mouse4 still labels correctly.
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
        const float badgeSize = ImGui::GetFontSize() * Settings::GetSingleton()->titleScale;
        const ImVec2 pad(8.0f, 4.0f);
        const ImVec2 pos(min.x + 10.0f, min.y + 10.0f);
        const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(badgeSize, FLT_MAX, 0.0f, badge);
        dl->AddRectFilled(ImVec2(pos.x - pad.x, pos.y - pad.y),
                          ImVec2(pos.x + sz.x + pad.x, pos.y + sz.y + pad.y),
                          FadeCol(MapTheme::kPanelBg, a_alpha), MapTheme::kPanelRounding);
        dl->AddText(ImGui::GetFont(), badgeSize, pos,
                    FadeCol(radarOn ? MapTheme::kBodyTextHover : MapTheme::kMutedText, a_alpha), badge);

        // Quick-mark hint under the badge. Mode toggle needs no hint — the
        // badge above already names its key. Built from Settings so it never
        // drifts from a rebind.
        const char* markKeyName =
            InputHandler::DIKToKeyName(Settings::GetSingleton()->navQuickMarkKey);
        const std::string hintStr = LocalizationManager::Format("Navigation.AddMarkerHint", markKeyName);
        const char* hint = hintStr.c_str();

        // AddText(font, size, ...) draws at an explicit size without needing a
        // bound ImGui window (this draw list has none) — same pattern used by
        // MapMarkerRenderer/LocalMapRenderer for smaller sub-labels.
        const float bodySize = ImGui::GetFontSize() * Settings::GetSingleton()->bodyScale;
        const ImVec2 hintPos(pos.x, pos.y + sz.y + pad.y + 4.0f);
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
            const ImVec2 trackingPos(hintPos.x, hintPos.y + hintSize.y + 4.0f);
            const ImU32 trackingColor = tracking
                ? IM_COL32(100, 255, 100, 255)
                : IM_COL32(200, 200, 200, 255);
            dl->AddText(ImGui::GetFont(), bodySize, trackingPos,
                        FadeCol(trackingColor, a_alpha), trackingText.c_str());
        }
    }

    // Location title, centered ABOVE the overlay's top border (outside the
    // map rect, so it never covers terrain or a marker near the top edge).
    // Uses Title Scale like the mode badge; no background plate -- a shadow
    // keeps it readable over whatever the HUD shows behind it.
    if (!f.locationName.empty()) {
        // Heading font like every other panel title (MapMarkerRenderer pattern):
        // its own atlas size folds into Title Scale via the ratio, so the slider
        // still drives the on-screen size exactly.
        ImFont* titleFont = FontManager::GetSingleton()->GetHeadingFont();
        if (!titleFont) titleFont = ImGui::GetFont();
        const float fontRatio = titleFont->FontSize / ImGui::GetFontSize();
        const float titleSize = ImGui::GetFontSize() *
            std::clamp(Settings::GetSingleton()->titleScale, 0.5f, 3.0f) * fontRatio;
        const char* title = f.locationName.c_str();
        const ImVec2 tsz = titleFont->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, title);
        // 8px gap between the text baseline box and the border.
        const ImVec2 tpos((min.x + max.x) * 0.5f - tsz.x * 0.5f, min.y - tsz.y - 8.0f);
        dl->AddText(titleFont, titleSize, ImVec2(tpos.x + 1.0f, tpos.y + 1.0f),
                    FadeCol(IM_COL32(0, 0, 0, 200), a_alpha), title);
        dl->AddText(titleFont, titleSize, tpos,
                    FadeCol(MapTheme::kHeaderText, a_alpha), title);
    }

    // Quick-mark confirmation toast, top-center of the overlay rect. Drawn
    // outside the mode-badge block so it doesn't inherit the badge's left
    // anchor: top-left sat in the corner the eye isn't on while reading the
    // map. Without any toast a placed breadcrumb is invisible while Inspect is
    // up (the marker only shows after the next snapshot refresh, up to 2s
    // later), so a press read as a no-op and got repeated. Pure timestamp
    // animation, no per-frame state to keep in sync: 150ms scale-pop in,
    // ~1.1s hold, 550ms fade.
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
                ImGui::GetFontSize() * Settings::GetSingleton()->titleScale * scale;
            const char* toast = _quickMarkToastText.c_str();
            const ImVec2 tsz = ImGui::GetFont()->CalcTextSizeA(toastSize, FLT_MAX, 0.0f, toast);
            const ImVec2 tpad(10.0f, 5.0f);
            // Centered on the overlay rect, 16px below its top edge. Recentred
            // per frame because the pop scale changes the measured width.
            const ImVec2 tpos((min.x + max.x) * 0.5f - tsz.x * 0.5f, min.y + 16.0f);
            dl->AddRectFilled(ImVec2(tpos.x - tpad.x, tpos.y - tpad.y),
                              ImVec2(tpos.x + tsz.x + tpad.x, tpos.y + tsz.y + tpad.y),
                              FadeCol(MapTheme::kPanelBg, toastAlpha), MapTheme::kPanelRounding);
            dl->AddRect(ImVec2(tpos.x - tpad.x, tpos.y - tpad.y),
                        ImVec2(tpos.x + tsz.x + tpad.x, tpos.y + tsz.y + tpad.y),
                        FadeCol(MapTheme::kPanelBorder, toastAlpha),
                        MapTheme::kPanelRounding, 0, MapTheme::kPanelBorderW);
            dl->AddText(ImGui::GetFont(), toastSize, tpos,
                        FadeCol(MapTheme::kBodyTextHover, toastAlpha), toast);
        } else {
            _quickMarkToastText.clear();  // stop re-measuring a dead toast
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

    // A live settings change can disable refresh after a request was queued. Drop
    // that request here so RenderOneSnapshot never runs while the gate is off.
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
        // Player-centred, fixed world width at the overlay aspect. No cell clamp:
        // the crop tracks the player and may extend past cell bounds (fog fills it).
        const float w = Settings::GetSingleton()->navInspectRadarRangeUnits;
        r.centerX = a_playerPos.x;
        r.centerY = a_playerPos.y;
        r.width = w;
        r.height = w / a_overlayAspect;
        break;
    }
    case InspectViewMode::Cover:
    default: {
        // Smallest rect at the overlay aspect that fully COVERS the cell: fit the
        // axis that fills better, crop the other. Centred on the cell, *1.05 margin.
        // width/height = extentX and extentY grown so aspect == overlayAspect while
        // both cell extents stay inside.
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
    // SKSE::log::debug(
    //     "MinimapRenderer: inspect frustum mode={} w={:.1f} h={:.1f} center=({:.1f},{:.1f}) overlayAspect={:.3f}",
    //     modeName, r.width, r.height, r.centerX, r.centerY, a_overlayAspect);
    return r;
}

void MinimapRenderer::RenderOneSnapshot() {
    // Perf watch: the whole snapshot (fog rebuild + culling + 16335 render) budgeted
    // ~3-4ms, fired at most once per kInspectRefreshSeconds. If this log climbs past
    // that on big cells, look at the 16336 node rebuild first.
    const auto perfStart = std::chrono::steady_clock::now();

    // Guard against engine target recreation (display-mode change resets
    // kLOCAL_MAP/kLOCAL_MAP_SWAP to 960x540). Cheap width check; re-applies the
    // 1920x1080 upscale only if it regressed. findings-LocalMap-RenderTarget-
    // Resolution.md step 8.
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

    // Rebuild the culling NiNode every refresh: FogOfWarOverlay is a baked mesh whose
    // vertex alphas come from the cell's seen data at build time, so a node built once
    // shows stale fog forever. 16336 releases the old node, allocates a fresh one and
    // repopulates it from the GridCellArray via the fog builder (16344/16101), which
    // re-reads seen data. findings-LocalMap-FogOfWar-Update.md.
    //
    // Ran unconditionally again as of 2026-07-21. It was briefly gated to interiors on a
    // hypothesis that this rebuild was hiding exterior geometry; the log proved otherwise
    // (buildings still vanished with it SKIPPED on every exterior snapshot). Real cause
    // was the misresolved render-gate globals -- see the note at the g_render call below.
    g_initNode(CullingBase());
    // Kept commented, not deleted: this line is what proved the interior-only gate was
    // firing correctly and the buildings STILL vanished, which cleared 16336 as a suspect.
    // Re-enable if exterior geometry ever misbehaves again.
    // SKSE::log::debug("MinimapRenderer: node init ran (exterior={})", isExterior);

    const float* playerPos = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(player) + 0x54);
    // TES -> cell/worldspace extents block. SE 0x2b0 / AE 0x2b8: the ONE field in this
    // whole path that moved between runtimes. Un-gated it read Papyrus script-name
    // strings on SE ("TelravMasterSCRIPT"), so +0x78/+0x84 came back as 1.42e+20 and
    // denormals, the frustum went to +-1e30, and every projection collapsed to a single
    // point -- blank Glance backdrop AND empty Inspect. findings-SE-TES-Worldspace-Bounds-Offset.md.
    // The block's internal layout (+0x78 max, +0x84 min) is identical on both.
    static const std::ptrdiff_t kTesBoundsBlockOffset = REL::Relocate(0x2b0, 0x2b8);
    auto* tesBlock = *reinterpret_cast<std::uint8_t**>(
        reinterpret_cast<std::uint8_t*>(tes) + kTesBoundsBlockOffset);

    g_camBegin(cam);
    g_camTarget(cam, playerPos);
    if (tesBlock) {
        g_camBounds(cam, reinterpret_cast<const float*>(tesBlock + 0x78), reinterpret_cast<const float*>(tesBlock + 0x84));
    }

    // Exterior cells can't have a north rotation (only interiors carry one), so
    // this stays 0 outdoors; kept as one variable for both branches (spec
    // decision 2's "keep 16:9 anyway for uniform code" note).
    const float northRot = parentCell ? -parentCell->GetNorthRotation() : 0.0f;
    g_camApply(cam, northRot);

    // The camera bounds (+0x38 max / +0x44 min) hold the TRUE cell extents at this point,
    // before any pad. Capture them now: the frustum frames the real cell, the +2000/side
    // pad below is only a geometry-clip guard for g_camBounds (SetAreaBounds), never for
    // the frustum. (Old code padded first, then had to subtract 4000 back out for the crop.)
    const float* boundsMax = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(cam) + 0x38);
    const float* boundsMin = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(cam) + 0x44);
    const float trueExtentX = boundsMax[0] - boundsMin[0];
    const float trueExtentY = boundsMax[1] - boundsMin[1];
    const float cellCenterX = (boundsMax[0] + boundsMin[0]) * 0.5f;
    const float cellCenterY = (boundsMax[1] + boundsMin[1]) * 0.5f;

    // Bounds sanity is what caught the SE 0x2b0/0x2b8 bug: a wrong TES offset shows up
    // here as absurd extents (1e+20) or float denormals, never as a crash. Re-enable if
    // the frustum ever looks wrong again on a new runtime.
    // SKSE::log::debug(
    //     "MinimapRenderer: DIAG bounds tesBlock={} isInterior={} boundsMax=({},{},{}) boundsMin=({},{},{}) extent=({},{})",
    //     static_cast<void*>(tesBlock), isInterior,
    //     boundsMax[0], boundsMax[1], boundsMax[2],
    //     boundsMin[0], boundsMin[1], boundsMin[2], trueExtentX, trueExtentY);

    if (isInterior) {
        const float pad = 2000.0f;
        float paddedMax[3] = { boundsMax[0] + pad, boundsMax[1] + pad, boundsMax[2] };
        float paddedMin[3] = { boundsMin[0] - pad, boundsMin[1] - pad, boundsMin[2] };
        g_camBounds(cam, paddedMax, paddedMin);
    }

    // Frustum framing via the single switch point. The engine's frustum setter (16332)
    // takes width and height independently ([RCX+0x38]=w, [RCX+0x3c]=h) and does NOT force
    // 16:9 (findings-LocalMap-Frustum-Framing.md). We frame it at the OVERLAY's aspect so
    // the render fills the target and the draw site blits it edge to edge with no
    // un-stretch. ComputeInspectFrustum picks the rect from the runtime mode:
    //   Cover = cover-fit the whole cell; Radar = fixed-range crop on the player.
    // cellExtentX/Y stay stored on the snapshot (Glance projection reads them via the
    // world->UV measure, not the box aspect, so they remain the true cell extents).
    ImVec2 snapCropMin(0.0f, 0.0f);
    ImVec2 snapCropMax(1.0f, 1.0f);
    float snapCellX = trueExtentX;
    float snapCellY = trueExtentY;
    // Frustum half-width for THIS snapshot, world units; only meaningful outdoors
    // (SnapshotIsStale scales its move-refresh threshold off it, decision 4).
    float snapFrustumHalfWidth = 0.0f;
    if (isExterior) {
        // Exterior framing (spec decisions 1-3): no cell to contain, so neither
        // the interior Cover-fit nor the Glance contain-the-cell rule applies.
        // Player-centred every refresh; recenter-to-cell-center (interior path
        // below) is skipped entirely.
        FrustumRect fr{};
        RE::NiPoint3 playerNi{ playerPos[0], playerPos[1], playerPos[2] };
        if (_inspectShown.load()) {
            // Radar-only outdoors (decision 1); _inspectMode is force-set to
            // Radar in Draw() whenever frame.isExterior, so this always reads
            // the exterior radar range, never Cover.
            fr.width = Settings::GetSingleton()->navInspectRadarRangeUnits;
            fr.height = fr.width / _overlayAspect.load();
        } else {
            // Glance patch (decision 2): isotropic 16:9, 1.5x pad over 2x range so
            // the UV-clamp crop in DrawGlance never clips before the next refresh.
            const float w = std::max(2.0f * Settings::GetSingleton()->navGlanceRangeUnits * 1.5f, 4000.0f);
            fr.width = w;
            fr.height = w / (1920.0f / 1080.0f);
        }
        fr.centerX = playerNi.x;
        fr.centerY = playerNi.y;
        // NOTE (live-tested 2026-07-17): the engine enforces a MINIMUM outdoor
        // frustum. A 1000x562.5 request came back as l/r +-5374 t/b +-3023 in the
        // DIAG log below -- do not try to defeat this floor, it's an engine clamp,
        // not a bug in fr.width/height here.
        g_camFrustum(cam, fr.width, fr.height);
        float center[3] = { fr.centerX, fr.centerY, playerPos[2] };
        g_camTarget(cam, center);
        snapCellX = fr.width;
        snapCellY = fr.height;
        snapFrustumHalfWidth = fr.width * 0.5f;
        // Muted (per-snapshot). Re-enable when framing/centering looks wrong outdoors.
        // SKSE::log::debug(
        //     "MinimapRenderer: exterior frustum {} w={:.1f} h={:.1f} center=({:.1f},{:.1f})",
        //     _inspectShown.load() ? "radar" : "glance", fr.width, fr.height, fr.centerX, fr.centerY);
    } else if (trueExtentX > 0.0f && trueExtentY > 0.0f) {
        FrustumRect fr{};
        if (_inspectShown.load()) {
            // Inspect is on screen: frame for it (Cover/Radar at the overlay's aspect).
            RE::NiPoint3 playerNi{ playerPos[0], playerPos[1], playerPos[2] };
            fr = ComputeInspectFrustum(
                _inspectMode.load(), trueExtentX, trueExtentY,
                cellCenterX, cellCenterY, playerNi, _overlayAspect.load());
        } else {
            // Glance-driven refresh. Two hard requirements, learned the painful way:
            // 1) CONTAIN cell + crop radius: Glance's backdrop crops a player-centred
            //    UV rect and CLAMPS it to [0,1]. If the frustum is tighter than
            //    cell + Settings::navGlanceRangeUnits per side, the clamp bites near
            //    cell edges and the image skews/drifts as the player walks.
            // 2) ISOTROPIC pixels (frustum aspect == texture 16:9): the snapshot is
            //    baked with the cell's north rotation and AddImageRounded cannot
            //    rotate a blit, so rotated content sampled through unequal per-axis
            //    world-per-pixel scales renders SHEARED (the "squished radar"). The
            //    old pre-2026-07-16 frustum was accidentally isotropic, which is why
            //    this never showed before the anisotropic quality fix.
            // Inspect keeps its anisotropic full-quality framing; only Glance pays
            // the (small, radar-sized) sharpness cost of isotropy.
            // A smaller GlanceRangeUnits tightens this pad automatically, sharpening
            // the backdrop; the value is read live (no cache), so an INI reload takes
            // effect on the next refresh with no extra plumbing.
            const float glanceWorldRadius = Settings::GetSingleton()->navGlanceRangeUnits;
            const float needW = trueExtentX + 2.0f * glanceWorldRadius;
            const float needH = trueExtentY + 2.0f * glanceWorldRadius;
            constexpr float kTexAspect = 1920.0f / 1080.0f;
            fr.centerX = cellCenterX;
            fr.centerY = cellCenterY;
            fr.width = std::max(needW, needH * kTexAspect);
            fr.height = fr.width / kTexAspect;
            // Muted (per-snapshot). Re-enable for interior Glance framing/skew bugs.
            // SKSE::log::debug(
            //     "MinimapRenderer: glance frustum iso16:9 w={:.1f} h={:.1f} (need {:.1f}x{:.1f}) center=({:.1f},{:.1f}) pad={:.1f}",
            //     fr.width, fr.height, needW, needH, fr.centerX, fr.centerY, glanceWorldRadius);
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
        // DIAG geometry: skew/squish shows up here. frustum l/r/t/b after the tick tells
        // whether Update() kept our full-cell dims; port != (0,1,1,0) means the engine
        // renders into a SUB-RECT of the target and our full 0..1 UV blit stretches it.
        const auto& fr = niCam->GetRuntimeData2().viewFrustum;
        // NiRect members are protected; raw-view the 4 floats (layout l,r,t,b per NiRect.h).
        const float* pf = reinterpret_cast<const float*>(&port);
        // A sub-rect port can explain a blank or stretched full-texture blit.
        if (Settings::GetSingleton()->minimapSnapshotLog)
            SKSE::log::info("MinimapSnapshot: camera frustum=({},{},{},{}) ortho={} port=({},{},{},{})",
                fr.fLeft, fr.fRight, fr.fTop, fr.fBottom, fr.bOrtho, pf[0], pf[1], pf[2], pf[3]);
    }

    // RENDER-GATE FORCING REMOVED 2026-07-21 -- the two ids were MISRESOLVED and we were
    // corrupting live engine memory on every snapshot (~1.9s while Glance is on):
    //   414746 -> AE 0x14332b210: a descriptor block, +0x10 points at the string
    //             "BSShaderAccumulator". We were forcing byte 0 to 1.
    //   390865 -> AE 0x142032dfc: middle of a float constant table (30.0f/10.0f/15.0f),
    //             not even 4-byte aligned. Its SE twin is named UNWIND_INFO_141b4f674,
    //             i.e. exception-unwind metadata. We were forcing byte 0 to 0.
    // Symptoms this caused: exterior world geometry (Riverwood's buildings) stopped
    // drawing while resident, and snapped back INSTANTLY on map close because the map's
    // real pipeline rebuilds the state we stomped. Both overlays also rendered blank.
    // Verified live via dump_memory + resolve_address on AE 1.6.1170, both ids exact.
    // findings-MiniMapMenu-RenderPipeline.md section 2 has the wrong ids; do NOT restore
    // this block from that finding without re-deriving the globals from the MiniMap Menu
    // DLL first. If the snapshot renders blank without the forcing, that's the signal the
    // gates are genuinely load-bearing and the REAL ids need to be found.

    // The local-map renderer installs its accumulator and render-mode dispatch
    // globally. HUD PreDisplay returns directly into the gameplay world draw, so
    // restore gameplay state before any later snapshot processing.
    auto* previousAccumulator = RE::BSShaderAccumulator::GetCurrentAccumulator();
    const bool logRenderState = Settings::GetSingleton()->minimapSnapshotLog;

    g_render(CullingBase());

    auto* snapshotAccumulator = logRenderState ?
        RE::BSShaderAccumulator::GetCurrentAccumulator() : nullptr;

    RE::BSShaderManager::SetRenderMode(
        RE::BSShaderAccumulator::RENDER_MODE::kNormal);
    RE::BSShaderAccumulator::SetCurrentAccumulator(previousAccumulator);

    if (logRenderState) {
        auto* restoredAccumulator = RE::BSShaderAccumulator::GetCurrentAccumulator();
        SKSE::log::info(
            "MinimapSnapshot: render state previousAccumulator={} snapshotAccumulator={} restoredAccumulator={} accumulatorRestored={} renderModeAction=kNormal",
            static_cast<void*>(previousAccumulator),
            static_cast<void*>(snapshotAccumulator),
            static_cast<void*>(restoredAccumulator),
            restoredAccumulator == previousAccumulator);
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

    // Entity layer: walk the cell (interior) or a radius around the player
    // (exterior, spec decision 8: there's no single cell to walk outdoors, the
    // loaded area is a grid) once per snapshot (never per frame). Gated on
    // either view's flat any-subtype helper (Glance or Inspect can want
    // entities even if the other is off) so the collect itself is skipped when
    // nothing wants it; per-subtype filtering happens at draw time so toggles
    // react instantly without waiting for the next refresh.
    auto* navSettings = Settings::GetSingleton();
    const bool wantGlanceEntities = navSettings->GlanceAnyEntity();
    const bool wantInspectEntities = navSettings->InspectAnyEntity();
    if (wantGlanceEntities || wantInspectEntities) {
        // First-reveal LOS gate, same treatment as loot beams: without it a glyph
        // appears for a chest through a wall the player has never seen into.
        // Glance and Inspect share one discovered set, so this gates both.
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

        // Per-category census, muted. The whole tally exists only to feed the log line,
        // so it's commented out with it rather than left computing into nothing.
        // int nCorpse = 0, nDoor = 0, nContainer = 0, nLoot = 0, nIngredient = 0, nDiscovered = 0;
        // for (const auto& e : snap.entities) {
        //     if (e.discovered) ++nDiscovered;
        //     switch (e.category) {
        //         case EntityMarkerSource::Category::Corpse:     ++nCorpse; break;
        //         case EntityMarkerSource::Category::Door:       ++nDoor; break;
        //         case EntityMarkerSource::Category::Container:  ++nContainer; break;
        //         case EntityMarkerSource::Category::LooseLoot:  ++nLoot; break;
        //         case EntityMarkerSource::Category::Ingredient: ++nIngredient; break;
        //     }
        // }
        // SKSE::log::debug(
        //     "MinimapRenderer: entity walk classified {} (discovered {}): corpse={} door={} container={} loot={} ingredient={}",
        //     snap.entities.size(), nDiscovered, nCorpse, nDoor, nContainer, nLoot, nIngredient);
    }

    // NOTE: the actor radar layer is NO LONGER collected here. It runs on its
    // own 2s cadence in CollectActorsIfDue (Present thread, snapshot-independent)
    // so a standing player keeps discovering approaching NPCs even while the
    // snapshot stationary skip suppresses this render (2026-07-17).

    {
        std::lock_guard<std::mutex> lock(_snapshotMutex);
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
