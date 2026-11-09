#include "DestinationMirror.h"

#include "PlaceMarkerHook.h"
#include "Settings.h"

#include <atomic>
#include <cmath>

namespace {
    bool         g_applied = false;
    std::atomic<bool> g_appliedPublic{ false };
    bool         g_reconcile = true;
    bool         g_lastEnabled = false;
    RE::NiPoint3 g_lastPos{};
    RE::FormID   g_lastWorld = 0;
    std::uint32_t g_lastId = 0;

    void SetApplied(bool a_value) {
        g_applied = a_value;
        g_appliedPublic.store(a_value);
    }

    void ClearVanilla(const char* a_reason) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        using func_t = void (*)(RE::PlayerCharacter*);
        static REL::Relocation<func_t> clearFn{ REL::RelocationID(39459, 40536) };
        logger::debug("[DestMirror] Clear ({})", a_reason);
        clearFn(player);
        SetApplied(false);
        g_lastId = 0;
        g_lastWorld = 0;
    }

    bool SamePoint(const RE::NiPoint3& a, const RE::NiPoint3& b) {
        return std::fabs(a.x - b.x) < 1.0f && std::fabs(a.y - b.y) < 1.0f && std::fabs(a.z - b.z) < 1.0f;
    }
}

namespace DestinationMirror {
    void Update(const std::vector<MapMarker>& a_markers) {
        const bool enabled = Settings::GetSingleton()->navShareDestination;

        if (!enabled) {
            if (g_applied) ClearVanilla("setting turned off");
            g_lastEnabled = false;
            return;
        }
        if (!g_lastEnabled) {
            g_lastEnabled = true;
            g_reconcile = true;
        }

        const MapMarker* dest = nullptr;
        for (const auto& m : a_markers) {
            if (m.isDestination) { dest = &m; break; }
        }

        // Interior setter behavior is unverified.
        const bool exterior = dest && dest->worldSpaceID != 0 &&
                              std::isfinite(dest->position.x) && std::isfinite(dest->position.y) &&
                              std::isfinite(dest->position.z);

        if (!exterior) {
            if (g_applied || g_reconcile) {
                ClearVanilla(dest ? "destination is interior" : "no destination");
            }
            g_reconcile = false;
            return;
        }

        const bool changed = !g_applied || g_reconcile || dest->id != g_lastId ||
                             dest->worldSpaceID != g_lastWorld || !SamePoint(dest->position, g_lastPos);
        if (!changed) return;
        g_reconcile = false;

        auto* ws = RE::TESForm::LookupByID<RE::TESWorldSpace>(dest->worldSpaceID);
        if (!ws) {
            logger::warn("[DestMirror] worldspace {:08X} not found, destination id={} not shared",
                         dest->worldSpaceID, dest->id);
            if (g_applied) ClearVanilla("worldspace missing");
            g_lastId = dest->id;
            g_lastWorld = dest->worldSpaceID;
            g_lastPos = dest->position;
            return;
        }

        logger::debug("[DestMirror] Set id={} ws={:08X} pos=({:.1f},{:.1f},{:.1f})",
                     dest->id, dest->worldSpaceID, dest->position.x, dest->position.y, dest->position.z);
        if (!PlaceMarkerHook::CallOriginalSetDestination(dest->position, ws)) {
            logger::warn("[DestMirror] Set skipped: PlaceMarkerHook not installed");
            return;
        }
        SetApplied(true);
        g_lastId = dest->id;
        g_lastWorld = dest->worldSpaceID;
        g_lastPos = dest->position;
    }

    ScopedHideHandle::ScopedHideHandle() {
        if (!IsApplied()) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;
        auto& handle = player->GetInfoRuntimeData().playerMapMarker;
        _saved = handle;
        handle = RE::ObjectRefHandle{};
        _active = true;
    }

    ScopedHideHandle::~ScopedHideHandle() {
        if (!_active) return;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            player->GetInfoRuntimeData().playerMapMarker = _saved;
        }
    }

    bool IsApplied() {
        return g_appliedPublic.load();
    }

    void RequestReconcile() {
        g_applied = false;
        g_appliedPublic.store(false);
        g_reconcile = true;
        g_lastId = 0;
        g_lastWorld = 0;
    }
}
