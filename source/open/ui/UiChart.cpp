#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <ranges>

#include "base/Assert.hpp"
#include "base/Logging.hpp"
#include "base/ServiceLocator.hpp"
#include "ecs/World.hpp"
#include "localization/Localization.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiScope.hpp"
#include "UiChartState.hpp"

// Charts: the runtime of Chart (UiWidgets.hpp). The model, the cost rules and the control vocabulary
// are documented there.

namespace spite
{
    namespace
    {
        constexpr float TEXT_WIDTH = 0.6f; // average glyph width in em: canvas text cannot be measured while emitting
        constexpr float MIN_PLOT = 8.0f;
        constexpr u64 LOST_CHECK_MAX = 64; // dropped window samples checked one by one before a block query
        constexpr u64 GAINED_SCAN_MAX = 4096;
        constexpr float FLOAT_LIMIT = 3.0e38f;
        constexpr double FLOAT_RANGE = 1.0e37; // fixed ranges clamp here: the series shader takes floats
        constexpr float DRAG_MIN = 2.0f; // pixels; a shorter drag is a click

        constexpr float inf = std::numeric_limits<float>::max();

        double stepOf(const UiChartDesc& desc)
        {
            return std::isfinite(desc.xStep) && desc.xStep > 0.0 ? std::clamp(desc.xStep, 1.0e-100, 1.0e100) : 1.0;
        }

        double startOf(const UiChartDesc& desc)
        {
            return std::isfinite(desc.xStart) ? std::clamp(desc.xStart, -1.0e150, 1.0e150) : 0.0;
        }

        u32 codepoints(const std::string_view text)
        {
            return static_cast<u32>(std::ranges::count_if(text, [](const char c) { return (c & 0xC0) != 0x80; }));
        }

        float snap(const float v) { return std::floor(v) + 0.5f; }

        float clampFloat(const double v)
        {
            return static_cast<float>(
                std::clamp(v, static_cast<double>(-FLOAT_LIMIT), static_cast<double>(FLOAT_LIMIT)));
        }

        UiChartRange orderedRange(double min, double max)
        {
            min = std::isfinite(min) ? std::clamp(min, -FLOAT_RANGE, FLOAT_RANGE) : 0.0;
            max = std::isfinite(max) ? std::clamp(max, -FLOAT_RANGE, FLOAT_RANGE) : 1.0;
            if (min > max)
            {
                std::swap(min, max);
            }
            // equal, or closer than a float tells apart (the shader's range is float): widen so no division by zero
            const double scale = std::max(std::abs(min), std::abs(max));
            if (max - min <= scale * 1.0e-6)
            {
                const double pad = std::max(0.5, scale * 1.0e-5);
                min -= pad;
                max += pad;
            }
            return {min, max};
        }

        bool sameComponent(const UiChartComponent& a, const UiChartComponent& b)
        {
            return a.seriesCount == b.seriesCount && a.histogram == b.histogram && a.xRelative == b.xRelative &&
                a.yAuto == b.yAuto && a.aligned == b.aligned && a.xMin == b.xMin && a.xMax == b.xMax &&
                a.yMin == b.yMin && a.yMax == b.yMax && a.windowFirst == b.windowFirst &&
                a.windowCount == b.windowCount && a.hovering == b.hovering && a.hoverSample == b.hoverSample &&
                a.hoverX == b.hoverX && a.selecting == b.selecting && a.selected == b.selected &&
                a.selectionMin == b.selectionMin && a.selectionMax == b.selectionMax &&
                a.selectionFirst == b.selectionFirst && a.selectionLast == b.selectionLast && a.banded == b.banded &&
                a.bandMin == b.bandMin && a.bandMax == b.bandMax;
        }
    } // namespace

    // -- Blocks ---------------------------------------------------------------------------------------------------

    void UiChartState::Blocks::clear()
    {
        blocks.clear();
        first = 0;
    }

    void UiChartState::Blocks::add(const u64 sample, const float value)
    {
        const u64 block = sample / SIZE;
        if (blocks.empty() || block > first + blocks.size() + UI_SERIES_MAX_CAPACITY / SIZE)
        {
            blocks.clear(); // a gap longer than a series can hold: nothing before it is in a window any more
            first = block;
        }
        while (first + blocks.size() <= block)
        {
            blocks.emplace_back(inf, -inf);
        }
        if (block >= first)
        {
            auto& entry = blocks[block - first];
            entry.first = std::min(entry.first, value);
            entry.second = std::max(entry.second, value);
        }
    }

    void UiChartState::Blocks::dropBefore(const u64 base)
    {
        while (!blocks.empty() && (first + 1) * SIZE <= base)
        {
            blocks.pop_front();
            ++first;
        }
    }

    std::pair<float, float> UiChartState::Blocks::query(const UiSeries& series, const u64 from, const u64 to) const
    {
        float low = inf;
        float high = -inf;
        if (to <= from)
        {
            return {low, high};
        }
        const u64 base = series.appended - series.count;
        const auto scan = [&](const u64 begin, const u64 end)
        {
            for (u64 sample = std::max(begin, base); sample < end && sample - base < series.count; ++sample)
            {
                const float v = series.at(static_cast<u32>(sample - base));
                low = std::min(low, v);
                high = std::max(high, v);
            }
        };
        const u64 firstWhole = (from + SIZE - 1) / SIZE;
        const u64 lastWhole = to / SIZE;
        if (firstWhole >= lastWhole)
        {
            scan(from, to);
            return {low, high};
        }
        scan(from, firstWhole * SIZE);
        for (u64 block = firstWhole; block < lastWhole; ++block)
        {
            if (block >= first && block - first < blocks.size())
            {
                low = std::min(low, blocks[block - first].first);
                high = std::max(high, blocks[block - first].second);
            }
            else
            {
                scan(block * SIZE, (block + 1) * SIZE);
            }
        }
        scan(lastWhole * SIZE, to);
        return {low, high};
    }

    // -- UiChartState: model --------------------------------------------------------------------------------------

    UiChartState::UiChartState(UiScope& owner, const Entity rootElement, const Entity plotElement,
                               const Entity overlayElement, const UiChartDesc& chartDesc) :
        plot(&owner, plotElement, owner.generation()), m_scope(&owner), m_root(rootElement), m_plotEntity(plotElement),
        m_overlayEntity(overlayElement), m_rootCanvas(&owner, rootElement, owner.generation()),
        m_overlay(&owner, overlayElement, owner.generation()), m_desc(chartDesc)
    {
        m_desc.textSize = std::isfinite(m_desc.textSize) ? std::clamp(m_desc.textSize, 6.0f, 64.0f) : 12.0f;
        m_followCount = m_desc.window;
        m_yAuto = m_desc.y.autoRange;
        m_yFixed = orderedRange(m_desc.y.min, m_desc.y.max);
    }

    bool UiChartState::alive() const { return m_rootCanvas.isValid() && plot.isValid() && m_overlay.isValid(); }

    const Localization* UiChartState::localization() const
    {
        if (const Localization* found = m_scope->hierarchy().localization())
        {
            return found;
        }
        if (!m_localization)
        {
            ServiceLocator& services = m_scope->world().services();
            m_localization = services.contains<Localization>() ? &services.get<Localization>() : nullptr;
        }
        return m_localization;
    }

    float UiChartState::textSize() const { return m_desc.textSize * m_scope->hierarchy().textScale(); }

    std::string UiChartState::locale() const
    {
        const Localization* found = localization();
        return found ? found->locale() : std::string("en");
    }

    void UiChartState::build()
    {
        UiScope& scope = *m_scope;
        scope.hierarchy().attach(m_plotEntity, m_root);
        scope.hierarchy().attach(m_overlayEntity, m_root);
        plot.setStyleClass(InternedString("chart.plot"));
        m_overlay.setStyleClass(InternedString("chart.overlay"));
        plot.setInteractable(false);
        m_overlay.setInteractable(false);
        m_rootCanvas.onPointer([this](const UiCanvasPointer& event) { pointer(event); });
        scope.hierarchy().observeSize(m_root);
        const Localization* found = localization();
        m_localeRevision = found ? found->revision() : 0;
        m_textScale = scope.hierarchy().textScale();
        if (histogram())
        {
            setHistogram(m_desc.histogram);
        }
        resized();
        syncComponent();
    }

    const UiSeries* UiChartState::held(const Series& series) const { return plot.series(series.id); }

    const UiSeries* UiChartState::seriesAt(const u32 index) const
    {
        return index < m_series.size() ? held(m_series[index]) : nullptr;
    }

    UiChartState::Window UiChartState::windowOf(const u64 base, const u32 count) const
    {
        if (m_following)
        {
            const u32 shown = m_followCount == 0 ? count : std::min(m_followCount, count);
            return {base + count - shown, shown};
        }
        const u64 first = std::max(m_fixedFirst, base);
        u64 end = base + count;
        if (m_fixedCount != 0)
        {
            end = std::min(end, m_fixedFirst + m_fixedCount);
        }
        return {first, end > first ? static_cast<u32>(end - first) : 0u};
    }

    UiChartState::Window UiChartState::windowOfSeries(const Series& series) const
    {
        const UiSeries* held = this->held(series);
        return held ? windowOf(held->appended - held->count, held->count) : Window{};
    }

    const UiChartState::Series* UiChartState::reference() const
    {
        for (const Series& series : m_series)
        {
            if (held(series))
            {
                return &series;
            }
        }
        return nullptr;
    }

    void UiChartState::ensureExtents(Series& series)
    {
        const UiSeries* held = this->held(series);
        if (!held)
        {
            return;
        }
        const Window window = windowOf(held->appended - held->count, held->count);
        if (series.extentsValid && series.window == window)
        {
            return;
        }
        series.window = window;
        series.extentsValid = true;
        const auto [low, high] = series.blocks.query(*held, window.first, window.first + window.count);
        series.min = low;
        series.max = high;
    }

    double UiChartState::xOf(const double position) const
    {
        return histogram() ? position : startOf(m_desc) + position * stepOf(m_desc);
    }

    double UiChartState::positionOf(const double x) const
    {
        return histogram() ? x : (x - startOf(m_desc)) / stepOf(m_desc);
    }

    double UiChartState::positionAt(const float pixel) const
    {
        const double span = m_shown.domainHigh - m_shown.domainLow;
        return m_shown.domainLow + (m_plotSize.x > 0.0f ? static_cast<double>(pixel) / m_plotSize.x * span : 0.0);
    }

    float UiChartState::pixelOf(const double position) const
    {
        const double span = m_shown.domainHigh - m_shown.domainLow;
        return span > 0.0 ? static_cast<float>((position - m_shown.domainLow) / span * m_plotSize.x) : 0.0f;
    }

    UiChartState::Evaluation UiChartState::evaluate()
    {
        Evaluation ev;
        if (histogram())
        {
            ev.domainLow = ev.labelLow = m_binMin;
            ev.domainHigh = ev.labelHigh = m_binMax;
            ev.y = m_yAuto ? uiChartNiceRange(0.0, static_cast<double>(std::max<u64>(m_binMaxCount, 1)), m_yTarget)
                           : m_yFixed;
            return ev;
        }
        const Series* ref = reference();
        const double step = stepOf(m_desc);
        if (ref)
        {
            ev.window = windowOfSeries(*ref);
            const double first = static_cast<double>(ev.window.first);
            const double n = static_cast<double>(ev.window.count);
            const double newest = ev.window.count != 0 ? first + n - 1.0 : first;
            // a following window of N samples spans N samples from the start: while it fills, the samples stand at
            // its right edge and the axes keep their labels (a filling window would move them on every append)
            const bool standing = m_following && m_followCount >= 2;
            const double span = standing ? static_cast<double>(m_followCount) - 1.0 : 0.0;
            if (ref->style == UiSeriesStyle::Bars)
            {
                ev.domainLow = standing ? newest - span - 0.5 : first - 0.5;
                ev.domainHigh = ev.window.count == 0 ? first + 0.5 : first + n - 0.5;
            }
            else if (standing)
            {
                ev.domainLow = newest - span;
                ev.domainHigh = newest;
            }
            else
            {
                ev.domainLow = first;
                ev.domainHigh = ev.window.count >= 2 ? first + n - 1.0 : first + 1.0;
            }
            ev.relative = m_following;
            ev.labelLow = ev.relative ? (ev.domainLow - newest) * step : xOf(ev.domainLow);
            ev.labelHigh = ev.relative ? (ev.domainHigh - newest) * step : xOf(ev.domainHigh);
            const UiSeries* refHeld = held(*ref);
            for (const Series& series : m_series)
            {
                const UiSeries* other = held(series);
                ev.aligned =
                    ev.aligned && other && other->count == refHeld->count && other->appended == refHeld->appended;
            }
        }
        else
        {
            ev.domainHigh = ev.labelHigh = 1.0;
        }
        if (!m_yAuto)
        {
            ev.y = m_yFixed;
            return ev;
        }
        double low = std::numeric_limits<double>::infinity();
        double high = -low;
        bool bars = false;
        for (Series& series : m_series)
        {
            if (!held(series))
            {
                continue;
            }
            ensureExtents(series);
            bars = bars || series.style == UiSeriesStyle::Bars;
            if (series.window.count != 0)
            {
                low = std::min(low, static_cast<double>(series.min));
                high = std::max(high, static_cast<double>(series.max));
            }
        }
        if (bars && low <= high)
        {
            low = std::min(low, 0.0);
            high = std::max(high, 0.0);
        }
        ev.y = uiChartNiceRange(low, high, m_yTarget);
        if (m_desc.y.hold && m_hasShown && low <= high)
        {
            // each bound moves out at once (the data must stay in) but in only by two steps of the shown ticks
            const double margin = 2.0 * uiChartTicks(m_shown.y.min, m_shown.y.max, m_yTarget).step;
            const UiChartRange& shown = m_shown.y;
            const double heldLow = ev.y.min < shown.min || ev.y.min - shown.min >= margin ? ev.y.min : shown.min;
            const double heldHigh = ev.y.max > shown.max || shown.max - ev.y.max >= margin ? ev.y.max : shown.max;
            ev.y = uiChartNiceRange(heldLow, heldHigh, m_yTarget);
        }
        return ev;
    }

    // -- UiChartState: mutation -----------------------------------------------------------------------------------

    u32 UiChartState::addSeries(const UiChartSeriesDesc& seriesDesc)
    {
        if (!alive() || histogram() || m_series.size() >= UI_CHART_MAX_SERIES)
        {
            return UI_CHART_NO_SERIES;
        }
        const UiSeriesId id = plot.createSeries(seriesDesc.capacity);
        if (!id.isValid())
        {
            return UI_CHART_NO_SERIES;
        }
        Series series;
        series.name = seriesDesc.name;
        series.color = seriesDesc.color;
        series.style = seriesDesc.style;
        series.thickness = std::isfinite(seriesDesc.thickness) ? seriesDesc.thickness : 1.5f;
        series.id = id;
        m_series.push_back(std::move(series));
        refresh(true);
        return static_cast<u32>(m_series.size() - 1);
    }

    bool UiChartState::setSeriesName(const u32 index, const std::string_view name)
    {
        if (!alive() || index >= m_series.size())
        {
            return false;
        }
        if (m_series[index].name != name)
        {
            m_series[index].name = std::string(name);
            refresh(true);
        }
        return true;
    }

    bool UiChartState::append(const u32 index, const std::span<const float> samples)
    {
        if (!alive() || index >= m_series.size())
        {
            return false;
        }
        Series& series = m_series[index];
        const UiSeries* before = held(series);
        if (!before || !std::ranges::all_of(samples, [](const float v) { return std::isfinite(v); }))
        {
            return false;
        }
        if (samples.empty())
        {
            return true;
        }
        // The window after the append, and the samples it drops, are known before the ring overwrites them.
        const u32 capacity = before->capacity();
        const u64 kept = std::min<u64>(samples.size(), capacity);
        const u64 appendedBefore = before->appended;
        const u64 baseBefore = appendedBefore - before->count;
        const u32 countAfter = static_cast<u32>(std::min<u64>(capacity, before->count + kept));
        const u64 appendedAfter = appendedBefore + samples.size();
        const u64 baseAfter = appendedAfter - countAfter;
        const Window old = series.window;
        const Window next = windowOf(baseAfter, countAfter);
        // extents are kept only for an automatic y range, and only while the window overlaps the one they describe
        bool incremental = m_yAuto && series.extentsValid && old.count != 0 && old.first >= baseBefore &&
            old.first + old.count <= appendedBefore && next.first >= old.first && next.first < old.first + old.count &&
            next.first + next.count >= old.first + old.count && next.first - old.first <= LOST_CHECK_MAX;
        if (incremental)
        {
            for (u64 sample = old.first; sample < next.first; ++sample)
            {
                const float v = before->at(static_cast<u32>(sample - baseBefore));
                incremental = incremental && v > series.min && v < series.max;
            }
        }
        if (!plot.appendSamples(series.id, samples))
        {
            return false;
        }
        const UiSeries* after = held(series);
        const u64 firstKept = appendedAfter - kept;
        for (u64 i = 0; i < kept; ++i)
        {
            series.blocks.add(firstKept + i, samples[samples.size() - kept + i]);
        }
        series.blocks.dropBefore(baseAfter);
        const u64 oldEnd = old.first + old.count;
        const u64 newEnd = next.first + next.count;
        if (incremental && newEnd - oldEnd <= GAINED_SCAN_MAX)
        {
            for (u64 sample = oldEnd; sample < newEnd; ++sample)
            {
                const float v = after->at(static_cast<u32>(sample - baseAfter));
                series.min = std::min(series.min, v);
                series.max = std::max(series.max, v);
            }
            series.window = next;
        }
        else
        {
            series.extentsValid = false; // the next evaluation queries the blocks
        }
        refresh(false);
        return true;
    }

    void UiChartState::clear()
    {
        if (!alive())
        {
            return;
        }
        for (Series& series : m_series)
        {
            plot.clearSeries(series.id);
            series.blocks.clear();
            series.extentsValid = false;
        }
        std::ranges::fill(m_bins, u64{0});
        m_binMaxCount = 0;
        m_outOfRange = 0;
        m_binRangeSet = histogram() && !m_desc.histogram.autoRange;
        m_hovering = m_selecting = m_selected = m_anchored = m_banded = false;
        refresh(true);
    }

    void UiChartState::follow(const u32 count)
    {
        if (!alive())
        {
            return;
        }
        m_following = true;
        m_followCount = count;
        refresh(false);
    }

    void UiChartState::setWindow(const u64 first, const u32 count)
    {
        if (!alive())
        {
            return;
        }
        m_following = false;
        m_fixedFirst = first;
        m_fixedCount = count;
        refresh(false);
    }

    void UiChartState::setYRange(const double min, const double max)
    {
        if (!alive())
        {
            return;
        }
        m_yAuto = false;
        m_yFixed = orderedRange(min, max);
        refresh(histogram());
    }

    void UiChartState::setYAuto()
    {
        if (!alive())
        {
            return;
        }
        m_yAuto = true;
        refresh(histogram());
    }

    void UiChartState::setHistogram(const UiChartHistogramDesc& histogramDesc)
    {
        if (!histogram())
        {
            return;
        }
        m_desc.histogram = histogramDesc;
        m_bins.assign(std::clamp(histogramDesc.bins, 1u, UI_CHART_MAX_BINS), 0);
        m_binRangeSet = !histogramDesc.autoRange;
        const UiChartRange range = orderedRange(histogramDesc.min, histogramDesc.max);
        m_binMin = m_binRangeSet ? range.min : 0.0;
        m_binMax = m_binRangeSet ? range.max : 1.0;
        m_binMaxCount = 0;
        m_outOfRange = 0;
        if (alive())
        {
            refresh(true);
        }
    }

    void UiChartState::addValues(const std::span<const float> values)
    {
        if (!alive() || !histogram() || values.empty())
        {
            return;
        }
        if (!m_binRangeSet)
        {
            float low = inf;
            float high = -inf;
            for (const float v : values)
            {
                if (std::isfinite(v))
                {
                    low = std::min(low, v);
                    high = std::max(high, v);
                }
            }
            if (low > high)
            {
                m_outOfRange += values.size();
                refresh(false);
                return;
            }
            const UiChartRange nice = uiChartNiceRange(low, high, 8);
            m_binMin = nice.min;
            m_binMax = nice.max;
            m_binRangeSet = true;
        }
        const double span = m_binMax - m_binMin;
        const auto bins = static_cast<double>(m_bins.size());
        for (const float v : values)
        {
            if (!std::isfinite(v) || v < m_binMin || v > m_binMax)
            {
                ++m_outOfRange;
                continue;
            }
            const auto bin =
                std::min(static_cast<size_t>((static_cast<double>(v) - m_binMin) / span * bins), m_bins.size() - 1);
            m_binMaxCount = std::max(m_binMaxCount, ++m_bins[bin]);
        }
        refresh(true);
    }

    // -- UiChartState: layout and emission ------------------------------------------------------------------------

    void UiChartState::measure()
    {
        const float ts = textSize();
        const bool legend = m_desc.legend && !histogram() && !m_series.empty();
        m_top = std::ceil(6.0f + (legend ? ts + 8.0f : 0.0f)); // whole pixels: gridlines and bars land on pixel rows
        m_bottom = ts + 10.0f;
        m_right = ts * 1.5f;
        const float plotHeight = std::max(0.0f, std::floor(m_size.y - m_top - m_bottom));
        m_yTarget = m_desc.y.targetTicks != 0 ? m_desc.y.targetTicks
                                              : std::clamp(static_cast<u32>(plotHeight / 40.0f), 2u, 10u);
    }

    void UiChartState::refresh(const bool plotChanged)
    {
        if (!alive() || !m_sized)
        {
            syncComponent();
            return;
        }
        if (plotChanged)
        {
            measure(); // the legend appears with the first series
        }
        const Evaluation ev = evaluate();
        if (plotChanged || !m_hasShown || !ev.sameAxes(m_shown))
        {
            layout(ev, plotChanged);
            return;
        }
        m_shown = ev;
        setRanges();
        if (interacting())
        {
            emitOverlay();
        }
        syncComponent();
    }

    void UiChartState::layout(const Evaluation& ev, bool plotChanged)
    {
        const float ts = textSize();
        const std::string loc = locale();
        if (loc != m_labelLocale)
        {
            m_labelLocale = loc;
            m_yTicks.clear();
            m_xTicks.clear();
        }
        // a tick the layout before had at the same value and step keeps its label: formatting is the cost of a relayout
        const auto labelOf = [this, &loc](const double value, const double step)
        {
            const auto same = std::ranges::find_if(m_spareTicks, [&](const Tick& old)
                                                   { return old.value == value && old.step == step; });
            return same != m_spareTicks.end() ? std::move(same->label) : uiChartFormat(loc, value, step);
        };
        // the y axis first: its labels decide the left margin, and its tick count depends on the height alone
        const float plotHeight = std::max(0.0f, std::floor(m_size.y - m_top - m_bottom));
        const UiChartTicks yTicks = uiChartTicks(ev.y.min, ev.y.max, m_yTarget);
        m_spareTicks.swap(m_yTicks);
        m_yTicks.clear();
        u32 labelWidth = 1;
        for (u32 i = 0; i < yTicks.count; ++i)
        {
            const double value = yTicks.at(i);
            const double t = (value - ev.y.min) / (ev.y.max - ev.y.min);
            if (!(t >= -1e-6 && t <= 1.0 + 1e-6))
            {
                continue;
            }
            Tick tick{value, labelOf(value, yTicks.step), static_cast<float>((1.0 - t) * plotHeight), yTicks.step};
            labelWidth = std::max(labelWidth, codepoints(tick.label));
            m_yTicks.push_back(std::move(tick));
        }
        const float left = std::ceil(12.0f + static_cast<float>(labelWidth) * TEXT_WIDTH * ts);
        const float plotWidth = std::max(0.0f, std::floor(m_size.x - left - m_right));
        const bool visible = plotWidth >= MIN_PLOT && plotHeight >= MIN_PLOT;

        m_spareTicks.swap(m_xTicks);
        m_xTicks.clear();
        if (visible)
        {
            const u32 target =
                m_desc.xTicks != 0 ? m_desc.xTicks : std::clamp(static_cast<u32>(plotWidth / (ts * 7.0f)), 2u, 12u);
            const UiChartTicks xTicks = uiChartTicks(ev.labelLow, ev.labelHigh, target);
            const double span = ev.labelHigh - ev.labelLow;
            for (u32 i = 0; i < xTicks.count; ++i)
            {
                const double value = xTicks.at(i);
                const double t = (value - ev.labelLow) / span;
                if (!(t >= -1e-6 && t <= 1.0 + 1e-6))
                {
                    continue;
                }
                m_xTicks.push_back(
                    {value, labelOf(value, xTicks.step), static_cast<float>(t * plotWidth), xTicks.step});
            }
        }

        const Vec2 position{left, m_top};
        const Vec2 size = visible ? Vec2{plotWidth, plotHeight} : Vec2{0.0f, 0.0f};
        if (position != m_plotPosition || size != m_plotSize)
        {
            m_plotPosition = position;
            m_plotSize = size;
            for (Canvas* child : {&plot, &m_overlay})
            {
                child->setAnchor(Vec2{0.0f, 0.0f});
                child->setOffset(position);
                child->setSize(size);
            }
            plotChanged = true;
        }
        // a histogram's bars are drawn against the y range
        plotChanged = plotChanged || (histogram() && (!m_hasShown || !(ev.y == m_shown.y)));
        m_shown = ev;
        m_hasShown = true;
        emitAxes();
        if (plotChanged)
        {
            emitPlot();
        }
        setRanges();
        if (interacting())
        {
            emitOverlay();
        }
        syncComponent();
    }

    void UiChartState::emitAxes()
    {
        const float ts = textSize();
        const UiChartColors& colors = m_desc.colors;
        const Vec2 pp = m_plotPosition;
        const Vec2 ps = m_plotSize;
        m_rootCanvas.update(
            [&](UiCanvasPainter& p)
            {
                if (ps.x <= 0.0f || ps.y <= 0.0f)
                {
                    return;
                }
                if (m_desc.grid)
                {
                    for (const Tick& tick : m_yTicks)
                    {
                        const float y = snap(pp.y + tick.pixel);
                        p.line({pp.x, y}, {pp.x + ps.x, y}, 1.0f, colors.grid);
                    }
                    for (const Tick& tick : m_xTicks)
                    {
                        const float x = snap(pp.x + tick.pixel);
                        p.line({x, pp.y}, {x, pp.y + ps.y}, 1.0f, colors.grid);
                    }
                }
                p.line({snap(pp.x), pp.y}, {snap(pp.x), pp.y + ps.y + 1.0f}, 1.0f, colors.axis);
                p.line({pp.x, snap(pp.y + ps.y)}, {pp.x + ps.x, snap(pp.y + ps.y)}, 1.0f, colors.axis);
                const float lineHeight = ts + 4.0f;
                for (const Tick& tick : m_yTicks)
                {
                    const float y = snap(pp.y + tick.pixel);
                    p.line({pp.x - 3.0f, y}, {pp.x, y}, 1.0f, colors.axis);
                    p.text({4.0f, y - lineHeight * 0.5f}, {pp.x - 9.0f, lineHeight}, tick.label, colors.text,
                           UiCanvasAlign::Right, ts);
                }
                for (const Tick& tick : m_xTicks)
                {
                    const float x = snap(pp.x + tick.pixel);
                    p.line({x, pp.y + ps.y}, {x, pp.y + ps.y + 3.0f}, 1.0f, colors.axis);
                    const float width = static_cast<float>(codepoints(tick.label)) * TEXT_WIDTH * ts + 8.0f;
                    p.text({x - width * 0.5f, pp.y + ps.y + 4.0f}, {width, lineHeight}, tick.label, colors.text,
                           UiCanvasAlign::Center, ts);
                }
                if (m_desc.legend && !histogram())
                {
                    float x = pp.x;
                    for (const Series& series : m_series)
                    {
                        const float width = static_cast<float>(codepoints(series.name)) * TEXT_WIDTH * ts + 4.0f;
                        if (x + 14.0f + width > m_size.x)
                        {
                            break;
                        }
                        p.rect({x, 6.0f + (lineHeight - 10.0f) * 0.5f}, {10.0f, 10.0f}, series.color, 2.0f);
                        p.text({x + 14.0f, 6.0f}, {width, lineHeight}, series.name, colors.text, UiCanvasAlign::Left,
                               ts);
                        x += 14.0f + width + 12.0f;
                    }
                }
            });
    }

    void UiChartState::emitPlot()
    {
        const Vec2 ps = m_plotSize;
        plot.update(
            [&](UiCanvasPainter& p)
            {
                if (ps.x <= 0.0f || ps.y <= 0.0f)
                {
                    return;
                }
                if (histogram())
                {
                    const auto bins = static_cast<float>(m_bins.size());
                    const double span = m_shown.y.max - m_shown.y.min;
                    for (size_t bin = 0; bin < m_bins.size(); ++bin)
                    {
                        if (m_bins[bin] == 0)
                        {
                            continue;
                        }
                        const double t =
                            std::clamp((static_cast<double>(m_bins[bin]) - m_shown.y.min) / span, 0.0, 1.0);
                        const float x0 = ps.x * static_cast<float>(bin) / bins;
                        const float x1 = ps.x * static_cast<float>(bin + 1) / bins;
                        const float gap = x1 - x0 >= 4.0f ? 1.0f : 0.0f;
                        const auto height = static_cast<float>(t) * ps.y;
                        p.rect({x0, ps.y - height}, {x1 - x0 - gap, height}, m_desc.colors.bars);
                    }
                    return;
                }
                for (const Series& series : m_series)
                {
                    if (held(series))
                    {
                        p.series(series.id, {0.0f, 0.0f}, ps, series.color, series.thickness, series.style);
                    }
                }
            });
    }

    void UiChartState::setRanges()
    {
        if (histogram())
        {
            return;
        }
        for (const Series& series : m_series)
        {
            const UiSeries* held = this->held(series);
            if (!held)
            {
                continue;
            }
            UiSeriesRange range;
            range.min = clampFloat(m_shown.y.min);
            range.max = clampFloat(m_shown.y.max);
            if (m_following)
            {
                range.tail = true;
                range.count = m_followCount;
            }
            else
            {
                const Window window = windowOf(held->appended - held->count, held->count);
                range.first =
                    window.count != 0 ? static_cast<u32>(window.first - (held->appended - held->count)) : held->count;
                range.count = window.count;
            }
            plot.setSeriesRange(series.id, range);
        }
    }

    UiChartState::Hover UiChartState::hoverAt() const
    {
        Hover hover;
        if (!m_sized || m_plotSize.x <= 0.0f)
        {
            return hover;
        }
        const double position = positionAt(m_hoverPixel);
        if (histogram())
        {
            if (m_bins.empty())
            {
                return hover;
            }
            const double span = m_binMax - m_binMin;
            const auto bins = static_cast<double>(m_bins.size());
            const auto bin =
                static_cast<size_t>(std::clamp(std::floor((position - m_binMin) / span * bins), 0.0, bins - 1.0));
            hover.valid = true;
            hover.sample = bin;
            hover.binLow = m_binMin + span * static_cast<double>(bin) / bins;
            hover.binHigh = m_binMin + span * static_cast<double>(bin + 1) / bins;
            hover.x = (hover.binLow + hover.binHigh) * 0.5;
            hover.position = pixelOf(hover.x);
            hover.values.push_back({"count", bin, hover.x, static_cast<double>(m_bins[bin])});
            hover.colors.push_back(m_desc.colors.bars);
            return hover;
        }
        const Window window = m_shown.window;
        if (window.count == 0)
        {
            return hover;
        }
        const double clamped = std::clamp(std::round(position), static_cast<double>(window.first),
                                          static_cast<double>(window.first + window.count - 1));
        hover.valid = true;
        hover.sample = static_cast<u64>(clamped);
        hover.x = xOf(clamped);
        hover.position = pixelOf(clamped);
        for (const Series& series : m_series)
        {
            const UiSeries* held = this->held(series);
            if (!held)
            {
                continue;
            }
            const u64 base = held->appended - held->count;
            if (hover.sample >= base && hover.sample - base < held->count)
            {
                hover.values.push_back({series.name, hover.sample, hover.x,
                                        static_cast<double>(held->at(static_cast<u32>(hover.sample - base)))});
                hover.colors.push_back(series.color);
            }
        }
        return hover;
    }

    void UiChartState::emitOverlay()
    {
        const float ts = textSize();
        const UiChartColors& colors = m_desc.colors;
        const Vec2 ps = m_plotSize;
        const Hover hover = m_hovering ? hoverAt() : Hover{};
        m_hoverValid = hover.valid;
        m_hoverSample = hover.sample;
        m_hoverX = hover.x;
        const std::string loc = locale();
        m_overlay.update(
            [&](UiCanvasPainter& p)
            {
                if (ps.x <= 0.0f || ps.y <= 0.0f)
                {
                    return;
                }
                if (m_banded)
                {
                    const float a = std::clamp(pixelOf(positionOf(m_bandFrom)), 0.0f, ps.x);
                    const float b = std::clamp(pixelOf(positionOf(m_bandTo)), a, ps.x);
                    p.rect({a, 0.0f}, {std::max(b - a, 2.0f), ps.y},
                           (colors.band & 0x00FFFFFFu) | (((colors.band >> 24) / 4u) << 24));
                    p.rect({a, 0.0f}, {std::max(b - a, 2.0f), 3.0f}, colors.band);
                    p.line({snap(a), 0.0f}, {snap(a), ps.y}, 1.0f, colors.band);
                    p.line({snap(b), 0.0f}, {snap(b), ps.y}, 1.0f, colors.band);
                }
                if (m_selecting || m_selected)
                {
                    const float a = std::clamp(pixelOf(std::min(m_selectionFrom, m_selectionTo)), 0.0f, ps.x);
                    const float b = std::clamp(pixelOf(std::max(m_selectionFrom, m_selectionTo)), 0.0f, ps.x);
                    p.rect({a, 0.0f}, {b - a, ps.y}, colors.selection);
                    p.line({snap(a), 0.0f}, {snap(a), ps.y}, 1.0f, colors.crosshair);
                    p.line({snap(b), 0.0f}, {snap(b), ps.y}, 1.0f, colors.crosshair);
                }
                if (!hover.valid)
                {
                    return;
                }
                const auto px = static_cast<float>(std::clamp<double>(hover.position, 0.0, ps.x));
                const float lineX = snap(px);
                p.line({lineX, 0.0f}, {lineX, ps.y}, 1.0f, colors.crosshair);
                const double span = m_shown.y.max - m_shown.y.min;
                if (!histogram())
                {
                    for (size_t i = 0; i < hover.values.size(); ++i)
                    {
                        const double t = std::clamp((hover.values[i].value - m_shown.y.min) / span, 0.0, 1.0);
                        const auto y = static_cast<float>((1.0 - t) * ps.y);
                        p.rect({px - 3.0f, y - 3.0f}, {6.0f, 6.0f}, hover.colors[i], 3.0f);
                    }
                }
                std::vector<std::string> lines;
                lines.push_back(histogram() ? uiChartFormat(loc, hover.binLow, 0.0) + " - " +
                                        uiChartFormat(loc, hover.binHigh, 0.0)
                                            : "x: " + uiChartFormat(loc, hover.x, 0.0));
                u32 chars = codepoints(lines.front());
                for (const UiChartHoverValue& value : hover.values)
                {
                    lines.push_back(value.series + ": " + uiChartFormat(loc, value.value, 0.0));
                    chars = std::max(chars, codepoints(lines.back()) + 3);
                }
                const float lineHeight = ts + 4.0f;
                const float width = static_cast<float>(chars) * TEXT_WIDTH * ts + 16.0f;
                const float height = static_cast<float>(lines.size()) * lineHeight + 6.0f;
                float boxX = px + 10.0f;
                if (boxX + width > ps.x)
                {
                    boxX = px - 10.0f - width;
                }
                boxX = std::max(boxX, 0.0f);
                p.rect({boxX, 4.0f}, {width, height}, colors.readoutBackground, 3.0f);
                float y = 7.0f;
                p.text({boxX + 6.0f, y}, {width - 12.0f, lineHeight}, lines[0], colors.text, UiCanvasAlign::Left, ts);
                for (size_t i = 1; i < lines.size(); ++i)
                {
                    y += lineHeight;
                    p.rect({boxX + 6.0f, y + (lineHeight - 6.0f) * 0.5f}, {6.0f, 6.0f}, hover.colors[i - 1], 3.0f);
                    p.text({boxX + 16.0f, y}, {width - 22.0f, lineHeight}, lines[i], colors.text, UiCanvasAlign::Left,
                           ts);
                }
            });
    }

    void UiChartState::syncComponent()
    {
        UiChartComponent c;
        c.seriesCount = static_cast<u32>(m_series.size());
        c.histogram = histogram();
        c.xRelative = m_shown.relative;
        c.yAuto = m_yAuto;
        c.aligned = m_shown.aligned;
        c.xMin = m_shown.labelLow;
        c.xMax = m_shown.labelHigh;
        c.yMin = m_shown.y.min;
        c.yMax = m_shown.y.max;
        c.windowFirst = m_shown.window.first;
        c.windowCount = m_shown.window.count;
        c.hovering = m_hovering && m_hoverValid;
        c.hoverSample = m_hoverSample;
        c.hoverX = c.hovering ? m_hoverX : 0.0;
        c.selecting = m_selecting;
        c.selected = m_selected;
        if (m_selecting || m_selected)
        {
            const UiChartSelection selection = selectionOf(m_selectionFrom, m_selectionTo);
            c.selectionMin = selection.xMin;
            c.selectionMax = selection.xMax;
            c.selectionFirst = selection.first;
            c.selectionLast = selection.last;
        }
        c.banded = m_banded;
        c.bandMin = m_banded ? m_bandFrom : 0.0;
        c.bandMax = m_banded ? m_bandTo : 0.0;
        if (sameComponent(c, m_component))
        {
            return;
        }
        m_component = c;
        m_scope->world().getComponent<UiChartComponent>(m_root) = c;
    }

    // -- UiChartState: interaction --------------------------------------------------------------------------------

    UiChartSelection UiChartState::selectionOf(const double from, const double to) const
    {
        UiChartSelection selection;
        selection.active = true;
        const double low = std::min(from, to);
        const double high = std::max(from, to);
        selection.xMin = xOf(low);
        selection.xMax = xOf(high);
        if (histogram())
        {
            if (!m_bins.empty())
            {
                const double span = m_binMax - m_binMin;
                const auto bins = static_cast<double>(m_bins.size());
                const auto binOf = [&](const double v)
                { return static_cast<u64>(std::clamp(std::floor((v - m_binMin) / span * bins), 0.0, bins - 1.0)); };
                selection.first = binOf(low);
                selection.last = binOf(high);
            }
            return selection;
        }
        const Window window = m_shown.window;
        if (window.count != 0)
        {
            const auto sampleOf = [&](const double v)
            {
                return static_cast<u64>(std::clamp(std::round(v), static_cast<double>(window.first),
                                                   static_cast<double>(window.first + window.count - 1)));
            };
            selection.first = sampleOf(low);
            selection.last = sampleOf(high);
        }
        return selection;
    }

    void UiChartState::setHoverPixel(const float pixel)
    {
        const bool was = m_hovering;
        const u64 sample = m_hoverSample;
        const bool valid = m_hoverValid;
        m_hovering = true;
        m_hoverPixel = pixel;
        const Hover hover = hoverAt();
        if (was && hover.valid == valid && (!valid || hover.sample == sample) && !m_selecting)
        {
            return; // the same sample: the crosshair and readout stand
        }
        emitOverlay();
        syncComponent();
    }

    void UiChartState::hover(const double x)
    {
        if (!alive() || !m_hasShown || m_plotSize.x <= 0.0f || !std::isfinite(x))
        {
            return;
        }
        m_hovering = false; // forces the overlay to repaint
        setHoverPixel(std::clamp(pixelOf(positionOf(x)), 0.0f, m_plotSize.x));
    }

    void UiChartState::clearHover()
    {
        if (!alive() || !m_hovering)
        {
            return;
        }
        m_hovering = false;
        m_hoverValid = false;
        emitOverlay();
        syncComponent();
    }

    void UiChartState::finishSelection(const double from, const double to)
    {
        m_selectionFrom = std::min(from, to);
        m_selectionTo = std::max(from, to);
        m_selecting = false;
        m_selected = true;
        emitOverlay();
        syncComponent();
        const UiChartSelection selection = selectionOf(m_selectionFrom, m_selectionTo);
        UiChartSelectEvent event;
        event.element = m_root;
        event.xMin = selection.xMin;
        event.xMax = selection.xMax;
        event.first = selection.first;
        event.last = selection.last;
        // copied before anything runs: the event is queued (not dispatched here), and the callback may replace
        // itself or destroy the chart, so nothing after the call uses this
        const auto callback = onSelect;
        m_scope->world().fireEvent(event);
        if (callback)
        {
            callback(selection);
        }
    }

    bool UiChartState::select(const double xMin, const double xMax)
    {
        if (!alive() || !m_hasShown || m_plotSize.x <= 0.0f || !std::isfinite(xMin) || !std::isfinite(xMax) ||
            (!histogram() && m_shown.window.count == 0))
        {
            return false;
        }
        const auto inDomain = [&](const double x)
        {
            return std::clamp(positionOf(x), std::min(m_shown.domainLow, m_shown.domainHigh),
                              std::max(m_shown.domainLow, m_shown.domainHigh));
        };
        finishSelection(inDomain(xMin), inDomain(xMax));
        return true;
    }

    void UiChartState::clearSelection()
    {
        if (!alive() || !(m_selecting || m_selected))
        {
            return;
        }
        m_selecting = m_selected = m_anchored = false;
        emitOverlay();
        syncComponent();
    }

    bool UiChartState::setBand(const double xMin, const double xMax)
    {
        if (!alive() || !m_hasShown || m_plotSize.x <= 0.0f || !std::isfinite(xMin) || !std::isfinite(xMax))
        {
            return false;
        }
        const double from = std::min(xMin, xMax), to = std::max(xMin, xMax);
        if (m_banded && from == m_bandFrom && to == m_bandTo)
        {
            return true;
        }
        m_banded = true;
        m_bandFrom = from;
        m_bandTo = to;
        emitOverlay();
        syncComponent();
        return true;
    }

    void UiChartState::clearBand()
    {
        if (!alive() || !m_banded)
        {
            return;
        }
        m_banded = false;
        emitOverlay();
        syncComponent();
    }

    void UiChartState::pointer(const UiCanvasPointer& event)
    {
        if (!alive() || !m_hasShown)
        {
            return;
        }
        const float px = event.local.x - m_plotPosition.x;
        const float py = event.local.y - m_plotPosition.y;
        const bool inPlot = m_plotSize.x > 0.0f && px >= 0.0f && px <= m_plotSize.x && py >= 0.0f && py <= m_plotSize.y;
        const float clamped = std::clamp(px, 0.0f, std::max(m_plotSize.x, 0.0f));
        switch (event.kind)
        {
        case UiCanvasPointerKind::Enter:
        case UiCanvasPointerKind::Move:
            if (inPlot)
            {
                setHoverPixel(px);
            }
            else
            {
                clearHover();
            }
            break;
        case UiCanvasPointerKind::Leave:
            if (!m_selecting)
            {
                clearHover();
            }
            break;
        case UiCanvasPointerKind::Down:
            m_anchored = event.button == 0 && inPlot;
            if (m_anchored)
            {
                m_anchor = positionAt(px);
            }
            break;
        case UiCanvasPointerKind::DragStart:
        case UiCanvasPointerKind::DragMove:
            if (m_anchored)
            {
                m_selecting = true;
                m_selected = false;
                m_selectionFrom = m_anchor;
                m_selectionTo = positionAt(clamped);
                m_hovering = true;
                m_hoverPixel = clamped;
                emitOverlay();
                syncComponent();
            }
            break;
        case UiCanvasPointerKind::DragEnd:
            if (m_anchored && m_selecting)
            {
                m_anchored = false;
                const double to = positionAt(clamped);
                if (std::abs(pixelOf(to) - pixelOf(m_anchor)) >= DRAG_MIN)
                {
                    finishSelection(m_anchor, to); // may destroy the chart
                }
                else
                {
                    m_selecting = false;
                    emitOverlay();
                    syncComponent();
                }
            }
            break;
        case UiCanvasPointerKind::Click:
            m_anchored = false;
            if (m_selected)
            {
                clearSelection();
            }
            if (inPlot && event.button == 0)
            {
                fireClick(); // may destroy the chart
            }
            break;
        case UiCanvasPointerKind::Up:
            m_anchored = false;
            break;
        case UiCanvasPointerKind::Wheel:
            break;
        }
    }

    void UiChartState::fireClick()
    {
        if (!onPlotClick || !m_hovering)
        {
            return;
        }
        const Hover hover = hoverAt();
        if (!hover.valid)
        {
            return;
        }
        const auto callback = onPlotClick; // the callback may replace itself or destroy the chart
        callback(UiChartClick{hover.sample, hover.x});
    }

    std::string UiChartState::setFromText(const std::string_view text)
    {
        constexpr std::string_view USAGE = "a chart takes 'hover <x>', 'click <x>', 'select <x0> <x1>' or 'clear'";
        if (!alive())
        {
            return "the chart is gone";
        }
        std::vector<std::string_view> words;
        for (size_t begin = 0; begin < text.size();)
        {
            const size_t end = std::min(text.find_first_of(" ,", begin), text.size());
            if (end > begin)
            {
                words.push_back(text.substr(begin, end - begin));
            }
            begin = end + 1;
        }
        const auto number = [](const std::string_view word, double& out)
        {
            const auto [stop, error] = std::from_chars(word.data(), word.data() + word.size(), out);
            return error == std::errc{} && stop == word.data() + word.size() && std::isfinite(out);
        };
        if (words.empty())
        {
            return std::string(USAGE);
        }
        if (!m_hasShown || m_plotSize.x <= 0.0f)
        {
            return "the chart has no plot yet (not laid out, or too small)";
        }
        double a = 0.0;
        double b = 0.0;
        if (words[0] == "clear" && words.size() == 1)
        {
            clearHover();
            clearSelection();
            return {};
        }
        if (words[0] == "hover" && words.size() == 2 && number(words[1], a))
        {
            hover(a);
            return {};
        }
        if (words[0] == "click" && words.size() == 2 && number(words[1], a))
        {
            hover(a);
            fireClick(); // may destroy the chart
            return {};
        }
        if (words[0] == "select" && words.size() == 3 && number(words[1], a) && number(words[2], b))
        {
            select(a, b);
            return {};
        }
        return std::string(USAGE);
    }

    void UiChartState::resized()
    {
        if (!alive())
        {
            return;
        }
        // the authored size, or what a layout container gave it at the last resolve (in UI pixels either way)
        const Vec2 size = m_scope->hierarchy().layoutSizeOf(m_root);
        const bool sized = std::isfinite(size.x) && std::isfinite(size.y) && size.x > 0.0f && size.y > 0.0f;
        if (sized == m_sized && (!sized || size == m_size))
        {
            return;
        }
        m_sized = sized;
        m_size = sized ? size : Vec2{};
        if (sized)
        {
            measure();
            layout(evaluate(), true);
        }
    }

    void UiChartState::checkLocale()
    {
        const Localization* found = localization();
        const u64 revision = found ? found->revision() : 0;
        const float textScale = m_scope->hierarchy().textScale();
        if (revision == m_localeRevision && textScale == m_textScale)
        {
            return;
        }
        const bool scaled = textScale != m_textScale;
        m_localeRevision = revision;
        m_textScale = textScale;
        if (m_sized && m_hasShown && alive())
        {
            if (scaled)
            {
                measure(); // the margins and the legend row follow the text size
            }
            layout(evaluate(), false);
        }
    }

    void UiChartState::readout(UiChartReadout& out) const
    {
        out = {};
        if (!alive())
        {
            return;
        }
        out.histogram = histogram();
        out.xRelative = m_shown.relative;
        out.aligned = m_shown.aligned;
        out.plotPosition = m_plotPosition;
        out.plotSize = m_plotSize;
        out.xMin = m_shown.labelLow;
        out.xMax = m_shown.labelHigh;
        out.yMin = m_shown.y.min;
        out.yMax = m_shown.y.max;
        out.windowFirst = m_shown.window.first;
        out.windowCount = m_shown.window.count;
        for (const Tick& tick : m_xTicks)
        {
            out.xTicks.push_back({tick.value, tick.label});
        }
        for (const Tick& tick : m_yTicks)
        {
            out.yTicks.push_back({tick.value, tick.label});
        }
        out.outOfRange = m_outOfRange;
        if (m_hovering)
        {
            const Hover hover = hoverAt();
            out.hovering = hover.valid;
            out.hover = hover.values;
        }
        if (m_selecting || m_selected)
        {
            out.selection = selectionOf(m_selectionFrom, m_selectionTo);
        }
        if (m_banded)
        {
            out.band = selectionOf(positionOf(m_bandFrom), positionOf(m_bandTo));
        }
    }

    // -- Chart handle ---------------------------------------------------------------------------------------------

    namespace
    {
        UiChartState* stateOf(const Widget& widget)
        {
            if (!widget.isValid())
            {
                SASSERTM(false, "Stale UI widget handle: the owning scope died or the element was destroyed.");
                return nullptr;
            }
            return widget.scope()->chartState(widget.entity());
        }
    } // namespace

    u32 Chart::addSeries(const UiChartSeriesDesc& desc)
    {
        UiChartState* state = stateOf(*this);
        return state ? state->addSeries(desc) : UI_CHART_NO_SERIES;
    }

    bool Chart::setSeriesName(const u32 series, const std::string_view name)
    {
        UiChartState* state = stateOf(*this);
        return state && state->setSeriesName(series, name);
    }

    u32 Chart::seriesCount() const
    {
        const UiChartState* state = stateOf(*this);
        return state ? state->seriesCount() : 0;
    }

    bool Chart::append(const u32 series, const std::span<const float> samples)
    {
        UiChartState* state = stateOf(*this);
        return state && state->append(series, samples);
    }

    void Chart::clear()
    {
        if (UiChartState* state = stateOf(*this))
        {
            state->clear();
        }
    }

    void Chart::follow(const u32 count)
    {
        if (UiChartState* state = stateOf(*this))
        {
            state->follow(count);
        }
    }

    void Chart::setWindow(const u64 first, const u32 count)
    {
        if (UiChartState* state = stateOf(*this))
        {
            state->setWindow(first, count);
        }
    }

    void Chart::setYRange(const double min, const double max)
    {
        if (UiChartState* state = stateOf(*this))
        {
            state->setYRange(min, max);
        }
    }

    void Chart::setYAuto()
    {
        if (UiChartState* state = stateOf(*this))
        {
            state->setYAuto();
        }
    }

    void Chart::addValues(const std::span<const float> values)
    {
        if (UiChartState* state = stateOf(*this))
        {
            state->addValues(values);
        }
    }

    void Chart::setHistogram(const UiChartHistogramDesc& desc)
    {
        if (UiChartState* state = stateOf(*this))
        {
            state->setHistogram(desc);
        }
    }

    void Chart::hover(const double x)
    {
        if (UiChartState* state = stateOf(*this))
        {
            state->hover(x);
        }
    }

    void Chart::clearHover()
    {
        if (UiChartState* state = stateOf(*this))
        {
            state->clearHover();
        }
    }

    bool Chart::select(const double xMin, const double xMax)
    {
        UiChartState* state = stateOf(*this);
        return state && state->select(xMin, xMax);
    }

    void Chart::clearSelection()
    {
        if (UiChartState* state = stateOf(*this))
        {
            state->clearSelection();
        }
    }

    bool Chart::setBand(const double xMin, const double xMax)
    {
        UiChartState* state = stateOf(*this);
        return state && state->setBand(xMin, xMax);
    }

    void Chart::clearBand()
    {
        if (UiChartState* state = stateOf(*this))
        {
            state->clearBand();
        }
    }

    void Chart::onSelect(std::function<void(const UiChartSelection&)> fn)
    {
        if (UiChartState* state = stateOf(*this))
        {
            state->onSelect = std::move(fn);
        }
    }

    void Chart::onPlotClick(std::function<void(const UiChartClick&)> fn)
    {
        if (UiChartState* state = stateOf(*this))
        {
            state->onPlotClick = std::move(fn);
        }
    }

    UiChartReadout Chart::readout() const
    {
        UiChartReadout out;
        if (const UiChartState* state = stateOf(*this))
        {
            state->readout(out);
        }
        return out;
    }

    Entity Chart::plotEntity() const
    {
        const UiChartState* state = stateOf(*this);
        return state ? state->plot.entity() : Entity::undefined();
    }

    const UiSeries* Chart::series(const u32 index) const
    {
        const UiChartState* state = stateOf(*this);
        return state ? state->seriesAt(index) : nullptr;
    }

    // -- UiScope --------------------------------------------------------------------------------------------------

    UiChartState* UiScope::chartState(const Entity element) const
    {
        if (m_charts.empty())
        {
            return nullptr;
        }
        const auto it = m_charts.find(element.id());
        return it == m_charts.end() ? nullptr : it->second.get();
    }

    void UiScope::syncCharts()
    {
        for (const auto& chart : m_charts | std::views::values)
        {
            chart->checkLocale();
        }
    }

    Chart UiScope::createChart(const InternedString id, const UiChartDesc& desc)
    {
        const std::string name(id.toView());
        const Entity root = createCanvasOfKind(id, UiWidgetKind::Chart, {desc.rect, 16384}).entity();
        m_world->addComponent<UiChartComponent>(root, UiChartComponent{});
        UiRectDesc childRect;
        childRect.pivot = Vec2{0.0f, 0.0f};
        const Entity plot =
            createCanvasOfKind(InternedString(name + ".plot"), UiWidgetKind::Canvas,
                               {childRect, UI_CHART_MAX_SERIES * UI_SERIES_MAX_COLUMNS + UI_CHART_MAX_BINS + 16})
                .entity();
        const Entity overlay =
            createCanvasOfKind(InternedString(name + ".overlay"), UiWidgetKind::Canvas, {childRect, 512}).entity();
        auto& state = m_charts[root.id()] = std::make_unique<UiChartState>(*this, root, plot, overlay, desc);
        state->build();
        return Chart(this, root, m_generation);
    }
} // namespace spite
