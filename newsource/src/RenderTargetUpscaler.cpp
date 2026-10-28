// ============================================================================
// RenderTargetUpscaler.cpp — upscale the engine's local-map render targets
// ----------------------------------------------------------------------------
// STATUS (update when behavior changes; do NOT mirror into CLAUDE.md):
//   Confirmed 2026-09-11: bypassing Apply and EnsureApplied restored Glance and
//     Inspect snapshots under Display Tweaks 0.75 upscaling (1440x810 render,
//     1920x1080 client). Keep engine/mod targets intact; do not re-enable the
//     fixed-size replacement without compatibility validation. See
//     findings/findings-Display-Tweaks-Rendering.md. Notes below describe the
//     retained, inactive replacement path.
//   What it does:
//     Recreates the engine's kLOCAL_MAP and kLOCAL_MAP_SWAP D3D11 targets at
//     1920x1080 (from the engine's hardcoded 960x540) so anything sampling
//     kLOCAL_MAP (Inspect overlay backdrop, Glance radar crop) stays sharp on
//     high-res displays. Same format/bind flags as vanilla, 2x each dim, 16:9
//     preserved. The local-map frustum/projection are resolution-agnostic
//     (finding), so no camera/culling change is needed.
//   finding: findings/findings-LocalMap-RenderTarget-Resolution.md (strategy b).
//   The both-targets rule: kLOCAL_MAP_SWAP is the ping-pong buffer for the fog
//     blur pass. If only one is resized the blur mixes resolutions, so Apply()
//     resizes BOTH atomically — if the second target's recreation fails it does
//     NOT half-swap; it logs loudly and leaves whatever it managed (the first
//     may already be swapped, but a stale first + vanilla second is the same
//     mixed-res failure). We create both new sets fully BEFORE swapping either
//     one in, so a failure on target 2 leaves BOTH vanilla targets untouched.
//   Recreation guard: EnsureApplied() re-reads the live kLOCAL_MAP width each
//     call; on a display-mode change the engine rebuilds the targets back to
//     960x540, so a regressed width re-triggers Apply(). Wired into
//     MinimapRenderer::RenderOneSnapshot before the render.
//   Dual-build: manager resolved via our own RELOCATION_ID(524970, 411451)
//     Relocation used as the object address. NG's GetSingleton() dereferences
//     that id as a pointer-to-manager and CRASHES (crash-2026-07-16-13-16-05);
//     the id is the struct itself. Everything else is version-agnostic D3D11
//     via typed CommonLib structs — no raw offsets.
//   Failure safety: every D3D11 call is checked; any failure keeps the vanilla
//     960x540 targets everywhere. The plugin already works at that resolution.
// ============================================================================

#include "RenderTargetUpscaler.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <d3d11.h>

namespace {
    // Isolate replacement from projection. Restart Skyrim for each test run;
    // bypassing cannot restore targets already replaced by an older DLL.
    constexpr bool kDiagnosticBypass = true;
    // 960->1920, 540->1080. One knob; 16:9 preserved so no aspect fix needed.
    constexpr std::uint32_t kUpscaleMultiplier = 2;

    // Small holder for a freshly-built target so we can create both before
    // swapping either into the engine (keeps the pair atomic on failure).
    struct NewTarget {
        ID3D11Texture2D*          texture = nullptr;
        ID3D11RenderTargetView*   rtv     = nullptr;
        ID3D11ShaderResourceView* srv     = nullptr;

        void ReleaseAll() {
            if (srv)     { srv->Release();     srv = nullptr; }
            if (rtv)     { rtv->Release();     rtv = nullptr; }
            if (texture) { texture->Release(); texture = nullptr; }
        }
    };

    // Build one 1920x1080 target matching an existing one's format/bind flags.
    // Returns false (and releases anything partial) on any D3D11 failure.
    bool BuildTarget(ID3D11Device* a_device, ID3D11Texture2D* a_existing,
                     std::uint32_t a_width, std::uint32_t a_height, NewTarget& a_out) {
        D3D11_TEXTURE2D_DESC desc{};
        a_existing->GetDesc(&desc);

        const auto oldFormat = desc.Format;
        const auto oldBind   = desc.BindFlags;
        desc.Width  = a_width;
        desc.Height = a_height;

        HRESULT hr = a_device->CreateTexture2D(&desc, nullptr, &a_out.texture);
        if (FAILED(hr) || !a_out.texture) {
            SKSE::log::warn("RenderTargetUpscaler: CreateTexture2D failed hr={:#x} fmt={} bind={:#x}",
                static_cast<std::uint32_t>(hr), static_cast<int>(oldFormat), oldBind);
            a_out.ReleaseAll();
            return false;
        }

        if (desc.BindFlags & D3D11_BIND_RENDER_TARGET) {
            hr = a_device->CreateRenderTargetView(a_out.texture, nullptr, &a_out.rtv);
            if (FAILED(hr) || !a_out.rtv) {
                SKSE::log::warn("RenderTargetUpscaler: CreateRenderTargetView failed hr={:#x}",
                    static_cast<std::uint32_t>(hr));
                a_out.ReleaseAll();
                return false;
            }
        }

        if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
            hr = a_device->CreateShaderResourceView(a_out.texture, nullptr, &a_out.srv);
            if (FAILED(hr) || !a_out.srv) {
                SKSE::log::warn("RenderTargetUpscaler: CreateShaderResourceView failed hr={:#x}",
                    static_cast<std::uint32_t>(hr));
                a_out.ReleaseAll();
                return false;
            }
        }

        return true;
    }

    // Swap a freshly-built target into the engine slot, releasing the vanilla
    // COM objects. Only the members we recreated are swapped; textureCopy /
    // SRVCopy / UAV are left as the engine set them (the local-map targets don't
    // use them, and mismatching them here would be worse than leaving them).
    void SwapIntoEngine(RE::BSGraphics::RenderTargetData& a_slot, NewTarget& a_new) {
        if (a_slot.SRV)     reinterpret_cast<ID3D11ShaderResourceView*>(a_slot.SRV)->Release();
        if (a_slot.RTV)     reinterpret_cast<ID3D11RenderTargetView*>(a_slot.RTV)->Release();
        if (a_slot.texture) reinterpret_cast<ID3D11Texture2D*>(a_slot.texture)->Release();

        a_slot.texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(a_new.texture);
        a_slot.RTV     = reinterpret_cast<REX::W32::ID3D11RenderTargetView*>(a_new.rtv);
        a_slot.SRV     = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(a_new.srv);
        // Ownership transferred to the engine; don't Release from NewTarget.
        a_new.texture = nullptr;
        a_new.rtv     = nullptr;
        a_new.srv     = nullptr;
    }

    // The two local-map color targets. kLOCAL_MAP_SWAP is the fog-blur ping-pong
    // buffer; both must be identical size or the blur mixes resolutions.
    constexpr RE::RENDER_TARGET kTargets[] = {
        RE::RENDER_TARGET::kLOCAL_MAP,
        RE::RENDER_TARGET::kLOCAL_MAP_SWAP,
    };

    // NG's RenderTargetManager::GetSingleton() is WRONG for this id: it declares
    // Relocation<RenderTargetManager**> and dereferences, but the id resolves to
    // the manager struct ITSELF, not a pointer to it (live-verified 2026-07-16:
    // width=960 readable at RVA+0x1DC directly; dereferencing crashed at
    // kDataLoaded with a garbage base built from the table's own bytes).
    // Treat the relocation address as the object. See the NG-vtable/abstraction
    // distrust rule in project memory.
    RE::BSGraphics::RenderTargetManager* GetRenderTargetManager() {
        static REL::Relocation<RE::BSGraphics::RenderTargetManager*> singleton{
            RELOCATION_ID(524970, 411451)
        };
        return singleton.get();
    }
}

namespace RenderTargetUpscaler {

    void Apply() {
        if (kDiagnosticBypass) {
            SKSE::log::info("RenderTargetUpscaler: diagnostic bypass active; local-map targets left untouched");
            return;
        }
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        auto* rtMgr    = GetRenderTargetManager();
        if (!renderer || !rtMgr) {
            SKSE::log::warn("RenderTargetUpscaler: renderer/manager singleton null, skipping upscale");
            return;
        }

        ID3D11Device* device = reinterpret_cast<ID3D11Device*>(renderer->GetRuntimeData().forwarder);
        if (!device) {
            SKSE::log::warn("RenderTargetUpscaler: D3D11 device (forwarder) null, skipping upscale");
            return;
        }

        // Target size is a fixed multiple of the vanilla 960x540 base (16:9), NOT
        // of the live dims — otherwise a second call would double an already-
        // doubled target. If the live width already reached the target, bail
        // (Apply is safe to call repeatedly via EnsureApplied).
        constexpr std::uint32_t kBaseW = 960;
        constexpr std::uint32_t kBaseH = 540;
        constexpr std::uint32_t newW = kBaseW * kUpscaleMultiplier;  // 1920
        constexpr std::uint32_t newH = kBaseH * kUpscaleMultiplier;  // 1080

        auto& baseProps = rtMgr->renderTargetData[RE::RENDER_TARGET::kLOCAL_MAP];
        if (baseProps.width >= newW) {
            SKSE::log::debug("RenderTargetUpscaler: kLOCAL_MAP already {}x{}, no upscale needed",
                baseProps.width, baseProps.height);
            return;
        }

        // Build BOTH new targets fully before swapping either in — keeps the pair
        // atomic: a failure on the second leaves both vanilla targets intact.
        NewTarget built[2]{};
        for (int i = 0; i < 2; ++i) {
            auto& slot = renderer->GetRuntimeData().renderTargets[kTargets[i]];
            if (!slot.texture) {
                SKSE::log::warn("RenderTargetUpscaler: target {} has no texture, aborting upscale",
                    static_cast<int>(kTargets[i]));
                built[0].ReleaseAll();
                built[1].ReleaseAll();
                return;
            }
            if (!BuildTarget(device, reinterpret_cast<ID3D11Texture2D*>(slot.texture), newW, newH, built[i])) {
                SKSE::log::warn("RenderTargetUpscaler: build failed for target {}, "
                    "rolling back — both local-map targets left at vanilla size",
                    static_cast<int>(kTargets[i]));
                built[0].ReleaseAll();
                built[1].ReleaseAll();
                return;
            }
        }

        // Both built. Now commit: swap the COM objects, then update the dims
        // table so any engine system reading the size (blur accumulator, image
        // space) uses the new resolution.
        for (int i = 0; i < 2; ++i) {
            auto& slot  = renderer->GetRuntimeData().renderTargets[kTargets[i]];
            auto& props = rtMgr->renderTargetData[kTargets[i]];
            const std::uint32_t oldW = props.width;
            const std::uint32_t oldH = props.height;

            SwapIntoEngine(slot, built[i]);
            props.width  = newW;
            props.height = newH;

            SKSE::log::info("RenderTargetUpscaler: target {} {}x{} -> {}x{} (format {})",
                static_cast<int>(kTargets[i]), oldW, oldH, newW, newH,
                static_cast<int>(props.format.get()));
        }
    }

    void EnsureApplied() {
        if (kDiagnosticBypass) return;
        auto* rtMgr = GetRenderTargetManager();
        if (!rtMgr) return;

        // We stamp 1920 on success, so a live width < that means the engine reset
        // the targets (display-mode change) and we must rebuild + re-swap both.
        // 960 is the vanilla base; kUpscaleMultiplier is the only knob.
        auto& props = rtMgr->renderTargetData[RE::RENDER_TARGET::kLOCAL_MAP];
        constexpr std::uint32_t kAppliedWidth = 960 * kUpscaleMultiplier;  // 1920
        if (props.width < kAppliedWidth) {
            SKSE::log::info("RenderTargetUpscaler: kLOCAL_MAP width {} regressed, re-applying upscale",
                props.width);
            Apply();
        }
    }
}
