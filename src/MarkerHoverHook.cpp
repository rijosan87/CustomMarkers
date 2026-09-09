// Hooks marker hover callbacks.

#include "MarkerHoverHook.h"
#include "MapMarkerRenderer.h"
#include "MarkerManager.h"
#include "InputHook.h"
#include "PlaceMarkerHook.h"
#include "HoverImageRenderer.h"
#include "MapLookRemapHook.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <Windows.h>
#include <memory>
#include "ImGui/Renderer.h"
#include <imgui.h>

// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):

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
    using FxDelegateInvokeFn    = void(*)(void* ,
                                          const char* ,
                                          void* ,
                                          std::uint64_t );
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

    static bool s_lastSuppressState = false;

    void HookedFxDelegateInvoke(void* a_movie, const char* a_methodName,
                                void* a_args, std::uint64_t a_reserved) {
        const bool isOurMethod = (a_methodName == g_setSelectedMarkerLiteral);
        if (isOurMethod && a_args) {
            const bool suppress = CtrlHeld() || MapLookRemapHook::IsTilting()
                               || InputHook::IsTextInputActive()
                               || MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen()
                               || MapMarkerRenderer::GetSingleton()->IsCustomPinHovered()
                               || MapMarkerRenderer::GetSingleton()->CursorOverPanel()
                               || (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse);
            if (suppress) {
                auto** vtbl = *reinterpret_cast<void***>(a_args);
                auto getValues = reinterpret_cast<GetValuesFn>(vtbl[1]);
                GFxValueRaw* values = nullptr;
                std::uint32_t count = getValues(a_args, &values);
                if (values) {
                    for (std::uint32_t i = 0; i < count; ++i) {
                        values[i].value       = nullptr;
                        values[i].type        = 3;
                        values[i].numberValue = -1.0;
                    }
                } else {
                }
            }
        }
        reinterpret_cast<FxDelegateInvokeFn>(g_fxInvokeTrampoline)(
            a_movie, a_methodName, a_args, a_reserved);
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
        reinterpret_cast<UpdateHoveredMarkerFn>(g_trampolineAddr)(a_menu);
        if (!a_menu) return;
        if (CtrlHeld() || MapLookRemapHook::IsTilting()
                       || MapMarkerRenderer::GetSingleton()->CursorOverPanel()
                       || InputHook::IsTextInputActive()
                       || MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen()
                       || MapMarkerRenderer::GetSingleton()->IsCustomPinHovered()
                       || (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse)) {
            auto base = reinterpret_cast<std::uintptr_t>(a_menu);
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
