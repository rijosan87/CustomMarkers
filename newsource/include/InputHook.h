#pragma once

namespace InputHook {
    // Hook BSInputDeviceManager::ProcessEvents to feed kChar events to ImGui
    // and block keyboard input from reaching the engine while WantTextInput is true.
    void Install();

    // True when ImGui has a text input widget focused (WantTextInput).
    // Used by MarkerHoverHook to suppress vanilla hover/fast-travel while typing.
    bool IsTextInputActive();

    // Clear release-latch state for the settings hotkey when settings closes.
    void ClearSettingsHotkeyLatch();
}
