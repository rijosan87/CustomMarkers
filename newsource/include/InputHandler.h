#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

// Hotkey naming + modifier detection for the settings-menu bind rows and the
// INI. Ported from PartySheet (src/InputHandler.cpp), collapsed to ONE
// GetHeldModifier: PartySheet needed two only to keep its already-shipped INI
// values meaningful, which CMM has no equivalent of.
//
// One keyspace throughout: keyboard 1-255 are DirectInput scancodes (DIK, NOT
// virtual keys), mouse buttons are 256-263, gamepad 266-281. Matches
// SKSE::InputMap's macro ranges so GamepadMaskToKeycode output drops straight in.
namespace InputHandler {

    // DirectInput scancode -> readable name. Returns a static "Key<N>" buffer
    // for anything unnamed, so the pointer is only valid until the next call.
    const char* DIKToKeyName(int dikCode);

    // False when DIKToKeyName fell through to the "Key<N>" fallback.
    bool IsValidKeyCode(int dikCode);

    // Held modifier at the instant of a keypress.
    //   0 none, 1 Alt, 2 Ctrl, 3 Shift,
    //   4 LAlt, 5 RAlt, 6 LCtrl, 7 RCtrl, 8 LShift, 9 RShift
    // Sided values are preferred, so a captured Ctrl bind records 6 or 7 and
    // fires on that ONE physical Ctrl. Deliberate (see the finding, decision 3):
    // the button label states which side was captured, and 1-3 stay reachable by
    // hand-editing the INI. Do not drop the sided checks to "fix" this.
    std::uint32_t GetHeldModifier();

    // True when the scancode is itself a modifier key. Capture skips these so it
    // keeps waiting for a real key while a modifier is held.
    bool IsModifierScancode(std::uint32_t scancode);

    // "Ctrl + N", "LShift + F4", "N". Modifier 0 yields the bare key name.
    std::string FormatHotkeyWithModifier(int dikCode, int modifier);

    // Poll sites need a VK for GetAsyncKeyState but hotkeys store DIK. Returns 0
    // for unbound (< 1) and for a mouse/gamepad bind (>= 256), neither of which
    // GetAsyncKeyState can see; callers must treat 0 as "not pressed".
    int DIKToVK(int dikCode);
}

// Defined in plugin.cpp. Strict modifier match: 0 means NO modifier may be held,
// which is what keeps X and Ctrl+X distinct. Declared here so InputHook's swallow
// blocks test the same condition the dispatch does.
bool ModifierMatches(std::int32_t modifierType);
std::uint32_t KeycodeFromScancode(std::uint32_t scancode, RE::INPUT_DEVICE device);
bool HotkeyMatches(std::uint32_t code, std::int32_t key, std::int32_t modifier);

// Press-to-capture rebinding, defined in plugin.cpp. Armed by SettingsMenu's
// PanelHotkeys; InputHook's ProcessInputQueue detour fires the callback with the
// next non-modifier key and swallows that key.
extern std::function<void(std::uint32_t)> g_hkCaptureCb;
extern std::chrono::steady_clock::time_point g_hkCaptureArmedAt;
