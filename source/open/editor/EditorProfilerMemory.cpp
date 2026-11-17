#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

#include "EditorPanelUi.hpp"
#include "ProfilerViews.hpp"
#include "ui/UiCanvas.hpp"

namespace spite
{
    using namespace panelui;

    namespace
    {
        constexpr u64 MAX_EVENTS = 2000; // the biggest allocations drawn: bounded whatever the capture holds
        constexpr u32 MAX_ERROR_CHARS = 160;
        constexpr u32 STATE_EVENTS = 32;
        constexpr float AXIS_LEFT = 52.0f, AXIS_RIGHT = 8.0f, PLOT_TOP = 18.0f, PLOT_BOTTOM = 18.0f;
        constexpr std::array<std::string_view, 3> SPACE_CHOICES{"all", "ram", "gpu"};
        constexpr const char* SPACE_NAMES[2] = {"ram", "gpu"};
        constexpr const char* KIND_NAMES[3] = {"alloc", "free", "grow"};
        // what the panel shows for them (profilerState keeps the names above)
        constexpr const char* SPACE_KEYS[2] = {"memory.space.ram", "memory.space.gpu"};
        constexpr const char* KIND_KEYS[3] = {"memory.kind.alloc", "memory.kind.free", "memory.kind.grow"};

        struct Data
        {
            CaptureId capture = 0;
            CaptureMemory memory;
            std::int64_t durationNs = 0;
            std::string error;
        };

        class MemoryView final : public ProfilerView
        {
        public:
            explicit MemoryView(const ProfilerViewHost& host) :
                m_host(host), m_generation(std::make_shared<std::atomic<u64>>(0)),
                m_theme(*host.scope, [this] { m_dirty = true; })
            {
                UiScope& scope = *m_host.scope;
                DensityFollower& density = *m_host.density;
                m_root = stackPanel(scope, "profiler.memory", Vec2{}, UiLayoutDirection::Column, UiLayoutAlign::Stretch,
                                    true);
                m_root.setLayoutItem(fillItem());
                m_summary = density.track(readOnlyText(scope, "profiler.memory.summary", barDesc(Vec2{})), HEIGHT_ROW);
                m_summary.setParent(m_root);
                m_summary.setAccessibleNameKey(profilerKey("memory.summary.name"));
                m_totals = density.track(readOnlyText(scope, "profiler.memory.totals", barDesc(Vec2{})), HEIGHT_ROW);
                m_totals.setParent(m_root);
                m_totals.setAccessibleNameKey(profilerKey("memory.totals.name"));
                Panel bar = density.track(stackPanel(scope, "profiler.memory.bar", Vec2{}, UiLayoutDirection::Row),
                                          HEIGHT_CONTROL);
                bar.setParent(m_root);
                static constexpr const char* LABELS[3] = {"memory.filter.all", "memory.filter.ram",
                                                          "memory.filter.gpu"};
                static constexpr const char* NAMES[3] = {"memory.filter.all.name", "memory.filter.ram.name",
                                                         "memory.filter.gpu.name"};
                for (u32 i = 0; i < 3; ++i)
                {
                    m_filters[i] = density.track(
                        scope.createButton("profiler.memory.filter", "", barDesc(Vec2{64.0f, 0.0f})), HEIGHT_ROW);
                    m_filters[i].setLocalizedText(profilerKey(LABELS[i]));
                    m_filters[i].setParent(bar);
                    m_filters[i].setAccessibleNameKey(profilerKey(NAMES[i]));
                    const std::string space(SPACE_CHOICES[i]);
                    m_filters[i].onClick(
                        [this, space] { m_host.send({{"command", "profilerMemory"}, {"args", {{"space", space}}}}); });
                }
                markFilters();
                Panel body =
                    stackPanel(scope, "profiler.memory.body", Vec2{}, UiLayoutDirection::Row, UiLayoutAlign::Stretch);
                body.setLayoutItem(fillItem());
                body.setParent(m_root);
                m_table = std::make_unique<Table>(
                    scope,
                    TableDesc{.id = "profiler.memoryTags",
                              .columns = {50.0f, 150.0f, 80.0f, 80.0f},
                              .cells = 5,
                              .read =
                                  [this](const u32 row, const std::span<std::string> cells, std::string&)
                              {
                                  const CaptureMemoryTag& tag = m_data.memory.tags[m_rows[row]];
                                  cells[0] = m_host.text(SPACE_KEYS[tag.space]);
                                  cells[1] = profilerSafeText(tag.tag);
                                  cells[2] = m_host.whole(tag.count);
                                  cells[3] = m_host.bytes(tag.bytes);
                                  cells[4] = m_host.bytes(tag.maxBytes);
                              },
                              .headers = {{profilerKey("memory.col.space")},
                                          {profilerKey("memory.col.tag")},
                                          {profilerKey("memory.col.count")},
                                          {profilerKey("memory.col.bytes")},
                                          {profilerKey("memory.col.max")}},
                              .lastMin = 70.0f});
                Widget list = m_table->widget();
                list.setParent(body);
                list.setLayoutItem(fillItem());
                m_table->list.setAccessibleNameKey(profilerKey("memory.list.name"));
                UiCanvasDesc canvasDesc;
                canvasDesc.rect = topLeft();
                canvasDesc.rect.layoutItem = fillItem();
                canvasDesc.maxPrimitives = 20000;
                m_canvas = scope.createCanvas("profiler.memory.events", canvasDesc);
                m_canvas.setParent(body);
                m_canvas.setAccessibleNameKey(profilerKey("memory.events.name"));
                m_canvas.onPointer([this](const UiCanvasPointer& pointer) { pointerEvent(pointer); });
            }

            [[nodiscard]] Widget root() const override { return m_root; }

            void activate(const bool active) override
            {
                m_active = active;
                if (!active)
                {
                    m_generation->fetch_add(1);
                    m_loading = false;
                    return;
                }
                if (m_capture != 0 && !m_loaded)
                {
                    request();
                }
                m_dirty = true;
            }

            void captureChanged(const CaptureId capture, const CaptureInfo& info) override
            {
                m_capture = capture;
                m_durationNs = capture != 0 ? info.durationNs : 0;
                m_generation->fetch_add(1);
                m_loading = false;
                m_loaded = false;
                m_data = {};
                m_hover.reset();
                rebuildRows();
                m_dirty = true;
                if (m_active && capture != 0)
                {
                    request();
                }
            }

            void selectionChanged(const ProfilerSelectionComponent&) override {}

            bool take(ProfilerEvent& event) override
            {
                if (event.token != m_value || event.token != m_generation->load())
                {
                    return true;
                }
                m_loading = false;
                if (const auto data = std::static_pointer_cast<Data>(event.payload))
                {
                    m_data = std::move(*data);
                    m_loaded = m_data.error.empty();
                    m_data.error = profilerSafeText(m_data.error, MAX_ERROR_CHARS);
                }
                else if (!event.error.empty() && event.error != "superseded")
                {
                    m_data = {};
                    m_data.error = profilerSafeText(event.error, MAX_ERROR_CHARS);
                }
                rebuildRows();
                m_dirty = true;
                return true;
            }

            void update() override
            {
                const Vec2 size = m_host.laidOutSize(m_canvas);
                if (*m_host.locale != m_localeUsed)
                {
                    m_localeUsed = *m_host.locale;
                    m_table->rebindVisible();
                    m_dirty = true;
                }
                if (m_dirty || size != m_drawnSize)
                {
                    redraw(size);
                }
            }

            [[nodiscard]] bool busy() const override { return m_loading; }

            void writeState(IFormatWriter& out) override
            {
                out.writeBool("loading", m_loading);
                out.writeBool("loaded", m_loaded);
                out.writeString("error", m_data.error);
                out.writeString("space", SPACE_CHOICES[m_space + 1]);
                out.beginObject("list");
                writeTableStats(out, *m_table);
                out.endObject();
                out.writeBool("hasTotals", m_data.memory.hasTotals);
                for (u32 space = 0; space < 2; ++space)
                {
                    const CaptureMemorySpace& totals = m_data.memory.spaces[space];
                    out.beginObject(SPACE_NAMES[space]);
                    out.writeBool("present", totals.present);
                    out.writeU64("allocCount", totals.allocCount);
                    out.writeU64("allocBytes", totals.allocBytes);
                    out.writeU64("freeCount", totals.freeCount);
                    out.writeU64("freeBytes", totals.freeBytes);
                    out.writeU64("netEndBytes", totals.netEndBytes);
                    out.writeU64("netPeakBytes", totals.netPeakBytes);
                    out.endObject();
                }
                out.beginArray("tags", static_cast<u32>(m_rows.size()));
                for (const u32 row : m_rows)
                {
                    const CaptureMemoryTag& tag = m_data.memory.tags[row];
                    out.beginObject({});
                    out.writeString("space", SPACE_NAMES[tag.space]);
                    out.writeString("tag", tag.tag);
                    out.writeU64("count", tag.count);
                    out.writeU64("bytes", tag.bytes);
                    out.writeU64("maxBytes", tag.maxBytes);
                    out.endObject();
                }
                out.endArray();
                out.writeU64("eventsInCapture", m_data.memory.eventsInWindow);
                out.writeU64("eventsDropped", m_data.memory.eventsDropped);
                out.writeU64("eventsDrawn", m_drawn);
                out.writeU64("maxEvents", MAX_EVENTS);
                const u64 listed = std::min<u64>(m_data.memory.events.size(), STATE_EVENTS);
                out.beginArray("events", static_cast<u32>(listed));
                for (u64 i = 0; i < listed; ++i)
                {
                    const CaptureMemoryEvent& event = m_data.memory.events[i];
                    out.beginObject({});
                    out.writeU64("tNs", static_cast<u64>(std::max<std::int64_t>(0, event.tNs)));
                    out.writeString("space", SPACE_NAMES[event.space]);
                    out.writeString("kind", KIND_NAMES[event.kind]);
                    out.writeU64("bytes", event.bytes);
                    out.writeString("tag", event.tag);
                    out.endObject();
                }
                out.endArray();
            }

            void registerCommands() override
            {
                m_host.control->registerViewCommand(
                    "profilerMemory", m_host.owner,
                    "filters the Profiler memory view's tag table and event plot to RAM, GPU or both. The totals are "
                    "the capture's (the zones report's memory section; profileMemory answers the same numbers)",
                    {requiredArg("space", ArgKind::String, "which memory space to show",
                                 std::span<const std::string_view>(SPACE_CHOICES))},
                    [this](const ControlArgs& args, IFormatWriter& out)
                    {
                        const std::string space = args.string("space").value_or("");
                        const auto it = std::ranges::find(SPACE_CHOICES, std::string_view(space));
                        if (it == SPACE_CHOICES.end())
                        {
                            return std::string("space is all, ram or gpu");
                        }
                        m_space = static_cast<i32>(it - SPACE_CHOICES.begin()) - 1;
                        markFilters();
                        rebuildRows();
                        m_dirty = true;
                        out.writeString("space", space);
                        out.writeU64("tags", m_rows.size());
                        return std::string{};
                    });
            }

        private:
            void request()
            {
                if (!m_host.data || m_capture == 0)
                {
                    return;
                }
                m_loading = true;
                m_value = m_generation->fetch_add(1) + 1;
                CaptureStore* store = m_host.store;
                const CaptureId capture = m_capture;
                const std::int64_t duration = m_durationNs;
                m_host.data->requestTask(
                    static_cast<u32>(ProfilerViewId::Memory), m_generation, m_value,
                    [store, capture, duration](const CaptureCancelFn& cancelled, std::string&) -> std::shared_ptr<void>
                    {
                        auto data = std::make_shared<Data>();
                        data->capture = capture;
                        data->durationNs = duration;
                        data->error = profilerStoreCall(
                            cancelled, [&] { return store->memory(capture, {}, MAX_EVENTS, data->memory); });
                        return data;
                    });
            }

            // the chosen space reads as selected to a reader of uiTree
            void markFilters()
            {
                for (u32 i = 0; i < 3; ++i)
                {
                    showSelected(m_filters[i], static_cast<i32>(i) - 1 == m_space);
                }
            }

            void rebuildRows()
            {
                m_rows.clear();
                for (u32 i = 0; i < m_data.memory.tags.size(); ++i)
                {
                    if (m_space < 0 || m_data.memory.tags[i].space == m_space)
                    {
                        m_rows.push_back(i);
                    }
                }
                m_table->setRowCount(static_cast<u32>(m_rows.size()));
                m_table->rebindVisible();
            }

            [[nodiscard]] std::string spaceLine(const u32 space) const
            {
                const CaptureMemorySpace& totals = m_data.memory.spaces[space];
                if (space == 0)
                {
                    return m_host.text("memory.line.ram",
                                       {{"count", m_host.whole(totals.allocCount)},
                                        {"bytes", m_host.bytes(totals.allocBytes)},
                                        {"peak", m_host.bytes(totals.netPeakBytes)}});
                }
                return m_host.text("memory.line.gpu",
                                   {{"count", m_host.whole(totals.allocCount)},
                                    {"bytes", m_host.bytes(totals.allocBytes)},
                                    {"peak", m_host.bytes(totals.netPeakBytes)},
                                    {"freed", m_host.bytes(totals.freeBytes)},
                                    {"freeCount", m_host.whole(totals.freeCount)},
                                    {"end", m_host.bytes(totals.netEndBytes)}});
            }

            void redraw(const Vec2 size)
            {
                m_dirty = false;
                m_drawnSize = size;
                m_points.clear();
                m_drawn = 0;
                std::string summary, totals;
                if (m_capture == 0)
                {
                    summary = m_host.text("status.noCapture");
                }
                else if (!m_data.error.empty())
                {
                    summary = m_host.text("status.error", {{"error", m_data.error}});
                }
                else if (!m_loaded)
                {
                    summary = m_host.text("memory.loading");
                }
                else if (!m_data.memory.hasTotals)
                {
                    summary = m_host.text("memory.noTotals", {{"count", m_host.whole(m_data.memory.eventsInWindow)}});
                }
                else
                {
                    summary = spaceLine(0);
                    totals = spaceLine(1);
                }
                show(m_summary, summary);
                show(m_totals, totals);
                if (size.x <= AXIS_LEFT + AXIS_RIGHT + 8.0f || size.y <= PLOT_TOP + PLOT_BOTTOM + 8.0f)
                {
                    m_canvas.clear();
                    return;
                }
                const float width = size.x - AXIS_LEFT - AXIS_RIGHT;
                const float height = size.y - PLOT_TOP - PLOT_BOTTOM;
                m_canvas.update(
                    [&](UiCanvasPainter& painter)
                    {
                        if (!m_loaded)
                        {
                            return;
                        }
                        const UiScope& scope = *m_host.scope;
                        const u32 text = canvasColor(scope, "canvas.text");
                        const u32 axis = canvasColor(scope, "canvas.axis");
                        const u32 baseline = canvasColor(scope, "canvas.series.a");
                        const u32 candidate = canvasColor(scope, "canvas.series.b");
                        const u32 released = canvasColor(scope, "canvas.series.e");
                        painter.rect(Vec2{AXIS_LEFT, PLOT_TOP + height}, Vec2{width, 1.0f}, axis);
                        painter.rect(Vec2{AXIS_LEFT, PLOT_TOP}, Vec2{1.0f, height}, axis);
                        u64 biggest = 0;
                        for (const CaptureMemoryEvent& event : m_data.memory.events)
                        {
                            if (m_space < 0 || event.space == m_space)
                            {
                                biggest = std::max(biggest, event.bytes);
                            }
                        }
                        std::int64_t span = std::max<std::int64_t>(1, m_data.durationNs);
                        for (const CaptureMemoryEvent& event : m_data.memory.events)
                        {
                            span = std::max(span, event.tNs + 1);
                        }
                        painter.text(Vec2{2.0f, PLOT_TOP - 4.0f}, Vec2{AXIS_LEFT - 6.0f, 14.0f}, m_host.bytes(biggest),
                                     text, UiCanvasAlign::Right, 10.0f);
                        painter.text(Vec2{AXIS_LEFT, PLOT_TOP + height + 2.0f}, Vec2{60.0f, 14.0f}, "0", text,
                                     UiCanvasAlign::Left, 10.0f);
                        painter.text(Vec2{AXIS_LEFT + width - 80.0f, PLOT_TOP + height + 2.0f}, Vec2{80.0f, 14.0f},
                                     m_host.ms(static_cast<double>(span)), text, UiCanvasAlign::Right, 10.0f);
                        if (biggest == 0)
                        {
                            painter.text(Vec2{AXIS_LEFT + 8.0f, PLOT_TOP + height * 0.4f}, Vec2{width, 16.0f},
                                         m_host.text("memory.noEvents"), text, UiCanvasAlign::Left, 11.0f);
                            return;
                        }
                        const double low = 15.0; // 32 KB: the bars of the smallest events stay visible
                        const double top = std::max(low + 1.0, std::log2(static_cast<double>(biggest)));
                        for (u32 i = 0; i < m_data.memory.events.size(); ++i)
                        {
                            const CaptureMemoryEvent& event = m_data.memory.events[i];
                            if (m_space >= 0 && event.space != m_space)
                            {
                                continue;
                            }
                            const double frac = std::clamp(
                                (std::log2(static_cast<double>(std::max<u64>(1, event.bytes))) - low) / (top - low),
                                0.05, 1.0);
                            const float x = AXIS_LEFT +
                                static_cast<float>(static_cast<double>(std::max<std::int64_t>(0, event.tNs)) /
                                                   static_cast<double>(span)) *
                                    (width - 3.0f);
                            const float h = static_cast<float>(frac) * height;
                            const u8 alpha = event.kind == 1 ? 120 : 230;
                            const u32 color = withAlpha(event.kind == 2        ? released
                                                            : event.space == 0 ? baseline
                                                                               : candidate,
                                                        alpha);
                            painter.rect(Vec2{x, PLOT_TOP + height - h}, Vec2{3.0f, h}, color);
                            m_points.push_back({x, i});
                            ++m_drawn;
                        }
                    });
            }

            void pointerEvent(const UiCanvasPointer& pointer)
            {
                if (pointer.kind == UiCanvasPointerKind::Leave)
                {
                    if (m_hover)
                    {
                        m_hover.reset();
                        showHover();
                    }
                    return;
                }
                if (pointer.kind != UiCanvasPointerKind::Move && pointer.kind != UiCanvasPointerKind::Enter)
                {
                    return;
                }
                std::optional<u32> best;
                float distance = 5.0f;
                for (const auto& [x, index] : m_points)
                {
                    if (const float d = std::fabs(pointer.local.x - (x + 1.5f)); d <= distance)
                    {
                        distance = d;
                        best = index;
                    }
                }
                if (best != m_hover)
                {
                    m_hover = best;
                    showHover();
                }
            }

            void showHover()
            {
                if (!m_hover || *m_hover >= m_data.memory.events.size())
                {
                    show(m_totals, m_data.memory.hasTotals ? spaceLine(1) : std::string());
                    return;
                }
                const CaptureMemoryEvent& event = m_data.memory.events[*m_hover];
                show(m_totals,
                     m_host.text("memory.event",
                                 {{"space", m_host.text(SPACE_KEYS[event.space])},
                                  {"kind", m_host.text(KIND_KEYS[event.kind])},
                                  {"bytes", m_host.bytes(event.bytes)},
                                  {"tag", profilerSafeText(event.tag, 64)},
                                  {"at", m_host.ms(static_cast<double>(event.tNs))}}));
            }

            ProfilerViewHost m_host;
            Panel m_root;
            TextInput m_summary, m_totals;
            std::array<Button, 3> m_filters;
            std::unique_ptr<Table> m_table;
            Canvas m_canvas;
            std::shared_ptr<std::atomic<u64>> m_generation;
            u64 m_value = 0;
            Data m_data;
            std::vector<u32> m_rows; // tags shown, indexes into m_data.memory.tags
            std::vector<std::pair<float, u32>> m_points; // x of each drawn event, its index
            std::optional<u32> m_hover;
            Vec2 m_drawnSize{};
            CaptureId m_capture = 0;
            std::int64_t m_durationNs = 0;
            std::string m_localeUsed;
            i32 m_space = -1; // -1 all
            u64 m_drawn = 0;
            bool m_active = false;
            bool m_loading = false;
            bool m_loaded = false;
            bool m_dirty = true;
            ThemeObserver m_theme; // after m_dirty, which its callback sets
        };
    } // namespace

    std::unique_ptr<ProfilerView> makeMemoryView(const ProfilerViewHost& host)
    {
        return std::make_unique<MemoryView>(host);
    }
} // namespace spite
