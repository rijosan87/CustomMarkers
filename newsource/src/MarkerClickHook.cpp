#include "MarkerClickHook.h"
#include "MapMarkerRenderer.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <memory>

// ============================================================================
// MarkerClickHook — swallow vanilla map marker clicks under the ImGui list panel
// ----------------------------------------------------------------------------
// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):
//   Working: when the engine cursor is inside MapMarkerRenderer's list panel
//            rect (hotkey C), vanilla "Travel to <location>?" never fires.
//            Outside the panel, vanilla fast travel works normally.
//   Notes: vanilla markers still HOVER under the panel — only clicks are
//          swallowed. Scaleform hover is a separate code path we don't touch
//          (cosmetic, not worth the RE).
//   Dual-build: uses REL::RelocationID(52208, 53095) (SE 1.5.97 / AE 1.6.1170).
// ============================================================================
//
// Hook target: vanilla world-map "MarkerClick" FxDelegate callback (SE 52208 / AE 53095).
// See findings/findings-SE-Hook-IDs.md (row 6).
//
// Why: clicks on Scaleform vanilla map markers (cities, dungeons, ...) follow a
// separate path from our PlaceMarkerHook. Without this hook, vanilla markers
// covered by our ImGui list panel still receive the click and pop the
// "Travel to <location>?" prompt. Swallow those clicks while the panel hits.
//
// Args layout (FxDelegate): rcx = FxDelegateArgs&, args[0] is the marker index.
// We don't introspect args - we only care whether the cursor is over our panel.
namespace {
    using MarkerClickFn = void(*)(void*);

    std::uint64_t                              g_mcTrampolineAddr = 0;
    std::unique_ptr<PLH::x64Detour>            g_mcDetour;

    void HookedMarkerClick(void* a_args) {
        if (MapMarkerRenderer::GetSingleton()->TryHandlePanelClick()) {
            logger::debug("[MarkerClickHook] swallowed vanilla marker click (cursor inside panel)");
            return;
        }
        reinterpret_cast<MarkerClickFn>(g_mcTrampolineAddr)(a_args);
    }
}

void MarkerClickHook::Install() {
    // Vanilla marker click handler: SE 52208 / AE 53095 (findings-SE-Hook-IDs.md row 6).
    REL::Relocation<std::uintptr_t> target{ REL::RelocationID(52208, 53095) };
    const std::uint64_t targetAddr = target.address();
    const std::uint64_t hookAddr   = reinterpret_cast<std::uint64_t>(&HookedMarkerClick);

    g_mcDetour = std::make_unique<PLH::x64Detour>(targetAddr, hookAddr, &g_mcTrampolineAddr);

    if (!g_mcDetour->hook()) {
        logger::error("MarkerClickHook: PolyHook x64Detour::hook() returned false");
        g_mcDetour.reset();
        return;
    }

    logger::info("MarkerClickHook installed | target={:X} | trampoline={:X}",
                 targetAddr, g_mcTrampolineAddr);
}
