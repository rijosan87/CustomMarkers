// ImGui DirectX 11 renderer and WndProc input hooking.

#include "ImGui/Renderer.h"
#include "MapMarkerRenderer.h"
#include "LocalMapRenderer.h"
#include "TextureManager.h"
#include "HoverImageRenderer.h"
#include "PathTracker.h"
#include "MinimapRenderer.h"
#include "WorldBeaconRenderer.h"
#include "LootBeamRenderer.h"
#include "LookPinRenderer.h"
#include "MapLookRemapHook.h"
#include "MapDrawHelpers.h"

#include "SettingsMenu.h"
#include "FontManager.h"
#include "BackgroundManager.h"
#include <algorithm>
#include <atomic>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <d3d11.h>
#include <dxgi.h>

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace ImGuiRenderer {
    static bool    g_Initialized   = false;
    static HWND    g_OutputWindow  = nullptr;
    static WNDPROC g_OriginalWndProc = nullptr;
    static std::atomic<bool> g_IsAnyPopupOpen{false};

    bool IsAnyPopupOpen() {
        return g_IsAnyPopupOpen.load();
    }

    static void PatchImGuiSamplerOnce() {
        static bool s_Patched = false;
        if (s_Patched) return;
        struct ImGui_ImplDX11_Data_Mirror {
            ID3D11Device*             pd3dDevice;
            ID3D11DeviceContext*      pd3dDeviceContext;
            IDXGIFactory*             pFactory;
            ID3D11Buffer*             pVB;
            ID3D11Buffer*             pIB;
            ID3D11VertexShader*       pVertexShader;
            ID3D11InputLayout*        pInputLayout;
            ID3D11Buffer*             pVertexConstantBuffer;
            ID3D11PixelShader*        pPixelShader;
            ID3D11SamplerState*       pFontSampler;
        };
        auto& io = ImGui::GetIO();
        auto* bd = static_cast<ImGui_ImplDX11_Data_Mirror*>(io.BackendRendererUserData);
        if (!bd || !bd->pd3dDevice || !bd->pFontSampler) return;
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU       = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.AddressV       = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.AddressW       = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MipLODBias     = 0.0f;
        sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        sd.MinLOD         = 0.0f;
        sd.MaxLOD         = D3D11_FLOAT32_MAX;
        ID3D11SamplerState* trilinearSampler = nullptr;
        if (SUCCEEDED(bd->pd3dDevice->CreateSamplerState(&sd, &trilinearSampler))) {
            bd->pFontSampler->Release();
            bd->pFontSampler = trilinearSampler;
            SKSE::log::info("ImGui sampler patched: MaxLOD raised to FLT_MAX (trilinear mip sampling enabled)");
        } else {
            SKSE::log::warn("Sampler patch failed: CreateSamplerState returned an error");
        }
        s_Patched = true;
    }

    static LRESULT CALLBACK WndProcThunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        if (g_Initialized && ImGui::GetCurrentContext()) {
            auto& io = ImGui::GetIO();
            if (io.WantCaptureMouse) {
                switch (uMsg) {
                    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
                    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
                        return 1;
                }
            }
            if (uMsg == WM_KILLFOCUS) {
                io.ClearInputKeys();
            }
        }
        return CallWindowProcA(g_OriginalWndProc, hWnd, uMsg, wParam, lParam);
    }

    struct PresentHook {
        static void thunk(std::uint32_t a_p1) {
            if (!g_Initialized) {
                if (const auto renderer = RE::BSGraphics::Renderer::GetSingleton()) {
                    const auto swapChain = (IDXGISwapChain*)renderer->GetRuntimeData().renderWindows[0].swapChain;
                    if (swapChain) {
                        DXGI_SWAP_CHAIN_DESC desc{};
                        if (SUCCEEDED(swapChain->GetDesc(std::addressof(desc)))) {
                            const auto device  = (ID3D11Device*)renderer->GetRuntimeData().forwarder;
                            const auto context = (ID3D11DeviceContext*)renderer->GetRuntimeData().context;
                            ImGui::CreateContext();
                            auto& io = ImGui::GetIO();
                            io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;
                            io.IniFilename = nullptr;
                            if (ImGui_ImplWin32_Init(desc.OutputWindow) && ImGui_ImplDX11_Init(device, context)) {
                                g_OutputWindow = desc.OutputWindow;
                                SKSE::log::info("ImGui initialized successfully");
                                g_Initialized = true;
                                FontManager::GetSingleton()->LoadFonts(io);
                                TextureManager::Init(device);
                                BackgroundManager::Init();
                                g_OriginalWndProc = reinterpret_cast<WNDPROC>(
                                    SetWindowLongPtrA(desc.OutputWindow, GWLP_WNDPROC,
                                                      reinterpret_cast<LONG_PTR>(WndProcThunk)));
                                SKSE::log::info("WndProc swap installed");
                            }
                        }
                    }
                }
            }
            if (g_Initialized) {
                PatchImGuiSamplerOnce();
                ImGui_ImplDX11_NewFrame();
                ImGui_ImplWin32_NewFrame();
                PathTracker::GetSingleton()->Update();
                MapLookRemapHook::PinCursorIfTilting();
                auto& io = ImGui::GetIO();
                static bool s_gameplayCursorActive = false;
                const bool gpHosted = SettingsMenu::IsGameplayHosted();
                if (gpHosted != s_gameplayCursorActive) {
                    s_gameplayCursorActive = gpHosted;
                    if (gpHosted) {
                        logger::debug("Gameplay cursor source activated");
                    }
                }
                if (gpHosted && g_OutputWindow) {
                    POINT p{};
                    if (GetCursorPos(&p) && ScreenToClient(g_OutputWindow, &p)) {
                        const float x = std::clamp(static_cast<float>(p.x), 0.0f,
                                                   (std::max)(0.0f, io.DisplaySize.x - 1.0f));
                        const float y = std::clamp(static_cast<float>(p.y), 0.0f,
                                                   (std::max)(0.0f, io.DisplaySize.y - 1.0f));
                        io.AddMousePosEvent(x, y);
                    }
                } else if (auto* ui = RE::UI::GetSingleton()) {
                    if (ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
                        if (auto* cursor = RE::MenuCursor::GetSingleton()) {
                            float w = cursor->screenWidthX;
                            float h = cursor->screenWidthY;
                            if (w > 0.0f && h > 0.0f) {
                                float mx = (cursor->cursorPosX / w) * io.DisplaySize.x;
                                float my = (cursor->cursorPosY / h) * io.DisplaySize.y;
                                io.AddMousePosEvent(mx, my);
                            }
                        }
                    }
                }
                const bool lDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                const bool rDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
                const bool mDown = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
                io.AddMouseButtonEvent(0, lDown);
                io.AddMouseButtonEvent(1, rDown);
                io.AddMouseButtonEvent(2, mDown);
                ImGui::NewFrame();
                MapMarkerRenderer::GetSingleton()->Draw();
                LocalMapRenderer::GetSingleton()->Draw();
                HoverImageRenderer::Draw();
                MinimapRenderer::GetSingleton()->Draw();
                WorldBeaconRenderer::GetSingleton()->Draw();
                LootBeamRenderer::GetSingleton()->Draw();
                LookPinRenderer::GetSingleton()->Draw();
                SettingsMenu::Draw();
                if (SettingsMenu::IsVisible()) {
                    const ImVec2 p = ImGui::GetIO().MousePos;
                    if (p.x >= 0.0f && p.y >= 0.0f) {
                        MapDraw::RequestCursor(p.x, p.y);
                    }
                }
                MapDraw::DrawRequestedCursor();
                g_IsAnyPopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
                ImGui::EndFrame();
                ImGui::Render();
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            }
            func(a_p1);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    void Install() {
        SKSE::log::info("Installing ImGui Present hook...");
        SKSE::AllocTrampoline(14);
        auto& trampoline = SKSE::GetTrampoline();
        REL::Relocation<std::uintptr_t> present_target{ RELOCATION_ID(75461, 77246) };
        MinimapRenderer::GetSingleton()->Install();
        PresentHook::func = trampoline.write_call<5>(present_target.address() + 0x9, PresentHook::thunk);
        SKSE::log::info("ImGui Present hook installed successfully");
    }
}
