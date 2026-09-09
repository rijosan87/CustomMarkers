// Handles render target resolution scaling and D3D11 resources.

// STATUS (update when behavior changes; do NOT mirror into CLAUDE.md):

#include "RenderTargetUpscaler.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <d3d11.h>

namespace {
    constexpr std::uint32_t kUpscaleMultiplier = 2;

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

    void SwapIntoEngine(RE::BSGraphics::RenderTargetData& a_slot, NewTarget& a_new) {
        if (a_slot.SRV)     reinterpret_cast<ID3D11ShaderResourceView*>(a_slot.SRV)->Release();
        if (a_slot.RTV)     reinterpret_cast<ID3D11RenderTargetView*>(a_slot.RTV)->Release();
        if (a_slot.texture) reinterpret_cast<ID3D11Texture2D*>(a_slot.texture)->Release();
        a_slot.texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(a_new.texture);
        a_slot.RTV     = reinterpret_cast<REX::W32::ID3D11RenderTargetView*>(a_new.rtv);
        a_slot.SRV     = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(a_new.srv);
        a_new.texture = nullptr;
        a_new.rtv     = nullptr;
        a_new.srv     = nullptr;
    }

    constexpr RE::RENDER_TARGET kTargets[] = {
        RE::RENDER_TARGET::kLOCAL_MAP,
        RE::RENDER_TARGET::kLOCAL_MAP_SWAP,
    };

    RE::BSGraphics::RenderTargetManager* GetRenderTargetManager() {
        static REL::Relocation<RE::BSGraphics::RenderTargetManager*> singleton{
            RELOCATION_ID(524970, 411451)
        };
        return singleton.get();
    }
}

namespace RenderTargetUpscaler {
    void Apply() {
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
        constexpr std::uint32_t kBaseW = 960;
        constexpr std::uint32_t kBaseH = 540;
        constexpr std::uint32_t newW = kBaseW * kUpscaleMultiplier;
        constexpr std::uint32_t newH = kBaseH * kUpscaleMultiplier;
        auto& baseProps = rtMgr->renderTargetData[RE::RENDER_TARGET::kLOCAL_MAP];
        if (baseProps.width >= newW) {
            SKSE::log::debug("RenderTargetUpscaler: kLOCAL_MAP already {}x{}, no upscale needed",
                baseProps.width, baseProps.height);
            return;
        }
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
                    "rolling back - both local-map targets left at vanilla size",
                    static_cast<int>(kTargets[i]));
                built[0].ReleaseAll();
                built[1].ReleaseAll();
                return;
            }
        }
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
        auto* rtMgr = GetRenderTargetManager();
        if (!rtMgr) return;
        auto& props = rtMgr->renderTargetData[RE::RENDER_TARGET::kLOCAL_MAP];
        constexpr std::uint32_t kAppliedWidth = 960 * kUpscaleMultiplier;
        if (props.width < kAppliedWidth) {
            SKSE::log::info("RenderTargetUpscaler: kLOCAL_MAP width {} regressed, re-applying upscale",
                props.width);
            Apply();
        }
    }
}
