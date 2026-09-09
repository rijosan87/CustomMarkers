#pragma once

// Hooks game input dispatching to capture key and mouse events.


namespace InputHook {
    void Install();

    bool IsTextInputActive();

    void ClearSettingsHotkeyLatch();
}
