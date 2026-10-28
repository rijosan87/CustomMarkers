#include "MinimapSnapshotTransition.h"
#include "MapDrawHelpers.h"
#include "Settings.h"

#include <SKSE/SKSE.h>
#include <Windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#pragma comment(lib, "d3dcompiler.lib")

struct MinimapRenderer::SnapshotImage {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
};

namespace MinimapSnapshotTransition {
    using Microsoft::WRL::ComPtr;
    using SnapshotImage = MinimapRenderer::SnapshotImage;

    struct SnapshotEaseConstants {
        float rowX[4]{};  // affine old-UV row, w = newest-image weight
        float rowY[4]{};
    };
    struct SnapshotEaseDraw {
        std::shared_ptr<SnapshotImage> oldImage, newImage;
        SnapshotEaseConstants constants;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11PixelShader> shader, savedShader;
        ComPtr<ID3D11Buffer> buffer, savedBuffer;
        ComPtr<ID3D11ShaderResourceView> savedOldSRV;
        std::array<ID3D11ClassInstance*, 256> savedClasses{};
        UINT classCount = 0;
    };
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

namespace {
    float SmoothstepLocal(float t) { return t * t * (3.0f - 2.0f * t); }
    using Microsoft::WRL::ComPtr;
    using Snapshot = MinimapRenderer::SnapshotProjection;
    using SnapshotImage = MinimapRenderer::SnapshotImage;
    using EaseTime = std::chrono::steady_clock;
    constexpr float kSnapshotEaseSeconds = 0.120f;

    // Capture may touch these only after matching the Present thread ID. The
    // engine target remains borrowed; the pool alone owns reusable pixel copies.
    std::atomic<DWORD> g_snapshotPresentThread{0};
    std::array<std::shared_ptr<SnapshotImage>, 3> g_snapshotImages;
    ComPtr<ID3D11Texture2D> g_snapshotSource;
    ComPtr<ID3D11PixelShader> g_snapshotShader;
    ComPtr<ID3D11Buffer> g_snapshotConstants;
    bool g_snapshotSetupFailed = false;
    bool g_snapshotVisible = false;
    bool g_snapshotDrawn = false;
    bool g_snapshotSessionVisible = false;
    unsigned g_snapshotViewMask = 0;
    Snapshot g_easeLatest, g_easeOld;
    EaseTime::time_point g_easeStarted{}, g_easeArrival{};
    float g_easeWeight = 1.0f;
    std::vector<std::unique_ptr<SnapshotEaseDraw>> g_snapshotDraws;

    void LogSnapshotEase(const char* reason, std::uint64_t generation, float interval = 0.0f) {
        if (Settings::GetSingleton()->minimapSnapshotLog)
            SKSE::log::info("MinimapSnapshot: ease reason={} generation={} intervalMs={:.2f} thread={} presentThread={}",
                reason, generation, interval * 1000.0f, GetCurrentThreadId(), g_snapshotPresentThread.load());
    }

    void ResetSnapshotEase() {
        if (g_easeOld.image) LogSnapshotEase("reset", g_easeLatest.generation);
        g_easeOld = {};
        g_easeLatest = {};
        g_easeWeight = 1.0f;
        g_snapshotVisible = false;
        g_snapshotViewMask = 0;
    }

    bool CreateSnapshotShader(ID3D11Device* device) {
        static constexpr char source[] = R"(
            struct PS_INPUT { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
            Texture2D newest : register(t0);
            Texture2D previous : register(t1);
            SamplerState sampler0 : register(s0);
            cbuffer Ease : register(b0) { float4 rowX; float4 rowY; };
            float4 main(PS_INPUT input) : SV_Target {
                float4 b = newest.Sample(sampler0, input.uv);
                float2 oldUV = float2(dot(rowX.xyz, float3(input.uv, 1)), dot(rowY.xyz, float3(input.uv, 1)));
                if (any(oldUV < 0) || any(oldUV > 1)) return input.col * b;
                float4 a = previous.Sample(sampler0, oldUV);
                float alpha = lerp(a.a, b.a, rowX.w);
                float3 rgb = lerp(a.rgb * a.a, b.rgb * b.a, rowX.w);
                return input.col * float4(alpha > 0.000001 ? rgb / alpha : float3(0, 0, 0), alpha);
            })";
        ComPtr<ID3DBlob> code, errors;
        HRESULT hr = D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr,
            "main", "ps_4_0", 0, 0, code.GetAddressOf(), errors.GetAddressOf());
        if (SUCCEEDED(hr))
            hr = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr,
                g_snapshotShader.ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr)) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = sizeof(SnapshotEaseConstants);
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            hr = device->CreateBuffer(&desc, nullptr, g_snapshotConstants.ReleaseAndGetAddressOf());
        }
        if (FAILED(hr)) {
            if (Settings::GetSingleton()->minimapSnapshotLog)
                SKSE::log::info("MinimapSnapshot: ease shader unavailable hr={:#x} error={}",
                    static_cast<std::uint32_t>(hr), errors ? static_cast<const char*>(errors->GetBufferPointer()) : "none");
            g_snapshotShader.Reset();
            g_snapshotConstants.Reset();
            return false;
        }
        return true;
    }

    std::shared_ptr<SnapshotImage> CopySnapshotImage(ID3D11ShaderResourceView* srv, std::uint64_t generation) {
        // Equality proves serialization without taking a lock around engine rendering.
        // Different threads keep the original borrowed-texture path.
        if (!srv || GetCurrentThreadId() != g_snapshotPresentThread.load()) {
            LogSnapshotEase("copy bypass: thread or SRV", generation);
            return {};
        }
        ComPtr<ID3D11Resource> resource;
        ComPtr<ID3D11Texture2D> source;
        srv->GetResource(resource.GetAddressOf());
        if (!resource || FAILED(resource.As(&source))) return {};
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        srv->GetDesc(&view);
        const bool rgba = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM || desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        const bool bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM || desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        if ((!rgba && !bgra) || desc.SampleDesc.Count != 1 || desc.ArraySize != 1 ||
            view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
            LogSnapshotEase("copy bypass: layout", generation);
            return {};
        }
        if (g_snapshotSource.Get() != source.Get()) {
            // Never exceed three owned images during target/device recreation.
            for (const auto& slot : g_snapshotImages) {
                if (slot && slot.use_count() != 1) {
                    LogSnapshotEase("copy bypass: target changed with leases", generation);
                    return {};
                }
            }
            g_snapshotImages = {};
            g_snapshotSource = source;
            g_snapshotShader.Reset();
            g_snapshotConstants.Reset();
            g_snapshotSetupFailed = false;
        }
        if (g_snapshotSetupFailed) return {};
        ComPtr<ID3D11Device> device;
        source->GetDevice(device.GetAddressOf());
        if (!g_snapshotShader && !CreateSnapshotShader(device.Get())) {
            g_snapshotSetupFailed = true;
            return {};
        }
        for (auto& slot : g_snapshotImages) {
            if (slot && slot.use_count() != 1) continue;
            if (!slot) {
                slot = std::make_shared<SnapshotImage>();
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags = 0;
                HRESULT hr = device->CreateTexture2D(&desc, nullptr, slot->texture.GetAddressOf());
                if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(slot->texture.Get(), &view, slot->srv.GetAddressOf());
                if (FAILED(hr)) {
                    slot.reset();
                    g_snapshotSetupFailed = true;
                    LogSnapshotEase("copy bypass: allocation", generation);
                    return {};
                }
            }
            ComPtr<ID3D11DeviceContext> context;
            device->GetImmediateContext(context.GetAddressOf());
            const auto start = EaseTime::now();
            // Reuse is safe after draw submission: copies and prior draws are
            // ordered on this same immediate context, without a GPU wait.
            context->CopyResource(slot->texture.Get(), source.Get());
            if (Settings::GetSingleton()->minimapSnapshotLog) {
                std::size_t leased = 0;
                for (const auto& item : g_snapshotImages) if (item && item.use_count() > 1) ++leased;
                SKSE::log::info("MinimapSnapshot: ease copied generation={} size={}x{} format={} leased={} submissionMs={:.3f} thread={} presentThread={}",
                    generation, desc.Width, desc.Height, static_cast<int>(desc.Format), leased + 1,
                    std::chrono::duration<float, std::milli>(EaseTime::now() - start).count(),
                    GetCurrentThreadId(), g_snapshotPresentThread.load());
            }
            return slot;
        }
        LogSnapshotEase("copy bypass: pool busy", generation);
        return {};
    }

    bool SnapshotEaseMap(const Snapshot& old, const Snapshot& latest, SnapshotEaseConstants& out) {
        ImVec2 n[3], o[3];
        for (int i = 0; i < 3; ++i) {
            auto p = latest.playerPos;
            if (i == 1) p.x += 128.0f;
            if (i == 2) p.y += 128.0f;
            if (!ProjectWithSnapshot(latest, p, {0, 0}, {1, 1}, n[i]) ||
                !ProjectWithSnapshot(old, p, {0, 0}, {1, 1}, o[i])) return false;
        }
        const float a = n[1].x - n[0].x, b = n[2].x - n[0].x;
        const float c = n[1].y - n[0].y, d = n[2].y - n[0].y;
        const float det = a * d - b * c;
        const float scale = std::sqrt((a*a + c*c) * (b*b + d*d));
        if (!std::isfinite(det) || !std::isfinite(scale) || scale <= 0 || std::abs(det) <= scale * 1e-6f) return false;
        const float ax = o[1].x - o[0].x, bx = o[2].x - o[0].x;
        const float ay = o[1].y - o[0].y, by = o[2].y - o[0].y;
        out.rowX[0] = (ax*d - bx*c) / det;
        out.rowX[1] = (bx*a - ax*b) / det;
        out.rowY[0] = (ay*d - by*c) / det;
        out.rowY[1] = (by*a - ay*b) / det;
        out.rowX[2] = o[0].x - out.rowX[0]*n[0].x - out.rowX[1]*n[0].y;
        out.rowY[2] = o[0].y - out.rowY[0]*n[0].x - out.rowY[1]*n[0].y;
        for (float v : out.rowX) if (!std::isfinite(v)) return false;
        for (float v : out.rowY) if (!std::isfinite(v)) return false;
        return true;
    }

    void PrepareSnapshotEase(const Snapshot& snap, const MinimapRenderer::NavigationFrame& frame, unsigned views) {
        g_snapshotDrawn = views != 0;
        const bool sameSpace = snap.isExterior == frame.isExterior &&
            (frame.isExterior ? snap.worldSpaceID == frame.worldSpaceID : snap.cellID == frame.cellID);
        if (!views || !Settings::GetSingleton()->navRefreshOnInspect || !sameSpace || !snap.valid || !snap.image) {
            ResetSnapshotEase();
            return;
        }
        const auto now = EaseTime::now();
        const float elapsed = std::chrono::duration<float>(now - g_easeStarted).count();
        if (g_easeOld.image) {
            g_easeWeight = SmoothstepLocal(std::clamp(elapsed / kSnapshotEaseSeconds, 0.0f, 1.0f));
            if (g_easeWeight >= 1.0f) {
                LogSnapshotEase("finished", g_easeLatest.generation);
                g_easeOld = {};
            }
        }
        if (!g_snapshotVisible || (views & ~g_snapshotViewMask) != 0) {
            ResetSnapshotEase();
        }
        if (snap.generation != g_easeLatest.generation) {
            const float interval = std::chrono::duration<float>(now - g_easeArrival).count();
            SnapshotEaseConstants map{};
            const bool compatible = g_easeLatest.image && snap.framing != 0 && snap.framing == g_easeLatest.framing &&
                snap.northRot == g_easeLatest.northRot && snap.isExterior == g_easeLatest.isExterior &&
                (snap.isExterior ? snap.worldSpaceID == g_easeLatest.worldSpaceID : snap.cellID == g_easeLatest.cellID) &&
                SnapshotEaseMap(g_easeLatest, snap, map);
            const bool interrupted = g_easeOld.image || interval < kSnapshotEaseSeconds;
            if (compatible && !interrupted) {
                g_easeOld = g_easeLatest;
                g_easeStarted = now;
                g_easeWeight = 0.0f;
            } else {
                g_easeOld = {};
                g_easeWeight = 1.0f;
            }
            g_easeLatest = snap;
            g_easeLatest.entities.clear();
            g_easeArrival = now;
            LogSnapshotEase(g_easeOld.image ? "started" : interrupted ? "newest wins" : "seed", snap.generation, interval);
        }
        g_snapshotVisible = true;
        g_snapshotViewMask = views;
    }

    void BeginSnapshotShader(const ImDrawList*, const ImDrawCmd* cmd) {
        auto& draw = *static_cast<SnapshotEaseDraw*>(cmd->UserCallbackData);
        auto* ctx = draw.context.Get();
        draw.classCount = static_cast<UINT>(draw.savedClasses.size());
        ctx->PSGetShader(draw.savedShader.ReleaseAndGetAddressOf(), draw.savedClasses.data(), &draw.classCount);
        ctx->PSGetShaderResources(1, 1, draw.savedOldSRV.ReleaseAndGetAddressOf());
        ctx->PSGetConstantBuffers(0, 1, draw.savedBuffer.ReleaseAndGetAddressOf());
        ctx->UpdateSubresource(draw.buffer.Get(), 0, nullptr, &draw.constants, 0, 0);
        ID3D11Buffer* buffer = draw.buffer.Get();
        ID3D11ShaderResourceView* old = draw.oldImage->srv.Get();
        ctx->PSSetConstantBuffers(0, 1, &buffer);
        ctx->PSSetShaderResources(1, 1, &old);
        ctx->PSSetShader(draw.shader.Get(), nullptr, 0);
    }

    void EndSnapshotShader(const ImDrawList*, const ImDrawCmd* cmd) {
        auto& draw = *static_cast<SnapshotEaseDraw*>(cmd->UserCallbackData);
        auto* ctx = draw.context.Get();
        ID3D11Buffer* buffer = draw.savedBuffer.Get();
        ID3D11ShaderResourceView* old = draw.savedOldSRV.Get();
        ctx->PSSetShader(draw.savedShader.Get(), draw.savedClasses.data(), draw.classCount);
        ctx->PSSetShaderResources(1, 1, &old);
        ctx->PSSetConstantBuffers(0, 1, &buffer);
        for (UINT i = 0; i < draw.classCount; ++i) {
            if (draw.savedClasses[i]) draw.savedClasses[i]->Release();
            draw.savedClasses[i] = nullptr;
        }
        draw.classCount = 0;
    }

    SnapshotEaseDraw* BeginSnapshotEaseDraw(ImDrawList* dl, const Snapshot& snap) {
        if (!g_easeOld.image || !snap.image || snap.generation != g_easeLatest.generation ||
            !g_snapshotShader || !g_snapshotConstants) return nullptr;
        auto draw = std::make_unique<SnapshotEaseDraw>();
        if (!SnapshotEaseMap(g_easeOld, snap, draw->constants)) return nullptr;
        draw->constants.rowX[3] = g_easeWeight;
        draw->oldImage = g_easeOld.image;
        draw->newImage = snap.image;
        draw->shader = g_snapshotShader;
        draw->buffer = g_snapshotConstants;
        ComPtr<ID3D11Device> device;
        snap.image->texture->GetDevice(device.GetAddressOf());
        device->GetImmediateContext(draw->context.GetAddressOf());
        auto* result = draw.get();
        g_snapshotDraws.push_back(std::move(draw));
        dl->AddCallback(BeginSnapshotShader, result);
        return result;
    }

    ID3D11ShaderResourceView* SnapshotDrawSRV(const Snapshot& snap) {
        return g_snapshotVisible && snap.image && snap.generation == g_easeLatest.generation
            ? snap.image->srv.Get() : snap.srv;
    }


}  // namespace

void BeginFrame() {
    g_snapshotPresentThread.store(GetCurrentThreadId());
    g_snapshotDrawn = false;
}

void Prepare(const MinimapRenderer::SnapshotProjection& snapshot,
             const MinimapRenderer::NavigationFrame& frame, unsigned views) {
    if (views && !g_snapshotSessionVisible) g_snapshotSetupFailed = false;
    g_snapshotSessionVisible = views != 0;
    PrepareSnapshotEase(snapshot, frame, views);
}

std::shared_ptr<MinimapRenderer::SnapshotImage> CopyImage(ID3D11ShaderResourceView* srv, std::uint64_t generation) {
    return CopySnapshotImage(srv, generation);
}

SnapshotEaseDraw* BeginDraw(ImDrawList* dl, const MinimapRenderer::SnapshotProjection& snapshot) {
    return BeginSnapshotEaseDraw(dl, snapshot);
}

void EndDraw(ImDrawList* dl, SnapshotEaseDraw* draw) {
    if (draw) dl->AddCallback(EndSnapshotShader, draw);
}

ID3D11ShaderResourceView* DrawSRV(const MinimapRenderer::SnapshotProjection& snapshot) {
    return SnapshotDrawSRV(snapshot);
}

void FinishFrame() {
    // ImGui has consumed every callback and texture reference for this frame.
    g_snapshotDraws.clear();
    if (!g_snapshotDrawn) {
        ResetSnapshotEase();
        g_snapshotSessionVisible = false;
    }
}
}  // namespace MinimapSnapshotTransition
