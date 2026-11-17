#include "EditorStats.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorPanelList.hpp"

#include <array>
#include <chrono>
#include <format>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "engine/FrameQuiescence.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "rendering/RenderingSingletons.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using namespace panelui;

        constexpr std::string_view STATS_VIEW = "editor.stats";
        constexpr std::string_view STATS_TOOLBAR = "stats.toolbar";
        constexpr std::string_view STATS_FILTER = "stats.filter";
        constexpr std::string_view STATS_CHART = "stats.chart";
        constexpr std::string_view STATS_ROWS = "stats.rows";

        // The sections of the list, in order; a name is the `statsSection` argument and a key of editor-stats.section.*
        enum Section : u32
        {
            SECTION_FRAME,
            SECTION_ENTITIES,
            SECTION_MEMORY,
            SECTION_RENDERING,
            SECTION_COUNT
        };
        constexpr std::array<std::string_view, SECTION_COUNT> SECTION_NAMES{"frame", "entities", "memory", "rendering"};

        constexpr float CHART_HEIGHT = 64.0f; // less the 6 px above and the 21 px of x labels below, a 37 px plot
        constexpr float MIN_TABLE_ROWS = 3.0f; // the chart leaves the table at least this many rows
        constexpr float NAME_WIDTH = 240.0f;
        constexpr float VALUE_WIDTH = 160.0f;
        constexpr u32 CHART_WINDOW = 300; // frames the sparkline shows
        constexpr u32 CHART_CAPACITY = 1200;
        constexpr u32 HEADER_ROW = ~0u;
        constexpr u32 HEADER_KEY_BASE = 1; // keys of the section header rows
        constexpr u32 ENTRY_KEY_BASE = 16;

        // What the table shows of a row.
        struct StatsCells
        {
            std::string name;
            std::string value;
            SPITE_OBSERVED_SCHEMA(StatsCells, name, value)
        };

        UiTableColumn column(const std::string_view field, const float width, const bool activates)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString("editor-stats.column." + std::string(field)),
                    .activates = activates,
                    .sortable = false};
        }
    } // namespace

    // -- stats --------------------------------------------------------------------------

    struct EditorStats::State
    {
        static constexpr u32 REFRESH_FRAMES = 30;

        explicit State(Engine& engine) :
            engine(&engine), control(&engine.control()), scope(editorUi(engine).ui()),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        using Clock = std::chrono::steady_clock;

        // One counter: `key` is its row name in statsView, `kind` and `subject` (a world, a gpu field) what the label
        // is made from.
        struct Entry
        {
            std::string key;
            std::string value;
            std::string label;
            std::string subject;
            std::string_view kind;
            u32 id = 0;
        };

        // A row of the list: a section header (entry HEADER_ROW) or one counter of an open section.
        struct ShownRow
        {
            u32 section;
            u32 entry;
        };

        Engine* engine;
        ControlServer* control;
        UiScope scope;
        DensityFollower density{scope};
        const Localization* localization; // not owned: the engine's service
        Panel root;
        Chart chart;
        std::unique_ptr<FilteredTable> list;
        std::unique_ptr<UiDocumentView> view;
        std::array<std::vector<Entry>, SECTION_COUNT> model; // the counters of each section, in order
        std::array<std::string, SECTION_COUNT> titles; // localized
        std::array<std::string, 2>
            quietWords; // the Quiescent frame row's shown value, no then yes; its query value stays yes or no
        std::array<bool, SECTION_COUNT> open{true, true, true, true};
        std::vector<ShownRow> rows;
        std::vector<std::string> knownKeys; // an entry's id is its key's place: stable for the session
        std::vector<std::string> pending;
        std::array<float, REFRESH_FRAMES> samples{}; // frame times since the last refresh, appended to the chart then
        u32 sampleCount = 0;
        std::string filter;
        StatsCells cells; // the row being read
        bool wasShown = false;
        bool dirty = true; // the list lags `rows`
        u32 frames = 0;
        Clock::time_point since = Clock::now();
        Clock::time_point last = since;
        u64 refreshes = 0;
        u64 frameSamples = 0;
        u64 seenLocale = 0;
        std::string worldName; // refresh scratch
        std::string key; // refresh scratch

        [[nodiscard]] std::string text(const std::string_view id, const std::span<const MessageArg> args = {}) const
        {
            if (!localization)
            {
                return std::string(id);
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(id), args, scratch));
        }

        void loadTexts()
        {
            for (u32 section = 0; section < SECTION_COUNT; ++section)
            {
                titles[section] = text("editor-stats.section." + std::string(SECTION_NAMES[section]));
                for (Entry& entry : model[section])
                {
                    label(entry);
                }
            }
            quietWords[0] = text("editor-stats.quiet.no");
            quietWords[1] = text("editor-stats.quiet.yes");
            if (chart.isValid() && chart.seriesCount() != 0)
            {
                chart.setSeriesName(0, text("editor-stats.chart.frame"));
            }
            seenLocale = localization ? localization->revision() : 0;
            dirty = true;
        }

        // a row's shown name: a counter with a key in the table, else its row name
        void label(Entry& entry) const
        {
            if (entry.kind == "frame" || entry.kind == "gpu" || entry.kind == "quiet")
            {
                const std::string id = entry.kind == "frame"
                    ? std::string("editor-stats.frame")
                    : "editor-stats." + std::string(entry.kind) + "." + entry.subject;
                entry.label = localization && localization->contains(InternedString(id)) ? text(id) : entry.key;
                return;
            }
            const std::array<MessageArg, 1> args{{{"world", std::string_view(entry.subject)}}};
            entry.label = text("editor-stats." + std::string(entry.kind), args);
        }

        static void keyOf(const std::string_view kind, const std::string_view subject, std::string& out)
        {
            out.clear();
            if (kind == "frame")
            {
                out = "frame";
            }
            else if (kind == "gpu" || kind == "quiet")
            {
                out.append(kind).append(" ").append(subject);
            }
            else
            {
                out.append(subject).append(" ").append(kind == "chunk" ? "chunk KiB" : kind);
            }
        }

        // Collects the counters in section order, rewriting an entry's strings only when they changed: they keep
        // their capacity, so a refresh allocates nothing once the entries exist.
        struct Collector
        {
            State& state;
            std::array<u32, SECTION_COUNT> used{};
            bool structural = false; // an entry came, went or was renamed
            bool changed = false; // a value moved

            void put(const Section section, const std::string_view kind, const std::string_view subject,
                     const std::string_view value)
            {
                std::vector<Entry>& entries = state.model[section];
                const u32 at = used[section]++;
                if (at == entries.size())
                {
                    entries.emplace_back();
                    structural = true;
                }
                Entry& entry = entries[at];
                State::keyOf(kind, subject, state.key);
                if (entry.key != state.key)
                {
                    entry.key = state.key;
                    entry.kind = kind;
                    entry.subject.assign(subject);
                    const auto known = std::ranges::find(state.knownKeys, entry.key);
                    entry.id = ENTRY_KEY_BASE + static_cast<u32>(known - state.knownKeys.begin());
                    if (known == state.knownKeys.end())
                    {
                        state.knownKeys.push_back(entry.key);
                    }
                    state.label(entry);
                    structural = true;
                }
                if (entry.value != value)
                {
                    entry.value.assign(value);
                    changed = true;
                }
            }

            void finish()
            {
                for (u32 section = 0; section < SECTION_COUNT; ++section)
                {
                    if (state.model[section].size() != used[section])
                    {
                        state.model[section].resize(used[section]);
                        structural = true;
                    }
                }
            }
        };

        using Text = std::array<char, 64>;

        template <typename... Args>
        static std::string_view print(Text& buffer, std::format_string<Args...> format, Args&&... args)
        {
            const auto end = std::format_to_n(buffer.data(), buffer.size(), format, std::forward<Args>(args)...).out;
            return {buffer.data(), static_cast<sizet>(end - buffer.data())};
        }

        // A query result's leaves; skips what it does not show.
        class RowWriter : public IFormatWriter
        {
        public:
            explicit RowWriter(Collector& sink) : m_sink(sink) {}

            void beginObject(const std::string_view name) override { enter(name); }
            void endObject() override { leave(); }
            void beginArray(const std::string_view name, u32) override { enter(name); }
            void endArray() override { leave(); }
            void writeBool(const std::string_view name, const bool v) override { leaf(name, v ? "true" : "false"); }
            void writeU8(const std::string_view name, const u8 v) override { integer(name, v); }
            void writeU16(const std::string_view name, const u16 v) override { integer(name, v); }
            void writeU32(const std::string_view name, const u32 v) override { integer(name, v); }
            void writeU64(const std::string_view name, const u64 v) override { integer(name, v); }
            void writeI32(const std::string_view name, const i32 v) override
            {
                Text buffer;
                leaf(name, print(buffer, "{}", v));
            }
            void writeF32(const std::string_view name, const f32 v) override { writeF64(name, v); }
            void writeF64(const std::string_view name, const double v) override
            {
                Text buffer;
                leaf(name, print(buffer, "{:.3f}", v));
            }
            void writeFloats(std::string_view, std::span<const f32>) override {}
            void writeString(const std::string_view name, const std::string_view v) override { onString(name, v); }
            void writeEntity(std::string_view, Entity) override {}
            void writeNull(std::string_view) override {}

        protected:
            virtual void enter(std::string_view) { ++m_depth; }
            virtual void leave() { --m_depth; }
            virtual void integer(const std::string_view name, const u64 v)
            {
                Text buffer;
                leaf(name, print(buffer, "{}", v));
            }
            virtual void leaf(std::string_view, std::string_view) {}
            virtual void onString(std::string_view, std::string_view) {}

            Collector& m_sink;
            u32 m_depth = 0;
        };

        // The gpu stats schema walk: every leaf a Rendering counter.
        class GpuRows final : public RowWriter
        {
        public:
            using RowWriter::RowWriter;

        protected:
            void leaf(const std::string_view name, const std::string_view value) override
            {
                m_sink.put(SECTION_RENDERING, "gpu", name.starts_with("m_") ? name.substr(2) : name, value);
            }
        };

        // The worlds query: per world its entity and archetype counts and chunk memory.
        class WorldRows final : public RowWriter
        {
        public:
            WorldRows(Collector& sink, std::string& name) : RowWriter(sink), m_name(name) {}

        protected:
            static constexpr u32 WORLD = 2; // worlds array, then the world's object
            void enter(const std::string_view name) override
            {
                RowWriter::enter(name);
                if (m_depth == WORLD + 1)
                {
                    m_inStats = name == "stats";
                }
            }
            void leave() override
            {
                if (m_inStats && m_depth == WORLD + 1)
                {
                    m_inStats = false;
                    Text buffer;
                    m_sink.put(SECTION_ENTITIES, "entities", m_name, print(buffer, "{}", m_entities));
                    m_sink.put(SECTION_ENTITIES, "archetypes", m_name, print(buffer, "{}", m_archetypes));
                    m_sink.put(SECTION_MEMORY, "chunk", m_name, print(buffer, "{}", m_chunkBytes / 1024));
                }
                RowWriter::leave();
            }
            void integer(const std::string_view name, const u64 v) override
            {
                if (!m_inStats)
                {
                    return;
                }
                if (name == "activeEntityCount")
                {
                    m_entities = v;
                }
                else if (name == "archetypeCount")
                {
                    m_archetypes = v;
                }
                else if (name == "totalChunkMemoryBytes")
                {
                    m_chunkBytes = v;
                }
            }
            void onString(const std::string_view name, const std::string_view v) override
            {
                if (m_depth == WORLD && name == "name")
                {
                    m_name.assign(v);
                }
            }

        private:
            std::string& m_name;
            bool m_inStats = false;
            u64 m_entities = 0;
            u64 m_archetypes = 0;
            u64 m_chunkBytes = 0;
        };

        [[nodiscard]] u32 entryCount() const
        {
            u32 count = 0;
            for (const std::vector<Entry>& section : model)
            {
                count += static_cast<u32>(section.size());
            }
            return count;
        }

        // The list's rows: headers with the open sections' counters; a filter shows the counters that match it, in
        // every section, with no headers.
        void rebuildRows()
        {
            rows.clear();
            for (u32 section = 0; section < SECTION_COUNT; ++section)
            {
                const std::vector<Entry>& entries = model[section];
                if (entries.empty())
                {
                    continue;
                }
                if (filter.empty())
                {
                    rows.push_back({section, HEADER_ROW});
                }
                for (u32 i = 0; i < entries.size() && (open[section] || !filter.empty()); ++i)
                {
                    if (filter.empty() || containsIgnoringCase(entries[i].label, filter) ||
                        containsIgnoringCase(entries[i].key, filter))
                    {
                        rows.push_back({section, i});
                    }
                }
            }
        }

        // Puts the rows into the list, which rebinds the shown ones.
        void present()
        {
            dirty = false;
            list->setShown(static_cast<u32>(rows.size()), entryCount());
        }

        void setOpen(const u32 section, const bool value)
        {
            if (open[section] != value)
            {
                open[section] = value;
                rebuildRows();
                dirty = true;
            }
        }

        // -- the rows --

        [[nodiscard]] static Entity keyOfRow(const ShownRow& row, const std::vector<Entry>& entries)
        {
            return Entity(row.entry == HEADER_ROW ? HEADER_KEY_BASE + row.section : entries[row.entry].id, 1u);
        }

        const void* fill(const u32 item)
        {
            cells.name.clear(); // keeps the capacity: a refresh rebinds every shown row
            cells.value.clear();
            if (item >= rows.size())
            {
                return &cells;
            }
            const ShownRow row = rows[item];
            if (row.entry == HEADER_ROW)
            {
                cells.name.append(open[row.section] ? "- " : "+ ").append(titles[row.section]);
                if (!open[row.section])
                {
                    cells.value = std::to_string(model[row.section].size());
                }
            }
            else
            {
                const Entry& entry = model[row.section][row.entry];
                cells.name.assign(entry.label);
                cells.value.assign(entry.kind == "quiet" && entry.subject == "quiescent"
                                       ? quietWords[entry.value == "yes"]
                                       : entry.value);
            }
            return &cells;
        }

        // The frames timed since the last flush go into the chart, together.
        void flushSamples()
        {
            if (sampleCount != 0)
            {
                SPITE_ZONE("EditorStats.chart"); // the chart's own cost, apart from the refresh's
                chart.append(0, std::span<const float>(samples.data(), sampleCount));
                frameSamples += sampleCount;
                sampleCount = 0;
            }
        }

        void refresh()
        {
            SPITE_ZONE("EditorStats.refresh");
            ++refreshes;
            Collector sink{*this};
            Text buffer;
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - since).count();
            sink.put(SECTION_FRAME, "frame", {},
                     wasShown && frames ? print(buffer, "{:.2f} ms  {:.0f} fps", ms / frames, 1000.0 * frames / ms)
                                        : std::string_view("-"));
            if (engine->services().contains<FrameQuiescence>())
            {
                const auto quiet = engine->services().read<FrameQuiescence>();
                sink.put(SECTION_FRAME, "quiet", "quiescent", quiet->quiescent ? "yes" : "no");
                sink.put(SECTION_FRAME, "quiet", "stagedBytes", print(buffer, "{}", quiet->stagedBytes));
            }
            // the worlds query an LLM runs, written straight into the entries
            WorldRows worlds(sink, worldName);
            if (const std::string error = control->query("worlds", worlds); !error.empty())
            {
                SWARNING_LOG("stats: worlds query: {}", error);
            }
            if (engine->services().contains<SceneGpuStatsSingleton>())
            {
                GpuRows gpuRows(sink);
                const auto gpu = engine->services().read<SceneGpuStatsSingleton>();
                writeValue(SceneGpuStatsSingleton::spiteSchema(), &gpu.get(), gpuRows, FieldSet::All);
            }
            sink.finish();
            if (sink.structural)
            {
                rebuildRows();
            }
            flushSamples();
            if (sink.structural || sink.changed || dirty)
            {
                present();
            }
            frames = 0;
            since = Clock::now();
        }

        // The chart takes what the panel can spare above the filter bar and the table's header and MIN_TABLE_ROWS
        // rows; a panel with less room, or a closed Frame section, shows none.
        void fitChart()
        {
            const float room = resolvedSize(scope, root).y - density.density().bar - list->headerHeight() -
                MIN_TABLE_ROWS * list->rowHeight();
            const bool show = open[SECTION_FRAME] && room >= CHART_HEIGHT;
            if (chart.isVisible() != show)
            {
                chart.setVisible(show);
            }
        }

        // -- control --

        void registerCommands()
        {
            control->registerViewCommand(
                "statsSection", "editorStats",
                "opens or closes a section of the stats panel's list (a closed one shows its header and the count of "
                "its counters): the header row's Enter or second click; open alone toggles",
                {requiredArg("section", ArgKind::String, "frame, entities, memory or rendering", SECTION_NAMES),
                 optionalArg("open", ArgKind::Boolean, "the state to set", "the opposite of now")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const auto at = std::ranges::find(SECTION_NAMES, args.string("section").value_or(""));
                    if (at == SECTION_NAMES.end())
                    {
                        return std::string("unknown section: frame, entities, memory or rendering");
                    }
                    const auto section = static_cast<u32>(at - SECTION_NAMES.begin());
                    setOpen(section, args.boolean("open").value_or(!open[section]));
                    return std::string{};
                });
            control->registerQuery(
                "statsView", "editorStats",
                "the stats panel's counters by row name (every section, shown or not), which sections are open, the "
                "frame-time chart, the filter and the rows the list shows",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    out.beginObject("rows");
                    for (const std::vector<Entry>& section : model)
                    {
                        for (const Entry& entry : section)
                        {
                            out.writeString(entry.key, entry.value);
                        }
                    }
                    out.endObject();
                    out.beginObject("sections");
                    for (u32 section = 0; section < SECTION_COUNT; ++section)
                    {
                        out.writeBool(SECTION_NAMES[section], open[section]);
                    }
                    out.endObject();
                    out.writeU64("refreshes", refreshes);
                    out.writeU64("frameSamples", frameSamples);
                    out.writeBool("chartShown", chart.isVisible());
                    out.writeU32("chartWindow", chart.readout().windowCount);
                    list->writeView(out);
                    return std::string{};
                });
        }
    };

    EditorStats::EditorStats(Engine& engine) : m_state(std::make_unique<State>(engine))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.root = scope.createPanel("editor.panel", columnDesc());
        state.loadTexts();

        UiChartDesc chartDesc;
        chartDesc.rect = barDesc(Vec2{0.0f, CHART_HEIGHT});
        chartDesc.window = CHART_WINDOW;
        chartDesc.textSize = 11.0f;
        chartDesc.y.hold = true; // the frame times drift across a tick: the axis labels stay until it matters
        chartDesc.legend =
            false; // a legend row would leave the 64 px chart no plot; its accessible name says what it is
        state.chart = scope.createChart("stats.graph", chartDesc);
        state.chart.setAccessibleNameKey(InternedString("editor-stats.chart"));
        state.chart.addSeries({.name = state.text("editor-stats.chart.frame"), .capacity = CHART_CAPACITY});
        state.chart.follow(CHART_WINDOW);

        FilteredTableDesc desc{.id = "stats",
                               .owner = "editorStats",
                               .nameKey = InternedString("editor-stats.table"),
                               .schema = &StatsCells::spiteSchema(),
                               .columns = {column("name", NAME_WIDTH, true), column("value", VALUE_WIDTH, false)},
                               .row = [&state](const u32 item) { return state.fill(item); },
                               .key =
                                   [&state](const u32 item)
                               {
                                   return item < state.rows.size()
                                       ? State::keyOfRow(state.rows[item], state.model[state.rows[item].section])
                                       : Entity(0u, 1u);
                               }};
        desc.expand = [&state](const u32 item)
        {
            return item < state.rows.size() && state.rows[item].entry == HEADER_ROW
                ? (state.open[state.rows[item].section] ? UiExpandState::Expanded : UiExpandState::Collapsed)
                : UiExpandState::None;
        };
        desc.refilter = [&state](const std::string_view typed)
        {
            state.filter = typed;
            state.rebuildRows();
            state.present();
        };
        desc.shows = [&state](const Entity key)
        {
            return std::ranges::any_of(state.rows, [&](const State::ShownRow& row)
                                       { return State::keyOfRow(row, state.model[row.section]) == key; });
        };
        desc.activate = [&state](const Entity key, InternedString)
        {
            for (u32 section = 0; section < SECTION_COUNT; ++section)
            {
                if (key.index() == HEADER_KEY_BASE + section)
                {
                    state.pending.push_back(
                        Json{{"command", "statsSection"}, {"args", {{"section", std::string(SECTION_NAMES[section])}}}}
                            .dump());
                }
            }
        };
        state.list = std::make_unique<FilteredTable>(scope, *state.control, std::move(desc));
        state.registerCommands();
        scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(STATS_CHART),
                      [&state](const Widget& slot) { putIn(state.scope, slot, state.chart); });
        contract.slot(InternedString(STATS_FILTER), [&state](const Widget& slot) { state.list->fillFilter(slot); });
        contract.slot(InternedString(STATS_ROWS), [&state](const Widget& slot) { state.list->fillRows(slot); });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                UiRectDesc chart = topLeft();
                chart.layout = stack(UiLayoutDirection::Column, 0.0f, {}, UiLayoutAlign::Stretch);
                chart.layoutItem = fixedItem();
                chart.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                Panel chartPanel = viewScope.createPanel(InternedString(STATS_CHART), chart);
                chartPanel.setParent(root);
                Panel toolbar = toolbarPanel(viewScope, STATS_TOOLBAR, root);
                filterSlotPanel(viewScope, STATS_FILTER, toolbar);
                fillSlotPanel(viewScope, STATS_ROWS, root);
            });
        state.view = openView(engine, STATS_VIEW, "assets/ui/editor/stats.json", std::move(contract), state.root);
    }

    EditorStats::~EditorStats() { m_state->control->unregisterOwner("editorStats"); }

    Widget EditorStats::content() const { return m_state->root; }

    void EditorStats::update()
    {
        State& state = *m_state;
        if (!state.pending.empty())
        {
            dispatch(*state.control, state.pending, "stats");
        }
        state.list->update();
        if (!shown(state.scope, state.root))
        {
            if (state.wasShown)
            {
                state.wasShown = false;
                state.flushSamples();
            }
            return;
        }
        const State::Clock::time_point now = State::Clock::now();
        if (state.wasShown)
        {
            state.samples[state.sampleCount++] = std::chrono::duration<float, std::milli>(now - state.last).count();
        }
        state.last = now;
        if (state.localization && state.localization->revision() != state.seenLocale)
        {
            state.loadTexts();
        }
        ++state.frames;
        if (!state.wasShown || state.frames >= State::REFRESH_FRAMES)
        {
            state.refresh();
            state.wasShown = true;
        }
        else if (state.dirty)
        {
            state.present();
        }
        state.fitChart();
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "stats",
             .category = "debug",
             .order = 80,
             .dock = {.target = "project"},
             .zone = "EditorStats",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<EditorStats>(c.engine); }}};
    } // namespace
} // namespace spite
