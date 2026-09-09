#pragma once

// In-game ImGui settings configuration window.


#include <cstdint>

namespace SettingsMenu {
    enum class Host : std::uint8_t { None, Map, Gameplay };

    bool IsVisible();
    bool IsGameplayHosted();
    bool Toggle();
    bool Show();
    void Hide(const char* reason = "explicit");

    void Draw();
}
