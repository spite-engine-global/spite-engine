#pragma once

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cmath>
#include <string>
#include <string_view>

#include "base/Platform.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    // The Profiler timeline's view: the window of the selected capture the timeline shows, in ns
    // since the capture began, and how far the lanes are scrolled. It lives on the Profiler panel's root element beside
    // ProfilerSelection, and only the profilerView command (and the gestures, through the same function) writes it
    // (every field is READONLY to `set`): a window always lies inside [0, durationNs], at least profview::MIN_NS wide
    // unless the capture is shorter. `generation` grows with every change.
    struct ProfilerViewComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "ProfilerView";
        u32 capture = 0; // CaptureStore id the window belongs to, 0: none
        u64 durationNs = 0; // the capture's length
        u64 startNs = 0;
        u64 endNs = 0;
        f32 laneScroll = 0.0f; // UI pixels the lanes are scrolled up
        u64 generation = 0;
        SPITE_SCHEMA(ProfilerViewComponent, SPITE_FIELD(capture, READONLY), SPITE_FIELD(durationNs, READONLY),
                     SPITE_FIELD(startNs, READONLY), SPITE_FIELD(endNs, READONLY), SPITE_FIELD(laneScroll, READONLY),
                     SPITE_FIELD(generation, READONLY))
    };

    // The view arithmetic, integers only: times are ns since the capture began (a 60 s capture is 6e10, far inside
    // 2^62), pointer positions are fixed-point pixels, and every product goes through a 128-bit intermediate, so a
    // zoom or a pan has no rounding drift beyond 1 ns per step and no overflow.
    namespace profview
    {
        inline constexpr u64 MIN_NS = 100; // the narrowest window
        inline constexpr u64 MAX_NS = u64{1} << 62; // times past this are refused
        inline constexpr u32 PIXEL_SCALE = 16; // pointer positions in 1/16 pixel

        // round(a * b / c); c > 0, saturates at ~0 when the result does not fit
        [[nodiscard]] inline u64 mulDivRound(const u64 a, const u64 b, const u64 c)
        {
#if defined(SPITE_COMPILER_MSVC)
            u64 high = 0;
            const u64 low = _umul128(a, b, &high);
            u64 sum = 0;
            const unsigned char carry = _addcarry_u64(0, low, c / 2, &sum);
            high += carry;
            if (high >= c)
            {
                return ~u64{0};
            }
            u64 remainder = 0;
            return _udiv128(high, sum, c, &remainder);
#else
            const auto rounded = (static_cast<unsigned __int128>(a) * b + c / 2) / c;
            return rounded > ~u64{0} ? ~u64{0} : static_cast<u64>(rounded);
#endif
        }

        struct Window
        {
            u64 start = 0;
            u64 end = 0;
            bool ok = false;
            [[nodiscard]] u64 width() const { return end - start; }
            bool operator==(const Window&) const = default;
        };

        // `width` ns from `start`, clamped into [0, duration] (the width first, then the start; a window wider than the
        // capture is the capture). Not ok for an empty capture.
        [[nodiscard]] inline Window fitWindow(std::int64_t start, u64 width, const u64 duration)
        {
            if (duration == 0 || duration > MAX_NS)
            {
                return {};
            }
            width = std::clamp(width, std::min(MIN_NS, duration), duration);
            start = std::clamp<std::int64_t>(start, 0, static_cast<std::int64_t>(duration - width));
            return {static_cast<u64>(start), static_cast<u64>(start) + width, true};
        }

        // The window zoomed by num/den (num > den zooms in) keeping the time under the pointer fixed. The pointer is at
        // xq of wq fixed-point pixels along the window.
        [[nodiscard]] inline Window zoomWindow(const Window& view, const u64 duration, u64 xq, const u64 wq,
                                               const u32 num, const u32 den)
        {
            if (!view.ok || num == 0 || den == 0 || wq == 0)
            {
                return view;
            }
            xq = std::min(xq, wq);
            const u64 width = view.width();
            Window zoomed = fitWindow(0, std::max<u64>(1, mulDivRound(width, den, num)), duration);
            if (!zoomed.ok)
            {
                return view;
            }
            const u64 widthAfter = zoomed.width();
            const u64 anchor = view.start + mulDivRound(xq, width, wq); // the time under the pointer
            return fitWindow(static_cast<std::int64_t>(anchor) -
                                 static_cast<std::int64_t>(mulDivRound(xq, widthAfter, wq)),
                             widthAfter, duration);
        }

        [[nodiscard]] inline Window panWindow(const Window& view, const std::int64_t delta, const u64 duration)
        {
            if (!view.ok)
            {
                return view;
            }
            return fitWindow(static_cast<std::int64_t>(view.start) +
                                 std::clamp<std::int64_t>(delta, -static_cast<std::int64_t>(MAX_NS),
                                                          static_cast<std::int64_t>(MAX_NS)),
                             view.width(), duration);
        }

        // "250", "1.5ms", "2 s": a finite number and a unit ns, us, ms or s (ns when none); the error text, or empty.
        [[nodiscard]] inline std::string parseTimeNs(std::string_view text, std::int64_t& ns)
        {
            const auto trim = [](std::string_view s)
            {
                while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                {
                    s.remove_prefix(1);
                }
                while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
                {
                    s.remove_suffix(1);
                }
                return s;
            };
            text = trim(text);
            double value = 0.0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || !std::isfinite(value))
            {
                return "'" + std::string(text.substr(0, 32)) + "' is not a time (a finite number and ns, us, ms or s)";
            }
            const std::string_view unit = trim(text.substr(static_cast<sizet>(end - text.data())));
            double scale = 1.0;
            if (unit == "us")
            {
                scale = 1e3;
            }
            else if (unit == "ms")
            {
                scale = 1e6;
            }
            else if (unit == "s")
            {
                scale = 1e9;
            }
            else if (!unit.empty() && unit != "ns")
            {
                return "'" + std::string(text.substr(0, 32)) + "' has an unknown unit (ns, us, ms or s)";
            }
            const double scaled = std::round(value * scale);
            if (!std::isfinite(scaled) || std::abs(scaled) > static_cast<double>(MAX_NS))
            {
                return "'" + std::string(text.substr(0, 32)) + "' is out of range (at most 2^62 ns)";
            }
            ns = static_cast<std::int64_t>(scaled);
            return {};
        }

        // "<start>..<end>" in the units above (ns since the capture began); the error text, or empty. The order is
        // checked by the caller, which knows the capture.
        [[nodiscard]] inline std::string parseRange(const std::string_view text, std::int64_t& start, std::int64_t& end)
        {
            const sizet split = text.find("..");
            if (split == std::string_view::npos)
            {
                return "range wants '<start>..<end>', such as 1.5s..2s or 1000000..2000000 (ns)";
            }
            if (std::string error = parseTimeNs(text.substr(0, split), start); !error.empty())
            {
                return error;
            }
            return parseTimeNs(text.substr(split + 2), end);
        }
    } // namespace profview
} // namespace spite
