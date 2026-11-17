#include "EditorTimelineImpl.hpp"
#include "ProfilerViews.hpp"

#include <algorithm>
#include <cmath>

#include <spite/profiling/Zone.hpp>
#include "ui/UiChart.hpp"
#include "ui/UiHierarchyManager.hpp"

namespace spite
{
    using namespace panelui;
    using namespace timeline;

    namespace
    {
        // HID scancodes of the keys the timeline answers
        namespace Key
        {
            constexpr u32 F = 9, N = 17;
            constexpr u32 MINUS = 45, EQUALS = 46;
            constexpr u32 HOME = 74, PAGE_UP = 75, RIGHT = 79, LEFT = 80, DOWN = 81, UP = 82, KP_MINUS = 86,
                          KP_PLUS = 87;
            constexpr u32 PAGE_DOWN = 78;
        } // namespace Key

        std::string clipUtf8(const std::string_view text, const u32 limit)
        {
            if (text.size() <= limit)
            {
                return std::string(text);
            }
            u32 end = limit;
            while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
            {
                --end;
            }
            return std::string(text.substr(0, end));
        }

        // A number argument of a command, as JSON gives it (a string or a missing member: none)
        std::optional<double> numberArg(const ControlArgs& args, const std::string_view key)
        {
            const Json value = Json::parse(args.json(key).value_or("null"), nullptr, false);
            if (!value.is_number())
            {
                return std::nullopt;
            }
            return value.get<double>();
        }

        std::string line(const std::string_view command, const Json& arguments)
        {
            return Json{{"command", std::string(command)}, {"args", arguments}}.dump(-1, ' ', false,
                                                                                     Json::error_handler_t::replace);
        }

        constexpr float FIND_MIN_WIDTH = 60.0f;
        constexpr float BUTTON_MIN_WIDTH = 54.0f;
        constexpr std::array<std::string_view, 4> FIND_DIRECTIONS{"next", "previous", "first", "last"};
    } // namespace

    // -- construction ----------------------------------------------------------------------------------------------

    ProfilerTimeline::Impl::Impl(const Host& h, const Widget& parent) :
        host(h), colors(TlColors::read(*h.scope)), theme(*h.scope, [this] { restyle(); })
    {
        UiScope& scope = *host.scope;
        DensityFollower& density = *host.density;
        bar = density.track(stackPanel(scope, "profiler.timeline.bar", Vec2{}, UiLayoutDirection::Row), HEIGHT_BUTTON);
        bar.setParent(parent);
        bar.setLandmark(true); // the timeline's controls and the timeline are two regions of the cycle
        UiRectDesc findDesc = barDesc(Vec2{170.0f, 0.0f});
        findDesc.layoutItem = UiLayoutItemComponent{.minSize = Vec2{FIND_MIN_WIDTH, 0.0f}}; // gives way to the buttons
        findBox = density.track(scope.createTextInput("profiler.find", findDesc), HEIGHT_BUTTON);
        bindInputPadding(findBox);
        findBox.setParent(bar);
        findBox.setLocalizedPlaceholder(profilerKey("timeline.find.placeholder"));
        findBox.setStyleClass(InternedString("profiler.find")); // the placeholder is cut with an ellipsis
        findBox.onCancel(
            [this]
            {
                if (host.leave)
                {
                    host.leave(); // a field's Escape also drops the focus, unless it moved on
                }
            });
        findBox.setAccessibleNameKey(profilerKey("timeline.find.name"));
        findBox.onSubmit(
            [this](const std::string_view text)
            {
                if (!text.empty())
                {
                    host.send(line("profilerFind", Json{{"name", clipUtf8(text, FIND_MAX_NAME)}}));
                }
            });
        UiRectDesc buttonDesc = fitWidthDesc();
        buttonDesc.layoutItem->minSize = Vec2{BUTTON_MIN_WIDTH, 0.0f}; // the least the bar is laid out for
        const auto make = [&](Button& button, const char* id, const char* textKey, std::function<void()> action)
        {
            button = density.track(scope.createButton(id, "", buttonDesc), HEIGHT_BUTTON);
            bindFitPadding(button, SPACE_XS, SPACE_NONE);
            button.setLocalizedText(profilerKey(textKey));
            button.setParent(bar);
            button.onClick(std::move(action));
        };
        const auto search = [this](const char* direction)
        {
            const std::string name = findBox.value().empty() ? findState.name : std::string(findBox.value());
            if (!name.empty())
            {
                host.send(
                    line("profilerFind", Json{{"name", clipUtf8(name, FIND_MAX_NAME)}, {"direction", direction}}));
            }
        };
        make(prevButton, "profiler.findPrev", "timeline.prev", [search] { search("previous"); });
        make(nextButton, "profiler.findNext", "timeline.next", [search] { search("next"); });
        make(fitButton, "profiler.fit", "timeline.fit",
             [this] { host.send(line("profilerView", Json{{"fit", true}})); });
        make(zoneButton, "profiler.zoomZone", "timeline.zone",
             [this] { host.send(line("profilerView", Json{{"zoomToZone", true}})); });
        UiRectDesc infoDesc = barDesc(Vec2{});
        infoDesc.layoutItem = fillItem();
        statusTips.create(scope, "profiler.timeline.statusTip");
        info.create(scope, "profiler.timeline.info", profilerKey("timeline.info.name"), bar, infoDesc, &statusTips);
        density.track(info.box, HEIGHT_BUTTON);

        UiCanvasDesc desc;
        desc.rect = topLeft();
        UiLayoutItemComponent grow = fillItem();
        grow.minSize = Vec2{0.0f, MIN_HEIGHT}; // the panel scrolls rather than squeeze the timeline
        grow.grow = 3.0f; // the timeline is the panel's main view: three times the frame graph's height
        desc.rect.layoutItem = grow;
        desc.maxPrimitives = 200000;
        canvas = scope.createCanvas("profiler.timeline", desc);
        canvas.setParent(parent);
        canvas.setAccessibleRole(UiRole::Table);
        canvas.setAccessibleNameKey(profilerKey("timeline.name"));
        scope.world().getComponent<UiStateComponent>(canvas.entity()).tabStop = true;
        scope.world().getComponent<UiStateComponent>(canvas.entity()).clipsChildren = true;
        canvas.setLandmark(true);
        UiCanvasDesc overlayDesc;
        overlayDesc.rect = topLeft();
        overlayDesc.maxPrimitives = 1024;
        overlay = scope.createCanvas("profiler.timeline.overlay", overlayDesc);
        overlay.setParent(canvas);
        overlay.setInteractable(false);
        canvas.onPointer([this](const UiCanvasPointer& event) { onPointer(event); });
        canvas.onKeyDown([this](const u32 scancode, const u8 modifiers, bool) { return onKey(scancode, modifiers); });

        Panel stats =
            stackPanel(scope, "profiler.timeline.stats", Vec2{}, UiLayoutDirection::Row, UiLayoutAlign::Stretch);
        density.apply([stats](const Density& d) mutable { stats.setSize(Vec2{0.0f, 3.0f * d.row + 2.0f * d.xs}); });
        stats.setParent(parent);
        Panel lines = scope.createPanel(
            "profiler.timeline.statLines",
            []
            {
                UiRectDesc d =
                    barDesc(Vec2{0.0f, 0.0f}, stack(UiLayoutDirection::Column, 0.0f, {}, UiLayoutAlign::Stretch));
                d.layoutItem = fillItem();
                return d;
            }());
        lines.setParent(stats);
        for (StatusText& statLine : statLines)
        {
            statLine.create(scope, "profiler.zoneStat", profilerKey("timeline.zoneStat.name"), lines, barDesc(Vec2{}),
                            &statusTips);
            density.track(statLine.box, HEIGHT_ROW);
        }
        UiCanvasDesc histogramDesc;
        histogramDesc.rect = barDesc(Vec2{230.0f, 0.0f});
        histogramDesc.maxPrimitives = 128;
        histogram = scope.createCanvas("profiler.zoneHistogram", histogramDesc);
        histogram.setParent(stats);

        UiTooltipDesc tipDesc;
        tipDesc.delay = 0.35f;
        tipDesc.follow = true;
        tip = scope.createTooltip("profiler.timeline.tip", tipDesc);
        tip.label().setRichText(true); // names are escaped into the markup
        tip.attach(canvas);
        tip.onShow(
            [this](Entity)
            {
                buildTooltip();
                tip.label().setText(tooltip);
            });
        names.push_back({"...", TL_OVERFLOW_NAME});
        nameIds.emplace("...", 0);
    }

    ProfilerTimeline::ProfilerTimeline(const Host& host, const Widget& parent) :
        m_impl(std::make_unique<Impl>(host, parent))
    {
        Impl* impl = m_impl.get();
        host.control->registerViewCommand(
            "profilerView", host.owner,
            "sets the Profiler timeline's view (a ProfilerView component on the profiler panel): the window of the "
            "selected capture in ns since it began, and the lane scroll. One of range (\"1.5s..2s\", units ns us ms "
            "s), "
            "startNs + endNs, firstFrame + lastFrame (a frame range of the graph), fit or zoomToZone (the selected "
            "zone, or lane + index) sets the window; zoom (a factor, "
            "2 halves the window; in steps of 1/1024) around a time (default the window's middle) and pan (ns, "
            "negative moves earlier) then move it. A window is at least 100 ns and stays inside the capture: one "
            "partly outside is clipped, one empty, inverted, not a number or entirely outside is refused",
            {optionalArg("capture", ArgKind::Integer, "refused unless this capture is the selected one", "any"),
             optionalArg("range", ArgKind::String, "\"<start>..<end>\", each a finite number with an optional unit"),
             optionalArg("startNs", ArgKind::Integer, "window start; with endNs"),
             optionalArg("endNs", ArgKind::Integer, "window end (exclusive)", "startNs"),
             optionalArg("firstFrame", ArgKind::Integer,
                         "the window of frames firstFrame..lastFrame (inclusive, as the frame graph numbers them), "
                         "from the first one's start to the last one's end; one of the two alone is that frame"),
             optionalArg("lastFrame", ArgKind::Integer, "the last frame of the window", "firstFrame"),
             optionalArg("fit", ArgKind::Boolean, "the whole capture"),
             optionalArg("zoomToZone", ArgKind::Boolean, "fit the selected zone (or lane + index) with a margin"),
             optionalArg("lane", ArgKind::Integer, "zoomToZone: the zone's lane, with index"),
             optionalArg("index", ArgKind::Integer, "zoomToZone: the zone's index in its lane"),
             optionalArg("zoom", ArgKind::Number, "window width divided by this (1/1024..1024)"),
             optionalArg("around", ArgKind::Integer, "zoom: the time that stays put", "the window's middle"),
             optionalArg("pan", ArgKind::Number, "ns to move the window by"),
             optionalArg("scroll", ArgKind::Number, "lane scroll in UI pixels, clamped to the lanes")},
            [impl](const ControlArgs& args, IFormatWriter& out) { return impl->viewCommand(args, out); });
        host.control->registerViewCommand(
            "profilerFind", host.owner,
            "searches the selected capture for zones whose name contains `name` (case-insensitive) on the Profiler "
            "worker and, when found, selects the zone (profilerSelect) and fits the view to it. next/previous go "
            "from the selected zone (else the window's start); first/last from the capture's ends. The answer is "
            "queued: profilerState.timeline.find shows searching, then found, notFound or error",
            {requiredArg("name", ArgKind::String, "case-insensitive substring of the zone name, at most 256 bytes"),
             optionalArg("direction", ArgKind::String, "next, previous, first or last", "next", FIND_DIRECTIONS),
             optionalArg("zoom", ArgKind::Boolean, "fit the view to the zone found", "true")},
            [impl](const ControlArgs& args, IFormatWriter& out) { return impl->findCommand(args, out); });
    }

    ProfilerTimeline::~ProfilerTimeline() = default;

    Widget ProfilerTimeline::canvas() const { return m_impl->canvas; }

    u64 ProfilerTimeline::viewToken() const { return m_impl->token(ProfilerSlot::View); }

    u64 ProfilerTimeline::matchToken() const { return m_impl->matchValue; }

    std::string ProfilerTimeline::view(const ControlArgs& args, IFormatWriter& out)
    {
        return m_impl->viewCommand(args, out);
    }

    std::string ProfilerTimeline::find(const ControlArgs& args, IFormatWriter& out)
    {
        return m_impl->findCommand(args, out);
    }

    void ProfilerTimeline::writeState(IFormatWriter& out) const { m_impl->writeStateImpl(out); }

    // -- the view ----------------------------------------------------------------------------------------------------

    profview::Window ProfilerTimeline::Impl::window() const
    {
        const ProfilerViewComponent& view = viewConst();
        return {view.startNs, view.endNs, capture != 0 && view.capture == capture && view.endNs > view.startNs};
    }

    bool ProfilerTimeline::Impl::applyView(const profview::Window& next, const std::optional<float> scroll)
    {
        ProfilerViewComponent& view = viewComponent();
        bool changed = false;
        if (next.ok && (view.startNs != next.start || view.endNs != next.end))
        {
            view.startNs = next.start;
            view.endNs = next.end;
            changed = true;
        }
        if (scroll)
        {
            const float clamped = std::isfinite(*scroll) ? std::clamp(*scroll, 0.0f, maxScroll) : view.laneScroll;
            if (view.laneScroll != clamped)
            {
                view.laneScroll = clamped;
                changed = true;
            }
        }
        if (changed)
        {
            ++view.generation;
            ++viewChanges;
            quiet = 0;
            fetchDue = true;
            dirtyMain = dirtyOverlay = dirtyInfo = dirtyFrames = true;
            hover.stable = 0;
            hoverStale = true;
        }
        return changed;
    }

    float ProfilerTimeline::Impl::gestureScroll() const
    {
        return wanted.has && wanted.scroll ? *wanted.scroll : viewConst().laneScroll;
    }

    void ProfilerTimeline::Impl::want(const profview::Window& next, const std::optional<float> scroll)
    {
        const profview::Window base = gestureWindow();
        const float baseScroll = gestureScroll();
        if (!wanted.has || wanted.sent)
        {
            wanted = {.has = true, .window = base, .scroll = std::nullopt};
        }
        if (next.ok && (next.start != base.start || next.end != base.end))
        {
            wanted.window = next;
            wanted.windowChanged = true;
        }
        if (scroll && std::isfinite(*scroll))
        {
            const float clamped = std::clamp(*scroll, 0.0f, maxScroll);
            if (clamped != baseScroll)
            {
                wanted.scroll = clamped;
            }
        }
    }

    void ProfilerTimeline::Impl::sendWanted()
    {
        if (!wanted.has || wanted.sent)
        {
            return;
        }
        Json args = Json{{"capture", capture}};
        if (wanted.windowChanged)
        {
            args["startNs"] = wanted.window.start;
            args["endNs"] = wanted.window.end;
        }
        if (wanted.scroll)
        {
            args["scroll"] = *wanted.scroll;
        }
        if (args.size() == 1)
        {
            wanted = {};
            return;
        }
        wanted.sent = true;
        (host.sendView ? host.sendView : host.send)(line("profilerView", args));
    }

    void ProfilerTimeline::Impl::setScroll(const float scroll) { want({}, scroll); }

    void ProfilerTimeline::Impl::zoomAt(const double x, const std::int32_t notches)
    {
        profview::Window next = gestureWindow();
        if (!next.ok || notches == 0)
        {
            return;
        }
        const double plot = plotWidth();
        const u64 wq = static_cast<u64>(std::llround(plot * profview::PIXEL_SCALE));
        const u64 xq = static_cast<u64>(std::llround(std::clamp(x, 0.0, plot) * profview::PIXEL_SCALE));
        for (std::int32_t i = 0; i < std::min<std::int32_t>(std::abs(notches), MAX_NOTCHES); ++i)
        {
            next = profview::zoomWindow(next, static_cast<u64>(durationNs), xq, wq, notches > 0 ? ZOOM_NUM : ZOOM_DEN,
                                        notches > 0 ? ZOOM_DEN : ZOOM_NUM);
        }
        want(next);
    }

    void ProfilerTimeline::Impl::zoomToZone(const CaptureZone& zone)
    {
        const u64 start = static_cast<u64>(std::max<std::int64_t>(0, zone.startNs));
        const u64 end = static_cast<u64>(std::max<std::int64_t>(zone.endNs, zone.startNs));
        const u64 width = std::max<u64>(end - start, 1);
        const u64 wide = width + width / 5; // a tenth of margin each side
        applyView(profview::fitWindow(static_cast<std::int64_t>(start) - static_cast<std::int64_t>((wide - width) / 2),
                                      wide, static_cast<u64>(durationNs)));
    }

    void ProfilerTimeline::Impl::layoutLanes()
    {
        // the thread named "main" first (it carries the frame), the others in the capture's order
        std::vector<TlLane*> order;
        for (TlLane& lane : lanes)
        {
            order.push_back(&lane);
        }
        std::ranges::stable_partition(order, [](const TlLane* lane) { return lane->info.name == "main"; });
        float y = 0.0f;
        for (TlLane* laneAt : order)
        {
            TlLane& lane = *laneAt;
            lane.top = y;
            lane.rows = lane.info.zones == 0 ? 0 : std::min(lane.info.maxDepth + 1, MAX_ROWS);
            lane.height = lane.rows == 0 ? 0.0f : LANE_HEADER + static_cast<float>(lane.rows) * ROW + LANE_GAP;
            y += lane.height;
        }
        contentHeight = y;
        maxScroll = std::max(0.0f, contentHeight - std::max(0.0f, canvasSize.y - RULER));
        if (viewConst().laneScroll > maxScroll)
        {
            applyView({}, maxScroll);
        }
        dirtyMain = true;
    }

    void ProfilerTimeline::Impl::requestZoneZoom(const u32 lane, const u64 seq)
    {
        if (selDetail.valid && selDetail.detail.zone.thread == lane && selDetail.detail.zone.index == seq)
        {
            zoomToZone(selDetail.detail.zone);
            return;
        }
        pending = {true, lane, seq};
        if (host.data)
        {
            host.data->requestDetail(ProfilerSlot::Zoom, capture, lane, seq, newToken(ProfilerSlot::Zoom), 0);
        }
    }

    std::string ProfilerTimeline::Impl::viewCommand(const ControlArgs& args, IFormatWriter& out)
    {
        const ProfilerViewComponent& component = viewConst();
        if (capture == 0)
        {
            return "select a capture first (profilerSelect capture=<id>)";
        }
        if (durationNs <= 0 || component.capture != capture)
        {
            return "capture " + std::to_string(capture) + " is still loading (profilerState shows its load)";
        }
        if (const std::optional<u64> named = args.integer("capture"); named && *named != capture)
        {
            return "capture " + std::to_string(*named) + " is not the selected capture";
        }
        const u64 duration = static_cast<u64>(durationNs);
        const std::optional<std::string> range = args.string("range");
        const std::optional<u64> startArg = args.integer("startNs"), endArg = args.integer("endNs");
        const std::optional<u64> firstFrame = args.integer("firstFrame"), lastFrame = args.integer("lastFrame");
        const bool fit = args.boolean("fit").value_or(false);
        const bool zone = args.boolean("zoomToZone").value_or(false);
        if ((range ? 1 : 0) + ((startArg || endArg) ? 1 : 0) + ((firstFrame || lastFrame) ? 1 : 0) + (fit ? 1 : 0) +
                (zone ? 1 : 0) >
            1)
        {
            return "give one of range, startNs + endNs, firstFrame + lastFrame, fit or zoomToZone";
        }
        profview::Window next = window();
        if (range || startArg || endArg || firstFrame || lastFrame)
        {
            std::int64_t start = 0, end = 0;
            if (firstFrame || lastFrame)
            {
                const u64 from = firstFrame ? *firstFrame : *lastFrame, to = lastFrame ? *lastFrame : from;
                if (std::string error = frameBounds(from, to, start, end); !error.empty())
                {
                    return error;
                }
            }
            else if (range)
            {
                if (std::string error = profview::parseRange(*range, start, end); !error.empty())
                {
                    return error;
                }
            }
            else
            {
                if (!startArg || !endArg)
                {
                    return "startNs and endNs go together";
                }
                start = static_cast<std::int64_t>(std::min<u64>(*startArg, profview::MAX_NS));
                end = static_cast<std::int64_t>(std::min<u64>(*endArg, profview::MAX_NS));
            }
            if (end <= start)
            {
                return "the window is empty or inverted (" + std::to_string(start) + " .. " + std::to_string(end) +
                    " ns)";
            }
            if (end <= 0 || start >= durationNs)
            {
                return "the window " + std::to_string(start) + " .. " + std::to_string(end) +
                    " ns is outside the capture (0 .. " + std::to_string(durationNs) + " ns)";
            }
            const std::int64_t from = std::max<std::int64_t>(start, 0), to = std::min(end, durationNs);
            next = profview::fitWindow(from, static_cast<u64>(to - from), duration);
        }
        else if (fit)
        {
            next = profview::fitWindow(0, duration, duration);
        }
        std::optional<std::pair<u32, u64>> target;
        if (zone)
        {
            const std::optional<u64> lane = args.integer("lane"), index = args.integer("index");
            if (lane.has_value() != index.has_value())
            {
                return "lane and index go together";
            }
            if (lane)
            {
                if (*lane >= lanes.size() || *index >= lanes[*lane].info.zones)
                {
                    return "zone " + std::to_string(*index) + " of lane " + std::to_string(*lane) +
                        " does not exist in capture " + std::to_string(capture);
                }
                target = {static_cast<u32>(*lane), *index};
            }
            else if (selection().hasZone)
            {
                target = {selection().zoneLane, selection().zoneIndex};
            }
            else
            {
                return "no zone selected (profilerSelect zoneLane=<lane> zoneIndex=<index>), or pass lane and index";
            }
        }
        if (const std::optional<double> zoom = numberArg(args, "zoom"))
        {
            if (!std::isfinite(*zoom) || *zoom <= 0.0)
            {
                return "zoom must be a positive number";
            }
            const u32 num = static_cast<u32>(std::llround(std::clamp(*zoom, 1.0 / 1024.0, 1024.0) * 1024.0));
            const u64 width = next.width();
            const std::optional<u64> around = args.integer("around");
            const u64 offset = around ? std::clamp<u64>(*around, next.start, next.end) - next.start : width / 2;
            constexpr u64 WQ = u64{1} << 20;
            next = profview::zoomWindow(next, duration, profview::mulDivRound(offset, WQ, width), WQ, num, 1024);
        }
        if (const std::optional<double> pan = numberArg(args, "pan"))
        {
            if (!std::isfinite(*pan))
            {
                return "pan must be a finite number of ns";
            }
            const double limit = static_cast<double>(profview::MAX_NS);
            next = profview::panWindow(next, static_cast<std::int64_t>(std::clamp(*pan, -limit, limit)), duration);
        }
        std::optional<float> scroll;
        if (const std::optional<double> value = numberArg(args, "scroll"))
        {
            if (!std::isfinite(*value))
            {
                return "scroll must be a finite number of pixels";
            }
            scroll = static_cast<float>(std::clamp(*value, -1.0e9, 1.0e9));
        }
        applyView(next, scroll);
        if (target)
        {
            requestZoneZoom(target->first, target->second);
        }
        writeView(out);
        return {};
    }

    void ProfilerTimeline::Impl::writeView(IFormatWriter& out) const
    {
        const ProfilerViewComponent& view = viewConst();
        out.writeU32("capture", view.capture);
        out.writeU64("startNs", view.startNs);
        out.writeU64("endNs", view.endNs);
        out.writeU64("durationNs", view.durationNs);
        out.writeF32("laneScroll", view.laneScroll);
        out.writeF32("maxScroll", maxScroll);
        out.writeU64("generation", view.generation);
        out.writeBool("pendingZoom", pending.zoom);
        if (const TlFrames frames = visibleFrames(); frames.has)
        {
            out.beginObject("frames");
            out.writeU64("first", frames.first);
            out.writeU64("last", frames.last);
            out.writeBool("approximate", frames.approximate); // last is counted from the mean frame length
            out.endObject();
        }
    }

    // The window of frames first..last as the store has them: the first one's start to the last one's end
    std::string ProfilerTimeline::Impl::frameBounds(const u64 first, const u64 last, std::int64_t& start,
                                                    std::int64_t& end) const
    {
        CaptureInfo info;
        if (first > last)
        {
            return "firstFrame " + std::to_string(first) + " is after lastFrame " + std::to_string(last);
        }
        if (!host.store || !host.store->info(capture, info) || last >= info.frames)
        {
            return "frames " + std::to_string(first) + ".." + std::to_string(last) + " are outside capture " +
                std::to_string(capture) + " (" + std::to_string(info.frames) + " frames)";
        }
        const auto frameAt = [&](const u64 index, CaptureFrame& out)
        {
            CaptureSlice<CaptureFrame> slice;
            CaptureRange range;
            range.firstFrame = range.lastFrame = static_cast<std::int64_t>(index);
            range.limit = 1;
            std::string error = profilerFrameStoreCall([&] { return host.store->frameSpans(capture, range, slice); });
            if (error.empty() && slice.rows.empty())
            {
                error = "frame " + std::to_string(index) + " is not in capture " + std::to_string(capture);
            }
            out = slice.rows.empty() ? CaptureFrame{} : slice.rows.front();
            return error;
        };
        CaptureFrame head, tail;
        if (std::string error = frameAt(first, head); !error.empty())
        {
            return error;
        }
        if (std::string error = last == first ? std::string{} : frameAt(last, tail); !error.empty())
        {
            return error;
        }
        start = head.startNs;
        end = last == first ? head.endNs : tail.endNs;
        return {};
    }

    // The marks are the frames of the last fetched window: they answer for the current one only while that window
    // lies inside them. A frame the cap cut off is counted from the frames before it.
    TlFrames ProfilerTimeline::Impl::visibleFrames() const
    {
        const profview::Window w = window();
        const auto start = static_cast<std::int64_t>(w.start), end = static_cast<std::int64_t>(w.end);
        if (!w.ok || marks.empty() || marksFrom > start || (!marksTruncated && marksTo < end) ||
            (start == 0 && end >= durationNs))
        {
            return {};
        }
        const auto firstIt = std::ranges::find_if(marks, [&](const TlMark& mark) { return mark.end > start; });
        const auto lastIt =
            std::ranges::find_if(marks.rbegin(), marks.rend(), [&](const TlMark& mark) { return mark.start < end; });
        if (firstIt == marks.end() || lastIt == marks.rend() || firstIt->start >= end)
        {
            return {};
        }
        u64 last = lastIt->index;
        bool approximate = false;
        if (marksTruncated && marks.back().end < end)
        {
            const double average =
                static_cast<double>(marks.back().end - marks.front().start) / static_cast<double>(marks.size());
            last += static_cast<u64>(static_cast<double>(end - marks.back().end) / std::max(average, 1.0));
            approximate = true;
            if (CaptureInfo info; host.store && host.store->info(capture, info) && info.frames > 0)
            {
                last = std::min(last, info.frames - 1); // a hitch-heavy tail makes the mean overshoot
            }
        }
        return {true, firstIt->index, std::max(firstIt->index, last), approximate};
    }

    void ProfilerTimeline::Impl::pushFrames()
    {
        const profview::Window w = window();
        const bool wholeCapture = !w.ok || (w.start == 0 && static_cast<std::int64_t>(w.end) >= durationNs);
        const TlFrames now = visibleFrames();
        if (!wholeCapture && !now.has && (fetchDue || viewInFlight))
        {
            return; // the marks of the new window are on their way: asked again every frame until they are in
        }
        dirtyFrames = false;
        if (now == shownFrames)
        {
            return;
        }
        shownFrames = now;
        if (host.showFrames)
        {
            host.showFrames(now.has ? std::optional(std::pair<u64, u64>{now.first, now.last}) : std::nullopt,
                            now.approximate);
        }
    }

    // -- search
    // --------------------------------------------------------------------------------------------------------

    std::string ProfilerTimeline::Impl::findCommand(const ControlArgs& args, IFormatWriter& out)
    {
        if (capture == 0 || durationNs <= 0 || !host.data)
        {
            return "select a capture first (profilerSelect capture=<id>)";
        }
        const std::string name = args.string("name").value_or("");
        if (name.empty() || name.size() > FIND_MAX_NAME)
        {
            return "name must be 1.." + std::to_string(FIND_MAX_NAME) + " bytes";
        }
        const std::string direction = args.string("direction").value_or("next");
        if (std::ranges::find(FIND_DIRECTIONS, direction) == FIND_DIRECTIONS.end())
        {
            return "direction is next, previous, first or last";
        }
        findState.zoomOnFound = args.boolean("zoom").value_or(true);
        startFind(name, direction);
        out.writeBool("queued", true);
        out.writeString("name", findState.name);
        out.writeString("direction", direction);
        return {};
    }

    void ProfilerTimeline::Impl::startFind(const std::string& name, const std::string& direction)
    {
        CaptureFindQuery query;
        query.name = name;
        query.backwards = direction == "previous" || direction == "last";
        const profview::Window w = window();
        if (direction == "first")
        {
            query.cursorNs = 0;
        }
        else if (direction == "last")
        {
            query.cursorNs = INT64_MAX;
        }
        else if (findState.cursorValid && findState.found)
        {
            const CaptureZone& cursor = findState.zone;
            query.hasCursor = true;
            query.cursorNs = cursor.startNs;
            query.cursorLane = cursor.thread;
            query.cursorSeq = cursor.index;
        }
        else if (selection().hasZone && selDetail.valid && selDetail.detail.zone.thread == selection().zoneLane &&
                 selDetail.detail.zone.index == selection().zoneIndex)
        {
            const CaptureZone& cursor = selDetail.detail.zone;
            query.hasCursor = true;
            query.cursorNs = cursor.startNs;
            query.cursorLane = cursor.thread;
            query.cursorSeq = cursor.index;
        }
        else
        {
            query.cursorNs = static_cast<std::int64_t>(w.ok ? w.start : 0);
        }
        findState.name = name;
        findState.direction = direction;
        findState.state = "searching";
        findState.error.clear();
        findState.found = false;
        findState.cursorValid = false;
        ++findState.searches;
        host.data->requestFind(capture, std::move(query), -1, newToken(ProfilerSlot::Find));
        if (matches.name != name || !matches.error.empty())
        {
            requestMatches(name);
        }
        dirtyInfo = true;
    }

    namespace
    {
        // What the match count's task hands back
        struct MatchData
        {
            CaptureId capture = 0;
            std::string name;
            std::string error;
            bool truncated = false;
            std::vector<TlMatchKey> keys;
        };
    } // namespace

    // The zones named like the search text, counted on the worker beside the search itself: the zones come lane by lane
    // up to MAX_MATCHES, so past that the count is a lower bound and a zone has no position
    void ProfilerTimeline::Impl::requestMatches(const std::string& name)
    {
        matches = {.name = name};
        if (!host.data || !host.store)
        {
            return;
        }
        matchValue = matchGeneration->fetch_add(1) + 1;
        host.data->requestTask(ProfilerTimeline::TASK_VIEW, matchGeneration, matchValue,
                               [store = host.store, capture = capture, name](const CaptureCancelFn& cancelled,
                                                                             std::string&) -> std::shared_ptr<void>
                               {
                                   auto data = std::make_shared<MatchData>();
                                   data->capture = capture;
                                   data->name = name;
                                   CaptureZoneQuery query;
                                   query.name = name;
                                   query.enclosing = false;
                                   query.range.limit = MAX_MATCHES + 1;
                                   CaptureSlice<CaptureZone> slice;
                                   data->error = profilerStoreCall(cancelled,
                                                                   [&] { return store->zones(capture, query, slice); });
                                   data->truncated = slice.truncated || slice.rows.size() > MAX_MATCHES;
                                   for (const CaptureZone& zone : slice.rows)
                                   {
                                       data->keys.push_back({zone.startNs, zone.thread, zone.index});
                                   }
                                   std::ranges::sort(data->keys);
                                   if (data->keys.size() > MAX_MATCHES)
                                   {
                                       data->keys.resize(MAX_MATCHES);
                                   }
                                   return data;
                               });
    }

    // "3 of 41" for the zone found when every match is counted, "matches: 41" or "matches: 5,000+" otherwise; empty
    // while the count is not in
    std::string ProfilerTimeline::Impl::matchCount() const
    {
        if (!matches.ready || !matches.error.empty())
        {
            return {};
        }
        const std::string total = whole(matches.keys.size());
        if (matches.truncated)
        {
            return say("timeline.find.many", {{"total", total}});
        }
        if (findState.found)
        {
            const MatchKey key{findState.zone.startNs, findState.zone.thread, findState.zone.index};
            const auto at = std::ranges::lower_bound(matches.keys, key);
            if (at != matches.keys.end() && *at == key)
            {
                return say("timeline.find.of",
                           {{"index", whole(static_cast<u64>(at - matches.keys.begin()) + 1)}, {"total", total}});
            }
        }
        return say("timeline.find.total", {{"total", total}});
    }

    void ProfilerTimeline::injectMatches(const CaptureId capture, const u64 token, const std::string& name,
                                         const u32 count)
    {
        auto data = std::make_shared<MatchData>();
        data->capture = capture;
        data->name = name;
        for (u32 i = 0; i < count; ++i)
        {
            data->keys.push_back({static_cast<std::int64_t>(i), 0, i});
        }
        ProfilerEvent event;
        event.kind = ProfilerEvent::Kind::Task;
        event.view = TASK_VIEW;
        event.token = token;
        event.payload = std::move(data);
        onEvent(event);
    }

    // -- the capture and its data -------------------------------------------------------------------------------------

    void ProfilerTimeline::reset(const CaptureId capture)
    {
        m_impl->clearCapture();
        m_impl->capture = capture;
    }

    void ProfilerTimeline::Impl::clearCapture()
    {
        if (host.data)
        {
            for (const ProfilerSlot slot : {ProfilerSlot::View, ProfilerSlot::Hover, ProfilerSlot::Select,
                                            ProfilerSlot::Zoom, ProfilerSlot::Stats, ProfilerSlot::Find})
            {
                host.data->cancel(slot, newToken(slot)); // whatever was in flight is superseded
            }
        }
        matchGeneration->fetch_add(1); // a count under way is superseded
        matches = {};
        findBox.setText("");
        capture = 0;
        durationNs = 0;
        lanes.clear();
        marks.clear();
        marksTruncated = false;
        marksFrom = marksTo = 0;
        if (std::exchange(shownFrames, TlFrames{}).has && host.showFrames)
        {
            host.showFrames(std::nullopt, false);
        }
        contentHeight = maxScroll = 0.0f;
        viewInFlight = false;
        viewError.clear();
        fetchDue = false;
        quiet = 0;
        clearHover();
        hoverDetail = {};
        clearSelectionData();
        pending = {};
        findState = {};
        drag = {};
        wanted = {};
        ProfilerViewComponent& view = viewComponent();
        if (view.capture != 0 || view.endNs != 0 || view.laneScroll != 0.0f)
        {
            view.capture = 0;
            view.durationNs = view.startNs = view.endNs = 0;
            view.laneScroll = 0.0f;
            ++view.generation;
        }
        dirtyMain = dirtyOverlay = dirtyStats = dirtyInfo = true;
    }

    void ProfilerTimeline::Impl::clearSelectionData()
    {
        selDetail = {};
        selStatsValid = false;
        selStats = {};
        selStatsName.clear();
        statsError.clear();
        dirtyStats = dirtyOverlay = true;
    }

    void ProfilerTimeline::onLanes(const CaptureId capture, std::vector<CaptureThread> lanes,
                                   const std::int64_t durationNs)
    {
        Impl& impl = *m_impl;
        if (capture != impl.capture)
        {
            return;
        }
        impl.durationNs = std::max<std::int64_t>(durationNs, 0);
        impl.lanes.clear();
        for (CaptureThread& lane : lanes)
        {
            if (impl.lanes.size() >= MAX_LANES)
            {
                break;
            }
            lane.name = clipUtf8(lane.name, MAX_NAME_BYTES);
            impl.lanes.push_back({.info = std::move(lane)});
        }
        impl.wanted = {};
        ProfilerViewComponent& view = impl.viewComponent();
        view.capture = capture;
        view.durationNs = static_cast<u64>(impl.durationNs);
        view.startNs = view.endNs = 0;
        view.laneScroll = 0.0f;
        ++view.generation;
        impl.layoutLanes();
        impl.applyView(profview::fitWindow(0, static_cast<u64>(impl.durationNs), static_cast<u64>(impl.durationNs)));
        impl.fetchDue = true;
        impl.requestSelectionDetail();
    }

    void ProfilerTimeline::onSelection()
    {
        Impl& impl = *m_impl;
        const ProfilerSelectionComponent& sel = impl.selection();
        const bool has = sel.hasZone && sel.capture == impl.capture && impl.capture != 0;
        if (impl.findState.cursorValid &&
            !(sel.hasZone && sel.zoneLane == impl.findState.zone.thread && sel.zoneIndex == impl.findState.zone.index))
        {
            impl.findState.cursorValid = false; // another zone was picked: searches go from that one
        }
        const bool same = has == impl.selDetailAsked &&
            (!has || (sel.zoneLane == impl.askedLane && sel.zoneIndex == impl.askedIndex));
        if (!same)
        {
            impl.clearSelectionData();
            impl.requestSelectionDetail();
        }
    }

    void ProfilerTimeline::Impl::requestSelectionDetail()
    {
        const ProfilerSelectionComponent& sel = selection();
        selDetailAsked = sel.hasZone && sel.capture == capture && capture != 0 && !lanes.empty();
        if (!selDetailAsked)
        {
            if (host.data)
            {
                host.data->cancel(ProfilerSlot::Select, newToken(ProfilerSlot::Select));
                host.data->cancel(ProfilerSlot::Stats, newToken(ProfilerSlot::Stats));
            }
            return;
        }
        askedLane = sel.zoneLane;
        askedIndex = sel.zoneIndex;
        if (host.data)
        {
            host.data->requestDetail(ProfilerSlot::Select, capture, askedLane, askedIndex,
                                     newToken(ProfilerSlot::Select), 0);
            host.data->cancel(ProfilerSlot::Stats, newToken(ProfilerSlot::Stats));
        }
    }

    void ProfilerTimeline::setLocale(const std::string_view locale)
    {
        if (m_impl->locale != locale)
        {
            m_impl->locale = std::string(locale);
            m_impl->dirtyMain = m_impl->dirtyStats = m_impl->dirtyInfo = true;
        }
    }

    u32 ProfilerTimeline::Impl::internName(const std::string_view name)
    {
        const std::string clipped = clipUtf8(name, MAX_NAME_BYTES);
        if (const auto it = nameIds.find(std::string_view(clipped)); it != nameIds.end())
        {
            return it->second;
        }
        if (names.size() >= MAX_NAMES)
        {
            return 0;
        }
        u32 hash = 2166136261u;
        for (const char c : clipped)
        {
            hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
        }
        const u32 id = static_cast<u32>(names.size());
        names.push_back({clipped, static_cast<u8>(hash % TL_OVERFLOW_NAME)});
        nameIds.emplace(clipped, id);
        return id;
    }

    bool ProfilerTimeline::Impl::laneVisible(const TlLane& lane, const float margin) const
    {
        if (lane.rows == 0)
        {
            return false;
        }
        const float y = RULER + lane.top - viewConst().laneScroll;
        return y + lane.height > RULER - margin && y < canvasSize.y + margin;
    }

    bool ProfilerTimeline::Impl::laneNeeds(const TlLane& lane, const profview::Window& w) const
    {
        if (!lane.loaded || static_cast<std::int64_t>(w.start) < lane.from ||
            static_cast<std::int64_t>(w.end) > lane.to)
        {
            return true;
        }
        // the view shows more pixels per ns than the spans were cut for
        return plotWidth() * static_cast<double>(lane.to - lane.from) >
            1.5 * lane.columns * static_cast<double>(w.width());
    }

    void ProfilerTimeline::Impl::fetchIfDue()
    {
        SPITE_ZONE("ProfilerTimelineFetch");
        const profview::Window w = window();
        if (!fetchDue || !w.ok || !host.data || canvasSize.x <= GUTTER + 8.0f || lanes.empty())
        {
            return;
        }
        const bool any = std::ranges::any_of(lanes, [](const TlLane& lane) { return lane.loaded; });
        if (any && quiet < QUIET_FRAMES)
        {
            return;
        }
        ProfilerViewRequest request;
        request.capture = capture;
        for (sizet i = 0; i < lanes.size(); ++i)
        {
            if (laneVisible(lanes[i], 160.0f) && laneNeeds(lanes[i], w))
            {
                request.lanes.push_back(static_cast<u32>(i));
            }
        }
        if (request.lanes.empty())
        {
            fetchDue = false;
            return;
        }
        const std::int64_t margin = static_cast<std::int64_t>(w.width() / 4);
        request.fromNs = std::max<std::int64_t>(0, static_cast<std::int64_t>(w.start) - margin);
        request.toNs = std::min<std::int64_t>(durationNs, static_cast<std::int64_t>(w.end) + margin);
        if (request.toNs <= request.fromNs)
        {
            request.fromNs = static_cast<std::int64_t>(w.start);
            request.toNs = static_cast<std::int64_t>(w.end);
        }
        const double columns = std::ceil(plotWidth() * static_cast<double>(request.toNs - request.fromNs) /
                                         static_cast<double>(w.width()));
        request.columns = static_cast<u32>(std::clamp<double>(columns, MIN_COLUMNS, CAPTURE_MAX_COLUMNS));
        request.maxDepth = MAX_ROWS - 1;
        fetchFrom = request.fromNs;
        fetchTo = request.toNs;
        fetchColumns = request.columns;
        viewInFlight = true;
        fetchDue = false;
        ++fetches;
        host.data->requestView(std::move(request), newToken(ProfilerSlot::View));
    }

    void ProfilerTimeline::Impl::applySpans(ProfilerEvent& event)
    {
        SPITE_ZONE("ProfilerTimelineSpans");
        if (event.lane >= lanes.size())
        {
            return;
        }
        TlLane& lane = lanes[event.lane];
        lane.spans.clear();
        lane.spans.reserve(event.spans.size());
        for (const CaptureSpan& span : event.spans)
        {
            if (span.depth < MAX_ROWS)
            {
                lane.spans.push_back({span.startNs, span.endNs, span.count, span.firstSeq, span.longestSeq,
                                      span.totalNs, span.longestNs, span.depth, internName(span.name)});
            }
        }
        lane.depthBegin.assign(MAX_ROWS + 1, 0);
        for (const TlSpan& span : lane.spans)
        {
            ++lane.depthBegin[span.depth + 1];
        }
        for (u32 d = 0; d < MAX_ROWS; ++d)
        {
            lane.depthBegin[d + 1] += lane.depthBegin[d];
        }
        lane.loaded = true;
        lane.cut = event.scanCut;
        lane.from = event.fromNs;
        lane.to = event.toNs;
        lane.columns = event.columns;
        ++spanBatches;
        dirtyMain = true;
        if (hover.has)
        {
            updateHover(hover.point);
        }
    }

    bool ProfilerTimeline::onEvent(ProfilerEvent& event)
    {
        using Kind = ProfilerEvent::Kind;
        Impl& impl = *m_impl;
        switch (event.kind)
        {
        case Kind::Marks:
        case Kind::Spans:
        case Kind::ViewDone:
            break;
        case Kind::Detail:
        case Kind::Stats:
        case Kind::Find:
            break;
        case Kind::Task:
            if (event.view != TASK_VIEW)
            {
                return false;
            }
            break;
        default:
            return false;
        }
        const auto live = [&](const ProfilerSlot slot)
        { return event.capture == impl.capture && event.token == impl.token(slot) && event.error != "superseded"; };
        switch (event.kind)
        {
        case Kind::Marks:
            if (!live(ProfilerSlot::View))
            {
                ++impl.droppedEvents;
                return true;
            }
            impl.marks.clear();
            for (const CaptureFrame& frame : event.marks)
            {
                impl.marks.push_back({frame.startNs, frame.endNs, frame.index});
            }
            impl.marksTruncated = event.truncated;
            impl.marksFrom = impl.fetchFrom;
            impl.marksTo = impl.fetchTo;
            impl.dirtyMain = impl.dirtyFrames = true;
            return true;
        case Kind::Spans:
            if (!live(ProfilerSlot::View))
            {
                ++impl.droppedEvents;
                return true;
            }
            impl.applySpans(event);
            return true;
        case Kind::ViewDone:
            if (event.token == impl.token(ProfilerSlot::View) && event.capture == impl.capture)
            {
                impl.viewInFlight = false;
                impl.viewError = event.error == "superseded" ? std::string{} : clipUtf8(event.error, 160);
                // whatever the window moved to meanwhile; after a failure only a changed view, size or capture asks
                // again (a retry per frame would hammer a store that already said no)
                impl.fetchDue = impl.viewError.empty();
                impl.dirtyInfo = true;
                ++impl.viewDones;
            }
            else
            {
                ++impl.droppedEvents;
            }
            return true;
        case Kind::Detail:
            ++impl.details;
            if (event.token == impl.token(ProfilerSlot::Hover) && live(ProfilerSlot::Hover))
            {
                if (event.error.empty())
                {
                    impl.hoverDetail = {true, std::move(event.detail)};
                    impl.buildTooltip();
                    if (impl.tip.isShown())
                    {
                        impl.tip.label().setText(impl.tooltip);
                    }
                }
            }
            else if (event.token == impl.token(ProfilerSlot::Select) && live(ProfilerSlot::Select))
            {
                if (event.error.empty())
                {
                    impl.selDetail = {true, std::move(event.detail)};
                    impl.statsError.clear();
                    if (impl.host.data)
                    {
                        impl.host.data->requestStats(impl.capture, impl.selDetail.detail.zone.name,
                                                     impl.newToken(ProfilerSlot::Stats));
                    }
                }
                else
                {
                    impl.statsError = clipUtf8(event.error, 160);
                }
                impl.dirtyStats = impl.dirtyOverlay = true;
            }
            else if (event.token == impl.token(ProfilerSlot::Zoom) && live(ProfilerSlot::Zoom))
            {
                impl.pending.zoom = false;
                if (event.error.empty())
                {
                    impl.zoomToZone(event.detail.zone);
                }
                else
                {
                    impl.viewError = clipUtf8(event.error, 160);
                    impl.dirtyInfo = true;
                }
            }
            return true;
        case Kind::Stats:
            if (live(ProfilerSlot::Stats))
            {
                if (event.error.empty())
                {
                    impl.selStats = std::move(event.zoneStats);
                    impl.selStatsName = std::move(event.name);
                    impl.selStatsValid = true;
                    impl.statsError.clear();
                }
                else
                {
                    impl.statsError = clipUtf8(event.error, 160);
                }
                impl.dirtyStats = true;
            }
            return true;
        case Kind::Task:
            if (const auto* data = static_cast<const MatchData*>(event.payload.get()); data &&
                event.token == impl.matchValue && event.error.empty() && data->capture == impl.capture &&
                data->name == impl.matches.name)
            {
                impl.matches.ready = true;
                impl.matches.error = clipUtf8(data->error, 160);
                impl.matches.truncated = data->truncated;
                impl.matches.keys = data->keys;
                impl.dirtyInfo = true;
            }
            return true;
        case Kind::Find:
            if (event.capture == impl.capture && event.token == impl.token(ProfilerSlot::Find) &&
                event.error != "superseded")
            {
                ++impl.searchesDone;
                if (!event.error.empty())
                {
                    impl.findState.state = "error";
                    impl.findState.error = clipUtf8(event.error, 160);
                }
                else if (!event.found.found)
                {
                    impl.findState.state = "notFound";
                }
                else
                {
                    impl.findState.state = "found";
                    impl.findState.found = true;
                    impl.findState.cursorValid = true;
                    impl.findState.zone = event.found.zone;
                    impl.findState.zone.name = clipUtf8(impl.findState.zone.name, MAX_NAME_BYTES);
                    impl.host.send(line("profilerSelect",
                                        Json{{"capture", impl.capture},
                                             {"zoneLane", event.found.zone.thread},
                                             {"zoneIndex", event.found.zone.index}}));
                    if (impl.findState.zoomOnFound)
                    {
                        impl.zoomToZone(event.found.zone);
                    }
                }
                impl.dirtyInfo = true;
            }
            return true;
        default:
            return false;
        }
    }

    // -- input
    // ---------------------------------------------------------------------------------------------------------

    const TlLane* ProfilerTimeline::Impl::laneAt(const float y, float& laneY) const
    {
        const float scroll = viewConst().laneScroll;
        for (const TlLane& lane : lanes)
        {
            const float top = RULER + lane.top - scroll;
            if (lane.rows != 0 && y >= top && y < top + lane.height)
            {
                laneY = top;
                return &lane;
            }
        }
        return nullptr;
    }

    void ProfilerTimeline::Impl::clearHover()
    {
        if (hover.has || hover.onSpan)
        {
            dirtyOverlay = true;
        }
        hover = {};
        tooltip.clear();
    }

    void ProfilerTimeline::Impl::updateHover(const Vec2 local)
    {
        const profview::Window w = window();
        float laneY = 0.0f;
        const TlLane* lane =
            w.ok && local.x >= GUTTER && local.x < canvasSize.x && local.y >= RULER ? laneAt(local.y, laneY) : nullptr;
        if (!lane)
        {
            clearHover();
            return;
        }
        TlHover next;
        next.has = true;
        next.point = local;
        next.lane = lane->info.index;
        const double plot = plotWidth();
        next.ns = static_cast<std::int64_t>(
            w.start +
            profview::mulDivRound(
                static_cast<u64>(std::llround(std::clamp<double>(local.x - GUTTER, 0.0, plot) * 16.0)), w.width(),
                static_cast<u64>(std::llround(plot * 16.0))));
        const float rowY = local.y - laneY - LANE_HEADER;
        if (rowY >= 0.0f && static_cast<u32>(rowY / ROW) < lane->rows && lane->loaded)
        {
            const u32 depth = static_cast<u32>(rowY / ROW);
            next.depth = depth;
            const double scale = plot / static_cast<double>(w.width());
            const auto xOf = [&](const std::int64_t t)
            { return static_cast<double>(t - static_cast<std::int64_t>(w.start)) * scale; };
            const double x = local.x - GUTTER;
            const TlSpan* begin = lane->spans.data() + lane->depthBegin[depth];
            const TlSpan* end = lane->spans.data() + lane->depthBegin[depth + 1];
            // the spans around the pointer's time: the last starting at or before it, the one before and the next
            const std::ptrdiff_t after = std::ranges::upper_bound(begin, end, next.ns, {}, &TlSpan::start) - begin;
            for (const std::ptrdiff_t i : {after - 1, after, after - 2})
            {
                if (i < 0 || i >= end - begin)
                {
                    continue;
                }
                const TlSpan& span = begin[i];
                const double x0 = xOf(span.start), x1 = std::max(xOf(span.end), x0 + 1.0);
                if (x >= x0 - 1.0 && x <= x1 + 1.0)
                {
                    next.onSpan = true;
                    next.span = span;
                    break;
                }
            }
        }
        const bool sameTarget = hover.has && hover.onSpan == next.onSpan && hover.lane == next.lane &&
            (!next.onSpan || (hover.span.firstSeq == next.span.firstSeq && hover.depth == next.depth));
        next.stable = sameTarget ? hover.stable : 0;
        next.wantDetail = next.onSpan && next.span.count == 1;
        if (!sameTarget)
        {
            hoverDetail = {};
            if (host.data)
            {
                host.data->cancel(ProfilerSlot::Hover, newToken(ProfilerSlot::Hover));
            }
        }
        const bool moved = !hover.has || hover.point != next.point || !sameTarget;
        hover = next;
        if (moved)
        {
            dirtyOverlay = true;
        }
        if (!sameTarget)
        {
            buildTooltip(); // the query has the text whether or not the popup is up yet
            if (tip.isShown())
            {
                tip.label().setText(tooltip);
            }
        }
    }

    void ProfilerTimeline::Impl::click()
    {
        if (!hover.has || capture == 0)
        {
            return;
        }
        if (hover.onSpan)
        {
            host.send(line("profilerSelect",
                           Json{{"capture", capture},
                                {"zoneLane", hover.lane},
                                {"zoneIndex", hover.span.count == 1 ? hover.span.firstSeq : hover.span.longestSeq}}));
        }
        else
        {
            host.send(line("profilerSelect", Json{{"capture", capture}, {"clear", "zone"}}));
        }
    }

    void ProfilerTimeline::Impl::onPointer(const UiCanvasPointer& event)
    {
        using K = UiCanvasPointerKind;
        switch (event.kind)
        {
        case K::Enter:
        case K::Move:
            if (!drag.active)
            {
                updateHover(event.local);
            }
            break;
        case K::Leave:
            if (!drag.active)
            {
                clearHover();
            }
            break;
        case K::Down:
            dragged = false;
            break;
        case K::DragStart:
        case K::DragMove:
            {
                const profview::Window current = gestureWindow();
                if (!current.ok)
                {
                    break;
                }
                if (!drag.active)
                {
                    drag = {true, current, gestureScroll(), {}};
                    dragged = true;
                    clearHover();
                }
                drag.total += event.delta;
                const double plot = plotWidth();
                const std::int64_t dxq = std::llround(static_cast<double>(drag.total.x) * profview::PIXEL_SCALE);
                const u64 dt = profview::mulDivRound(static_cast<u64>(std::abs(dxq)), drag.window.width(),
                                                     static_cast<u64>(std::llround(plot * profview::PIXEL_SCALE)));
                // the content follows the pointer: dragging right shows earlier times
                const std::int64_t delta = dxq > 0 ? -static_cast<std::int64_t>(std::min(dt, profview::MAX_NS))
                                                   : static_cast<std::int64_t>(std::min(dt, profview::MAX_NS));
                want(profview::panWindow(drag.window, delta, static_cast<u64>(durationNs)), drag.scroll - drag.total.y);
                break;
            }
        case K::DragEnd:
            drag.active = false;
            break;
        case K::Click:
            if (event.button == 0 && !dragged)
            {
                updateHover(event.local);
                click();
            }
            break;
        case K::Wheel:
            {
                // The wheel over the canvas always zooms and never scrolls the page (the canvas is most of it): scroll
                // with the scroll bar, the margins around the canvas or profilerScroll.
                std::int32_t notches = static_cast<std::int32_t>(std::lround(event.wheel.y));
                if (notches == 0 && event.wheel.y != 0.0f)
                {
                    notches = event.wheel.y > 0.0f ? 1 : -1;
                }
                zoomAt(event.local.x - GUTTER,
                       std::clamp<std::int32_t>(notches, -static_cast<std::int32_t>(MAX_NOTCHES), MAX_NOTCHES));
                if (event.wheel.x != 0.0f && std::isfinite(event.wheel.x))
                {
                    const profview::Window w = gestureWindow();
                    const double step = std::clamp<double>(event.wheel.x, -double(MAX_NOTCHES), double(MAX_NOTCHES)) *
                        static_cast<double>(w.width() / 10);
                    if (w.ok)
                    {
                        want(profview::panWindow(w, static_cast<std::int64_t>(step), static_cast<u64>(durationNs)));
                    }
                }
                break;
            }
        default:
            break;
        }
    }

    bool ProfilerTimeline::Impl::onKey(const u32 scancode, const u8 modifiers)
    {
        const profview::Window w = gestureWindow();
        if (!w.ok)
        {
            return false;
        }
        const bool ctrl = (modifiers & UiKeyModifier::CTRL) != 0;
        const bool shift = (modifiers & UiKeyModifier::SHIFT) != 0;
        const double plot = plotWidth();
        const double anchor = hover.has ? std::clamp<double>(hover.point.x - GUTTER, 0.0, plot) : plot * 0.5;
        // false when there is nothing to search for: the key is left to others (the gamepad's tab intents)
        const auto search = [this](const char* direction)
        {
            if (findState.name.empty() || !host.data)
            {
                return false;
            }
            startFind(findState.name, direction);
            return true;
        };
        switch (scancode)
        {
        case Key::LEFT:
        case Key::RIGHT:
            {
                const std::int64_t step = static_cast<std::int64_t>(w.width() / (shift ? 2 : 10));
                want(profview::panWindow(w, scancode == Key::LEFT ? -step : step, static_cast<u64>(durationNs)));
                return true;
            }
        case Key::UP:
        case Key::EQUALS:
        case Key::KP_PLUS:
            zoomAt(anchor, 1);
            return true;
        case Key::DOWN:
        case Key::MINUS:
        case Key::KP_MINUS:
            zoomAt(anchor, -1);
            return true;
        case Key::HOME:
            want(profview::fitWindow(0, static_cast<u64>(durationNs), static_cast<u64>(durationNs)));
            return true;
        case Key::PAGE_DOWN:
        case Key::PAGE_UP:
            if (ctrl) // the gamepad's next/previous tab
            {
                return search(scancode == Key::PAGE_DOWN ? "next" : "previous");
            }
            {
                const float page = std::max(ROW, canvasSize.y - RULER) * 0.8f;
                setScroll(gestureScroll() + (scancode == Key::PAGE_DOWN ? page : -page));
            }
            return true;
        case Key::F:
            if (selection().hasZone && selection().capture == capture)
            {
                host.send(line("profilerView", Json{{"zoomToZone", true}}));
                return true;
            }
            return false;
        case Key::N:
            return search(shift ? "previous" : "next");
        default:
            return false;
        }
    }

    // -- the panel's per-frame call
    // --------------------------------------------------------------------------------------

    void ProfilerTimeline::update(const bool shown)
    {
        Impl& impl = *m_impl;
        if (!shown)
        {
            impl.shown = false;
            return;
        }
        if (!impl.shown)
        {
            impl.shown = true;
            impl.dirtyMain = impl.dirtyOverlay = impl.dirtyStats = impl.dirtyInfo = impl.dirtyHistogram = true;
            impl.dirtyFrames = true;
            impl.canvasSize = {}; // measured again: the panel may have been resized while hidden
        }
        const UiHierarchyManager& hierarchy = impl.host.scope->hierarchy();
        const Vec2 size =
            hierarchy.absoluteRectOf(impl.canvas.entity()).size * (1.0f / std::max(hierarchy.layoutScale(), 1e-6f));
        for (StatusText* item : impl.statusTips.items())
        {
            item->tick();
        }
        if (size != impl.canvasSize)
        {
            impl.canvasSize = size;
            place(impl.overlay, Vec2{0.0f, 0.0f}, size);
            impl.layoutLanes();
            impl.fetchDue = true;
            impl.dirtyMain = impl.dirtyOverlay = true;
        }
        if (impl.wanted.has && impl.wanted.sent)
        {
            impl.wanted = {}; // its command ran at the start of this frame
        }
        impl.sendWanted();
        if (impl.quiet < 1000)
        {
            ++impl.quiet;
        }
        if (impl.hover.has && impl.hover.wantDetail && !impl.hoverDetail.valid && impl.host.data &&
            ++impl.hover.stable == HOVER_FRAMES)
        {
            impl.host.data->requestDetail(ProfilerSlot::Hover, impl.capture, impl.hover.lane, impl.hover.span.firstSeq,
                                          impl.newToken(ProfilerSlot::Hover), 0);
        }
        if (impl.hoverStale)
        {
            impl.hoverStale = false;
            if (impl.hover.has)
            {
                impl.updateHover(impl.hover.point);
            }
        }
        impl.followTextMetrics();
        impl.fetchIfDue();
        if (impl.dirtyFrames)
        {
            impl.pushFrames();
        }
        const Vec2 histogramSize =
            hierarchy.absoluteRectOf(impl.histogram.entity()).size * (1.0f / std::max(hierarchy.layoutScale(), 1e-6f));
        if (histogramSize != impl.histogramSize)
        {
            impl.histogramSize = histogramSize;
            impl.dirtyHistogram = true;
        }
        if (impl.dirtyMain)
        {
            impl.drawMain();
        }
        if (impl.dirtyOverlay)
        {
            impl.drawOverlay();
        }
        if (impl.dirtyStats)
        {
            impl.showStats();
        }
        if (impl.dirtyHistogram)
        {
            impl.drawHistogram();
        }
        if (impl.dirtyInfo)
        {
            impl.showInfo();
        }
    }
} // namespace spite
