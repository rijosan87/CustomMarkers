#pragma once

// Filters map markers by category without triggering full menu reloads.


#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>

#include <SKSE/Interfaces.h>

namespace RE { class TESObjectREFR; }

namespace MarkerFilterHook {
    enum class FocusMode : std::size_t {
        kCities = 0,
        kTravel,
        kDungeons,
        kCamps,
        kIndustry,
        kLandmarks,
        kDwellings,
        kQuests,
        kShops,
        kLoot,
        kModded,
        kCount
    };
    inline constexpr const char* kModeIcons[] = {
        "city",
        "stable",
        "cave",
        "camp",
        "smelter",
        "landmark",
        "dwelling",
        "quest",
        "shop",
        "loot",
        "custom",
    };
    static_assert(std::size(kModeIcons) == static_cast<std::size_t>(FocusMode::kCount),
                  "kModeIcons must stay in sync with FocusMode");

    void Install();
    FocusMode ModeForRef(RE::TESObjectREFR* a_refr, std::uint16_t a_markerType);

    const char* GetModeName(FocusMode a_mode);

    bool IsModeEnabled(FocusMode a_mode);

    void SetModeEnabled(FocusMode a_mode, bool a_enabled);

    bool IsTypeFiltered(std::uint16_t a_markerType);

    bool IsMarkerFiltered(RE::TESObjectREFR* a_refr, std::uint16_t a_markerType);

    std::uint32_t GetModeCount(FocusMode a_mode);

    void OnForeignMessage(SKSE::MessagingInterface::Message* a_msg);

    std::string GetCoMAPIconName(RE::TESObjectREFR* a_refr);
}
