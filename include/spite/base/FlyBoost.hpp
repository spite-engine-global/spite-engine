#pragma once

#include <algorithm>

namespace spite
{
    // The editor fly camera's Shift boost: 1x, plus rate per second held, capped at max; released, back to 1x.
    struct FlyBoost
    {
        float held = 0.0f;

        float advance(const bool shift, const float seconds, const float rate, const float max)
        {
            held = shift ? held + seconds : 0.0f;
            return std::min(1.0f + rate * held, max);
        }
    };
} // namespace spite
