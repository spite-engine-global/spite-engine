#include <algorithm>
#include <cmath>
#include <format>

#include "EditorTimelineImpl.hpp"
#include "ProfilerViews.hpp"
#include "ui/UiChart.hpp"
#include <spite/profiling/Zone.hpp>
#include "ui/UiRichText.hpp"
#include "ui/UiTextMeasurer.hpp"

namespace spite
{
    using namespace panelui;
    using namespace timeline;

    namespace
    {
        constexpr float TEXT_SIZE = 10.0f;
        constexpr float EMPTY_MARGIN = 8.0f; // the note of a canvas with nothing to draw: its side margin
        constexpr float EMPTY_LINE = 14.0f; // and its line height
        constexpr float FALLBACK_CHAR_WIDTH = 5.6f; // an upper bound of a 10 px glyph's advance, without a measurer
        constexpr std::string_view PROBE = "0123456789 ms";

        bool continuation(const char byte) { return (static_cast<unsigned char>(byte) & 0xC0) == 0x80; }

        // `text` cut to at most `limit` bytes at a character boundary: the texts of the info and statistics lines
        std::string clipText(const std::string_view text, const sizet limit)
        {
            if (text.size() <= limit)
            {
                return std::string(text);
            }
            sizet end = limit;
            while (end > 0 && continuation(text[end]))
            {
                --end;
            }
            return std::string(text.substr(0, end));
        }

        bool outline(UiCanvasPainter& p, const float x, const float y, const float w, const float h, const u32 color)
        {
            const float t = 1.5f;
            return p.rect({x, y}, {w, t}, color) && p.rect({x, y + h - t}, {w, t}, color) &&
                p.rect({x, y}, {t, h}, color) && p.rect({x + w - t, y}, {t, h}, color);
        }

        double roundedMs(const double ns) { return std::round(ns / 1e6 * 1000.0) / 1000.0; }

        void writeStats(IFormatWriter& out, const CaptureZoneStats& stats)
        {
            out.writeU64("count", stats.count);
            out.writeU64("totalNs", stats.totalNs);
            out.writeF64("meanNs", stats.meanNs);
            out.writeF64("p50Ns", stats.p50Ns);
            out.writeF64("p99Ns", stats.p99Ns);
            out.writeU64("minNs", stats.minNs);
            out.writeU64("maxNs", stats.maxNs);
            out.writeF64("meanMs", roundedMs(stats.meanNs));
            out.writeF64("p50Ms", roundedMs(stats.p50Ns));
            out.writeF64("p99Ms", roundedMs(stats.p99Ns));
            out.writeF64("minMs", roundedMs(static_cast<double>(stats.minNs)));
            out.writeF64("maxMs", roundedMs(static_cast<double>(stats.maxNs)));
            out.beginArray("histogram", static_cast<u32>(stats.histogram.size()));
            for (const u64 bin : stats.histogram)
            {
                out.writeU64({}, bin);
            }
            out.endArray();
        }

        void writeDetail(IFormatWriter& out, const CaptureZoneDetail& detail)
        {
            const CaptureZone& zone = detail.zone;
            out.writeU32("lane", zone.thread);
            out.writeString("laneName", detail.laneName);
            out.writeU64("index", zone.index);
            out.writeString("name", zone.name);
            out.writeU64("startNs", static_cast<u64>(std::max<std::int64_t>(0, zone.startNs)));
            out.writeU64("endNs", static_cast<u64>(std::max<std::int64_t>(0, zone.endNs)));
            out.writeU64("durationNs", static_cast<u64>(std::max<std::int64_t>(0, zone.endNs - zone.startNs)));
            out.writeU64("selfNs", static_cast<u64>(std::max<std::int64_t>(0, detail.selfNs)));
            out.writeBool("selfExact", detail.selfExact);
            out.writeU64("children", detail.children);
            out.writeU32("depth", zone.depth);
            out.writeBool("hasFrame", detail.hasFrame);
            out.writeU64("frame", detail.frame);
        }
    } // namespace

    // -- text metrics ------------------------------------------------------------------------------------------------

    TlColors TlColors::read(const UiScope& scope)
    {
        TlColors colors;
        colors.header = canvasColor(scope, "canvas.timeline.header");
        colors.body = canvasColor(scope, "canvas.timeline.body");
        colors.grid = canvasColor(scope, "canvas.grid");
        colors.text = canvasColor(scope, "canvas.text");
        colors.dimText = canvasColor(scope, "canvas.text.dim");
        colors.narrow = canvasColor(scope, "canvas.timeline.span.narrow");
        colors.merged = canvasColor(scope, "canvas.timeline.span.merged");
        const u32 series = canvasColor(scope, "canvas.series.a");
        colors.frameA = withAlpha(series, 150);
        colors.frameB = withAlpha(series, 70);
        colors.bar = series;
        colors.outline = canvasColor(scope, "canvas.outline");
        colors.selected = canvasColor(scope, "canvas.selected");
        colors.crosshair = canvasColor(scope, "canvas.crosshair");
        colors.tipFill = canvasColor(scope, "canvas.tip.fill");
        colors.onSpan = canvasColor(scope, "canvas.on-data");
        for (sizet slot = 0; slot < TL_OVERFLOW_NAME; ++slot)
        {
            colors.names[slot] = canvasColor(scope, std::format("canvas.palette.{}", slot));
        }
        colors.names[TL_OVERFLOW_NAME] = colors.dimText;
        return colors;
    }

    void ProfilerTimeline::Impl::restyle()
    {
        colors = TlColors::read(*host.scope);
        dirtyMain = dirtyOverlay = dirtyHistogram = dirtyFrames = true;
    }

    float ProfilerTimeline::Impl::textWidth(const Entity element, const std::string_view text) const
    {
        const UiTextMeasurer* measurer = host.scope->hierarchy().textMeasurer();
        return measurer ? measurer->measureTextSized(element, text, TEXT_SIZE).x
                        : static_cast<float>(text.size()) * FALLBACK_CHAR_WIDTH;
    }

    std::string ProfilerTimeline::Impl::fitText(const Entity element, const std::string_view text,
                                                const float room) const
    {
        if (textWidth(element, text) <= room)
        {
            return std::string(text);
        }
        sizet low = 0, high = text.size(); // the prefix of `low` bytes fits, the one of `high` does not
        for (;;)
        {
            sizet mid = low + (high - low) / 2;
            while (mid > low && continuation(text[mid]))
            {
                --mid;
            }
            if (mid == low)
            {
                mid = low + 1;
                while (mid < high && continuation(text[mid]))
                {
                    ++mid;
                }
                if (mid >= high)
                {
                    break;
                }
            }
            (textWidth(element, text.substr(0, mid)) <= room ? low : high) = mid;
        }
        return std::string(text.substr(0, low));
    }

    std::vector<std::string> ProfilerTimeline::Impl::wrapText(const Entity element, const std::string_view text,
                                                              const float room, const u32 maxLines) const
    {
        std::vector<std::string> lines;
        sizet at = 0;
        while (lines.size() < maxLines)
        {
            while (at < text.size() && text[at] == ' ')
            {
                ++at;
            }
            if (at >= text.size())
            {
                break;
            }
            const std::string_view rest = text.substr(at);
            if (textWidth(element, rest) <= room)
            {
                lines.emplace_back(rest);
                break;
            }
            if (lines.size() + 1 == maxLines)
            {
                const std::string cut = fitText(element, rest, room - textWidth(element, "..."));
                lines.push_back(cut + "...");
                break;
            }
            sizet end = 0; // the longest run of whole words that fits
            for (sizet word = 0; word < rest.size();)
            {
                const sizet next = std::min(rest.find(' ', word), rest.size());
                if (textWidth(element, rest.substr(0, next)) > room)
                {
                    break;
                }
                end = next;
                word = next + 1;
            }
            std::string line(end != 0 ? rest.substr(0, end) : std::string_view(fitText(element, rest, room)));
            if (line.empty())
            {
                break;
            }
            at += line.size();
            lines.push_back(std::move(line));
        }
        return lines;
    }

    void ProfilerTimeline::Impl::followTextMetrics()
    {
        const u64 generation = host.scope->hierarchy().layoutGeneration();
        if (generation == seenLayout)
        {
            return;
        }
        seenLayout = generation;
        const float probe = textWidth(canvas.entity(), PROBE) + textWidth(overlay.entity(), PROBE);
        if (probe != probeWidth)
        {
            probeWidth = probe;
            dirtyMain = dirtyOverlay = true;
        }
    }

    // -- numbers -----------------------------------------------------------------------------------------------------

    std::string ProfilerTimeline::Impl::say(const std::string_view key,
                                            const std::initializer_list<MessageArg> args) const
    {
        return profilerText(*host.scope, key, args);
    }

    std::string ProfilerTimeline::Impl::whole(const u64 value) const
    {
        return uiChartFormat(locale, static_cast<double>(value), 1.0);
    }

    std::string ProfilerTimeline::Impl::ms3(const double ns) const { return uiChartFormat(locale, roundedMs(ns), 0.0); }

    std::string ProfilerTimeline::Impl::fmtNs(const std::int64_t ns) const
    {
        const double magnitude = std::abs(static_cast<double>(ns));
        if (magnitude < 1e3)
        {
            return whole(static_cast<u64>(std::max<std::int64_t>(0, ns))) + " ns";
        }
        const auto scaled = [&](const double divisor, const char* unit)
        {
            return uiChartFormat(locale, std::round(static_cast<double>(ns) / divisor * 1000.0) / 1000.0, 0.0) + " " +
                unit;
        };
        return magnitude < 1e6 ? scaled(1e3, "us") : magnitude < 1e9 ? scaled(1e6, "ms") : scaled(1e9, "s");
    }

    // -- the tooltip -------------------------------------------------------------------------------------------------

    // Rich text: the one place markup is built, with every zone and thread name escaped (they come from module images
    // and capture files).
    void ProfilerTimeline::Impl::buildTooltip()
    {
        tooltip.clear();
        const auto escaped = [](const std::string_view text, const sizet limit)
        {
            std::string out;
            escapeUiMarkup(clipText(text, limit), out);
            return out;
        };
        if (!hover.has)
        {
            return;
        }
        const TlLane* lane = hover.lane < lanes.size() ? &lanes[hover.lane] : nullptr;
        const std::string laneName = lane ? escaped(lane->info.name, 40) : std::string{};
        const std::string where =
            say("timeline.tip.where", {{"lane", laneName}, {"index", std::to_string(hover.lane)}});
        if (!hover.onSpan)
        {
            tooltip = "[b]" + where + "[/b]\n" + say("timeline.tip.at", {{"time", fmtNs(hover.ns)}});
            return;
        }
        const TlSpan& span = hover.span;
        const std::string zoneName = escaped(names[span.name].text, TIP_NAME_CHARS);
        if (span.count > 1)
        {
            tooltip = "[b]" + say("timeline.zones", {{"count", whole(span.count)}}) + "[/b] " +
                say("timeline.tip.narrow") + "\n" +
                say("timeline.tip.depth", {{"where", where}, {"depth", std::to_string(span.depth)}}) + "\n" +
                say("timeline.tip.total",
                    {{"total", fmtNs(static_cast<std::int64_t>(span.totalNs))}, {"ns", whole(span.totalNs)}}) +
                "\n" +
                say("timeline.tip.longest",
                    {{"longest", fmtNs(static_cast<std::int64_t>(span.longestNs))}, {"name", zoneName}}) +
                "\n" + say("timeline.tip.span", {{"from", fmtNs(span.start)}, {"to", fmtNs(span.end)}});
            return;
        }
        const bool exact = hoverDetail.valid && hoverDetail.detail.zone.thread == hover.lane &&
            hoverDetail.detail.zone.index == span.firstSeq;
        const std::int64_t duration = span.end - span.start;
        tooltip = "[b]" + zoneName + "[/b]\n" +
            say("timeline.tip.depth", {{"where", where}, {"depth", std::to_string(span.depth)}}) + "\n" +
            say("timeline.tip.duration",
                {{"duration", fmtNs(duration)}, {"ns", whole(static_cast<u64>(std::max<std::int64_t>(0, duration)))}}) +
            "\n";
        if (exact)
        {
            const CaptureZoneDetail& detail = hoverDetail.detail;
            tooltip += say("timeline.tip.self",
                           {{"self", detail.selfExact ? fmtNs(detail.selfNs) : say("timeline.na")},
                            {"children", (detail.selfExact ? "" : ">= ") + whole(detail.children)}}) +
                "\n" +
                say("timeline.tip.start",
                    {{"start", fmtNs(span.start)},
                     {"frame",
                      "  " +
                          (detail.hasFrame ? say("timeline.frame", {{"frame", whole(detail.frame)}})
                                           : say("timeline.betweenFrames"))}});
        }
        else
        {
            tooltip += say("timeline.tip.pending") + "\n" +
                say("timeline.tip.start", {{"start", fmtNs(span.start)}, {"frame", std::string()}});
        }
    }

    // -- the canvases ------------------------------------------------------------------------------------------------

    void ProfilerTimeline::Impl::drawMain()
    {
        SPITE_ZONE("ProfilerTimelineDraw");
        dirtyMain = false;
        ++redraws;
        lastSpansDrawn = lastSpansVisited = lastLanesDrawn = lastLabels = 0;
        rulerLabels.clear();
        emptyLines.clear();
        const profview::Window w = window();
        const Vec2 size = canvasSize;
        canvas.update(
            [&](UiCanvasPainter& p)
            {
                if (!w.ok)
                {
                    const std::string message = say(capture == 0             ? "timeline.empty.none"
                                                        : !viewError.empty() ? "timeline.empty.error"
                                                                             : "timeline.empty.loading");
                    // the note wraps at the canvas width (as many lines as the canvas under the ruler holds)
                    const float room = std::max(0.0f, size.x - 2.0f * EMPTY_MARGIN);
                    const u32 fits = static_cast<u32>(std::max(1.0f, (size.y - RULER) / EMPTY_LINE));
                    emptyLines = wrapText(canvas.entity(), message, room, fits);
                    for (u32 i = 0; i < emptyLines.size(); ++i)
                    {
                        p.text({EMPTY_MARGIN, RULER + static_cast<float>(i) * EMPTY_LINE}, {room, EMPTY_LINE},
                               emptyLines[i], colors.dimText, UiCanvasAlign::Left, TEXT_SIZE);
                    }
                    lastPrimitives = p.primitives();
                    return;
                }
                const double plot = plotWidth();
                const double scale = plot / static_cast<double>(w.width());
                const auto xOf = [&](const std::int64_t t)
                { return static_cast<double>(t - static_cast<std::int64_t>(w.start)) * scale; };

                // the ruler: ticks in the unit that fits the window, and the frames as a band under them
                const double width = static_cast<double>(w.width());
                const double unit = width >= 2e9 ? 1e9 : width >= 2e6 ? 1e6 : width >= 2e3 ? 1e3 : 1.0;
                const char* suffix = unit == 1e9 ? " s" : unit == 1e6 ? " ms" : unit == 1e3 ? " us" : " ns";
                p.rect({0.0f, 0.0f}, {size.x, RULER - 2.0f}, colors.header);
                rulerLabels.clear();
                // The densest ticks whose labels, measured as painted, keep RULER_LABEL_GAP between them and stay on
                // the canvas; the coarsest step (a label or two) when none does
                for (u32 target = static_cast<u32>(std::max(2.0, plot / 120.0));; --target)
                {
                    const UiChartTicks ticks =
                        uiChartTicks(static_cast<double>(w.start) / unit, static_cast<double>(w.end) / unit, target);
                    rulerLabels.clear();
                    bool clash = false;
                    float edge = -RULER_LABEL_GAP;
                    for (u32 i = 0; i < ticks.count; ++i)
                    {
                        const double value = ticks.at(i);
                        const double x = xOf(static_cast<std::int64_t>(std::llround(value * unit)));
                        if (x < 0.0 || x >= plot)
                        {
                            continue;
                        }
                        std::string text = uiChartFormat(locale, value, ticks.step) + suffix;
                        const float left = GUTTER + static_cast<float>(x) + 3.0f;
                        const float width = textWidth(canvas.entity(), text);
                        if (left + width > size.x)
                        {
                            continue; // the tick at the edge keeps its mark and loses its label
                        }
                        if (left < edge + RULER_LABEL_GAP)
                        {
                            clash = true;
                            continue;
                        }
                        edge = left + width;
                        rulerLabels.push_back({left, 2.0f, width, 12.0f, std::move(text)});
                    }
                    if (!clash || target <= 1)
                    {
                        for (u32 i = 0; i < ticks.count; ++i)
                        {
                            const double x = xOf(static_cast<std::int64_t>(std::llround(ticks.at(i) * unit)));
                            if (x >= 0.0 && x < plot)
                            {
                                const float px = GUTTER + static_cast<float>(x);
                                p.rect({px, RULER - 8.0f}, {1.0f, 6.0f}, colors.dimText);
                                p.rect({px, RULER}, {1.0f, std::max(0.0f, size.y - RULER)}, colors.grid);
                            }
                        }
                        break;
                    }
                }
                for (const TlLabel& label : rulerLabels)
                {
                    p.text({label.x, label.y}, {label.width, label.height}, label.text, colors.text,
                           UiCanvasAlign::Left, TEXT_SIZE);
                }
                if (!marksTruncated && marks.size() * 6 <= static_cast<sizet>(plot))
                {
                    bool odd = false;
                    for (const auto& [start, end, index] : marks)
                    {
                        odd = !odd;
                        const double x0 = std::max(xOf(start), 0.0), x1 = std::min(xOf(end), plot);
                        if (x1 > x0)
                        {
                            p.rect({GUTTER + static_cast<float>(x0), RULER - 5.0f},
                                   {std::max(1.0f, static_cast<float>(x1 - x0) - 1.0f), 3.0f},
                                   odd ? colors.frameA : colors.frameB);
                        }
                    }
                }

                // the lanes
                const float scroll = viewConst().laneScroll;
                u32 labels = 0;
                for (const TlLane& lane : lanes)
                {
                    if (!laneVisible(lane, 0.0f) || p.dropped() != 0)
                    {
                        continue;
                    }
                    ++lastLanesDrawn;
                    const float y0 = RULER + lane.top - scroll;
                    p.rect({0.0f, y0}, {size.x, LANE_HEADER}, colors.header);
                    const std::string title = lane.info.name.empty()
                        ? say("timeline.thread", {{"id", std::to_string(lane.info.osId)}})
                        : lane.info.name;
                    p.text({4.0f, y0}, {GUTTER - 8.0f, LANE_HEADER}, fitText(canvas.entity(), title, GUTTER - 8.0f),
                           colors.text, UiCanvasAlign::Left, TEXT_SIZE);
                    p.rect({GUTTER, y0 + LANE_HEADER}, {static_cast<float>(plot), lane.height - LANE_HEADER - LANE_GAP},
                           colors.body);
                    if (!lane.loaded)
                    {
                        p.text({GUTTER + 6.0f, y0 + LANE_HEADER}, {120.0f, ROW}, say("timeline.lane.loading"),
                               colors.dimText, UiCanvasAlign::Left, TEXT_SIZE);
                        continue;
                    }
                    for (u32 depth = 0; depth < lane.rows && depth + 1 < lane.depthBegin.size(); ++depth)
                    {
                        const float y = y0 + LANE_HEADER + static_cast<float>(depth) * ROW;
                        const TlSpan* begin = lane.spans.data() + lane.depthBegin[depth];
                        const TlSpan* end = lane.spans.data() + lane.depthBegin[depth + 1];
                        // spans of one depth do not overlap, so their ends rise with their starts
                        const TlSpan* it = std::ranges::partition_point(
                            begin, end, [&](const TlSpan& s) { return s.end <= static_cast<std::int64_t>(w.start); });
                        bool run = false;
                        double runStart = 0.0, runEnd = 0.0;
                        const auto flush = [&]
                        {
                            if (run)
                            {
                                p.rect({GUTTER + static_cast<float>(runStart), y},
                                       {static_cast<float>(runEnd - runStart), ROW - 1.0f}, colors.narrow);
                                ++lastSpansDrawn;
                                run = false;
                            }
                        };
                        for (; it < end && it->start < static_cast<std::int64_t>(w.end); ++it)
                        {
                            ++lastSpansVisited;
                            const double x0 = xOf(it->start), x1 = xOf(it->end);
                            if (x1 - x0 < 1.0)
                            {
                                // under a pixel: merged with its neighbours into one run
                                const double a = std::max(std::floor(x0), 0.0),
                                             b = std::min(std::max(std::ceil(x1), a + 1.0), plot);
                                if (run && a <= runEnd)
                                {
                                    runEnd = std::max(runEnd, b);
                                }
                                else
                                {
                                    flush();
                                    run = true;
                                    runStart = a;
                                    runEnd = b;
                                }
                                continue;
                            }
                            flush();
                            const double a = std::max(x0, 0.0), b = std::min(x1, plot);
                            if (b <= a)
                            {
                                continue;
                            }
                            const float wide = static_cast<float>(b - a);
                            p.rect({GUTTER + static_cast<float>(a), y}, {wide, ROW - 1.0f},
                                   it->count > 1 ? colors.merged : colors.names[names[it->name].slot]);
                            ++lastSpansDrawn;
                            if (wide >= LABEL_MIN_WIDTH && labels < MAX_LABELS)
                            {
                                ++labels;
                                const std::string label = it->count > 1
                                    ? say("timeline.zones", {{"count", whole(it->count)}})
                                    : names[it->name].text;
                                p.text({GUTTER + static_cast<float>(a) + 3.0f, y}, {wide - 6.0f, ROW - 1.0f},
                                       fitText(canvas.entity(), label, wide - 6.0f), colors.onSpan, UiCanvasAlign::Left,
                                       TEXT_SIZE);
                            }
                        }
                        flush();
                    }
                }
                lastLabels = labels;
                lastPrimitives = p.primitives();
                lastDropped = p.dropped();
            });
        maxPrimitives = std::max(maxPrimitives, lastPrimitives);
    }

    void ProfilerTimeline::Impl::drawOverlay()
    {
        SPITE_ZONE("ProfilerTimelineOverlay");
        dirtyOverlay = false;
        ++overlayRedraws;
        const profview::Window w = window();
        const float scroll = viewConst().laneScroll;
        hoverLabel = {};
        overlay.update(
            [&](UiCanvasPainter& p)
            {
                if (!w.ok)
                {
                    return;
                }
                const double plot = plotWidth();
                const double scale = plot / static_cast<double>(w.width());
                const auto xOf = [&](const std::int64_t t)
                { return static_cast<double>(t - static_cast<std::int64_t>(w.start)) * scale; };
                const auto zoneRect = [&](const u32 laneIndex, const u32 depth, const std::int64_t start,
                                          const std::int64_t end, const u32 color)
                {
                    if (laneIndex >= lanes.size() || depth >= lanes[laneIndex].rows ||
                        !laneVisible(lanes[laneIndex], 0.0f))
                    {
                        return;
                    }
                    const double a = std::max(xOf(start), 0.0),
                                 b = std::min(std::max(xOf(end), xOf(start) + 1.0), plot);
                    if (b > a)
                    {
                        outline(p, GUTTER + static_cast<float>(a),
                                RULER + lanes[laneIndex].top - scroll + LANE_HEADER + static_cast<float>(depth) * ROW,
                                static_cast<float>(b - a), ROW - 1.0f, color);
                    }
                };
                if (selection().hasZone && selDetail.valid && selection().capture == capture)
                {
                    const CaptureZone& zone = selDetail.detail.zone;
                    zoneRect(zone.thread, zone.depth, zone.startNs, zone.endNs, colors.selected);
                }
                if (hover.has)
                {
                    p.rect({hover.point.x, RULER}, {1.0f, std::max(0.0f, canvasSize.y - RULER)}, colors.crosshair);
                    // under the ruler, on the first lane's header strip, where the tick labels are not
                    std::string time = fmtNs(hover.ns);
                    const float width = textWidth(overlay.entity(), time) + 6.0f;
                    hoverLabel = {std::clamp(hover.point.x + 4.0f, GUTTER, std::max(GUTTER, canvasSize.x - width)),
                                  RULER, width, 12.0f, std::move(time)};
                    p.rect({hoverLabel.x, hoverLabel.y}, {hoverLabel.width, hoverLabel.height}, colors.tipFill);
                    p.text({hoverLabel.x + 3.0f, hoverLabel.y}, {hoverLabel.width - 6.0f, hoverLabel.height},
                           hoverLabel.text, colors.text, UiCanvasAlign::Left, TEXT_SIZE);
                    if (hover.onSpan)
                    {
                        zoneRect(hover.lane, hover.depth, hover.span.start, hover.span.end, colors.outline);
                    }
                }
            });
    }

    void ProfilerTimeline::Impl::drawHistogram()
    {
        dirtyHistogram = false;
        const Vec2 size = histogramSize; // as laid out: the canvas's own size is its declared one (0 high)
        histogramBars = 0;
        histogram.update(
            [&](UiCanvasPainter& p)
            {
                if (!selStatsValid || selStats.histogram.empty() || size.x < 8.0f || size.y < 24.0f)
                {
                    return;
                }
                const u64 peak = *std::ranges::max_element(selStats.histogram);
                const float barWidth = (size.x - 8.0f) / static_cast<float>(selStats.histogram.size());
                const float top = 14.0f, bottom = size.y - 2.0f;
                for (sizet i = 0; i < selStats.histogram.size(); ++i)
                {
                    const float h = peak == 0
                        ? 0.0f
                        : (bottom - top) * static_cast<float>(selStats.histogram[i]) / static_cast<float>(peak);
                    if (selStats.histogram[i] != 0)
                    {
                        p.rect({4.0f + static_cast<float>(i) * barWidth, bottom - std::max(h, 1.0f)},
                               {std::max(1.0f, barWidth - 1.0f), std::max(h, 1.0f)}, colors.bar);
                        ++histogramBars;
                    }
                }
                p.text({4.0f, 0.0f}, {size.x - 8.0f, 12.0f},
                       say("timeline.range",
                           {{"min", ms3(static_cast<double>(selStats.minNs))},
                            {"max", ms3(static_cast<double>(selStats.maxNs))}}),
                       colors.dimText, UiCanvasAlign::Left, TEXT_SIZE);
            });
    }

    void ProfilerTimeline::Impl::showStats()
    {
        dirtyStats = false;
        const ProfilerSelectionComponent& sel = selection();
        std::string a, b, c;
        if (!sel.hasZone || sel.capture != capture || capture == 0)
        {
            a = say("timeline.stats.none");
        }
        else if (!selDetail.valid)
        {
            a = statsError.empty()
                ? say("timeline.stats.loading",
                      {{"index", std::to_string(sel.zoneIndex)}, {"lane", std::to_string(sel.zoneLane)}})
                : say("timeline.stats.zoneError", {{"error", statsError}});
        }
        else
        {
            const CaptureZoneDetail& d = selDetail.detail;
            a = say("timeline.stats.zone",
                    {{"name", clipText(d.zone.name, TIP_NAME_CHARS)},
                     {"thread", std::to_string(d.zone.thread)},
                     {"lane", clipText(d.laneName, 40)},
                     {"index", whole(d.zone.index)},
                     {"depth", std::to_string(d.zone.depth)}});
            b = say(
                "timeline.stats.this",
                {{"duration", fmtNs(d.zone.endNs - d.zone.startNs)},
                 {"self", d.selfExact ? fmtNs(d.selfNs) : say("timeline.na")},
                 {"start", fmtNs(d.zone.startNs)},
                 {"frame", d.hasFrame ? "  " + say("timeline.frame", {{"frame", whole(d.frame)}}) : std::string()}});
            if (selStatsValid)
            {
                c = say("timeline.stats.all",
                        {{"name", clipText(selStatsName, 40)},
                         {"count", whole(selStats.count)},
                         {"mean", ms3(selStats.meanNs)},
                         {"p50", ms3(selStats.p50Ns)},
                         {"p99", ms3(selStats.p99Ns)},
                         {"max", ms3(static_cast<double>(selStats.maxNs))}});
            }
            else
            {
                c = statsError.empty() ? say("timeline.stats.reading")
                                       : say("timeline.stats.error", {{"error", statsError}});
            }
        }
        statLines[0].set(a);
        statLines[1].set(b);
        statLines[2].set(c);
        dirtyHistogram = true;
    }

    void ProfilerTimeline::Impl::showInfo()
    {
        dirtyInfo = false;
        std::string text;
        const profview::Window w = window();
        if (w.ok)
        {
            text = say("timeline.info.view",
                       {{"from", fmtNs(static_cast<std::int64_t>(w.start))},
                        {"to", fmtNs(static_cast<std::int64_t>(w.end))},
                        {"width", fmtNs(static_cast<std::int64_t>(w.width()))},
                        {"total", fmtNs(durationNs)}});
        }
        if (!viewError.empty())
        {
            text += "  " + say("status.error", {{"error", viewError}});
        }
        if (findState.state != "idle")
        {
            const std::string key = "timeline.find." + findState.state;
            const std::string count = matchCount();
            text += "  " +
                say(count.empty() ? "timeline.info.find" : "timeline.info.findCounted",
                    {{"name", clipText(findState.name, 24)}, {"state", say(key)}, {"count", count}});
            if (findState.state == "error")
            {
                text += " " + findState.error;
            }
        }
        if (text != lastInfo)
        {
            lastInfo = text;
            info.set(text);
        }
    }

    // -- state -------------------------------------------------------------------------------------------------------

    void ProfilerTimeline::Impl::writeStateImpl(IFormatWriter& out) const
    {
        const ProfilerViewComponent& view = viewConst();
        const ProfilerSelectionComponent& sel = selection();
        out.beginObject("timeline");
        out.writeU32("capture", capture);
        out.writeU64("lanes", lanes.size());
        out.writeU64("canvasPrimitives", canvas.primitives());
        out.beginObject("view");
        writeView(out);
        out.writeString("error", viewError);
        out.endObject();
        out.beginObject("load");
        out.writeBool("inFlight", viewInFlight);
        out.writeU64("fetches", fetches);
        out.writeU64("viewDones", viewDones);
        out.writeU64("spanBatches", spanBatches);
        out.writeU64("droppedEvents", droppedEvents);
        out.writeU64("fromNs", static_cast<u64>(std::max<std::int64_t>(0, fetchFrom)));
        out.writeU64("toNs", static_cast<u64>(std::max<std::int64_t>(0, fetchTo)));
        out.writeU32("columns", fetchColumns);
        out.writeU64("marks", marks.size());
        out.writeBool("marksTruncated", marksTruncated);
        out.endObject();
        out.beginObject("draw");
        out.writeU64("redraws", redraws);
        out.writeU64("overlayRedraws", overlayRedraws);
        out.writeU64("viewChanges", viewChanges);
        out.writeU32("primitives", lastPrimitives);
        out.writeU32("maxPrimitives", maxPrimitives);
        out.writeU32("dropped", lastDropped);
        out.writeU32("spansDrawn", lastSpansDrawn);
        out.writeU32("spansVisited", lastSpansVisited);
        out.writeU32("lanesDrawn", lastLanesDrawn);
        out.writeU32("labels", lastLabels);
        out.writeF32("contentHeight", contentHeight);
        out.writeF32("canvasWidth", canvasSize.x);
        out.writeF32("canvasHeight", canvasSize.y);
        out.endObject();
        out.beginArray("emptyLines", static_cast<u32>(emptyLines.size())); // the note a canvas with no capture shows
        for (const std::string& line : emptyLines)
        {
            out.writeString("", line);
        }
        out.endArray();
        const u32 shownLanes = static_cast<u32>(std::min<sizet>(lanes.size(), 64));
        out.beginArray("laneRows", shownLanes);
        for (u32 i = 0; i < shownLanes; ++i)
        {
            const TlLane& lane = lanes[i];
            out.beginObject({});
            out.writeU32("lane", lane.info.index);
            out.writeString("name", lane.info.name);
            out.writeU64("zones", lane.info.zones);
            out.writeU32("maxDepth", lane.info.maxDepth);
            out.writeU64("spans", lane.spans.size());
            out.writeBool("loaded", lane.loaded);
            out.writeBool("cut", lane.cut);
            out.writeU32("columns", lane.columns);
            out.writeU64("fromNs", static_cast<u64>(std::max<std::int64_t>(0, lane.from)));
            out.writeU64("toNs", static_cast<u64>(std::max<std::int64_t>(0, lane.to)));
            out.writeU32("rows", lane.rows);
            out.writeF32("top", lane.top);
            out.endObject();
        }
        out.endArray();
        const auto writeLabel = [&out](const std::string_view key, const TlLabel& label)
        {
            out.beginObject(key);
            out.writeF32("x", label.x);
            out.writeF32("y", label.y);
            out.writeF32("width", label.width);
            out.writeF32("height", label.height);
            out.writeString("text", label.text);
            out.endObject();
        };
        out.beginArray("ruler", static_cast<u32>(rulerLabels.size()));
        for (const TlLabel& label : rulerLabels)
        {
            writeLabel({}, label);
        }
        out.endArray();
        out.beginObject("hover");
        writeLabel("label", hoverLabel);
        out.writeBool("has", hover.has);
        out.writeBool("onSpan", hover.onSpan);
        out.writeU32("lane", hover.lane);
        out.writeU32("depth", hover.depth);
        out.writeU64("ns", static_cast<u64>(std::max<std::int64_t>(0, hover.ns)));
        if (hover.onSpan)
        {
            out.writeU64("count", hover.span.count);
            out.writeString("name", names[hover.span.name].text);
            out.writeU64("startNs", static_cast<u64>(std::max<std::int64_t>(0, hover.span.start)));
            out.writeU64("endNs", static_cast<u64>(std::max<std::int64_t>(0, hover.span.end)));
            out.writeU64("firstIndex", hover.span.firstSeq);
            out.writeU64("longestIndex", hover.span.longestSeq);
            out.writeU64("totalNs", hover.span.totalNs);
            out.writeU64("longestNs", hover.span.longestNs);
        }
        out.writeBool("detail", hoverDetail.valid);
        if (hoverDetail.valid)
        {
            out.beginObject("zone");
            writeDetail(out, hoverDetail.detail);
            out.endObject();
        }
        out.writeBool("tooltipShown", tip.isShown());
        out.writeString("tooltip", tooltip);
        out.endObject();
        out.beginObject("zone");
        out.writeBool("selected", sel.hasZone && sel.capture == capture);
        out.writeBool("detail", selDetail.valid);
        if (selDetail.valid)
        {
            out.beginObject("zone");
            writeDetail(out, selDetail.detail);
            out.endObject();
        }
        out.writeU32("histogramBars", histogramBars);
        out.writeBool("statsReady", selStatsValid);
        out.writeString("statsName", selStatsName);
        out.writeString("statsError", statsError);
        if (selStatsValid)
        {
            out.beginObject("stats");
            writeStats(out, selStats);
            out.endObject();
        }
        out.endObject();
        out.beginObject("find");
        out.writeString("name", findState.name);
        out.writeString("direction", findState.direction);
        out.writeString("state", findState.state);
        out.beginObject("matches");
        out.writeBool("ready", matches.ready);
        out.writeU64("count", matches.keys.size());
        out.writeBool("truncated", matches.truncated);
        out.writeString("error", matches.error);
        out.writeString("text", matchCount());
        out.endObject();
        out.writeString("error", findState.error);
        out.writeU64("searches", findState.searches);
        out.writeBool("found", findState.found);
        if (findState.found)
        {
            out.writeU32("lane", findState.zone.thread);
            out.writeU64("index", findState.zone.index);
            out.writeString("zoneName", findState.zone.name);
            out.writeU64("startNs", static_cast<u64>(std::max<std::int64_t>(0, findState.zone.startNs)));
            out.writeU64("endNs", static_cast<u64>(std::max<std::int64_t>(0, findState.zone.endNs)));
        }
        out.endObject();
        out.beginArray("texts", static_cast<u32>(statusTips.items().size()));
        for (const StatusText* item : statusTips.items())
        {
            out.beginObject({});
            out.writeString("full", item->full());
            out.writeString("shown", item->shown());
            out.writeBool("truncated", item->truncated());
            out.writeF64("width", item->width());
            out.endObject();
        }
        out.endArray();
        (void)view;
        out.endObject();
    }
} // namespace spite
