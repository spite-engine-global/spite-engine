#pragma once

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "base/MathTypes.hpp"
#include "base/Platform.hpp"
#include "ui/UiCanvas.hpp"
#include "ui/UiExport.hpp"

namespace spite
{
    // Charts: a line, bar or histogram plot with nice-number axes, gridlines, tick labels, a legend, a
    // hover readout and drag range selection, composed of three canvases (UiWidgets.hpp, Chart). This header holds
    // the pure parts: axis arithmetic and number formatting, and what a chart reports.

    inline constexpr u32 UI_CHART_MAX_SERIES = 16;
    inline constexpr u32 UI_CHART_MAX_BINS = 1024; // histogram bins
    inline constexpr u32 UI_CHART_NO_SERIES = ~0u; // Chart::addSeries when the series budget refused it
    inline constexpr u32 UI_CHART_MAX_TICKS = 64;

    struct UiChartTicks
    {
        double first = 0.0; // the lowest tick, >= the axis minimum
        double step = 1.0; // 1, 2 or 5 times a power of ten (no 2.5: a step of at least 0.001 formats exactly with
                           // the three fraction digits the number format keeps)
        u32 count = 0; // ticks first, first + step, ... up to the axis maximum
        // the index-th tick, a whole multiple of the step (zero is exactly zero)
        [[nodiscard]] double at(const u32 index) const
        {
            return (std::round(first / step) + static_cast<double>(index)) * step;
        }
    };

    // Ticks of [min, max] about `target` of them. Non-finite bounds read as 0 and 1, bounds past +-1e150 clamp to it, a
    // reversed range swaps, an empty one (min == max) is widened around the value first; at most UI_CHART_MAX_TICKS.
    [[nodiscard]] SPITE_UI_API UiChartTicks uiChartTicks(double min, double max, u32 target);

    struct UiChartRange
    {
        double min = 0.0;
        double max = 1.0;
        bool operator==(const UiChartRange&) const = default;
    };

    // The axis range that holds [min, max] with whole tick steps at both ends (the range an automatic axis shows, so
    // it changes only when the data crosses a tick), sanitized as uiChartTicks does. Empty data (min > max, both
    // infinite) gives 0..1, a single value a range around it.
    [[nodiscard]] SPITE_UI_API UiChartRange uiChartNiceRange(double min, double max, u32 target);

    // A number for an axis label or readout in `locale` (the Localization's separators). `step` is the tick spacing the
    // label sits on (0: up to three fraction digits). A value that three fraction digits cannot show (a step below
    // 0.001) or past 1e15 is written as 5e-05, in plain ASCII.
    [[nodiscard]] SPITE_UI_API std::string uiChartFormat(std::string_view locale, double value, double step);

    enum class UiChartKind : u8
    {
        Series, // line or bar series appended sample by sample (Chart::addSeries, Chart::append)
        Histogram, // a bar per bin of the values added (Chart::addValues)
    };

    struct UiChartSeriesDesc
    {
        std::string name;
        u32 color = uiCanvasColor(90, 170, 255);
        UiSeriesStyle style = UiSeriesStyle::Line;
        float thickness = 1.5f;
        u32 capacity = 4096; // samples held; a full series drops its oldest (UI_SERIES_MAX_CAPACITY at most)
    };

    struct UiChartAxisDesc
    {
        bool autoRange = true; // fit the data, in whole tick steps
        double min = 0.0; // the fixed range when autoRange is off
        double max = 1.0;
        u32 targetTicks = 0; // 0: by the plot's size
        bool hold = false; // autoRange of a series chart: grow at once, shrink only once the data's range is two tick
                           // steps inside the shown one (a live readout whose extremes drift across a tick relayouts
                           // once, not on every crossing)
    };

    struct UiChartHistogramDesc
    {
        u32 bins = 20; // 1..UI_CHART_MAX_BINS
        bool autoRange = true; // the first values added set the range (nice bounds); later values outside count as
                               // out of range
        double min = 0.0;
        double max = 1.0;
    };

    struct UiChartColors
    {
        u32 axis = uiCanvasColor(170, 170, 180);
        u32 grid = uiCanvasColor(255, 255, 255, 36);
        u32 text = uiCanvasColor(210, 210, 220);
        u32 crosshair = uiCanvasColor(255, 255, 255, 140);
        u32 selection = uiCanvasColor(90, 170, 255, 60);
        u32 band = uiCanvasColor(255, 190, 80, 235); // Chart::setBand: the edges and top bar, the fill at 1/4 alpha
        u32 readoutBackground = uiCanvasColor(20, 22, 28, 230);
        u32 bars = uiCanvasColor(90, 170, 255); // histogram bars
    };

    // A drag-selected x range: in x units (xMin <= xMax) and as the samples (a series chart, absolute sample numbers
    // counted from the series' creation or clear) or bins (a histogram) it covers, first..last inclusive.
    struct UiChartSelection
    {
        bool active = false;
        double xMin = 0.0;
        double xMax = 0.0;
        u64 first = 0;
        u64 last = 0;
    };

    // A click in the plot: the sample (a histogram: the bin) nearest the pointer, as the hover reads it.
    struct UiChartClick
    {
        u64 sample = 0;
        double x = 0.0;
    };

    struct UiChartTick
    {
        double value = 0.0;
        std::string label;
    };

    // One series' nearest sample to the hover (a histogram: the bin under it, series "count").
    struct UiChartHoverValue
    {
        std::string series;
        u64 sample = 0; // absolute sample number, or the bin
        double x = 0.0; // its x
        double value = 0.0;
    };

    // What a chart shows, for tools: tick values and labels, the plot's place in the chart (unscaled UI pixels from
    // the chart's top-left, where a pointer injection lands), the hover and the selection.
    struct UiChartReadout
    {
        bool histogram = false;
        bool xRelative = false; // x labels are offsets from the newest sample (a following window)
        bool aligned = true; // every series holds the same samples (they were appended in lockstep)
        Vec2 plotPosition{};
        Vec2 plotSize{};
        double xMin = 0.0;
        double xMax = 1.0;
        double yMin = 0.0;
        double yMax = 1.0;
        u64 windowFirst = 0;
        u32 windowCount = 0;
        std::vector<UiChartTick> xTicks;
        std::vector<UiChartTick> yTicks;
        bool hovering = false;
        std::vector<UiChartHoverValue> hover;
        UiChartSelection selection;
        UiChartSelection band; // Chart::setBand; active when set
        u64 outOfRange = 0; // histogram: values outside the range
    };
} // namespace spite
