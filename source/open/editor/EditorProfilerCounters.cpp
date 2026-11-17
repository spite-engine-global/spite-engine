#include <algorithm>
#include <array>
#include <atomic>

#include "EditorPanelUi.hpp"
#include "ProfilerViews.hpp"
#include "ui/UiChart.hpp"

namespace spite
{
    using namespace panelui;

    namespace
    {
        constexpr u32 MAX_TRACKS = 4; // charts at once: each holds its own series (1 MiB) and a share of the pane
        constexpr u64 PAGE_FRAMES =
            16384; // frames of one counter per worker call (the store's 150 ms bound is far off)
        constexpr u32 MAX_ERROR_CHARS = 160;

        // what a Task of this view answers; an error travels in the payload so the view knows which request failed
        struct Payload
        {
            enum class Kind : u8
            {
                Names,
                Page,
            };
            Kind kind = Kind::Names;
            u32 slot = 0;
            std::string error;
            std::vector<CaptureCounterName> names;
            CaptureCounterSeries series;
        };

        class CounterView final : public ProfilerView
        {
        public:
            explicit CounterView(const ProfilerViewHost& host) :
                m_host(host), m_namesGeneration(std::make_shared<std::atomic<u64>>(0))
            {
                UiScope& scope = *m_host.scope;
                DensityFollower& density = *m_host.density;
                m_root = stackPanel(scope, "profiler.counters", Vec2{}, UiLayoutDirection::Row, UiLayoutAlign::Stretch,
                                    true);
                m_root.setLayoutItem(fillItem());
                Panel left = stackPanel(scope, "profiler.counters.left", Vec2{}, UiLayoutDirection::Column,
                                        UiLayoutAlign::Stretch);
                left.setLayoutItem(fillItem());
                left.setParent(m_root);
                m_status = density.track(readOnlyText(scope, "profiler.counters.status", barDesc(Vec2{})), HEIGHT_ROW);
                m_status.setParent(left);
                m_status.setAccessibleNameKey(profilerKey("counters.status.name"));
                m_table = std::make_unique<Table>(
                    scope,
                    TableDesc{.id = "profiler.counterList",
                              .columns = {150.0f, 70.0f, 80.0f},
                              .cells = 4,
                              .buttonWidth = 52.0f,
                              .read =
                                  [this](const u32 row, const std::span<std::string> cells, std::string& button)
                              {
                                  const Counter& counter = m_counters[row];
                                  const Track* track = trackOf(counter.name);
                                  cells[0] = profilerSafeText(counter.name);
                                  button = m_host.text(track ? "counters.hide" : "counters.show");
                                  cells[1] = m_host.whole(counter.samples);
                                  // the totals of a shown track, once its frames are in
                                  const bool loaded = track && track->loaded > 0;
                                  cells[2] = loaded ? m_host.whole(track->total) : m_host.none();
                                  cells[3] = loaded ? m_host.whole(track->max) : m_host.none();
                              },
                              .press =
                                  [this](const u32 row)
                              {
                                  const Counter& counter = m_counters[row];
                                  m_host.send(
                                      {{"command", "profilerCounterTrack"},
                                       {"args", {{"name", counter.name}, {"show", trackOf(counter.name) == nullptr}}}});
                              },
                              .buttonName =
                                  [this](const u32 row)
                              {
                                  const std::string name = profilerSafeText(m_counters[row].name);
                                  return m_host.text(trackOf(m_counters[row].name) ? "counters.track.hide"
                                                                                   : "counters.track.show",
                                                     {{"name", name}});
                              },
                              .headers = {{profilerKey("counters.col.name")},
                                          {profilerKey("counters.col.frames")},
                                          {profilerKey("counters.col.total")},
                                          {profilerKey("counters.col.max")}},
                              .lastMin = 60.0f});
                Widget list = m_table->widget();
                list.setParent(left);
                list.setLayoutItem(fillItem());
                m_table->list.setAccessibleNameKey(profilerKey("counters.list.name"));
                Panel tracks = stackPanel(scope, "profiler.counters.tracks", Vec2{}, UiLayoutDirection::Column,
                                          UiLayoutAlign::Stretch);
                tracks.setLayoutItem(fillItem());
                tracks.setParent(m_root);
                for (u32 slot = 0; slot < MAX_TRACKS; ++slot)
                {
                    Track& track = m_tracks[slot];
                    track.generation = std::make_shared<std::atomic<u64>>(0);
                    track.holder = scope.createPanel("profiler.counters.track",
                                                     [&]
                                                     {
                                                         UiRectDesc desc = topLeft();
                                                         desc.layout = stack(UiLayoutDirection::Column, 2.0f, {},
                                                                             UiLayoutAlign::Stretch);
                                                         desc.layoutItem = fillItem();
                                                         return desc;
                                                     }());
                    track.holder.setParent(tracks);
                    track.holder.setVisible(false);
                    track.title = scope.createLabel("profiler.counters.track.title", "", barDesc(Vec2{0.0f, 16.0f}));
                    track.title.setParent(track.holder);
                    UiChartDesc chartDesc;
                    chartDesc.rect = topLeft();
                    chartDesc.rect.layoutItem = fillItem();
                    chartDesc.textSize = 10.0f;
                    chartDesc.legend = false;
                    track.chart = scope.createChart("profiler.counters.track.chart", chartDesc);
                    track.chart.setParent(track.holder);
                    track.chart.onSelect([this](const UiChartSelection& selection)
                                         { selectFrames(selection.first, selection.last); });
                    track.chart.onPlotClick([this](const UiChartClick& click)
                                            { selectFrames(click.sample, click.sample); });
                }
            }

            [[nodiscard]] Widget root() const override { return m_root; }

            void activate(const bool active) override
            {
                m_active = active;
                if (!active)
                {
                    cancelAll();
                    return;
                }
                if (m_capture != 0 && !m_namesLoaded)
                {
                    requestNames();
                }
                for (u32 slot = 0; slot < MAX_TRACKS; ++slot)
                {
                    if (m_tracks[slot].used && !m_tracks[slot].done && m_tracks[slot].error.empty())
                    {
                        requestPage(slot);
                    }
                }
                m_mirrorDirty = true;
            }

            void captureChanged(const CaptureId capture, const CaptureInfo& info) override
            {
                m_capture = capture;
                m_frames = capture != 0 ? info.frames : 0;
                cancelAll();
                for (u32 slot = 0; slot < MAX_TRACKS; ++slot)
                {
                    releaseTrack(slot);
                }
                m_counters.clear();
                m_namesLoaded = false;
                m_error.clear();
                m_selection = {};
                updateList();
                if (m_active && capture != 0)
                {
                    requestNames();
                }
            }

            void selectionChanged(const ProfilerSelectionComponent& selection) override
            {
                m_selection = selection;
                m_mirrorDirty = true;
            }

            bool take(ProfilerEvent& event) override
            {
                const auto payload = std::static_pointer_cast<Payload>(event.payload);
                if (!payload)
                {
                    if (!isCurrent(event.token))
                    {
                        return true;
                    }
                    if (!event.error.empty() && event.error != "superseded")
                    {
                        m_error = profilerSafeText(event.error, MAX_ERROR_CHARS);
                        updateList();
                    }
                    return true;
                }
                if (payload->kind == Payload::Kind::Names)
                {
                    if (event.token != m_namesValue || event.token != m_namesGeneration->load())
                    {
                        return true;
                    }
                    m_namesLoading = false;
                    if (payload->error == "superseded")
                    {
                        return true; // the next activation asks again
                    }
                    if (!payload->error.empty())
                    {
                        m_error = profilerSafeText(payload->error, MAX_ERROR_CHARS);
                    }
                    else
                    {
                        m_counters = std::move(payload->names);
                        m_namesLoaded = true;
                        m_error.clear();
                    }
                    updateList();
                    return true;
                }
                Track& track = m_tracks[payload->slot];
                if (!track.used || event.token != track.value || event.token != track.generation->load())
                {
                    return true;
                }
                track.loading = false;
                if (payload->error == "superseded")
                {
                    return true; // cancelled by a pane switch: the next activation asks again, no error shown
                }
                if (!payload->error.empty())
                {
                    track.error = profilerSafeText(payload->error, MAX_ERROR_CHARS);
                }
                else if (payload->series.first != track.loaded || payload->series.values.empty())
                {
                    track.error = m_host.text("counters.error.pages");
                }
                else
                {
                    m_scratch.resize(payload->series.values.size());
                    for (sizet i = 0; i < m_scratch.size(); ++i)
                    {
                        m_scratch[i] = static_cast<float>(payload->series.values[i]);
                    }
                    track.chart.append(0, m_scratch);
                    track.loaded += payload->series.values.size();
                    track.total = track.total > UINT64_MAX - payload->series.total
                        ? UINT64_MAX
                        : track.total + payload->series.total;
                    track.max = std::max(track.max, payload->series.max);
                    track.done = track.loaded >= track.frames;
                    if (!track.done)
                    {
                        requestPage(payload->slot);
                    }
                    else
                    {
                        m_mirrorDirty = true;
                    }
                }
                showTitle(payload->slot);
                m_table->rebindVisible();
                return true;
            }

            void update() override
            {
                if (*m_host.locale != m_localeUsed)
                {
                    m_localeUsed = *m_host.locale; // numbers follow the Localization's locale
                    updateList();
                    for (u32 slot = 0; slot < MAX_TRACKS; ++slot)
                    {
                        if (m_tracks[slot].used)
                        {
                            showTitle(slot);
                        }
                    }
                }
                if (m_mirrorDirty)
                {
                    mirrorSelection();
                }
            }

            [[nodiscard]] bool busy() const override
            {
                return m_namesLoading ||
                    std::ranges::any_of(m_tracks, [](const Track& track) { return track.loading; });
            }

            void writeState(IFormatWriter& out) override
            {
                out.writeBool("namesLoaded", m_namesLoaded);
                out.writeBool("loading", busy());
                out.writeString("error", m_error);
                out.writeU32("maxTracks", MAX_TRACKS);
                out.beginObject("list");
                writeTableStats(out, *m_table);
                out.endObject();
                out.beginArray("counters", static_cast<u32>(m_counters.size()));
                for (const Counter& counter : m_counters)
                {
                    out.beginObject({});
                    out.writeString("name", counter.name);
                    out.writeU64("samples", counter.samples);
                    out.writeBool("shown", trackOf(counter.name) != nullptr);
                    out.endObject();
                }
                out.endArray();
                u32 used = 0;
                for (const Track& track : m_tracks)
                {
                    used += track.used;
                }
                out.beginArray("tracks", used);
                for (u32 slot = 0; slot < MAX_TRACKS; ++slot)
                {
                    const Track& track = m_tracks[slot];
                    if (!track.used)
                    {
                        continue;
                    }
                    out.beginObject({});
                    out.writeString("name", track.name);
                    out.writeU32("slot", slot);
                    out.writeU64("frames", track.frames);
                    out.writeU64("loaded", track.loaded);
                    out.writeU64("total", track.total);
                    out.writeU64("max", track.max);
                    out.writeBool("loading", track.loading);
                    out.writeBool("done", track.done);
                    out.writeString("error", track.error);
                    out.writeBool("capped", m_frames > PROFILER_MAX_GRAPH_FRAMES);
                    out.endObject();
                }
                out.endArray();
            }

            void registerCommands() override
            {
                m_host.control->registerViewCommand(
                    "profilerCounterTrack", m_host.owner,
                    "shows or hides a counter of the selected capture as a line chart track in the Profiler's counters "
                    "view (at most 4 at once); showing one also shows the counters view. The track's total and max "
                    "equal profileCounterSeries over the same frames and the zones report's counters.<name>",
                    {optionalArg("name", ArgKind::String, "the counter's exact name (profileCounterNames lists them)"),
                     optionalArg("show", ArgKind::Boolean, "true shows the track, false hides it", "true"),
                     optionalArg("clear", ArgKind::Boolean, "hides every track first", "false")},
                    [this](const ControlArgs& args, IFormatWriter& out) { return command(args, out); });
            }

        private:
            using Counter = CaptureCounterName;

            struct Track
            {
                bool used = false;
                std::string name;
                Panel holder;
                Label title;
                Chart chart;
                bool seriesMade = false;
                u64 frames = 0, loaded = 0, total = 0, max = 0;
                bool loading = false, done = false;
                std::string error;
                std::shared_ptr<std::atomic<u64>> generation;
                u64 value = 0;
            };

            // an event of a request that nothing cancelled since
            [[nodiscard]] bool isCurrent(const u64 token) const
            {
                return token == m_namesGeneration->load() ||
                    std::ranges::any_of(m_tracks,
                                        [token](const Track& t) { return t.used && token == t.generation->load(); });
            }

            [[nodiscard]] Track* trackOf(const std::string_view name)
            {
                for (Track& track : m_tracks)
                {
                    if (track.used && track.name == name)
                    {
                        return &track;
                    }
                }
                return nullptr;
            }

            std::string command(const ControlArgs& args, IFormatWriter& out)
            {
                if (args.boolean("clear").value_or(false))
                {
                    for (u32 slot = 0; slot < MAX_TRACKS; ++slot)
                    {
                        releaseTrack(slot);
                    }
                    updateList();
                }
                if (const std::optional<std::string> name = args.string("name"))
                {
                    if (m_capture == 0)
                    {
                        return "select a capture first (profilerSelect capture=<id>)";
                    }
                    if (!m_namesLoaded)
                    {
                        if (!m_host.store)
                        {
                            return "no capture store";
                        }
                        if (std::string error = profilerFrameStoreCall(
                                [&] { return m_host.store->counterNames(m_capture, m_counters); });
                            !error.empty())
                        {
                            return error;
                        }
                        m_namesLoaded = true;
                        updateList();
                    }
                    const bool known =
                        std::ranges::any_of(m_counters, [&](const Counter& counter) { return counter.name == *name; });
                    if (!known)
                    {
                        return "the capture has no counter named '" + profilerSafeText(*name, 64) +
                            "' (profileCounterNames lists them)";
                    }
                    if (args.boolean("show").value_or(true))
                    {
                        if (std::string error = showTrack(*name); !error.empty())
                        {
                            return error;
                        }
                    }
                    else if (Track* track = trackOf(*name))
                    {
                        releaseTrack(static_cast<u32>(track - m_tracks.data()));
                        updateList();
                    }
                }
                out.writeU32("tracks",
                             static_cast<u32>(std::ranges::count_if(m_tracks, [](const Track& t) { return t.used; })));
                out.writeBool("queued", !m_active); // a hidden counters view is shown by the next frame's profilerPane
                return {};
            }

            std::string showTrack(const std::string& name)
            {
                if (trackOf(name))
                {
                    return {};
                }
                const auto free = std::ranges::find_if(m_tracks, [](const Track& track) { return !track.used; });
                if (free == m_tracks.end())
                {
                    return "at most " + std::to_string(MAX_TRACKS) + " counter tracks at once: hide one first";
                }
                const auto slot = static_cast<u32>(free - m_tracks.begin());
                Track& track = *free;
                if (!track.seriesMade)
                {
                    UiChartSeriesDesc series;
                    series.name = "value";
                    series.color = canvasColor(*m_host.scope, "canvas.series.c");
                    series.thickness = 1.5f;
                    series.capacity = PROFILER_MAX_GRAPH_FRAMES;
                    track.chart.addSeries(series);
                    track.seriesMade = true;
                }
                track.used = true;
                track.name = name;
                track.frames = std::min<u64>(m_frames, PROFILER_MAX_GRAPH_FRAMES);
                track.loaded = track.total = track.max = 0;
                track.done = track.loading = false;
                track.error.clear();
                track.chart.clear();
                track.chart.setWindow(0, 0);
                track.holder.setVisible(true);
                showTitle(slot);
                updateList();
                if (!m_active)
                {
                    m_host.send({{"command", "profilerPane"}, {"args", {{"pane", "counters"}}}});
                }
                else
                {
                    requestPage(slot);
                }
                return {};
            }

            void releaseTrack(const u32 slot)
            {
                Track& track = m_tracks[slot];
                if (!track.used)
                {
                    return;
                }
                track.generation->fetch_add(1);
                track.used = track.loading = track.done = false;
                track.holder.setVisible(false);
                if (track.seriesMade)
                {
                    track.chart.clear();
                }
            }

            void cancelAll()
            {
                m_namesGeneration->fetch_add(1);
                m_namesLoading = false;
                for (Track& track : m_tracks)
                {
                    track.generation->fetch_add(1);
                    track.loading = false;
                }
            }

            void requestNames()
            {
                if (!m_host.data || m_capture == 0)
                {
                    return;
                }
                m_namesLoading = true;
                m_namesValue = m_namesGeneration->fetch_add(1) + 1;
                CaptureStore* store = m_host.store;
                const CaptureId capture = m_capture;
                m_host.data->requestTask(
                    static_cast<u32>(ProfilerViewId::Counters), m_namesGeneration, m_namesValue,
                    [store, capture](const CaptureCancelFn& cancelled, std::string&) -> std::shared_ptr<void>
                    {
                        auto payload = std::make_shared<Payload>();
                        payload->error =
                            profilerStoreCall(cancelled, [&] { return store->counterNames(capture, payload->names); });
                        return payload;
                    });
            }

            void requestPage(const u32 slot)
            {
                Track& track = m_tracks[slot];
                if (!m_host.data || m_capture == 0 || !track.used || track.done)
                {
                    return;
                }
                track.loading = true;
                track.value = track.generation->fetch_add(1) + 1;
                CaptureStore* store = m_host.store;
                const CaptureId capture = m_capture;
                const std::string name = track.name;
                const u64 first = track.loaded;
                const u64 count = std::min<u64>(PAGE_FRAMES, track.frames - track.loaded);
                m_host.data->requestTask(
                    static_cast<u32>(ProfilerViewId::Counters), track.generation, track.value,
                    [store, capture, name, first, count, slot](const CaptureCancelFn& cancelled,
                                                               std::string&) -> std::shared_ptr<void>
                    {
                        auto payload = std::make_shared<Payload>();
                        payload->kind = Payload::Kind::Page;
                        payload->slot = slot;
                        payload->error = profilerStoreCall(
                            cancelled,
                            [&] { return store->counterSeries(capture, name, first, count, payload->series); });
                        return payload;
                    });
            }

            void showTitle(const u32 slot)
            {
                Track& track = m_tracks[slot];
                const std::string name = profilerSafeText(track.name, 64);
                track.chart.setAccessibleName(m_host.text("counters.track.name", {{"name", name}}));
                std::string text = m_host.text("counters.title",
                                               {{"name", name},
                                                {"total", m_host.whole(track.total)},
                                                {"max", m_host.whole(track.max)},
                                                {"loaded", m_host.whole(track.loaded)},
                                                {"frames", m_host.whole(track.frames)}});
                if (!track.error.empty())
                {
                    text += "   " + m_host.text("status.error", {{"error", track.error}});
                }
                else if (m_frames > PROFILER_MAX_GRAPH_FRAMES)
                {
                    text +=
                        "   " + m_host.text("counters.capped", {{"frames", m_host.whole(PROFILER_MAX_GRAPH_FRAMES)}});
                }
                if (track.title.text() != text)
                {
                    track.title.setText(text);
                }
            }

            void updateList()
            {
                m_table->setRowCount(static_cast<u32>(m_counters.size()));
                m_table->rebindVisible();
                std::string text;
                if (m_capture == 0)
                {
                    text = m_host.text("status.noCapture");
                }
                else if (!m_error.empty())
                {
                    text = m_host.text("status.error", {{"error", m_error}});
                }
                else if (!m_namesLoaded)
                {
                    text = m_host.text("counters.loading");
                }
                else if (m_counters.empty())
                {
                    text = m_host.text("counters.empty");
                }
                else
                {
                    text = m_host.text(
                        "counters.count",
                        {{"count", m_host.whole(m_counters.size())}, {"tracks", std::to_string(MAX_TRACKS)}});
                }
                show(m_status, text);
            }

            void selectFrames(const u64 first, const u64 last)
            {
                if (m_mirroring || m_capture == 0)
                {
                    return;
                }
                m_host.send({{"command", "profilerSelect"},
                             {"args", {{"capture", m_capture}, {"firstFrame", first}, {"lastFrame", last}}}});
            }

            void mirrorSelection()
            {
                m_mirrorDirty = false;
                m_mirroring = true;
                for (Track& track : m_tracks)
                {
                    if (!track.used || !track.done)
                    {
                        continue;
                    }
                    if (m_selection.hasFrames)
                    {
                        if (!track.chart.select(static_cast<double>(m_selection.firstFrame),
                                                static_cast<double>(m_selection.lastFrame)))
                        {
                            m_mirrorDirty = true; // the chart has no plot yet
                        }
                    }
                    else
                    {
                        track.chart.clearSelection();
                    }
                }
                m_mirroring = false;
            }

            ProfilerViewHost m_host;
            Panel m_root;
            TextInput m_status;
            std::unique_ptr<Table> m_table;
            std::array<Track, MAX_TRACKS> m_tracks;
            std::vector<Counter> m_counters;
            std::vector<float> m_scratch;
            std::shared_ptr<std::atomic<u64>> m_namesGeneration;
            u64 m_namesValue = 0;
            ProfilerSelectionComponent m_selection;
            CaptureId m_capture = 0;
            u64 m_frames = 0;
            std::string m_error;
            std::string m_localeUsed;
            bool m_active = false;
            bool m_namesLoaded = false;
            bool m_namesLoading = false;
            bool m_mirrorDirty = false;
            bool m_mirroring = false;
        };
    } // namespace

    std::unique_ptr<ProfilerView> makeCounterView(const ProfilerViewHost& host)
    {
        return std::make_unique<CounterView>(host);
    }
} // namespace spite
