#include <algorithm>
#include <charconv>
#include <cmath>

#include "localization/Localization.hpp"
#include "ui/UiChart.hpp"

// Axis arithmetic of charts: nice ticks, nice ranges and number labels.

namespace spite
{
    namespace
    {
        constexpr double LIMIT = 1.0e150; // bounds clamp here, so spans and tick arithmetic stay finite
        constexpr double SNAP = 1.0e-9; // a tick within this many steps of a bound counts as on it

        struct Bounds
        {
            double min;
            double max;
        };

        // NaN reads as the default bound, infinities and huge values clamp, a reversed range swaps, an empty one
        // widens around its value.
        Bounds sanitized(double min, double max)
        {
            min = std::isnan(min) ? 0.0 : std::clamp(min, -LIMIT, LIMIT);
            max = std::isnan(max) ? 1.0 : std::clamp(max, -LIMIT, LIMIT);
            if (min > max)
            {
                std::swap(min, max);
            }
            if (min == max)
            {
                const double pad = min != 0.0 ? std::abs(min) * 0.1 : 1.0;
                min -= pad;
                max += pad;
            }
            return {min, max};
        }

        // 1, 2 or 5 times a power of ten, the nearest to `raw` on a log scale (d3's thresholds)
        double niceStep(double raw)
        {
            raw = std::max(raw, 1.0e-300); // below it pow(10, ...) underflows to 0
            const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
            const double fraction = raw / magnitude;
            const double nice = fraction < 1.4142135623730951 ? 1.0
                : fraction < 3.1622776601683795               ? 2.0
                : fraction < 7.0710678118654755               ? 5.0
                                                              : 10.0;
            return nice * magnitude;
        }

        UiChartTicks ticksOf(const Bounds bounds, const u32 target)
        {
            const double span = bounds.max - bounds.min;
            double step = niceStep(span / static_cast<double>(std::clamp<u32>(target, 2, 20) - 1));
            // bounds so far from zero for their span that a tick would not be distinct from its neighbour
            while (std::max(std::abs(bounds.min), std::abs(bounds.max)) / step > 1.0e15)
            {
                step *= 10.0;
            }
            for (;;)
            {
                const double first = std::ceil(bounds.min / step - SNAP);
                const double last = std::floor(bounds.max / step + SNAP);
                if (last - first + 1.0 <= static_cast<double>(UI_CHART_MAX_TICKS))
                {
                    UiChartTicks ticks;
                    ticks.step = step;
                    ticks.first = first * step;
                    ticks.count = last >= first ? static_cast<u32>(last - first + 1.0) : 0u;
                    return ticks;
                }
                step *= 2.0;
                step = niceStep(step);
            }
        }
    } // namespace

    UiChartTicks uiChartTicks(const double min, const double max, const u32 target)
    {
        return ticksOf(sanitized(min, max), target);
    }

    UiChartRange uiChartNiceRange(const double min, const double max, const u32 target)
    {
        // data given as empty (min above max, as an empty scan leaves it) is the unit range
        Bounds bounds = min > max ? Bounds{0.0, 1.0} : sanitized(min, max);
        UiChartRange range{bounds.min, bounds.max};
        // widening the range can make the next step larger: settle on a range whose own ticks share its step
        for (int round = 0; round < 4; ++round)
        {
            const UiChartTicks ticks = ticksOf({range.min, range.max}, target);
            const UiChartRange widened{std::floor(bounds.min / ticks.step + SNAP) * ticks.step,
                                       std::ceil(bounds.max / ticks.step - SNAP) * ticks.step};
            if (widened == range)
            {
                break;
            }
            range = widened;
        }
        if (!(range.max > range.min) || !std::isfinite(range.min) || !std::isfinite(range.max))
        {
            return {bounds.min, bounds.max};
        }
        return range;
    }

    std::string uiChartFormat(const std::string_view locale, const double value, const double step)
    {
        if (std::isnan(value))
        {
            return "NaN";
        }
        if (std::isinf(value))
        {
            return value < 0.0 ? "-inf" : "inf";
        }
        const double magnitude = std::abs(value);
        // three fraction digits cannot show a step (or a value) below 0.001
        const bool tooFine = step > 0.0 ? step < 0.001 : magnitude != 0.0 && magnitude < 0.001;
        if (magnitude >= 1.0e15 || (tooFine && magnitude != 0.0))
        {
            char buffer[48];
            const auto result = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::general, 3);
            return std::string(buffer, result.ptr);
        }
        std::string out;
        const MessageArg arg{"n", value};
        if (!formatMessage(locale, "{n, number}", std::span(&arg, 1), out))
        {
            char buffer[48];
            const auto result = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::general, 6);
            return std::string(buffer, result.ptr);
        }
        return out;
    }
} // namespace spite
