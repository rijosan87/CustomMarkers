#pragma once

#include <atomic>

namespace GlanceBounds {
    inline std::atomic<int> maxPosX{ -1 };
    inline std::atomic<int> maxPosY{ -1 };
}
