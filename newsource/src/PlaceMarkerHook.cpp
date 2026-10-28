#include "PlaceMarkerHook.h"
#include "MapMarkerRenderer.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <memory>
#include <imgui.h>

// ============================================================================
// PlaceMarkerHook - world-map right-click placement bridge
// ----------------------------------------------------------------------------
// STATUS (source of truth for this file; update when behavior changes):
//   Working: MapMarkerRenderer arms one placement from the right-click terrain
//            menu. Vanilla MapMenu::PlaceMarker resolves the exact terrain hit;
//            this detour opens the custom note dialog. Plain left-click is ignored.
//   Guards: suppresses snapped vanilla-marker dispatch and ignores local-map,
//           active-dialog, and ImGui-captured calls.
//   Runtime: REL::RelocationID(39458, 40535), SE 1.5.97 plus the AE Address
//            Library slot used by 1.6.1170 and 1.7.104.
// ============================================================================
//
// Hook target: SetDestination / custom destination helper (SE 39458 / AE 40535).
//
// Vanilla's left-click "Set Destination" chain, reused by our right-click menu:
//   MapMenu::PlaceMarker  ->  ray-terrain intersection  ->  THIS(player, &hitPos, worldspace)
// arg2 (hitPos) is the exact world position already resolved by the engine.
// No projection math needed on our side.
// See findings/findings-placemarker.md.
//
// Why not hook MapMenu::PlaceMarker instead: hitPos is a local inside PlaceMarker.
// By the time our hook would run (post-call), that local is gone. Hooking the
// callee (this function) gets it while it's still live as a parameter.
namespace {
    std::uint64_t                              g_trampolineAddr = 0;
    std::unique_ptr<PLH::x64Detour>            g_detour;

    // Set by MarkerHoverHook before it forwards a snapped vanilla-marker click.
    // Vanilla may still walk the terrain-ray path and hit this detour; suppress
    // that one invocation so no note dialog opens behind vanilla UI.
    bool g_suppressNextPlacement = false;

    // Left-click placement is disabled. Only a right-click "Add Marker Here"
    // sets this, allowing exactly the next placement through. Every other
    // call (plain left-click on terrain) is ignored.
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

            // Skip when the local-map tab is active. The engine still routes
            // some clicks through MapMenu::PlaceMarker (this function's caller)
            // even when the local map is showing, which would re-open the note
            // dialog on top of the local-map placement that LocalMapClickHook
            // just handled - or worse, re-open it after Remove was clicked.
            // MapMenu+0x3045c == 1 means local map is active. See
            // findings/findings-localmap.md.
            bool isLocalMap = false;
            if (auto mm = ui->GetMenu<RE::MapMenu>()) {
                const auto base = reinterpret_cast<std::uintptr_t>(mm.get());
                isLocalMap = *reinterpret_cast<const bool*>(base + 0x3045c);
            }

            if (isLocalMap) {
                return 0;
            }

            // While our note dialog is open we are in the middle of placing a
            // marker. Re-entering OpenNoteDialog here would reset state and
            // visually look like a second dialog. Skip the placement path.
            if (noteDialogOpen) {
                return 0;
            }

            if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse) {
                return 0;
            }
            // Defer to original function if a dialog is open.
            bool isDialogOpen = ui->IsMenuOpen(RE::MessageBoxMenu::MENU_NAME) ||
                                ui->IsMenuOpen(RE::Console::MENU_NAME) ||
                                ui->IsMenuOpen(RE::JournalMenu::MENU_NAME);
            if (isDialogOpen) {
                using func_t = std::uintptr_t(*)(void*, RE::NiPoint3*, RE::TESWorldSpace*);
                return REL::Relocation<func_t>(g_trampolineAddr)(a_player, a_pos, a_worldspace);
            }

            // MarkerHoverHook routes panel actions before the engine raycast.
            // This detour handles only a successful terrain-placement call.

            // Left-click no longer places. Only the right-click "Add Marker Here"
            // sets g_allowNextPlacement just before calling MapMenu::PlaceMarker.
            // Any other route here (plain left-click terrain ray) is ignored.
            // Consume the flag so the allowance is strictly one-shot.
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

    // Placement helper proven in findings/findings-SE-Hook-IDs.md row 9.
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
