#pragma once

namespace ImGuiRenderer {
    void Install();
    bool IsAnyPopupOpen();

    // InputVerbose-only snapshot of the real Windows pointer/window state.
    // Used at map, tilt, and wheel boundaries where Skyrim's MenuCursor alone
    // cannot show cursor escape onto another monitor.
    void LogInputEnvironment(const char* reason);
}
