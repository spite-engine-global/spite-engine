#pragma once

#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui/UiChart.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class UiScope;
    class Localization;

    // The runtime half of a chart, owned by its scope and keyed by the root element. The root draws axes,
    // gridlines, labels and the legend, `plot` the series or histogram bars, `overlay` the hover and the selection;
    // see Chart in UiWidgets.hpp for the model. Everything a chart does is driven by calls on this class, so the
    // pointer, the Chart handle and uiActivate share one path.
    class UiChartState
    {
    public:
        UiChartState(UiScope& owner, Entity rootElement, Entity plotElement, Entity overlayElement,
                     const UiChartDesc& chartDesc);

        void build();

        u32 addSeries(const UiChartSeriesDesc& seriesDesc);
        bool setSeriesName(u32 index, std::string_view name);
        bool append(u32 index, std::span<const float> samples);
        void clear();
        void follow(u32 count);
        void setWindow(u64 first, u32 count);
        void setYRange(double min, double max);
        void setYAuto();
        void addValues(std::span<const float> values);
        void setHistogram(const UiChartHistogramDesc& histogramDesc);
        void hover(double x);
        void clearHover();
        bool select(double xMin, double xMax);
        void clearSelection();
        bool setBand(double xMin, double xMax);
        void clearBand();
        // The click callback with the sample under the hover; may destroy the chart.
        void fireClick();
        // uiActivate setValue: why the text is not "hover <x>", "select <x0> <x1>" or "clear", or empty.
        [[nodiscard]] std::string setFromText(std::string_view text);

        // The root's size changed (set, laid out or rescaled).
        void resized();
        // The Localization's revision moved: labels are formatted again.
        void checkLocale();
        void readout(UiChartReadout& out) const;
        [[nodiscard]] u32 seriesCount() const { return static_cast<u32>(m_series.size()); }
        [[nodiscard]] const UiSeries* seriesAt(u32 index) const;

        std::function<void(const UiChartSelection&)> onSelect;
        std::function<void(const UiChartClick&)> onPlotClick;
        Canvas plot;

    private:
        // A run of absolute samples a series shows.
        struct Window
        {
            u64 first = 0;
            u32 count = 0;
            bool operator==(const Window&) const = default;
        };

        // Min and max of fixed blocks of absolute sample numbers, so a window's extremes cost two partial blocks and
        // one entry per whole block rather than a scan.
        struct Blocks
        {
            static constexpr u64 SIZE = 512;
            std::deque<std::pair<float, float>> blocks;
            u64 first = 0; // the block number of blocks.front()

            void clear();
            void add(u64 sample, float value);
            void dropBefore(u64 base);
            [[nodiscard]] std::pair<float, float> query(const UiSeries& series, u64 from, u64 to) const;
        };

        struct Series
        {
            std::string name;
            u32 color = 0;
            UiSeriesStyle style = UiSeriesStyle::Line;
            float thickness = 1.5f;
            UiSeriesId id;
            Blocks blocks;
            Window window; // the window the extents below describe
            bool extentsValid = false;
            float min = 0.0f;
            float max = 0.0f;
        };

        struct Tick
        {
            double value = 0.0;
            std::string label;
            float pixel = 0.0f; // along the plot, from its left or top edge
            double step = 0.0; // the label is a function of the locale, the value and the step
        };

        // The plot's domain in position units: absolute sample numbers (a histogram: values).
        struct Evaluation
        {
            Window window;
            double domainLow = 0.0;
            double domainHigh = 1.0;
            double labelLow = 0.0;
            double labelHigh = 1.0;
            bool relative = false;
            UiChartRange y;
            bool aligned = true;
            // what the axes and gridlines draw; the window and its domain in sample numbers are not part of it (a
            // following window of N samples that is full slides without changing a label)
            [[nodiscard]] bool sameAxes(const Evaluation& o) const
            {
                return labelLow == o.labelLow && labelHigh == o.labelHigh && relative == o.relative && y == o.y;
            }
        };

        struct Hover
        {
            bool valid = false;
            u64 sample = 0; // absolute sample, or the bin
            double x = 0.0;
            double position = 0.0; // pixel along the plot of the sample (or bin centre)
            std::vector<UiChartHoverValue> values;
            std::vector<u32> colors; // of the series the values belong to
            double binLow = 0.0;
            double binHigh = 0.0;
        };

        [[nodiscard]] bool alive() const;
        [[nodiscard]] bool histogram() const { return m_desc.kind == UiChartKind::Histogram; }
        [[nodiscard]] const UiSeries* held(const Series& series) const;
        [[nodiscard]] Window windowOf(u64 base, u32 count) const;
        [[nodiscard]] Window windowOfSeries(const Series& series) const;
        [[nodiscard]] const Series* reference() const;
        void ensureExtents(Series& series);
        [[nodiscard]] Evaluation evaluate();
        [[nodiscard]] double xOf(double position) const;
        [[nodiscard]] double positionOf(double x) const;
        [[nodiscard]] double positionAt(float pixel) const;
        [[nodiscard]] float pixelOf(double position) const;
        [[nodiscard]] std::string locale() const;
        [[nodiscard]] float textSize() const; // the description's size at the hierarchy's text scale
        [[nodiscard]] const Localization* localization() const;

        void refresh(bool plotChanged);
        void layout(const Evaluation& evaluation, bool plotChanged);
        void measure();
        void emitAxes();
        void emitPlot();
        void emitOverlay();
        void setRanges();
        void syncComponent();
        void pointer(const UiCanvasPointer& event);
        void setHoverPixel(float pixel);
        void finishSelection(double from, double to);
        [[nodiscard]] Hover hoverAt() const;
        [[nodiscard]] UiChartSelection selectionOf(double from, double to) const;
        [[nodiscard]] bool interacting() const { return m_hovering || m_selecting || m_selected || m_banded; }

        UiScope* m_scope = nullptr;
        Entity m_root = Entity::undefined();
        Entity m_plotEntity = Entity::undefined();
        Entity m_overlayEntity = Entity::undefined();
        Canvas m_rootCanvas;
        Canvas m_overlay;
        UiChartDesc m_desc;

        std::vector<Series> m_series;
        bool m_following = true;
        u32 m_followCount = 0;
        u64 m_fixedFirst = 0;
        u32 m_fixedCount = 0;
        bool m_yAuto = true;
        UiChartRange m_yFixed;

        std::vector<u64> m_bins;
        double m_binMin = 0.0;
        double m_binMax = 1.0;
        bool m_binRangeSet = false;
        u64 m_binMaxCount = 0;
        u64 m_outOfRange = 0;

        // layout, in unscaled UI pixels from the root's top-left
        bool m_sized = false;
        Vec2 m_size{};
        Vec2 m_plotPosition{};
        Vec2 m_plotSize{};
        u32 m_yTarget = 5;
        float m_top = 0.0f;
        float m_bottom = 0.0f;
        float m_right = 0.0f;
        Evaluation m_shown;
        bool m_hasShown = false;
        std::vector<Tick> m_xTicks;
        std::vector<Tick> m_yTicks;
        std::vector<Tick> m_spareTicks; // the ticks of the layout before: a label whose value and step stand is moved
        std::string m_labelLocale; // the locale the held labels are formatted in
        u64 m_localeRevision = 0;
        float m_textScale = 1.0f; // the hierarchy's text scale the held layout was made at
        mutable const Localization* m_localization = nullptr;

        bool m_hovering = false;
        float m_hoverPixel = 0.0f;
        bool m_hoverValid = false; // the hover found a sample
        u64 m_hoverSample = 0;
        double m_hoverX = 0.0;
        bool m_selecting = false;
        bool m_selected = false;
        bool m_anchored = false; // a primary press inside the plot began a possible drag
        double m_anchor = 0.0; // positions
        double m_selectionFrom = 0.0;
        double m_selectionTo = 0.0;
        bool m_banded = false;
        double m_bandFrom = 0.0; // x units
        double m_bandTo = 0.0;
        UiChartComponent m_component;
    };
} // namespace spite
