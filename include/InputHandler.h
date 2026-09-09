#pragma once

// Dispatches user keybinds, hotkeys, and mouse inputs.


#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace InputHandler {
    const char* DIKToKeyName(int dikCode);

    bool IsValidKeyCode(int dikCode);

    std::uint32_t GetHeldModifier();

    bool IsModifierScancode(std::uint32_t scancode);

    std::string FormatHotkeyWithModifier(int dikCode, int modifier);

    int DIKToVK(int dikCode);
}

bool ModifierMatches(std::int32_t modifierType);
std::uint32_t KeycodeFromScancode(std::uint32_t scancode, RE::INPUT_DEVICE device);
bool HotkeyMatches(std::uint32_t code, std::int32_t key, std::int32_t modifier);

extern std::function<void(std::uint32_t)> g_hkCaptureCb;
extern std::chrono::steady_clock::time_point g_hkCaptureArmedAt;
