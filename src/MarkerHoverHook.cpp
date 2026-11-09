#include "MarkerHoverHook.h"
#include "MapMarkerRenderer.h"
#include "MarkerManager.h"
#include "InputHook.h"
#include "PlaceMarkerHook.h"
#include "HoverImageRenderer.h"
#include "MapLookRemapHook.h"
#include "Settings.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <Windows.h>
#include <atomic>
#include <cstring>
#include <memory>
#include "ImGui/Renderer.h"
#include <imgui.h>

namespace {
    constexpr std::ptrdiff_t kHoveredMarkerOffset = 0x30558;
    constexpr REL::RelocationID kSetSelectedMarkerStringID{ 270681, 216427 };
    constexpr REL::RelocationID kFxDelegateInvokeID{ 80520, 82640 };
    constexpr REL::RelocationID kOnMouseDownID{ 52230, 53117 };

    std::uint64_t                              g_trampolineAddr = 0;
    std::unique_ptr<PLH::x64Detour>            g_detour;

    std::uint64_t                              g_fxInvokeTrampoline = 0;
    std::unique_ptr<PLH::x64Detour>            g_fxInvokeDetour;

    std::uint64_t                              g_onMouseDownTrampoline = 0;
    std::unique_ptr<PLH::x64Detour>            g_onMouseDownDetour;

    const char* g_setSelectedMarkerLiteral = nullptr;

    using UpdateHoveredMarkerFn = void(*)(RE::MapMenu*);
    using FxDelegateInvokeFn    = void(*)(void* /*movie*/,
                                          const char* /*methodName*/,
                                          void* /*args*/,
                                          std::uint64_t /*reserved*/);
    using OnMouseDownFn         = void(*)(RE::MapMenu*);

    bool CtrlHeld() {
        return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    }

    struct GFxValueRaw {
        void*         value;
        std::uint32_t type;
        std::uint32_t pad;
        double        numberValue;
    };
    static_assert(sizeof(GFxValueRaw) == 0x18);

    using GetValuesFn = std::uint32_t(*)(void* a_this, GFxValueRaw** a_values);

    // Tracks last suppress state so we only log on transitions, not every frame.
    static bool s_lastSuppressState = false;
    constexpr std::uint64_t kCameraSettleMs = 150;
    float                   g_lastWorldToCam[4][4]{};
    std::uint64_t           g_lastCameraMoveTick = 0;
    std::atomic<bool>       g_cameraMoving{ false };

    RE::MapMenu*            g_hoverPassMenu = nullptr;

    void UpdateCameraMotion(RE::MapMenu* a_menu) {
        auto root = a_menu->GetRuntimeData2()->camera.cameraRoot;
        if (!root) return;
        auto& children = root->GetChildren();
        if (children.empty() || !children[0]) return;
        auto* camera = netimmerse_cast<RE::NiCamera*>(children[0].get());
        if (!camera) return;

        // The vanilla map camera keeps drifting by float noise after it stops, so
        // compare against the last accepted matrix with a relative threshold.
        constexpr float kMoveEpsilon = 1e-4f;
        const auto& worldToCam = camera->GetRuntimeData().worldToCam;
        const std::uint64_t now = GetTickCount64();
        float maxDelta = 0.0f;
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                const float ref = g_lastWorldToCam[r][c];
                const float d = std::fabs(worldToCam[r][c] - ref) / (std::max)(1.0f, std::fabs(ref));
                maxDelta = (std::max)(maxDelta, d);
            }
        }
        if (maxDelta > kMoveEpsilon) {
            std::memcpy(g_lastWorldToCam, worldToCam, sizeof(g_lastWorldToCam));
            g_lastCameraMoveTick = now;
        }

        const bool moving = now - g_lastCameraMoveTick < kCameraSettleMs;
        if (moving != g_cameraMoving.exchange(moving) &&
            Settings::GetSingleton()->cursorVerboseLog) {
            logger::info("MarkerHoverHook: map camera {}", moving ? "moving, hover off" : "settled, hover on");
        }
    }

    void HookedFxDelegateInvoke(void* a_movie, const char* a_methodName,
                                void* a_args, std::uint64_t a_reserved) {
        const bool isOurMethod = (a_methodName == g_setSelectedMarkerLiteral);

        if (isOurMethod && a_args) {
            bool suppress = CtrlHeld() || MapLookRemapHook::IsTilting()
                               || g_cameraMoving.load()
                               || InputHook::IsTextInputActive()
                               || MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen()
                               || MapMarkerRenderer::GetSingleton()->IsCustomPinHovered()
                               || MapMarkerRenderer::GetSingleton()->CursorOverPanel()
                               || (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse);


            if (suppress || g_hoverPassMenu) {
                auto** vtbl = *reinterpret_cast<void***>(a_args);
                auto getValues = reinterpret_cast<GetValuesFn>(vtbl[1]);

                GFxValueRaw* values = nullptr;
                std::uint32_t count = getValues(a_args, &values);

                if (!suppress && values && count > 0 && (values[count - 1].type & 0xFF) == 3) {
                    suppress = !HoverImageRenderer::IsWithinHoverRange(
                        g_hoverPassMenu, static_cast<std::int32_t>(values[count - 1].numberValue));
                }

                if (suppress && values) {
                    // Keep the icon zoomed while its right-click menu is open.
                    const std::int32_t menuIdx = HoverImageRenderer::GetOpenMenuMarkerIndex();
                    const double sent = menuIdx >= 0 ? static_cast<double>(menuIdx) : -1.0;
                    for (std::uint32_t i = 0; i < count; ++i) {
                        values[i].value       = nullptr;
                        values[i].type        = 3; // kNumber
                        values[i].numberValue = sent;
                    }
                }
            }
        }

        reinterpret_cast<FxDelegateInvokeFn>(g_fxInvokeTrampoline)(
            a_movie, a_methodName, a_args, a_reserved);
    }

    bool IsBCDCarriageMapReady(RE::MapMenu* a_menu) {
        return a_menu && a_menu->uiMovie &&
               a_menu->uiMovie->IsAvailable("_root.WorldMap.BCD__ClickSelectedMarker");
    }

    void HookedOnMouseDown(RE::MapMenu* a_menu) {
        if (auto* ui = RE::UI::GetSingleton(); ui &&
            ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME)) {
            reinterpret_cast<OnMouseDownFn>(g_onMouseDownTrampoline)(a_menu);
            return;
        }

        if (MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen() ||
            ImGuiRenderer::IsAnyPopupOpen() ||
            (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse)) {
            return;
        }

        if (auto removeId = MapMarkerRenderer::GetSingleton()->GetRemoveButtonTarget();
            removeId != 0) {
            MarkerManager::GetSingleton()->RemoveMarker(removeId);
            return;
        }

        if (auto destId = MapMarkerRenderer::GetSingleton()->GetDestButtonTarget();
            destId != 0) {
            // Real markers only: the synthesized vanilla destination has no
            // hover panel and must never be toggled through this path.
            auto markers = MarkerManager::GetSingleton()->GetUserMarkers();
            bool isCurrentlyDest = false;
            for (const auto& m : markers) {
                if (m.id == destId) { isCurrentlyDest = m.isDestination; break; }
            }
            MarkerManager::GetSingleton()->SetDestination(destId, !isCurrentlyDest);
            return;
        }

        if (auto editId = MapMarkerRenderer::GetSingleton()->GetEditButtonTarget();
            editId != 0) {
            MapMarkerRenderer::GetSingleton()->BeginEditMarker(editId);
            return;
        }

        if (MapMarkerRenderer::GetSingleton()->IsCustomPinHovered()) return;

        if (a_menu) {
            auto base = reinterpret_cast<std::uintptr_t>(a_menu);
            auto hoveredIdx = *reinterpret_cast<std::int32_t*>(base + kHoveredMarkerOffset);
            if (hoveredIdx >= 0) {
                if (HoverImageRenderer::IsHoveredUndiscovered()) {
                    const bool bcdReady = IsBCDCarriageMapReady(a_menu);
                    logger::debug("[BCDCompat] undiscovered click | markerIndex={} | overrideReady={}",
                                  hoveredIdx, bcdReady);
                    if (bcdReady) {
                        reinterpret_cast<OnMouseDownFn>(g_onMouseDownTrampoline)(a_menu);
                    }
                    return;
                }

                PlaceMarkerHook::SuppressNextPlacement();
                reinterpret_cast<OnMouseDownFn>(g_onMouseDownTrampoline)(a_menu);
                return;
            }
        }

        if (MapMarkerRenderer::GetSingleton()->TryHandlePanelClick()) return;

        if (a_menu && MapMarkerRenderer::GetSingleton()->CursorOverPanel()) {
            auto base = reinterpret_cast<std::uintptr_t>(a_menu);
            *reinterpret_cast<std::int32_t*>(base + kHoveredMarkerOffset) = -1;
        }
        reinterpret_cast<OnMouseDownFn>(g_onMouseDownTrampoline)(a_menu);
    }

    void HookedUpdateHoveredMarker(RE::MapMenu* a_menu) {
        if (a_menu) UpdateCameraMotion(a_menu);
        // Menu just closed: seed the hovered slot so vanilla sends a fresh
        // SetSelectedMarker and the held zoom cannot stick.
        static std::int32_t s_lastMenuIdx = -1;
        const std::int32_t menuIdx = HoverImageRenderer::GetOpenMenuMarkerIndex();
        if (a_menu && s_lastMenuIdx >= 0 && menuIdx < 0) {
            auto base = reinterpret_cast<std::uintptr_t>(a_menu);
            *reinterpret_cast<std::int32_t*>(base + kHoveredMarkerOffset) = s_lastMenuIdx;
        }
        s_lastMenuIdx = menuIdx;
        g_hoverPassMenu = a_menu;
        reinterpret_cast<UpdateHoveredMarkerFn>(g_trampolineAddr)(a_menu);
        g_hoverPassMenu = nullptr;
        if (!a_menu) return;
        if (CtrlHeld() || MapLookRemapHook::IsTilting()
                       || g_cameraMoving.load()
                       || MapMarkerRenderer::GetSingleton()->CursorOverPanel()
                       || InputHook::IsTextInputActive()
                       || MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen()
                       || MapMarkerRenderer::GetSingleton()->IsCustomPinHovered()
                       || (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse)) {
            auto base = reinterpret_cast<std::uintptr_t>(a_menu);
            *reinterpret_cast<std::int32_t*>(base + kHoveredMarkerOffset) = -1;
        } else if (auto base = reinterpret_cast<std::uintptr_t>(a_menu);
                   !HoverImageRenderer::IsWithinHoverRange(
                       a_menu, *reinterpret_cast<std::int32_t*>(base + kHoveredMarkerOffset))) {
            *reinterpret_cast<std::int32_t*>(base + kHoveredMarkerOffset) = -1;
        }
        HoverImageRenderer::SampleCardState(a_menu);
    }
}

void MarkerHoverHook::Install() {
    REL::Relocation<std::uintptr_t> target{ REL::RelocationID(52224, 53111) };
    const std::uint64_t targetAddr = target.address();
    const std::uint64_t hookAddr   = reinterpret_cast<std::uint64_t>(&HookedUpdateHoveredMarker);

    g_detour = std::make_unique<PLH::x64Detour>(targetAddr, hookAddr, &g_trampolineAddr);

    if (!g_detour->hook()) {
        logger::error("MarkerHoverHook: PolyHook x64Detour::hook() returned false");
        g_detour.reset();
        return;
    }

    logger::info("MarkerHoverHook installed | target={:X} | trampoline={:X}",
                 targetAddr, g_trampolineAddr);

    // Resolve the runtime address of the ".rdata" literal "SetSelectedMarker"
    // so the FxDelegate::Invoke detour can pointer-compare against it.
    REL::Relocation<const char*> stringLit{ kSetSelectedMarkerStringID };
    g_setSelectedMarkerLiteral = stringLit.get();

    REL::Relocation<std::uintptr_t> fxInvokeTarget{ kFxDelegateInvokeID };
    const std::uint64_t fxTargetAddr = fxInvokeTarget.address();
    const std::uint64_t fxHookAddr   = reinterpret_cast<std::uint64_t>(&HookedFxDelegateInvoke);

    g_fxInvokeDetour = std::make_unique<PLH::x64Detour>(
        fxTargetAddr, fxHookAddr, &g_fxInvokeTrampoline);

    if (!g_fxInvokeDetour->hook()) {
        logger::error("MarkerHoverHook: FxDelegate::Invoke detour failed");
        g_fxInvokeDetour.reset();
        return;
    }

    logger::info("FxDelegate::Invoke hook installed | target={:X} | trampoline={:X} | literalVA={}",
                 fxTargetAddr, g_fxInvokeTrampoline,
                 static_cast<const void*>(g_setSelectedMarkerLiteral));

    REL::Relocation<std::uintptr_t> onMouseDownTarget{ kOnMouseDownID };
    const std::uint64_t mdTargetAddr = onMouseDownTarget.address();
    const std::uint64_t mdHookAddr   = reinterpret_cast<std::uint64_t>(&HookedOnMouseDown);

    g_onMouseDownDetour = std::make_unique<PLH::x64Detour>(
        mdTargetAddr, mdHookAddr, &g_onMouseDownTrampoline);

    if (!g_onMouseDownDetour->hook()) {
        logger::error("MarkerHoverHook: OnMouseDown detour failed");
        g_onMouseDownDetour.reset();
        return;
    }

    logger::info("OnMouseDown hook installed | target={:X} | trampoline={:X}",
                 mdTargetAddr, g_onMouseDownTrampoline);
}
