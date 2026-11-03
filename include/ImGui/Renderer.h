#pragma once

struct ImDrawList;

namespace ImGuiRenderer {
    void Install();
    bool IsAnyPopupOpen();

    void LogInputEnvironment(const char* reason);

    // Brackets a draw-list span with a bias-0 sampler for small radar icons.
    void BeginSmallIconSampler(ImDrawList* dl);
    void EndSmallIconSampler(ImDrawList* dl);
}
