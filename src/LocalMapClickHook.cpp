// Handles mouse clicks on local and interior map surfaces.

#include "LocalMapClickHook.h"
#include "MarkerManager.h"
#include "MapMarkerRenderer.h"
#include "LocalMapRenderer.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <polyhook2/Detour/x64Detour.hpp>

#include <imgui.h>

#include <Windows.h>
#include <cmath>
#include <memory>

// STATUS (update when behavior changes; do NOT update CLAUDE.md for this):

namespace {
    std::uint64_t                              g_trampolineAddr = 0;
    std::unique_ptr<PLH::x64Detour>            g_detour;

    using ScreenToWorldRay_t =
        bool(*)(RE::NiCamera*, int, int, RE::NiPoint3*, RE::NiPoint3*, float, float);

    bool RayAtZ(const RE::NiPoint3& a, const RE::NiPoint3& b, float planeZ,
                RE::NiPoint3& out) {
        float dz = b.z - a.z;
        if (std::abs(dz) < 1e-6f) return false;
        float t = (planeZ - a.z) / dz;
        out.x   = a.x + t * (b.x - a.x);
        out.y   = a.y + t * (b.y - a.y);
        out.z   = planeZ;
        return true;
    }

    bool TryPlaceAtCursor() {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return false;
        auto mapMenu = ui->GetMenu<RE::MapMenu>();
        if (!mapMenu) return false;
        const auto base         = reinterpret_cast<std::uintptr_t>(mapMenu.get());
        const auto localMapBase = base + 0x60;
        auto* cam = *reinterpret_cast<RE::NiCamera**>(localMapBase + 0x302F8);
        if (!cam) return false;
        auto* menuCursor = RE::MenuCursor::GetSingleton();
        if (!menuCursor || menuCursor->screenWidthX <= 0.f || menuCursor->screenWidthY <= 0.f) {
            return false;
        }
        float vpL = *reinterpret_cast<const float*>(localMapBase + 0x30);
        float vpT = *reinterpret_cast<const float*>(localMapBase + 0x34);
        float vpR = *reinterpret_cast<const float*>(localMapBase + 0x38);
        float vpB = *reinterpret_cast<const float*>(localMapBase + 0x3c);
        float vpWidth = vpR - vpL;
        float vpHeight = vpB - vpT;
        if (vpWidth <= 0.f || vpHeight <= 0.f) return false;
        float localX = menuCursor->cursorPosX - vpL;
        float localY = menuCursor->cursorPosY - vpT;
        REL::Relocation<ScreenToWorldRay_t> screenToWorldRay{ REL::RelocationID(70630, 70630) };
        RE::NiPoint3 rayA{}, rayDir{};
        bool rayOk = screenToWorldRay(cam,
                                      static_cast<int>(localX),
                                      static_cast<int>(localY),
                                      &rayA, &rayDir,
                                      vpWidth, vpHeight);
        if (!rayOk) return false;
        RE::NiPoint3 rayB = rayA + rayDir * 10000.f;
        RE::FormID   cellID       = 0;
        RE::FormID   worldSpaceID = 0;
        float        groundZ      = rayA.z;
        bool         haveCell     = false;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            if (auto* cell = player->GetParentCell()) {
                cellID   = cell->GetFormID();
                haveCell = true;
                if (!cell->IsInteriorCell()) {
                    if (auto* ws = player->GetWorldspace()) {
                        worldSpaceID = ws->GetFormID();
                    }
                }
            }
            groundZ = player->GetPositionZ();
        }
        if (!haveCell) return false;
        RE::NiPoint3 hitPos{};
        if (!RayAtZ(rayA, rayB, groundZ, hitPos)) return false;
        MapMarkerRenderer::GetSingleton()->OpenNoteDialog(hitPos, worldSpaceID, cellID);
        return true;
    }

    bool HookedProcessButton(std::uintptr_t a_handler, RE::ButtonEvent* a_event) {
        using func_t = bool(*)(std::uintptr_t, RE::ButtonEvent*);
        auto callOriginal = [&]() -> bool {
            return REL::Relocation<func_t>(g_trampolineAddr)(a_handler, a_event);
        };
        if (!a_handler || !a_event) return callOriginal();
        const bool isLeftMouseDown =
            a_event->GetDevice() == RE::INPUT_DEVICE::kMouse &&
            a_event->GetIDCode() == 0 &&
            a_event->IsDown();
        if (!isLeftMouseDown) return callOriginal();
        struct NeutralizeOnExit {
            RE::ButtonEvent* ev;
            ~NeutralizeOnExit() {
                if (ev) ev->userEvent = RE::BSFixedString("");
            }
        } _neutralize{ a_event };
        if (MapMarkerRenderer::GetSingleton()->IsNoteDialogOpen()) {
            return true;
        }
        if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse) {
            return true;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return callOriginal();
        auto mapMenu = ui->GetMenu<RE::MapMenu>();
        if (!mapMenu) return callOriginal();
        const auto base         = reinterpret_cast<std::uintptr_t>(mapMenu.get());
        const auto localMapBase = base + 0x60;
        auto* cam = *reinterpret_cast<RE::NiCamera**>(localMapBase + 0x302F8);
        if (!cam) return callOriginal();
        if (auto removeId = LocalMapRenderer::GetSingleton()->GetRemoveButtonTarget();
            removeId != 0) {
            MarkerManager::GetSingleton()->RemoveMarker(removeId);
            return true;
        }
        if (auto destId = LocalMapRenderer::GetSingleton()->GetDestButtonTarget();
            destId != 0) {
            auto markers = MarkerManager::GetSingleton()->GetUserMarkers();
            bool isCurrentlyDest = false;
            for (const auto& m : markers) {
                if (m.id == destId) { isCurrentlyDest = m.isDestination; break; }
            }
            MarkerManager::GetSingleton()->SetDestination(destId, !isCurrentlyDest);
            return true;
        }
        if (auto editId = LocalMapRenderer::GetSingleton()->GetEditButtonTarget();
            editId != 0) {
            MapMarkerRenderer::GetSingleton()->BeginEditMarker(editId);
            return true;
        }
        if (LocalMapRenderer::GetSingleton()->IsCustomPinHovered()) {
            return true;
        }
        if (LocalMapRenderer::GetSingleton()->IsBeaconHovered()) {
            return true;
        }
        const bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        const auto hoveredMarkerIndex =
            *reinterpret_cast<const std::int32_t*>(localMapBase + 0x303F8);
        if (hoveredMarkerIndex != -1 && !ctrlHeld) {
            return callOriginal();
        }
        return callOriginal();
    }
}

bool LocalMapClickHook::PlaceAtCursor() {
    return TryPlaceAtCursor();
}

void LocalMapClickHook::Install() {
    REL::Relocation<std::uintptr_t> target{ REL::RelocationID(52088, 52978) };
    const std::uint64_t targetAddr = target.address();
    const std::uint64_t hookAddr   = reinterpret_cast<std::uint64_t>(&HookedProcessButton);

    g_detour = std::make_unique<PLH::x64Detour>(targetAddr, hookAddr, &g_trampolineAddr);

    if (!g_detour->hook()) {
        logger::error("LocalMapClickHook: PolyHook x64Detour::hook() returned false");
        g_detour.reset();
        return;
    }

    logger::info("LocalMapClickHook installed | target={:X} | trampoline={:X}",
                 targetAddr, g_trampolineAddr);
}
