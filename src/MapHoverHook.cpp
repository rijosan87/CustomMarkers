#include "MapHoverHook.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <memory>

// Hooks map menu hover events to detect hovered markers.
namespace {
    constexpr REL::RelocationID kObjectInterfaceInvokeID{ 80233, 82256 };
    constexpr REL::RelocationID kUpdateDescriptionCardID{ 52187, 53079 };
    constexpr REL::RelocationID kSetSelectedMarkerStringID{ 270681, 216427 };
    constexpr REL::RelocationID kRefreshMarkersID{ 52090, 52980 };

    // AE call sits at +0x885 and the card call at +0x91C, so 0x1000 covers the
    // body with margin on every runtime.
    constexpr std::size_t kRefreshScanLen = 0x1000;
    // LEA R9 and the CALL are a few movs apart (AE: 0x18 bytes).
    constexpr std::size_t kLeaToCallWindow = 0x40;

    std::uint64_t                              g_cardTrampoline = 0;
    std::unique_ptr<PLH::x64Detour>            g_cardDetour;

    using ObjectInterfaceInvokeFn =
        bool(*)(RE::GFxValue::ObjectInterface* a_this, void* a_data,
                RE::GFxValue* a_result, const char* a_name,
                RE::GFxValue* a_args, RE::UPInt a_numArgs, bool a_isDObj);

    // Original call target returned by write_call (Invoke, or whoever else
    // entry-hooked it; calling through keeps their chain intact).
    ObjectInterfaceInvokeFn g_originalInvoke = nullptr;

    std::uintptr_t FindSetSelectedMarkerCall(std::uintptr_t a_fn,
                                             std::uintptr_t a_invoke,
                                             std::uintptr_t a_literal) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(a_fn);
        for (std::size_t i = 0; i + 7 <= kRefreshScanLen; ++i) {
            if (p[i] != 0x4C || p[i + 1] != 0x8D || p[i + 2] != 0x0D) continue;
            std::int32_t disp;
            std::memcpy(&disp, p + i + 3, sizeof(disp));
            if (a_fn + i + 7 + disp != a_literal) continue;

            const std::size_t end = (std::min)(i + 7 + kLeaToCallWindow, kRefreshScanLen);
            for (std::size_t j = i + 7; j + 5 <= end; ++j) {
                if (p[j] != 0xE8) continue;
                std::int32_t rel;
                std::memcpy(&rel, p + j + 1, sizeof(rel));
                if (a_fn + j + 5 + rel == a_invoke) return a_fn + j;
            }
        }
        return 0;
    }

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

    // Only reached from the SetSelectedMarker call site, so no name filter.
    bool HookedSetSelectedMarkerCall(RE::GFxValue::ObjectInterface* a_this,
                                     void* a_data, RE::GFxValue* a_result,
                                     const char* a_name, RE::GFxValue* a_args,
                                     RE::UPInt a_numArgs, bool a_isDObj) {
        if (a_args && a_numArgs > 0 && CtrlHeld()) {
            a_args[0].SetNumber(-1.0);
        }
        return g_originalInvoke(a_this, a_data, a_result, a_name, a_args,
                                a_numArgs, a_isDObj);
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
    // The literal's address is what the poll's LEA R9 loads, so it doubles as
    // the scan anchor for the one call we want.
    REL::Relocation<const char*> stringLit{ kSetSelectedMarkerStringID };
    REL::Relocation<std::uintptr_t> invokeTarget{ kObjectInterfaceInvokeID };
    REL::Relocation<std::uintptr_t> refreshTarget{ kRefreshMarkersID };

    const std::uintptr_t literalAddr = reinterpret_cast<std::uintptr_t>(stringLit.get());
    const std::uintptr_t invokeAddr  = invokeTarget.address();
    const std::uintptr_t refreshAddr = refreshTarget.address();

    const std::uintptr_t callSite =
        (literalAddr && invokeAddr && refreshAddr)
            ? FindSetSelectedMarkerCall(refreshAddr, invokeAddr, literalAddr)
            : 0;

    // Skip only this hook on a miss; the card detour below still installs.
    if (callSite == 0) {
        logger::warn("MapHoverHook: SetSelectedMarker call site not found | refresh={:X} | invoke={:X} | literal={:X}; Ctrl hover-zoom suppression disabled",
                     refreshAddr, invokeAddr, literalAddr);
    } else {
        g_originalInvoke = reinterpret_cast<ObjectInterfaceInvokeFn>(
            SKSE::GetTrampoline().write_call<5>(callSite, &HookedSetSelectedMarkerCall));
        logger::info("MapHoverHook: SetSelectedMarker call-site hook installed | site=+{:X} | original={:X}",
                     callSite - refreshAddr,
                     reinterpret_cast<std::uintptr_t>(g_originalInvoke));
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
