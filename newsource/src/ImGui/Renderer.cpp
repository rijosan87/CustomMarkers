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
#include "MapDrawHelpers.h"   // MapDraw::DrawRequestedCursor (frame-end cursor)
// #include "LocalMapHudSpike.h"
#include "SettingsMenu.h"
#include "Settings.h"
#include "FontManager.h"
#include "BackgroundManager.h"
#include <algorithm>
#include <atomic>

#include <imgui.h>
#include <imgui_internal.h>
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

    // Read ImGui internals only on the render thread. WantTextInput alone
    // cannot identify which field owns input or whether NewFrame processed it.
    static void LogInputFrame() {
        static bool wasEnabled = false;
        static ImGuiID lastActive = 0;
        static unsigned lastFlags = 0;
        static double lastHeartbeat = 0.0;
        if (!Settings::GetSingleton()->inputVerboseLog) {
            wasEnabled = false;
            return;
        }
        auto& g = *ImGui::GetCurrentContext();
        auto& io = ImGui::GetIO();
        const bool dialog = MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen();
        const bool settings = SettingsMenu::IsVisible();
        const unsigned flags = (io.WantTextInput ? 1u : 0u) |
            (io.WantCaptureKeyboard ? 2u : 0u) | (io.WantCaptureMouse ? 4u : 0u) |
            (dialog ? 8u : 0u) | (settings ? 16u : 0u) |
            (SettingsMenu::IsGameplayHosted() ? 32u : 0u);
        if (!wasEnabled) {
            SKSE::log::info("[InputVerbose] enabled imgui={} wcharBytes={} trickle={} scope=all_arriving_characters_and_buttons upstream_removed_events_are_not_visible",
                           IMGUI_VERSION, sizeof(ImWchar), io.ConfigInputTrickleEventQueue);
        }
        bool textOrKey = false;
        for (const auto& event : g.InputEventsTrail) {
            if (event.Type == ImGuiInputEventType_Text) {
                textOrKey = true;
                SKSE::log::info("[InputVerbose] imgui_processed frame={} thread={} codepoint=U+{:04X}",
                               g.FrameCount, GetCurrentThreadId(), event.Text.Char);
            } else if (event.Type == ImGuiInputEventType_Key) {
                textOrKey = true;
                SKSE::log::info("[InputVerbose] imgui_key frame={} key={} down={} analog={}",
                               g.FrameCount, static_cast<int>(event.Key.Key), event.Key.Down, event.Key.AnalogValue);
            } else if (event.Type == ImGuiInputEventType_MouseButton) {
                textOrKey = true;
                SKSE::log::info("[InputVerbose] imgui_mouse frame={} button={} down={}",
                               g.FrameCount, event.MouseButton.Button, event.MouseButton.Down);
            }
        }
        const bool changed = !wasEnabled || lastActive != g.ActiveId || lastFlags != flags;
        if (changed || textOrKey || g.Time - lastHeartbeat >= 1.0) {
            SKSE::log::info(
                "[InputVerbose] frame={} thread={} active=0x{:X} window={} wantText={} captureKeyboard={} captureMouse={} dialog={} settings={} gameplay={} pending={} processed={} mouseX={} mouseY={} left={} right={} ctrl={} shift={} alt={} super={}",
                g.FrameCount, GetCurrentThreadId(), g.ActiveId, g.NavWindow ? g.NavWindow->Name : "none",
                io.WantTextInput, io.WantCaptureKeyboard, io.WantCaptureMouse, dialog, settings,
                SettingsMenu::IsGameplayHosted(), g.InputEventsQueue.Size, g.InputEventsTrail.Size,
                io.MousePos.x, io.MousePos.y, io.MouseDown[0], io.MouseDown[1],
                io.KeyCtrl, io.KeyShift, io.KeyAlt, io.KeySuper);
            if (auto* textState = ImGui::GetInputTextState(g.ActiveId)) {
                SKSE::log::info("[InputVerbose] text_state frame={} id=0x{:X} cursor={} selectionStart={} selectionEnd={} offsets=imgui_native_units",
                               g.FrameCount, textState->ID, textState->GetCursorPos(),
                               textState->GetSelectionStart(), textState->GetSelectionEnd());
            }
            lastHeartbeat = g.Time;
        }
        lastActive = g.ActiveId;
        lastFlags = flags;
        wasEnabled = true;
    }

    bool IsAnyPopupOpen() {
        return g_IsAnyPopupOpen.load();
    }

    void LogInputEnvironment(const char* reason) {
        if (!Settings::GetSingleton()->inputVerboseLog) return;

        POINT screen{};
        const bool cursorValid = GetCursorPos(&screen) != FALSE;
        const HWND foreground = GetForegroundWindow();
        const HWND active = GetActiveWindow();
        const HWND focus = GetFocus();
        const HWND underCursor = cursorValid ? WindowFromPoint(screen) : nullptr;

        DWORD foregroundPid = 0;
        DWORD activePid = 0;
        DWORD focusPid = 0;
        DWORD underCursorPid = 0;
        if (foreground) GetWindowThreadProcessId(foreground, &foregroundPid);
        if (active) GetWindowThreadProcessId(active, &activePid);
        if (focus) GetWindowThreadProcessId(focus, &focusPid);
        if (underCursor) GetWindowThreadProcessId(underCursor, &underCursorPid);

        RECT clientScreen{};
        bool clientValid = false;
        bool insideClient = false;
        if (g_OutputWindow) {
            RECT client{};
            if (GetClientRect(g_OutputWindow, &client)) {
                POINT topLeft{ client.left, client.top };
                POINT bottomRight{ client.right, client.bottom };
                if (ClientToScreen(g_OutputWindow, &topLeft) &&
                    ClientToScreen(g_OutputWindow, &bottomRight)) {
                    clientScreen = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
                    clientValid = true;
                    insideClient = cursorValid && PtInRect(&clientScreen, screen) != FALSE;
                }
            }
        }

        RECT clip{};
        const bool clipValid = GetClipCursor(&clip) != FALSE;
        const HMONITOR cursorMonitor = cursorValid
            ? MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST) : nullptr;
        const HMONITOR gameMonitor = g_OutputWindow
            ? MonitorFromWindow(g_OutputWindow, MONITOR_DEFAULTTONEAREST) : nullptr;
        const DWORD processPid = GetCurrentProcessId();

        logger::info(
            "[InputVerbose] windows reason={} thread={} cursorValid={} cursor=({}, {}) insideGameClient={} clientValid={} client=({}, {}, {}, {}) clipValid={} clip=({}, {}, {}, {}) gameWindow={} foreground={} foregroundPid={} foregroundIsGame={} active={} activePid={} activeIsGame={} focus={} focusPid={} focusIsGame={} underCursor={} underCursorPid={} underCursorIsGame={} cursorMonitor={} gameMonitor={} sameMonitor={}",
            reason ? reason : "unknown", GetCurrentThreadId(), cursorValid,
            screen.x, screen.y, insideClient, clientValid,
            clientScreen.left, clientScreen.top, clientScreen.right, clientScreen.bottom,
            clipValid, clip.left, clip.top, clip.right, clip.bottom,
            static_cast<void*>(g_OutputWindow), static_cast<void*>(foreground), foregroundPid,
            foregroundPid == processPid, static_cast<void*>(active), activePid,
            activePid == processPid, static_cast<void*>(focus), focusPid,
            focusPid == processPid, static_cast<void*>(underCursor), underCursorPid,
            underCursorPid == processPid, static_cast<void*>(cursorMonitor),
            static_cast<void*>(gameMonitor), cursorMonitor && cursorMonitor == gameMonitor);
    }

    // The OS pointer can leave Skyrim while the visible map cursor remains in
    // the game. Sample every frame but emit only ownership/boundary transitions,
    // otherwise ordinary pointer motion would make InputVerbose unusable.
    static void LogMapCursorEnvironmentTransitions() {
        static bool wasMapOpen = false;
        static bool lastCursorValid = false;
        static bool lastInsideClient = false;
        static DWORD lastUnderCursorPid = 0;
        static HMONITOR lastCursorMonitor = nullptr;

        if (!Settings::GetSingleton()->inputVerboseLog || !g_OutputWindow) {
            wasMapOpen = false;
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        const bool mapOpen = ui && ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
        if (!mapOpen) {
            wasMapOpen = false;
            return;
        }

        POINT screen{};
        const bool cursorValid = GetCursorPos(&screen) != FALSE;
        bool insideClient = false;
        if (cursorValid) {
            RECT client{};
            if (GetClientRect(g_OutputWindow, &client)) {
                POINT topLeft{ client.left, client.top };
                POINT bottomRight{ client.right, client.bottom };
                if (ClientToScreen(g_OutputWindow, &topLeft) &&
                    ClientToScreen(g_OutputWindow, &bottomRight)) {
                    const RECT clientScreen{
                        topLeft.x, topLeft.y, bottomRight.x, bottomRight.y
                    };
                    insideClient = PtInRect(&clientScreen, screen) != FALSE;
                }
            }
        }

        DWORD underCursorPid = 0;
        if (cursorValid) {
            if (const HWND underCursor = WindowFromPoint(screen)) {
                GetWindowThreadProcessId(underCursor, &underCursorPid);
            }
        }
        const HMONITOR cursorMonitor = cursorValid
            ? MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST) : nullptr;

        const bool changed = !wasMapOpen || cursorValid != lastCursorValid ||
            insideClient != lastInsideClient || underCursorPid != lastUnderCursorPid ||
            cursorMonitor != lastCursorMonitor;
        if (changed) {
            LogInputEnvironment("map_cursor_transition");
        }

        wasMapOpen = true;
        lastCursorValid = cursorValid;
        lastInsideClient = insideClient;
        lastUnderCursorPid = underCursorPid;
        lastCursorMonitor = cursorMonitor;
    }

    // Sample every frame but log only transitions and one summary per map
    // session. The existing SetVisible log proves the call ran; this proves
    // whether the same Scaleform movie stayed hidden afterward.
    static void LogCursorMenuState() {
        static bool wasEnabled = false;
        static bool lastMapOpen = false;
        static std::uintptr_t lastMenu = 0;
        static std::uintptr_t lastMovie = 0;
        static int lastVisible = -2;
        static std::uint64_t frames = 0;
        static std::uint64_t availableSamples = 0;
        static std::uint64_t visibleSamples = 0;

        if (!Settings::GetSingleton()->cursorVerboseLog) {
            wasEnabled = false;
            lastMapOpen = false;
            lastMenu = 0;
            lastMovie = 0;
            lastVisible = -2;
            frames = 0;
            availableSamples = 0;
            visibleSamples = 0;
            return;
        }

        auto* ui = RE::UI::GetSingleton();
        const bool mapOpen = ui && ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
        auto cursorMenu = ui ? ui->GetMenu<RE::CursorMenu>() : nullptr;
        auto* movie = cursorMenu ? cursorMenu->uiMovie.get() : nullptr;
        const auto menuAddress = reinterpret_cast<std::uintptr_t>(cursorMenu.get());
        const auto movieAddress = reinterpret_cast<std::uintptr_t>(movie);
        const int visible = movie ? static_cast<int>(movie->GetVisible()) : -1;

        if (mapOpen) {
            ++frames;
            if (visible >= 0) {
                ++availableSamples;
                if (visible == 1) {
                    ++visibleSamples;
                }
            }
        }

        if (lastMapOpen && !mapOpen) {
            logger::info(
                "[CursorVerbose] map_session_end frames={} availableSamples={} hiddenSamples={} visibleSamples={} visibleObserved={}",
                frames, availableSamples, availableSamples - visibleSamples,
                visibleSamples, visibleSamples > 0);
            frames = 0;
            availableSamples = 0;
            visibleSamples = 0;
        }

        if (!wasEnabled || mapOpen != lastMapOpen || menuAddress != lastMenu ||
            movieAddress != lastMovie || visible != lastVisible) {
            logger::info(
                "[CursorVerbose] state frame={} mapOpen={} cursorMenu={} uiMovie={} visible={} unexpectedVisible={}",
                ImGui::GetFrameCount(), mapOpen,
                static_cast<const void*>(cursorMenu.get()),
                static_cast<const void*>(movie),
                visible < 0 ? "unavailable" : (visible ? "true" : "false"),
                mapOpen && visible == 1);
        }

        wasEnabled = true;
        lastMapOpen = mapOpen;
        lastMenu = menuAddress;
        lastMovie = movieAddress;
        lastVisible = visible;
    }

    // Sampler patch (lazy, runs once after ImGui creates its device objects)
    // ImGui's DX11 backend creates pFontSampler with MaxLOD=0, which clamps the
    // GPU to mip 0 only — making generated mipmaps in TextureManager useless
    // for downscaled UI icons. Replace pFontSampler with one that has
    // MaxLOD=FLT_MAX so trilinear filtering picks lower mips.
    //
    // Must run AFTER ImGui_ImplDX11_CreateDeviceObjects (first RenderDrawData),
    // not at Init time — CreateDeviceObjects has logic that calls Invalidate
    // when pFontSampler is already non-null, which would nuke the font texture
    // and crash on next draw. So we patch lazily once the sampler exists.
    //
    // Mirrors imgui 1.89.1 ImGui_ImplDX11_Data layout (imgui_impl_dx11.cpp L47-56).
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
        if (!bd || !bd->pd3dDevice || !bd->pFontSampler) return;  // not ready yet

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

    // WndProc swap.
    // Skyrim's map menu absorbs Scaleform input, so left/right mouse-button
    // events never reach BSInputDeviceManager's kButton queue. We can't catch
    // them in InputHook. Instead, we install our own WndProc to read raw Win32
    // mouse messages and (a) swallow them while ImGui wants the click, so the
    // engine's MapMenu raycast doesn't double-fire underneath the dialog.
    // The actual click state is fed to ImGui below via GetAsyncKeyState so we
    // don't rely on WM_*BUTTON* messages — they're only used here for blocking.
    static LRESULT CALLBACK WndProcThunk(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        // Observe Windows delivery without feeding another text path. These
        // messages are supporting evidence, not a guaranteed upstream probe.
        if (Settings::GetSingleton()->inputVerboseLog &&
            (uMsg == WM_SETFOCUS || uMsg == WM_KILLFOCUS || uMsg == WM_ACTIVATEAPP ||
             uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYDOWN ||
             uMsg == WM_SYSKEYUP || uMsg == WM_CHAR || uMsg == WM_UNICHAR ||
             uMsg == WM_DEADCHAR || uMsg == WM_SYSCHAR || uMsg == WM_INPUTLANGCHANGE ||
             uMsg == WM_IME_STARTCOMPOSITION || uMsg == WM_IME_COMPOSITION || uMsg == WM_IME_ENDCOMPOSITION ||
             uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONUP ||
             uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONUP ||
             uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL)) {
            SKSE::log::info("[InputVerbose] win32 thread={} message=0x{:X} wparam=0x{:X} lparam=0x{:X} initialized={}",
                           GetCurrentThreadId(), uMsg, static_cast<std::uintptr_t>(wParam),
                           static_cast<std::uintptr_t>(lParam), g_Initialized);
            if (uMsg == WM_SETFOCUS || uMsg == WM_KILLFOCUS || uMsg == WM_ACTIVATEAPP ||
                uMsg == WM_MBUTTONDOWN || uMsg == WM_MBUTTONUP ||
                uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL) {
                LogInputEnvironment("win32_message");
            }
        }
        // GetCurrentContext() is the real liveness check, not g_Initialized.
        // On force-quit (triple-Q) the engine tears the window down while this
        // thunk is still installed; reading GetIO() off a destroyed context
        // returns garbage, WantCaptureMouse comes back true, and we swallow
        // every mouse-button message system-wide until the window dies.
        if (g_Initialized && ImGui::GetCurrentContext()) {
            auto& io = ImGui::GetIO();
            if (io.WantCaptureMouse) {
                switch (uMsg) {
                    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
                    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
                    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
                        if (Settings::GetSingleton()->inputVerboseLog) {
                            SKSE::log::info("[InputVerbose] win32_blocked thread={} message=0x{:X} reason=WantCaptureMouse",
                                           GetCurrentThreadId(), uMsg);
                        }
                        return 1;
                }
            }
            if (uMsg == WM_KILLFOCUS) {
                io.ClearInputKeys();
            }
        }
        return CallWindowProcA(g_OriginalWndProc, hWnd, uMsg, wParam, lParam);
    }

    // Capture the inherited target before the ImGui backend changes viewport state.
    // Present-time state does not reveal the viewport used inside the snapshot pass.
    static void LogRenderSpace() {
        const auto* settings = Settings::GetSingleton();
        if (!settings->mapPinProjectionLog && !settings->minimapSnapshotLog) return;
        static double nextLog = 0.0;
        if (ImGui::GetTime() < nextLog) return;
        nextLog = ImGui::GetTime() + 1.0;

        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer) return;
        auto& data = renderer->GetRuntimeData();
        auto* context = reinterpret_cast<ID3D11DeviceContext*>(data.context);
        auto* swapChain = reinterpret_cast<IDXGISwapChain*>(data.renderWindows[0].swapChain);
        if (!context) return;

        D3D11_TEXTURE2D_DESC boundDesc{}, backDesc{};
        ID3D11RenderTargetView* rtv = nullptr;
        context->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv) {
            ID3D11Resource* resource = nullptr;
            rtv->GetResource(&resource);
            if (resource) {
                ID3D11Texture2D* texture = nullptr;
                if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)))) {
                    texture->GetDesc(&boundDesc);
                    texture->Release();
                }
                resource->Release();
            }
            rtv->Release();
        }
        if (swapChain) {
            ID3D11Texture2D* texture = nullptr;
            if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture)))) {
                texture->GetDesc(&backDesc);
                texture->Release();
            }
        }
        D3D11_VIEWPORT ports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        context->RSGetViewports(&count, ports);
        RECT client{};
        const bool clientValid = g_OutputWindow && GetClientRect(g_OutputWindow, &client);
        const auto& io = ImGui::GetIO();
        auto* cursor = RE::MenuCursor::GetSingleton();
        SKSE::log::info("[RenderSpace] frame={} stage=before-imgui bound={}x{} backbuffer={}x{} clientValid={} client={}x{} display={}x{} framebufferScale=({}, {}) cursorSpace={}x{} viewportCount={} missingTextureSize=0",
            ImGui::GetFrameCount(), boundDesc.Width, boundDesc.Height, backDesc.Width, backDesc.Height,
            clientValid, client.right - client.left, client.bottom - client.top,
            io.DisplaySize.x, io.DisplaySize.y, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y,
            cursor ? cursor->screenWidthX : 0.0f, cursor ? cursor->screenWidthY : 0.0f, count);
        for (UINT i = 0; i < count; ++i) {
            const auto& port = ports[i];
            SKSE::log::info("[RenderSpace] frame={} viewport={} origin=({}, {}) size={}x{} depth=({}, {})",
                ImGui::GetFrameCount(), i, port.TopLeftX, port.TopLeftY, port.Width, port.Height,
                port.MinDepth, port.MaxDepth);
        }
    }

    // The installed DX11 backend uses DisplaySize directly for its viewport and
    // ignores FramebufferScale. Convert only submitted draw data so layout and
    // cursor hit testing remain in client coordinates under borderless upscaling.
    // Confirmed in-game 2026-09-11 at 0.75 scale: pins, badges and animated rings
    // stay aligned through tilt/pan, and cursor clicks land correctly. Evidence:
    // findings/findings-Display-Tweaks-Rendering.md; native 4K/ultrawide untested.
    static void RenderToBoundTarget(ImDrawData* drawData) {
        if (!drawData || drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) return;
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer) return;
        auto* context = reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context);
        if (!context) return;
        ID3D11RenderTargetView* rtv = nullptr;
        context->OMGetRenderTargets(1, &rtv, nullptr);
        if (!rtv) return;
        ID3D11Resource* resource = nullptr;
        rtv->GetResource(&resource);
        rtv->Release();
        if (!resource) return;
        ID3D11Texture2D* texture = nullptr;
        const HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
        resource->Release();
        if (FAILED(hr)) return;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        texture->Release();
        if (!desc.Width || !desc.Height) return;

        const ImVec2 scale(static_cast<float>(desc.Width) / drawData->DisplaySize.x,
                           static_cast<float>(desc.Height) / drawData->DisplaySize.y);
        if (scale.x == 1.0f && scale.y == 1.0f) {
            ImGui_ImplDX11_RenderDrawData(drawData);
            return;
        }
        const ImVec2 displaySize = drawData->DisplaySize;
        const ImVec2 displayPos = drawData->DisplayPos;
        auto scaleVertices = [drawData](ImVec2 factor) {
            for (int i = 0; i < drawData->CmdListsCount; ++i) {
                for (auto& vertex : drawData->CmdLists[i]->VtxBuffer) {
                    vertex.pos.x *= factor.x;
                    vertex.pos.y *= factor.y;
                }
            }
            drawData->ScaleClipRects(factor);
        };
        scaleVertices(scale);
        drawData->DisplayPos = ImVec2(displayPos.x * scale.x, displayPos.y * scale.y);
        drawData->DisplaySize = ImVec2(static_cast<float>(desc.Width), static_cast<float>(desc.Height));
        ImGui_ImplDX11_RenderDrawData(drawData);
        scaleVertices(ImVec2(1.0f / scale.x, 1.0f / scale.y));
        drawData->DisplayPos = displayPos;
        drawData->DisplaySize = displaySize;
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

                                // Load the TTF atlas now, before the DX11 backend
                                // creates its device objects on the first
                                // RenderDrawData — the font texture is built there.
                                FontManager::GetSingleton()->LoadFonts(io);

                                TextureManager::Init(device);
                                // After TextureManager: the scan itself is
                                // filesystem-only, but every texture it hands
                                // out later needs the device set first.
                                BackgroundManager::Init();

                                // Install WndProc swap. Required because the map menu
                                // routes mouse input through Scaleform, so we never see
                                // mouse buttons in the kButton queue. WndProc is upstream
                                // of Scaleform and always sees them.
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

                // Pin the MenuCursor at the tilt press point while middle-mouse
                // tilt is active, before anything reads it this frame. Stops the
                // cursor reaching the screen edge and edge-panning during tilt.
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

                // Cursor position: in gameplay host mode, read the Win32 client coordinates.
                // In MapMenu, read Skyrim's animated MenuCursor (the OS cursor is hidden).
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

                // Mouse buttons: poll the kernel async key state directly.
                const bool lDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                const bool rDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
                const bool mDown = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
                io.AddMouseButtonEvent(0, lDown);
                io.AddMouseButtonEvent(1, rDown);
                io.AddMouseButtonEvent(2, mDown);

                ImGui::NewFrame();

                LogMapCursorEnvironmentTransitions();
                LogCursorMenuState();

                MapMarkerRenderer::GetSingleton()->Draw();
                LocalMapRenderer::GetSingleton()->Draw();
                HoverImageRenderer::Draw();
                MinimapRenderer::GetSingleton()->Draw();
                WorldBeaconRenderer::GetSingleton()->Draw();
                LootBeamRenderer::GetSingleton()->Draw();
                LookPinRenderer::GetSingleton()->Draw();
                // LocalMapHudSpike::Draw();
                SettingsMenu::Draw();

                // Map/HUD cursor, last.
                if (SettingsMenu::IsVisible()) {
                    const ImVec2 p = ImGui::GetIO().MousePos;
                    if (p.x >= 0.0f && p.y >= 0.0f) {
                        MapDraw::RequestCursor(p.x, p.y);
                    }
                }
                MapDraw::DrawRequestedCursor();

                g_IsAnyPopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

                ImGui::EndFrame();
                LogInputFrame();
                ImGui::Render();
                LogRenderSpace();
                RenderToBoundTarget(ImGui::GetDrawData());
            }

            func(a_p1);
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    void Install() {
        SKSE::log::info("Installing ImGui Present hook...");

        auto& trampoline = SKSE::GetTrampoline();

        REL::Relocation<std::uintptr_t> present_target{ RELOCATION_ID(75461, 77246) };
        MinimapRenderer::GetSingleton()->Install();
        PresentHook::func = trampoline.write_call<5>(present_target.address() + 0x9, PresentHook::thunk);

        SKSE::log::info("ImGui Present hook installed successfully");
    }
}
