#pragma once

#include <RE/Skyrim.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "MarkerFilterHook.h"

namespace RimMarkerSource {
    struct Entry {
        RE::NiPoint3 position;
        std::uint16_t type = 0;
        MarkerFilterHook::FocusMode mode = MarkerFilterHook::FocusMode::kModded;
        bool undiscovered = false;
        bool dimmed = false;
        std::string packPath;
        RE::FormID formID = 0;
        std::string name;
        std::string logLabel;
        bool priority = false;
        std::uint8_t questIcon = 0;
        std::string subtitle;
    };

    struct Snapshot {
        RE::FormID worldSpaceID = 0;
        std::vector<Entry> entries;
    };

    void RequestUpdateIfDue();

    std::shared_ptr<const Snapshot> Get();

    void Invalidate();
}