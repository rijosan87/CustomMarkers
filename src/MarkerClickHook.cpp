// Hooks mouse clicks on world map markers.

#include "MarkerClickHook.h"
#include "MapMarkerRenderer.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <memory>

// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):

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
