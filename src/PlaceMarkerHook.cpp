// Hooks map menu right-clicks to spawn custom markers and context menus.

#include "PlaceMarkerHook.h"
#include "MapMarkerRenderer.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <memory>
#include <imgui.h>

// STATUS (source of truth for this file; update when behavior changes):

namespace {
    std::uint64_t                              g_trampolineAddr = 0;
    std::unique_ptr<PLH::x64Detour>            g_detour;

    bool g_suppressNextPlacement = false;

    bool g_allowNextPlacement = false;

    std::uintptr_t HookedSetDestination(void* a_player, RE::NiPoint3* a_pos,
                                        RE::TESWorldSpace* a_worldspace) {
        if (g_suppressNextPlacement) {
            g_suppressNextPlacement = false;
            return 0;
        }
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
            bool isDialogOpen = ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME) ||
                                ui->IsMenuOpen(RE::Console::MENU_NAME) ||
                                ui->IsMenuOpen(RE::JournalMenu::MENU_NAME);
            if (isDialogOpen) {
                using func_t = std::uintptr_t(*)(void*, RE::NiPoint3*, RE::TESWorldSpace*);
                return REL::Relocation<func_t>(g_trampolineAddr)(a_player, a_pos, a_worldspace);
            }
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
