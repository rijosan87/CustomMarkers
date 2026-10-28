#include "InputHandler.h"

#include <Windows.h>
#include <cstdio>

namespace InputHandler {

    const char* DIKToKeyName(int dikCode) {
        static char unknownKey[16];  // static buffer for unnamed codes
        switch (dikCode) {
            case 0x01: return "Esc";
            case 0x02: return "1"; case 0x03: return "2"; case 0x04: return "3";
            case 0x05: return "4"; case 0x06: return "5"; case 0x07: return "6";
            case 0x08: return "7"; case 0x09: return "8"; case 0x0A: return "9";
            case 0x0B: return "0";
            case 0x0F: return "Tab";
            case 0x10: return "Q"; case 0x11: return "W"; case 0x12: return "E";
            case 0x13: return "R"; case 0x14: return "T"; case 0x15: return "Y";
            case 0x16: return "U"; case 0x17: return "I"; case 0x18: return "O";
            case 0x19: return "P";
            case 0x1E: return "A"; case 0x1F: return "S"; case 0x20: return "D";
            case 0x21: return "F"; case 0x22: return "G"; case 0x23: return "H";
            case 0x24: return "J"; case 0x25: return "K"; case 0x26: return "L";
            case 0x2C: return "Z"; case 0x2D: return "X"; case 0x2E: return "C";
            case 0x2F: return "V"; case 0x30: return "B"; case 0x31: return "N";
            case 0x32: return "M";
            case 0x39: return "Space";
            case 0x3B: return "F1"; case 0x3C: return "F2"; case 0x3D: return "F3";
            case 0x3E: return "F4"; case 0x3F: return "F5"; case 0x40: return "F6";
            case 0x41: return "F7"; case 0x42: return "F8"; case 0x43: return "F9";
            case 0x44: return "F10"; case 0x57: return "F11"; case 0x58: return "F12";
            case 0xD2: return "Insert"; case 0xD3: return "Delete";
            case 0xC7: return "Home"; case 0xCF: return "End";
            case 0xC9: return "PgUp"; case 0xD1: return "PgDn";
            // Modifier keys
            case 0x2A: return "LShift"; case 0x36: return "RShift";
            case 0x1D: return "LCtrl"; case 0x9D: return "RCtrl";
            case 0x38: return "LAlt"; case 0xB8: return "RAlt";
            // Additional common keys
            case 0x1C: return "Enter"; case 0x0E: return "Backspace";
            case 0x3A: return "CapsLock";
            case 0xC8: return "Up"; case 0xD0: return "Down";
            case 0xCB: return "Left"; case 0xCD: return "Right";
            // OEM / punctuation keys (US layout names)
            case 0x0C: return "-"; case 0x0D: return "=";
            case 0x1A: return "["; case 0x1B: return "]";
            case 0x27: return ";"; case 0x28: return "'";
            case 0x29: return "`"; case 0x2B: return "\\";
            case 0x33: return ","; case 0x34: return "."; case 0x35: return "/";
            // Numpad
            case 0x47: return "Num7"; case 0x48: return "Num8"; case 0x49: return "Num9";
            case 0x4B: return "Num4"; case 0x4C: return "Num5"; case 0x4D: return "Num6";
            case 0x4F: return "Num1"; case 0x50: return "Num2"; case 0x51: return "Num3";
            case 0x52: return "Num0"; case 0x53: return "Num.";
            case 0x4A: return "Num-"; case 0x4E: return "Num+"; case 0x37: return "Num*";
            case 0xB5: return "Num/"; case 0x9C: return "NumEnter";
            // Locks
            case 0x45: return "NumLock"; case 0x46: return "ScrollLock";
            // Mouse buttons. SKSE::InputMap defines EIGHT (kMacro_NumMouseButtons,
            // 256-263); 264/265 are wheel up/down and are deliberately left
            // unnamed, both because a wheel tick is a poor hotkey and because
            // InputHook already routes the wheel to the note dialog.
            case 256: return "LMB"; case 257: return "RMB"; case 258: return "MMB";
            case 259: return "Mouse4"; case 260: return "Mouse5";
            case 261: return "Mouse6"; case 262: return "Mouse7"; case 263: return "Mouse8";
            // Gamepad (266-281), SKSE::InputMap's kMacro_GamepadOffset block,
            // produced by GamepadMaskToKeycode in plugin.cpp. INI-only:
            // press-to-capture never reads kGamepad, so these appear only when a
            // user hand-edits CustomMapMarkers.ini. Xbox labels; PlayStation pads
            // report the same codes (A=Cross, B=Circle).
            case 266: return "DPadUp"; case 267: return "DPadDown";
            case 268: return "DPadLeft"; case 269: return "DPadRight";
            case 270: return "Start"; case 271: return "Back";
            case 272: return "LStick"; case 273: return "RStick";
            case 274: return "LB"; case 275: return "RB";
            case 276: return "A"; case 277: return "B";
            case 278: return "X"; case 279: return "Y";
            case 280: return "LT"; case 281: return "RT";
            default:
                snprintf(unknownKey, sizeof(unknownKey), "Key%d", dikCode);
                return unknownKey;
        }
    }

    bool IsValidKeyCode(int dikCode) {
        const char* name = DIKToKeyName(dikCode);
        return !(name[0] == 'K' && name[1] == 'e' && name[2] == 'y' &&
                 name[3] >= '0' && name[3] <= '9');
    }

    std::uint32_t GetHeldModifier() {
        // Sided first: LCtrl and RCtrl must bind to the physical key held. The
        // generic VK_CONTROL check below cannot tell the sides apart, so it is
        // only the fallback for a side that did not resolve.
        if (GetAsyncKeyState(VK_LMENU)    & 0x8000) return 4;
        if (GetAsyncKeyState(VK_RMENU)    & 0x8000) return 5;
        if (GetAsyncKeyState(VK_LCONTROL) & 0x8000) return 6;
        if (GetAsyncKeyState(VK_RCONTROL) & 0x8000) return 7;
        if (GetAsyncKeyState(VK_LSHIFT)   & 0x8000) return 8;
        if (GetAsyncKeyState(VK_RSHIFT)   & 0x8000) return 9;
        if (GetAsyncKeyState(VK_MENU)     & 0x8000) return 1;
        if (GetAsyncKeyState(VK_CONTROL)  & 0x8000) return 2;
        if (GetAsyncKeyState(VK_SHIFT)    & 0x8000) return 3;
        return 0;
    }

    bool IsModifierScancode(std::uint32_t scancode) {
        // LAlt=0x38, RAlt=0xB8, LCtrl=0x1D, RCtrl=0x9D, LShift=0x2A, RShift=0x36
        return scancode == 0x38 || scancode == 0xB8 || scancode == 0x1D ||
               scancode == 0x9D || scancode == 0x2A || scancode == 0x36;
    }

    std::string FormatHotkeyWithModifier(int dikCode, int modifier) {
        const std::string keyName = DIKToKeyName(dikCode);
        const char* modName = "";

        switch (modifier) {
            case 1: modName = "Alt"; break;
            case 2: modName = "Ctrl"; break;
            case 3: modName = "Shift"; break;
            case 4: modName = "LAlt"; break;
            case 5: modName = "RAlt"; break;
            case 6: modName = "LCtrl"; break;
            case 7: modName = "RCtrl"; break;
            case 8: modName = "LShift"; break;
            case 9: modName = "RShift"; break;
            default: break;
        }

        if (modName[0] != '\0') {
            return std::string(modName) + " + " + keyName;
        }
        return keyName;
    }

    int DIKToVK(int dikCode) {
        if (dikCode < 1 || dikCode >= 256) return 0;  // unbound, or mouse/gamepad
        return static_cast<int>(MapVirtualKeyA(static_cast<UINT>(dikCode), MAPVK_VSC_TO_VK));
    }
}
