#include "PlaceMarkerHook.h"
#include "MapMarkerRenderer.h"
#include "MenuUtils.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <memory>
#include <imgui.h>

// Hooks map menu right-clicks to spawn custom markers and context menus.
namespace {
    std::uint64_t                              g_trampolineAddr = 0;
    std::unique_ptr<PLH::x64Detour>            g_detour;

    bool g_suppressNextPlacement = false;

    bool g_allowNextPlacement = false;

    std::uintptr_t HookedSetDestination(void* a_player, RE::NiPoint3* a_pos,
                                        RE::TESWorldSpace* a_worldspace) {
        // Consume the one-shot suppression before checking placement authority.
        if (g_suppressNextPlacement) {
            g_suppressNextPlacement = false;
            return 0;
        }

        // Only react when the world map is open - this function can be driven
        // by other systems, and we only want user map clicks.
        auto ui = RE::UI::GetSingleton();
        if (a_pos && ui && ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
            const bool noteDialogOpen = MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen();

            bool isLocalMap = false;
            if (auto mm = ui->GetMenu<RE::MapMenu>()) {
                const auto base = reinterpret_cast<std::uintptr_t>(mm.get());
                isLocalMap = *reinterpret_cast<const bool*>(base + 0x3045c);
            }

            if (isLocalMap) {
                return 0;
            }

            if (noteDialogOpen) {
                return 0;
            }

            if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse) {
                return 0;
            }
            // Defer to original function if a dialog is open.
            bool isDialogOpen = MenuUtils::IsMapModalOpen();
            if (isDialogOpen) {
                using func_t = std::uintptr_t(*)(void*, RE::NiPoint3*, RE::TESWorldSpace*);
                return REL::Relocation<func_t>(g_trampolineAddr)(a_player, a_pos, a_worldspace);
            }

            // MarkerHoverHook routes panel actions before the engine raycast.
            // This detour handles only a successful terrain-placement call.

            if (!g_allowNextPlacement) {
                return 0;
            }
            g_allowNextPlacement = false;

            RE::FormID cellID = 0;
            if (auto* player = RE::PlayerCharacter::GetSingleton()) {
                if (auto* c = player->GetParentCell()) {
                    cellID = c->GetFormID();
                }
            }

            MapMarkerRenderer::GetSingleton()->OpenNoteDialog(
                *a_pos,
                a_worldspace ? a_worldspace->GetFormID() : 0,
                cellID);
        }

        return 0;
    }
}

void PlaceMarkerHook::Install() {
    // Log the public entry used by the right-click terrain menu.
    REL::Relocation<std::uintptr_t> placeMarker{ RELOCATION_ID(52226, 53113) };
    logger::debug("[verify] MapMenu::PlaceMarker (ID 53113) RVA = {:X}",
                 placeMarker.offset());

    REL::Relocation<std::uintptr_t> target{ REL::RelocationID(39458, 40535) };
    const std::uint64_t targetAddr = target.address();
    const std::uint64_t hookAddr   = reinterpret_cast<std::uint64_t>(&HookedSetDestination);

    g_detour = std::make_unique<PLH::x64Detour>(targetAddr, hookAddr, &g_trampolineAddr);

    if (!g_detour->hook()) {
        logger::error("PlaceMarkerHook: PolyHook x64Detour::hook() returned false");
        g_detour.reset();
        return;
    }

    logger::info("PlaceMarkerHook installed | target={:X} | trampoline={:X}",
                 targetAddr, g_trampolineAddr);
}

void PlaceMarkerHook::SuppressNextPlacement() {
    g_suppressNextPlacement = true;
}

void PlaceMarkerHook::AllowNextPlacement() {
    g_allowNextPlacement = true;
}
