#pragma once

#include "MarkerManager.h"

#include <vector>

// Mirrors our destination into vanilla's custom destination for AutoMove/AutoHorse.
namespace DestinationMirror {
    void Update(const std::vector<MapMarker>& a_markers);

    bool IsApplied();

    void RequestReconcile();

    // PlaceMarker shows vanilla's replace-marker box when the handle is set; blank it for one call.
    class ScopedHideHandle {
    public:
        ScopedHideHandle();
        ~ScopedHideHandle();
        ScopedHideHandle(const ScopedHideHandle&) = delete;
        ScopedHideHandle& operator=(const ScopedHideHandle&) = delete;
    private:
        RE::ObjectRefHandle _saved{};
        bool _active = false;
    };
}
