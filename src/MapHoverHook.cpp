// Hooks map menu hover events to detect hovered markers.

#include "MapHoverHook.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <Windows.h>
#include <memory>

// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):

namespace {
    constexpr REL::RelocationID kObjectInterfaceInvokeID{ 80233, 82256 };
    constexpr REL::RelocationID kUpdateDescriptionCardID{ 52187, 53079 };
    constexpr REL::RelocationID kSetSelectedMarkerStringID{ 270681, 216427 };

    std::uint64_t                              g_invokeTrampoline = 0;
    std::unique_ptr<PLH::x64Detour>            g_invokeDetour;

    std::uint64_t                              g_cardTrampoline = 0;
    std::unique_ptr<PLH::x64Detour>            g_cardDetour;

    const char* g_setSelectedMarkerLiteral = nullptr;

    using ObjectInterfaceInvokeFn =
        bool(*)(RE::GFxValue::ObjectInterface* a_this, void* a_data,
                RE::GFxValue* a_result, const char* a_name,
                RE::GFxValue* a_args, RE::UPInt a_numArgs, bool a_isDObj);

    using UpdateDescriptionCardFn =
        void(*)(void* a_obj, void* a_menu, std::int32_t a_index, void* a_unk);

    bool CtrlHeld() {
        return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    }

    constexpr std::uintptr_t kLocalMapBaseOffset = 0x60;
    constexpr std::uintptr_t kModeByteOffset     = 0x303FC;

    bool IsWorldMapShowing() {
        auto* ui = RE::UI::GetSingleton();
        if (!ui || !ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) return false;
        auto mapMenu = ui->GetMenu<RE::MapMenu>();
        if (!mapMenu) return false;
        const auto lmBase =
            reinterpret_cast<std::uintptr_t>(mapMenu.get()) + kLocalMapBaseOffset;
        return *reinterpret_cast<const std::uint8_t*>(lmBase + kModeByteOffset) != 1;
    }

    bool HookedObjectInterfaceInvoke(RE::GFxValue::ObjectInterface* a_this,
                                     void* a_data, RE::GFxValue* a_result,
                                     const char* a_name, RE::GFxValue* a_args,
                                     RE::UPInt a_numArgs, bool a_isDObj) {
        if (a_name == g_setSelectedMarkerLiteral && a_args && a_numArgs > 0 &&
            CtrlHeld()) {
            a_args[0].SetNumber(-1.0);
        }
        return reinterpret_cast<ObjectInterfaceInvokeFn>(g_invokeTrampoline)(
            a_this, a_data, a_result, a_name, a_args, a_numArgs, a_isDObj);
    }

    void HookedUpdateDescriptionCard(void* a_obj, void* a_menu,
                                     std::int32_t a_index, void* a_unk) {
        if (CtrlHeld()) {
            return;
        }
        if (IsWorldMapShowing()) {
            static std::int32_t s_lastIdx = -2;
            if (a_index != s_lastIdx) {
                s_lastIdx = a_index;
                logger::debug("[CardSuppress] skipped populator, world map | idx={}",
                              a_index);
            }
            return;
        }
        reinterpret_cast<UpdateDescriptionCardFn>(g_cardTrampoline)(
            a_obj, a_menu, a_index, a_unk);
    }
}

void MapHoverHook::Install() {
    REL::Relocation<const char*> stringLit{ kSetSelectedMarkerStringID };
    g_setSelectedMarkerLiteral = stringLit.get();

    REL::Relocation<std::uintptr_t> invokeTarget{ kObjectInterfaceInvokeID };
    const std::uint64_t invokeAddr = invokeTarget.address();
    const std::uint64_t invokeHook =
        reinterpret_cast<std::uint64_t>(&HookedObjectInterfaceInvoke);

    if (invokeAddr == 0) {
        logger::warn("MapHoverHook: ObjectInterface::Invoke target unresolved; skipping Invoke detour");
    } else {
        g_invokeDetour = std::make_unique<PLH::x64Detour>(
            invokeAddr, invokeHook, &g_invokeTrampoline);
        if (!g_invokeDetour->hook()) {
            logger::error("MapHoverHook: ObjectInterface::Invoke detour failed");
            g_invokeDetour.reset();
        } else {
            logger::info("MapHoverHook: Invoke hook installed | target={:X} | trampoline={:X} | literalVA={}",
                         invokeAddr, g_invokeTrampoline,
                         static_cast<const void*>(g_setSelectedMarkerLiteral));
        }
    }

    REL::Relocation<std::uintptr_t> cardTarget{ kUpdateDescriptionCardID };
    const std::uint64_t cardAddr = cardTarget.address();
    const std::uint64_t cardHook =
        reinterpret_cast<std::uint64_t>(&HookedUpdateDescriptionCard);

    g_cardDetour = std::make_unique<PLH::x64Detour>(
        cardAddr, cardHook, &g_cardTrampoline);

    if (!g_cardDetour->hook()) {
        logger::error("MapHoverHook: description-card detour failed");
        g_cardDetour.reset();
        return;
    }

    logger::info("MapHoverHook: card hook installed | target={:X} | trampoline={:X}",
                 cardAddr, g_cardTrampoline);
}
