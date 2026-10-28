#include "LocalMapHudSpike.h"

#include <imgui.h>
#include <d3d11.h>
#include <Windows.h>
#include <cstring>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// Spike prototype for local map HUD integration.

namespace LocalMapHudSpike {

    // Render-struct (LocalMapCullingProcess) offset inside the LocalMapMenu layout.
    static constexpr std::size_t kCullingOffset = 0x40;
    static constexpr std::size_t kBlockSize = 0x30500;

    // Camera lives at LocalMapMenu base + 0x302a0 == culling base + 0x30260.
    static constexpr std::size_t kCameraOffsetFromMenu = 0x302a0;

    using CtorFn   = void*(*)(void*);                 // 16333  LocalMapCullingProcess ctor
    using RenderFn = void (*)(void*);                 // 16335  the snapshot
    // Camera sub-functions extracted from 52969's first half (the menu-free part).
    using CamBeginFn  = void (*)(void*);              // 0x140242910 reset/begin
    using CamTargetFn = void (*)(void*, const float*); // 0x140242930 set target = player pos vec3
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

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* tes    = RE::TES::GetSingleton();
        if (!player || !tes) {
            SKSE::log::info("LocalMapHudSpike: no player/TES - skipping render");
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
            SKSE::log::info("LocalMapHudSpike: tesBlock null - skipping bounds");
        }
        auto* parentCell = player->GetParentCell();
        const bool isInterior = parentCell && parentCell->IsInteriorCell();
        const float northRot = parentCell ? -parentCell->GetNorthRotation() : 0.0f;
        SKSE::log::info("LocalMapHudSpike: SetNorthRotation (16330) rot={} interior={}", northRot, isInterior);
        g_camApply(cam, northRot);

        if (isInterior) {
            const float* maxE = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(cam) + 0x38);
            const float* minE = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(cam) + 0x44);
            const float pad = 2000.0f;  // DAT_141ad2a80 = 0x44fa0000
            float paddedMax[3] = { maxE[0] + pad, maxE[1] + pad, maxE[2] };
            float paddedMin[3] = { minE[0] - pad, minE[1] - pad, minE[2] };
            g_camBounds(cam, paddedMax, paddedMin);
        }

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
        if (!srv) return;

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
