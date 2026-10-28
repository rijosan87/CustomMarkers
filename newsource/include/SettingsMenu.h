#pragma once

#include <cstdint>

namespace SettingsMenu {
    enum class Host : std::uint8_t { None, Map, Gameplay };

    // Visibility and host state of the settings menu
    bool IsVisible();
    bool IsGameplayHosted();
    bool Toggle();  // true only when visibility changed
    bool Show();    // detect/validate host, true on open/already open
    void Hide(const char* reason = "explicit");  // idempotent centralized reset

    // Call this inside ImGui::NewFrame() / ImGui::EndFrame()
    void Draw();

    void RegisterWithMenuFramework();
}
