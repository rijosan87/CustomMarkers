#include "LocalMapHudSpike.h"

#include <imgui.h>
#include <d3d11.h>
#include <Windows.h>
#include <cstring>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// See header STATUS block. Engine local-map snapshot on the HUD.
//
// FROZEN DISPLAY (working): sample kLOCAL_MAP's SRV and blit it. The engine fills
// that target whenever the local map renders (i.e. when the player opens the map).
//
// LIVE-SNAPSHOT EXPERIMENT (wiring, may CTD — fix from crash logs):
//   We drive the engine's own render path ourselves so the snapshot refreshes during
//   gameplay. Press N to fire one render. Sequence from
//   findings/findings-LocalMap-RenderDriver-Setup.md:
//     1. Allocate a zeroed LocalMapMenu-sized block from the game allocator (ONCE).
//        The render struct (LocalMapCullingProcess) sits at base + 0x40.
//     2. Construct it: 16333 on (base + 0x40) — builds camera + allocates the
//        BSShaderAccumulator at +0x302c8 (the null that crashed the cold call).
//     3. Position camera onto the player: 52969 on (base) with DL=1 — only function
//        that reads gPlayerCharacter. Camera at base+0x302a0 == cullingbase+0x30260.
//     4. Render: 16335 on (base + 0x40) — culls, draws kLOCAL_MAP_SWAP, shaders to
//        kLOCAL_MAP. We already sample kLOCAL_MAP below.
//   16333 is run once (construct); 52969 + 16335 run per press (reposition + render).
//
// BLANK-IMAGE FIXES (2026-05-29):
//  (a) Camera tick: the input setters do NOT build the NiCamera frustum/world transform.
//      That is LocalMapCamera::DefaultState::Update (16337), reached only via
//      TESCamera::Update (vtable idx 2), which early-outs unless TESCamera::enabled is true.
//      So we set enabled = true and call Update() before 16335. (s6/6a)
//  (b) Scene is NOT a missing setup step. 16335 self-populates: for an interior it calls
//      16343 (0x140243ea0) on *(gTES+0xc0) and registers the cell ShadowSceneNode tiers
//      (idx 2/3/6/7) into the accumulator. So the blank was camera FRAMING, not scene.
//      (findings-LocalMap-RenderScene-Population.md)
//  (c) Camera framing now faithfully replicates 52969's first half: begin, target(player),
//      worldspace bounds, real negated cell rotation (was 0.0f), and the interior re-bounds
//      call (camera bounds padded by 2000 on X/Y) that 52969 does for interior cells.
//      Without (c) the interior frustum framed empty space -> blank.

namespace LocalMapHudSpike {

    // Render-struct (LocalMapCullingProcess) offset inside the LocalMapMenu layout.
    static constexpr std::size_t kCullingOffset = 0x40;
    // LocalMapMenu spans to ~0x30480 (ctor writes +0x303fc, +0x607e-element fields).
    // Over-allocate and zero it; we only need the layout, not the menu ctor.
    static constexpr std::size_t kBlockSize = 0x30500;

    // Camera lives at LocalMapMenu base + 0x302a0 == culling base + 0x30260.
    static constexpr std::size_t kCameraOffsetFromMenu = 0x302a0;

    using CtorFn   = void*(*)(void*);                 // 16333  LocalMapCullingProcess ctor
    using RenderFn = void (*)(void*);                 // 16335  the snapshot
    // Camera sub-functions extracted from 52969's first half (the menu-free part).
    using CamBeginFn  = void (*)(void*);              // 0x140242910 reset/begin
    using CamTargetFn = void (*)(void*, const float*);// 0x140242930 set target = player pos vec3
    using CamBoundsFn = void (*)(void*, const float*, const float*); // 16331 0x140242a60
    using CamApplyFn  = void (*)(void*, float);       // 0x1402429a0 SetNorthRotation

    static REL::Relocation<CtorFn>      g_ctor   { REL::ID(16333) };
    static REL::Relocation<RenderFn>    g_render { REL::ID(16335) };
    static REL::Relocation<CamBeginFn>  g_camBegin { REL::ID(16327) };   // 0x140242910
    static REL::Relocation<CamTargetFn> g_camTarget{ REL::ID(16328) };   // 0x140242930
    static REL::Relocation<CamBoundsFn> g_camBounds{ REL::ID(16331) };   // 0x140242a60
    static REL::Relocation<CamApplyFn>  g_camApply { REL::ID(16330) };   // 0x1402429a0

    static std::uint8_t* g_block      = nullptr;  // LocalMapMenu base
    static bool          g_constructed = false;

    static void* CullingBase() { return g_block + kCullingOffset; }
    static void* CameraBase()  { return g_block + kCameraOffsetFromMenu; }

    static void EnsureConstructed() {
        if (g_constructed) return;

        // Allocate from the game's heap so engine frees/refcounts behave.
        g_block = static_cast<std::uint8_t*>(RE::malloc(kBlockSize));
        if (!g_block) {
            SKSE::log::error("LocalMapHudSpike: malloc({:#x}) failed", kBlockSize);
            return;
        }
        std::memset(g_block, 0, kBlockSize);

        SKSE::log::info("LocalMapHudSpike: constructing render struct (16333) at culling base {}", CullingBase());
        g_ctor(CullingBase());
        g_constructed = true;
        SKSE::log::info("LocalMapHudSpike: 16333 returned (no crash)");
    }

    static void RenderOneSnapshot() {
        EnsureConstructed();
        if (!g_constructed) return;

        // Position the camera onto the player — 52969's first half, without the
        // Scaleform second half that crashed. See findings-LocalMap-RenderDriver-Setup.md.
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* tes    = RE::TES::GetSingleton();
        if (!player || !tes) {
            SKSE::log::info("LocalMapHudSpike: no player/TES — skipping render");
            return;
        }
        void* cam = CameraBase();

        // Player position vec3 lives at PlayerCharacter + 0x54 (matches 52969's
        // global player + 0x54). Read it as 3 floats.
        const float* playerPos =
            reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(player) + 0x54);

        // Worldspace/TES bounds at TES + 0x2b8, fields +0x78 and +0x84 (52969).
        auto* tesBlock = *reinterpret_cast<std::uint8_t**>(
            reinterpret_cast<std::uint8_t*>(tes) + 0x2b8);

        // DIAG: the worldspace bounds SOURCE. If these are zero/garbage, the frustum frames
        // a zero box no matter what else we do (engine fills this when the menu opens).
        if (tesBlock) {
            const float* wsMax = reinterpret_cast<const float*>(tesBlock + 0x78);
            const float* wsMin = reinterpret_cast<const float*>(tesBlock + 0x84);
            SKSE::log::info("LocalMapHudSpike: DIAG wsBounds src={} max=({},{},{}) min=({},{},{})",
                static_cast<const void*>(tesBlock), wsMax[0], wsMax[1], wsMax[2], wsMin[0], wsMin[1], wsMin[2]);
        } else {
            SKSE::log::info("LocalMapHudSpike: DIAG wsBounds tesBlock NULL");
        }
        SKSE::log::info("LocalMapHudSpike: DIAG playerPos=({},{},{})", playerPos[0], playerPos[1], playerPos[2]);

        SKSE::log::info("LocalMapHudSpike: camera begin (16327) on cam {}", cam);
        g_camBegin(cam);
        SKSE::log::info("LocalMapHudSpike: camera target (16328)");
        g_camTarget(cam, playerPos);
        if (tesBlock) {
            SKSE::log::info("LocalMapHudSpike: camera bounds (16331)");
            g_camBounds(cam,
                reinterpret_cast<const float*>(tesBlock + 0x78),
                reinterpret_cast<const float*>(tesBlock + 0x84));
        } else {
            SKSE::log::info("LocalMapHudSpike: tesBlock null — skipping bounds");
        }
        // North rotation. 52969 reads TESObjectCELL::GetNorthRotation (0x1402c27e0) on the
        // player's parent cell and sign-flips it (XOR 0x80000000 == float negate) before
        // SetNorthRotation. CommonLib exposes GetNorthRotation() directly. Passing 0.0f (the
        // old spike value) orients the camera wrong. Use the real, negated cell rotation.
        auto* parentCell = player->GetParentCell();
        const bool isInterior = parentCell && parentCell->IsInteriorCell();
        const float northRot = parentCell ? -parentCell->GetNorthRotation() : 0.0f;
        SKSE::log::info("LocalMapHudSpike: SetNorthRotation (16330) rot={} interior={}", northRot, isInterior);
        g_camApply(cam, northRot);

        // Interior re-bounds. For interior cells (cell->flags & 1), 52969 calls SetAreaBounds
        // a SECOND time with the camera's own just-set bounds expanded by 2000 (DAT_141ad2a80)
        // on X/Y. Faithful to 52969 @ 0x14097c000 (reads camera +0x38..+0x4c). The double
        // +40000 Z add this produces matches vanilla; the altitude double-add hypothesis was
        // ruled out (exterior, single add, still blank).
        if (isInterior) {
            const float* maxE = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(cam) + 0x38);
            const float* minE = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(cam) + 0x44);
            const float pad = 2000.0f;  // DAT_141ad2a80 = 0x44fa0000
            float paddedMax[3] = { maxE[0] + pad, maxE[1] + pad, maxE[2] };
            float paddedMin[3] = { minE[0] - pad, minE[1] - pad, minE[2] };
            g_camBounds(cam, paddedMax, paddedMin);
        }

        // The render NiCamera frustum + world transform are NOT built by the input
        // setters above. They are built by LocalMapCamera::DefaultState::Update (16337),
        // which only runs when the camera is TICKED via TESCamera::Update (vtable idx 2).
        // The construct path set the DefaultState as currentState but left `enabled`
        // false, and TESCamera::Update early-outs unless `enabled` is true. So enable
        // then tick. Without this the NiCamera stays at an identity view and 16335 culls
        // everything -> blank kLOCAL_MAP. See findings-LocalMap-RenderDriver-Setup.md s6/6a.
        auto* lmCamera = reinterpret_cast<RE::LocalMapCamera*>(cam);

        // DIAG: camera object state after all setters, before tick.
        RE::NiCamera* niCam = lmCamera->camera.get();
        RE::TESCameraState* curState = lmCamera->currentState.get();
        const float* camMax = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(cam) + 0x38);
        const float* camMin = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(cam) + 0x44);
        SKSE::log::info("LocalMapHudSpike: DIAG niCam={} currentState={} camBounds max=({},{},{}) min=({},{},{})",
            static_cast<const void*>(niCam), static_cast<const void*>(curState),
            camMax[0], camMax[1], camMax[2], camMin[0], camMin[1], camMin[2]);
        if (niCam) {
            const auto& t = niCam->world.translate;
            SKSE::log::info("LocalMapHudSpike: DIAG niCam world.translate BEFORE tick=({},{},{})", t.x, t.y, t.z);
        }

        lmCamera->enabled = true;
        SKSE::log::info("LocalMapHudSpike: ticking camera (TESCamera::Update) to build frustum");
        lmCamera->Update();

        if (niCam) {
            const auto& t = niCam->world.translate;
            SKSE::log::info("LocalMapHudSpike: DIAG niCam world.translate AFTER tick=({},{},{})", t.x, t.y, t.z);
        }

        SKSE::log::info("LocalMapHudSpike: camera done; firing render (16335)");
        g_render(CullingBase());
        SKSE::log::info("LocalMapHudSpike: 16335 returned (no crash)");
    }

    using PreDisplayFn = void (*)(RE::IMenu*);
    static REL::Relocation<PreDisplayFn> g_origPreDisplay;

    static void HookedPreDisplay(RE::IMenu* a_this) {
        g_origPreDisplay(a_this);

        // Edge-triggered: fire one render per N keypress (matches the old spike trigger).
        // Gate the render so we don't cull the scene every frame when not requested.
        static bool s_prevDown = false;
        const bool down = (GetAsyncKeyState('N') & 0x8000) != 0;
        if (down && !s_prevDown) {
            RenderOneSnapshot();
        }
        s_prevDown = down;
    }

    void Install() {
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_HUDMenu[0] };
        g_origPreDisplay = vtbl.write_vfunc(0x7, &HookedPreDisplay);
        SKSE::log::info("LocalMapHudSpike: hooked HUDMenu::PreDisplay (IMenu vtable idx 7)");
    }

    void Draw() {
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer) return;

        // kLOCAL_MAP is the final processed local-map texture; .SRV is an
        // ID3D11ShaderResourceView*, exactly what ImGui takes as an ImTextureID.
        auto& rt = renderer->data.renderTargets[RE::RENDER_TARGET::kLOCAL_MAP];
        ID3D11ShaderResourceView* srv = rt.SRV;
        if (!srv) return;  // not allocated yet (local map never rendered this session)

        const ImVec2 size(256.0f, 256.0f);

        ImGui::SetNextWindowSize(ImVec2(size.x + 16.0f, size.y + 16.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(20.0f, 200.0f), ImGuiCond_FirstUseEver);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoInputs;

        if (ImGui::Begin("LocalMapHudSpike", nullptr, flags)) {
            ImGui::Image(reinterpret_cast<ImTextureID>(srv), size);
        }
        ImGui::End();
    }
}
