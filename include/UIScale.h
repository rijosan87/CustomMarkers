#pragma once

#include "Settings.h"
#include <algorithm>
#include <cmath>

namespace UIScale
{
    inline float g_presentationCompensation = 1.0f;

    inline void SetPresentationCompensation(float value)
    {
        g_presentationCompensation = std::isfinite(value)
            ? std::clamp(value, 1.0f, 4.0f)
            : 1.0f;
    }

    inline float PresentationCompensation()
    {
        return g_presentationCompensation;
    }

    inline float UserMaster()
    {
        const auto* settings = Settings::GetSingleton();
        return settings
            ? std::clamp(static_cast<float>(settings->interfaceScale) / 100.0f, 0.5f, 2.0f)
            : 1.0f;
    }

    inline float Master()
    {
        return UserMaster() * PresentationCompensation();
    }

    inline float Font()
    {
        const auto dampen = [](float value) {
            return 1.0f + (value - 1.0f) * 0.5f;
        };
        return dampen(UserMaster()) * dampen(PresentationCompensation());
    }

    inline float Px(float value)
    {
        return value * Master();
    }

    inline int PxInt0(int value)
    {
        if (value <= 0) return 0;
        return (std::max)(1, static_cast<int>(std::lround(Px(static_cast<float>(value)))));
    }

    inline int PxSigned(int value)
    {
        return static_cast<int>(std::lround(Px(static_cast<float>(value))));
    }
}
