#pragma once

namespace ImGuiRenderer {
    void Install();
    bool IsAnyPopupOpen();

    void LogInputEnvironment(const char* reason);
}
