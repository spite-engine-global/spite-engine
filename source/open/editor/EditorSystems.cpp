#include "EditorSystems.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorEntityLabel.hpp"
#include "EditorPanelList.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/ControlServer.hpp"
#include "ecs/World.hpp"
#include "engine/Engine.hpp"
#include "EditorSettings.hpp"
#include "EditorSystemCost.hpp"
#include "EditorSystemsGraph.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "serialization/FormatWalk.hpp"
#include "ui/UiCanvas.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiScope.hpp"
#include "uiview/UiStyleSheet.hpp"
#include "uiview/UiViewContext.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using namespace panelui;

        constexpr std::string_view SYSTEMS_VIEW = "editor.systems";
        constexpr std::string_view SYSTEMS_BAR = "systems.bar";
        constexpr std::string_view SYSTEMS_DETAIL = "systems.detail";
        constexpr std::string_view SYSTEMS_FILTER = "systems.filter";
        constexpr std::string_view SYSTEMS_ROWS = "systems.rows";
        constexpr std::string_view SYSTEMS_TOOLBAR = "systems.toolbar";
        constexpr std::array<std::string_view, 2> WORLDS{"scene", "play"};
        constexpr std::array<std::string_view, 2> MODES{"list", "graph"};
        constexpr std::array<std::string_view, 2> PHASES = SYSTEM_PHASES;
        // The cost column follows the scheduler's measurements while the panel is shown (the cadence and the step the
        // readouts share, EditorSystemCost.hpp)
        constexpr u32 COST_REFRESH_FRAMES = systemcost::REFRESH_FRAMES;
        constexpr float COST_STEP_MS = systemcost::STEP_MS;
        constexpr u32 HISTORY_SAMPLES =
            120; // reads a row's sparkline holds: a minute at one read per COST_REFRESH_FRAMES
        constexpr float SPARK_MIN_TOP_MS = 0.05f; // the sparkline's top edge never falls below one cost step
        constexpr float SPARK_HEADROOM = 1.25f;
        constexpr float DETAIL_TEXT_GROW = 3.0f; // the detail line's share of the strip; the sparkline takes the rest
        constexpr float DETAIL_SPARK_GROW = 2.0f;
        constexpr float TAB_PAD = 16.0f; // a tab is as wide as its text and this much
        constexpr std::chrono::milliseconds DOUBLE_CLICK{400};

        struct SystemEntry
        {
            std::string module;
            std::string name;
            bool enabled = true;
            bool required =
                false; // the systems query's required: setSystemEnabled refuses it, so the row never toggles
            u32 phase = 0; // index into PHASES
            u64 stage = 0;
            std::string stageName;
            float avgMs = 0.0f; // the scheduler's measured cost of this phase (the systems query)
            float maxMs = 0.0f;
            float shownMs = 0.0f; // what the cost cell shows: avgMs held until it moved by COST_STEP_MS
            bool warn = false; // the cell is over the budget (held with shownMs)
        };

        // the cost a row showed at the last read: the cell keeps it until the average moves by COST_STEP_MS, the
        // warning flips, or the average becomes or stops being exactly 0 (a disabled or skipped system), so a
        // steady system rewrites nothing
        struct HeldCost
        {
            float avgMs = 0.0f;
            bool warn = false;
        };

        // a row's measured average at the last HISTORY_SAMPLES reads, a ring
        struct History
        {
            std::array<float, HISTORY_SAMPLES> samples{};
            u32 head = 0;
            u32 count = 0;

            void push(const float value)
            {
                samples[head] = value;
                head = (head + 1) % HISTORY_SAMPLES;
                count = std::min(count + 1, HISTORY_SAMPLES);
            }

            [[nodiscard]] float at(const u32 index) const // 0: the oldest held
            {
                return samples[(head + HISTORY_SAMPLES - count + index) % HISTORY_SAMPLES];
            }
        };

        struct SystemCells
        {
            u32 order = 0;
            std::string system;
            std::string stage;
            std::string phase;
            std::string module;
            std::string state;
            std::string cost;
            std::string costClass; // the cost cell's class suffix: "warn" over the budget, else none; not a column
            SPITE_OBSERVED_SCHEMA(SystemCells, order, system, stage, phase, module, state, cost, costClass)
        };

        // the index into PHASES of a phases-array entry of the systems query; PHASES.size() for an unknown one
        u32 phaseOf(const Json& phase)
        {
            return static_cast<u32>(std::ranges::find(PHASES, phase.get<std::string>()) - PHASES.begin());
        }

        UiRectDesc fittedButton()
        {
            UiRectDesc desc = barDesc(Vec2{});
            desc.contentSize =
                UiContentSizeComponent{.fitWidth = true, .fitHeight = false, .padding = Vec2{TAB_PAD, 0.0f}};
            return desc;
        }

        UiTableColumn column(const std::string_view field, const float width, const bool activates = false)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString("editor-systems.column." + std::string(field)),
                    .activates = activates,
                    .classField = field == "cost" ? InternedString("costClass") : InternedString{}};
        }
    } // namespace

    struct EditorSystems::State
    {
        State(Engine& engine, const EditorSettingsService& settingsService) :
            engine(&engine), settings(&settingsService), control(&engine.control()), scope(editorUi(engine).ui()),
            styles(&editorUi(engine).uiView().stylesheet()),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        const EditorSettingsService* settings;
        ControlServer* control;
        UiScope scope;
        DensityFollower density{scope};
        const UiStyleSheet* styles;
        const Localization* localization;
        Panel root;
        std::array<Button, WORLDS.size()> tabs;
        std::array<Button, MODES.size()> modes; // the table or the dependency graph
        Panel groupGap; // the space between the world tabs and the mode buttons
        SystemsGraph graph; // of the shown world, rebuilt when its schedule changes
        std::unique_ptr<SystemsGraphView> graphView;
        bool graphMode = false;
        bool graphBuilt = false;
        u64 graphBuilds = 0;
        u32 graphWorld = 0;
        u64 graphSchedule = 0;
        std::unique_ptr<FilteredTable> list;
        std::unique_ptr<UiDocumentView> view;
        std::vector<SystemEntry> rows; // execution order
        std::vector<SystemCells> cells; // parallel to rows
        std::vector<Entity> keys; // parallel to rows
        std::unordered_map<u32, HeldCost> held; // by key, as of the last read
        std::unordered_map<u32, History> history; // by key: the average at each read, for the selected row's sparkline
        std::unordered_map<std::string, u32> keyIndex; // "module::name/phase" -> key, stable for the session
        std::vector<std::string> pending;
        std::string shownSignature;
        u32 world = 0; // index into WORLDS
        static constexpr u32 COMMAND_REFRESH_FRAMES = 20;
        bool wasShown = false;
        bool playing = false;
        bool commanded = false; // this panel dispatched a command: read once more
        bool stale = false; // a command ran since the last read
        u32 sinceRefresh = 0;
        u64 seenCommand = 0;
        u64 seenSchedule = 0;
        u64 seenLocale = 0;
        u64 refreshes = 0;
        Label detail; // the selected system's cost line, or the hint to select one
        Canvas spark; // its history, one series of HISTORY_SAMPLES
        UiSeriesId series;
        std::string seenFilter; // the filter the detail was last worked out under: it may hide the selected row
        Entity seriesKey = Entity::undefined(); // whose history the series holds
        bool detailStale = false; // the selection moved since the detail was worked out
        bool rebuildSeries = false; // the histories were dropped (a world change)
        bool sparkShown = false;
        bool sparkWarn = false; // the selected row is over the budget: the line draws in the warning colour
        float sparkTop = 0.0f;
        u32 sinceSample = COST_REFRESH_FRAMES;
        bool paintedWarn = false;
        Vec2 paintedSize{};
        u64 paintedSheet = ~0ull;
        u64 seenLayout = ~0ull;
        Entity lastClickToggle = Entity::undefined();
        std::chrono::steady_clock::time_point lastClickAt;

        [[nodiscard]] std::string message(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            std::string scratch;
            return localization ? std::string(localization->resolve(InternedString(key), args, scratch))
                                : std::string(key);
        }

        // the shown world's schedule counter; 0 for a play world that is gone
        [[nodiscard]] u64 schedule() const
        {
            return world == 1 ? (engine->playWorld() ? engine->gameWorld().systemRevision() : 0)
                              : engine->world().systemRevision();
        }

        [[nodiscard]] static std::string idOf(const SystemEntry& entry)
        {
            return entry.module + "::" + entry.name + "/" + std::string(PHASES[entry.phase]);
        }

        [[nodiscard]] Entity keyOf(const SystemEntry& entry)
        {
            const auto [it, added] = keyIndex.try_emplace(idOf(entry), static_cast<u32>(keyIndex.size() + 1));
            return Entity(it->second, 1u);
        }

        [[nodiscard]] SystemCells cellsOf(const SystemEntry& entry, const u32 order) const
        {
            const double shownMs = std::round(static_cast<double>(entry.shownMs) * 100.0) / 100.0;
            return {order,
                    entry.name,
                    entry.stageName.empty() ? std::to_string(entry.stage) : identifierWords(entry.stageName),
                    message("editor-systems.phase." + std::string(PHASES[entry.phase])),
                    entry.module.empty() ? message("editor-systems.local") : entry.module,
                    message(!entry.enabled       ? "editor-systems.state.off"
                                : entry.required ? "editor-systems.state.required"
                                                 : "editor-systems.state.on"),
                    message("editor-systems.cost", std::array{MessageArg{"ms", shownMs}}),
                    entry.warn ? "warn" : ""};
        }

        [[nodiscard]] bool matches(const u32 item, const std::string_view needle) const
        {
            // the stage as shown ("Post Update") or as registered (POST_UPDATE, the systemsLane name)
            const SystemCells& row = cells[item];
            return containsIgnoringCase(row.system, needle) || containsIgnoringCase(row.module, needle) ||
                containsIgnoringCase(row.stage, needle) || containsIgnoringCase(rows[item].stageName, needle);
        }

        void refresh()
        {
            SPITE_ZONE("EditorSystems.refresh");
            ++refreshes;
            const bool sampling = sinceSample >= COST_REFRESH_FRAMES; // a command's read adds no sample
            const Json reply =
                request(*control, Json{{"query", "systems"}, {"args", {{"world", std::string(WORLDS[world])}}}});
            rows.clear();
            for (const Json& system : reply.value("result", Json::object()).value("systems", Json::array()))
            {
                for (const Json& phase : system.value("phases", Json::array()))
                {
                    const u32 index = phaseOf(phase);
                    if (index >= PHASES.size())
                    {
                        continue;
                    }
                    const std::string prefix(PHASES[index]);
                    rows.push_back({system.value("module", std::string{}), system.value("name", std::string{}),
                                    system.value("enabled", true), system.value("required", false), index,
                                    system.value(prefix + "Stage", u64{0}),
                                    system.value(prefix + "StageName", std::string{}),
                                    system.value(prefix + "AvgMs", 0.0f), system.value(prefix + "MaxMs", 0.0f)});
                }
            }
            std::ranges::stable_sort(rows, {}, [](const SystemEntry& e) { return std::pair(e.phase, e.stage); });
            cells.clear();
            keys.clear();
            const float budget = settings->settings().systemBudgetMs;
            std::unordered_map<u32, HeldCost> nextHeld;
            for (u32 i = 0; i < rows.size(); ++i)
            {
                SystemEntry& row = rows[i];
                keys.push_back(keyOf(row));
                const bool warn = budget > 0.0f && row.avgMs > budget;
                HeldCost now{row.avgMs, warn};
                if (const auto previous = held.find(keys[i].id()); previous != held.end())
                {
                    const HeldCost& was = previous->second;
                    if (was.warn == warn && (was.avgMs == 0.0f) == (row.avgMs == 0.0f) &&
                        std::abs(row.avgMs - was.avgMs) < COST_STEP_MS)
                    {
                        now = was;
                    }
                }
                nextHeld[keys[i].id()] = now;
                if (sampling)
                {
                    history[keys[i].id()].push(row.avgMs);
                }
                row.shownMs = now.avgMs;
                row.warn = now.warn;
                cells.push_back(cellsOf(row, i + 1));
            }
            held = std::move(nextHeld);
            sinceSample = sampling ? 0 : sinceSample;
            std::erase_if(history, [this](const auto& entry) { return !held.contains(entry.first); });
            seenLocale = localization ? localization->revision() : 0;
            // a read that found nothing changed writes nothing
            std::string signature;
            for (u32 i = 0; i < cells.size(); ++i)
            {
                const SystemCells& c = cells[i];
                signature += std::format("{}|{}|{}|{}|{}|{}|{}|{}\n", c.system, c.stage, c.phase, c.module, c.state,
                                         keys[i].id(), c.cost, c.costClass);
            }
            if (signature != shownSignature)
            {
                shownSignature = std::move(signature);
                list->setItems(static_cast<u32>(rows.size()));
            }
            syncDetail(sampling);
            if (graphMode && graphBuilt && graphWorld == world)
            {
                syncGraphEnabled();
            }
        }

        // The selected row's cost line and sparkline: written only when the text, the samples or the shape moved.
        void syncDetail(const bool sampled)
        {
            detailStale = false;
            seenFilter = list->filterText();
            const std::span<const Entity> selection = list->table().selection();
            const Entity selectedKey = selection.empty() ? Entity::undefined() : selection.front();
            const auto row = static_cast<u32>(std::ranges::find(keys, selectedKey) - keys.begin());
            const bool has = selectedKey != Entity::undefined() && row < rows.size();
            graphView->setSelected(has ? idOf(rows[row]) : std::string{});
            std::string line;
            if (has)
            {
                const double nowMs = std::round(static_cast<double>(rows[row].shownMs) * 100.0) / 100.0;
                // the cell's value is held up to a step off the average: the line never shows a peak below it
                const double peakMs =
                    std::round(static_cast<double>(std::max(rows[row].maxMs, rows[row].shownMs)) * 100.0) / 100.0;
                line = message("editor-systems.detail",
                               std::array{MessageArg{"name", std::string_view(rows[row].name)}, MessageArg{"ms", nowMs},
                                          MessageArg{"peak", peakMs}});
            }
            else
            {
                line = message("editor-systems.detail.none");
            }
            if (detail.text() != line)
            {
                detail.setText(line);
            }
            if (has != sparkShown)
            {
                sparkShown = has;
                spark.setVisible(has);
                seriesKey = Entity::undefined();
            }
            if (!has)
            {
                return;
            }
            sparkWarn = rows[row].warn;
            static const History none;
            const auto found = history.find(selectedKey.id());
            const History& past = found == history.end() ? none : found->second;
            if (seriesKey != selectedKey || rebuildSeries)
            {
                seriesKey = selectedKey;
                rebuildSeries = false;
                spark.clearSeries(series);
                std::array<float, HISTORY_SAMPLES> ordered;
                for (u32 i = 0; i < past.count; ++i)
                {
                    ordered[i] = past.at(i);
                }
                spark.appendSamples(series, std::span<const float>(ordered.data(), past.count));
            }
            else if (sampled && past.count > 0)
            {
                const float latest = past.at(past.count - 1);
                spark.appendSamples(series, std::span<const float>(&latest, 1));
            }
            float peak = 0.0f;
            for (u32 i = 0; i < past.count; ++i)
            {
                peak = std::max(peak, past.at(i));
            }
            const float top = std::max(peak * SPARK_HEADROOM, SPARK_MIN_TOP_MS);
            if (top != sparkTop)
            {
                sparkTop = top;
                spark.setSeriesRange(series, UiSeriesRange{.tail = true, .min = 0.0f, .max = top});
            }
        }

        // Repaints the sparkline when its size, the stylesheet or the warning changed: integer compares per frame.
        void repaintSpark()
        {
            const u64 sheet = styles->generation();
            const u64 layout = scope.hierarchy().layoutGeneration();
            if (layout == seenLayout && sheet == paintedSheet && sparkWarn == paintedWarn)
            {
                return;
            }
            seenLayout = layout;
            const Vec2 size = spark.size();
            if (size.x <= 0.0f || size.y <= 0.0f ||
                (size == paintedSize && sheet == paintedSheet && sparkWarn == paintedWarn))
            {
                return;
            }
            paintedSize = size;
            paintedSheet = sheet;
            paintedWarn = sparkWarn;
            const UiStyle style =
                styles->resolve(InternedString("systems.spark"), UiWidgetKind::Label, UiInteractionState::Normal,
                                InternedString(sparkWarn ? "list.row.text.warn" : "list.filter.count"));
            const UiSeriesId id = series;
            const u32 color = style.textColor;
            spark.update([=](UiCanvasPainter& painter) { painter.series(id, Vec2{}, size, color); });
        }

        void showTabs()
        {
            for (u32 i = 0; i < tabs.size(); ++i)
            {
                tabs[i].setLocalizedText(InternedString("editor-systems.world." + std::string(WORLDS[i])));
                tabs[i].setAccessibleNameKey(InternedString("editor-systems.world." + std::string(WORLDS[i])));
                showSelected(tabs[i], i == world);
                tabs[i].setInteractable(i == 0 || playing);
            }
            for (u32 i = 0; i < modes.size(); ++i)
            {
                modes[i].setLocalizedText(InternedString("editor-systems.mode." + std::string(MODES[i])));
                modes[i].setAccessibleNameKey(InternedString("editor-systems.mode." + std::string(MODES[i]) + ".name"));
                showSelected(modes[i], (i == 1) == graphMode);
            }
        }

        // The graph of the shown world's systems, from the systems query again only when its schedule changed.
        void ensureGraph()
        {
            const u64 now = schedule();
            if (graphBuilt && graphWorld == world && graphSchedule == now)
            {
                return;
            }
            const Json reply =
                request(*control, Json{{"query", "systems"}, {"args", {{"world", std::string(WORLDS[world])}}}});
            const auto names = [](const Json& list)
            {
                std::vector<std::string> out;
                for (const Json& item : list)
                {
                    if (item.is_string())
                    {
                        out.push_back(item.get<std::string>());
                    }
                }
                return out;
            };
            std::vector<SystemsGraphInput> inputs;
            for (const Json& system : reply.value("result", Json::object()).value("systems", Json::array()))
            {
                for (const Json& phase : system.value("phases", Json::array()))
                {
                    const u32 index = phaseOf(phase);
                    if (index >= PHASES.size())
                    {
                        continue;
                    }
                    const std::string prefix(PHASES[index]);
                    inputs.push_back({system.value("module", std::string{}), system.value("name", std::string{}), index,
                                      system.value(prefix + "Stage", u64{0}),
                                      system.value(prefix + "StageName", std::string{}), system.value("enabled", true),
                                      names(system.value(prefix + "RunsAfter", Json::array())),
                                      names(system.value(prefix + "Reads", Json::array())),
                                      names(system.value(prefix + "Writes", Json::array()))});
                }
            }
            graph = buildSystemsGraph(inputs);
            graphBuilt = true;
            ++graphBuilds;
            graphWorld = world;
            graphSchedule = now;
            graphView->graphChanged();
        }

        // A toggle does not move the schedule revision: the nodes follow the rows' enabled state in place.
        void syncGraphEnabled()
        {
            bool moved = false;
            for (const SystemEntry& row : rows)
            {
                if (const auto found = graph.index.find(idOf(row)); found != graph.index.end())
                {
                    bool& shown = graph.nodes[found->second].enabled;
                    moved = moved || shown != row.enabled;
                    shown = row.enabled;
                }
            }
            if (moved)
            {
                graphView->nodesChanged();
            }
        }

        // The table or the graph shows in the rows slot. The graph ignores the filter, so a row the filter hides can
        // be selected there; back in the table that selection goes, as a filter that hides the selected row clears it.
        void setMode(const bool toGraph)
        {
            graphMode = toGraph;
            list->setVisible(!toGraph);
            graphView->root().setVisible(toGraph);
            if (toGraph)
            {
                ensureGraph();
                syncGraphEnabled();
                graphView->shownChanged();
            }
            else if (const std::span<const Entity> selection = list->table().selection(); !selection.empty())
            {
                const auto row = static_cast<u32>(std::ranges::find(keys, selection.front()) - keys.begin());
                if (row >= rows.size() || !std::ranges::contains(list->shown(), row))
                {
                    list->table().setSelection({});
                    detailStale = true;
                }
            }
            showTabs();
        }

        void setWorld(const u32 index)
        {
            world = index;
            held.clear();
            history.clear();
            sinceSample = COST_REFRESH_FRAMES;
            rebuildSeries = true;
            showTabs();
            seenSchedule = schedule();
            refresh();
        }

        // Enter on a row, or a click on the state of an already selected row: the system toggles (setSystemEnabled,
        // undoable). Clicks of one row within DOUBLE_CLICK of each other count once.
        void toggle(const Entity key, const InternedString column)
        {
            const auto row = static_cast<u32>(std::ranges::find(keys, key) - keys.begin());
            if (row >= rows.size() || !std::ranges::contains(list->shown(), row) || rows[row].required)
            {
                return;
            }
            if (column.isValid())
            {
                const auto now = std::chrono::steady_clock::now();
                const bool repeated = key == lastClickToggle && now - lastClickAt < DOUBLE_CLICK;
                lastClickToggle = key;
                lastClickAt = now;
                if (repeated)
                {
                    return;
                }
            }
            const SystemEntry& entry = rows[row];
            pending.push_back(Json{{"command", "setSystemEnabled"},
                                   {"args",
                                    {{"world", std::string(WORLDS[world])},
                                     {"module", entry.module},
                                     {"name", entry.name},
                                     {"enabled", !entry.enabled}}}}
                                  .dump());
        }

        void registerCommands()
        {
            control->registerViewCommand(
                "systemsWorld", "editorSystems", "sets the world the systems panel shows",
                {requiredArg("world", ArgKind::String, "scene or play", WORLDS)},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const u32 index =
                        static_cast<u32>(std::ranges::find(WORLDS, args.string("world").value_or("")) - WORLDS.begin());
                    if (index >= WORLDS.size() || (index == 1 && !playing))
                    {
                        return std::string("the play world exists only while playing");
                    }
                    setWorld(index);
                    return std::string{};
                });
            control->registerViewCommand(
                "systemsSelect", "editorSystems",
                "selects a system's row of the systems panel: the detail line and sparkline below the table show its "
                "cost history (systemsView detail)",
                {requiredArg("name", ArgKind::String, "the system's name"),
                 optionalArg("module", ArgKind::String, "its module; empty for a system of the engine itself", "any"),
                 optionalArg("phase", ArgKind::String, "update or fixed", "the first row of the name", PHASES)},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const std::string name = args.string("name").value_or("");
                    const std::optional<std::string> module = args.string("module");
                    const std::optional<std::string> phase = args.string("phase");
                    if (!wasShown)
                    {
                        refresh(); // the rows are read only while the panel is shown
                    }
                    std::string names;
                    bool hidden = false;
                    for (u32 i = 0; i < rows.size(); ++i)
                    {
                        const SystemEntry& row = rows[i];
                        if (row.name != name || (module && row.module != *module) ||
                            (phase && PHASES[row.phase] != *phase))
                        {
                            names += (names.empty() ? "" : ", ") + row.module + "::" + row.name + "/" +
                                std::string(PHASES[row.phase]);
                            continue;
                        }
                        if (!graphMode && !std::ranges::contains(list->shown(), i))
                        {
                            hidden = true; // the name's other phase may be shown
                            continue;
                        }
                        list->table().setSelection(std::span<const Entity>(&keys[i], 1));
                        syncDetail(false);
                        return std::string{};
                    }
                    if (hidden)
                    {
                        return "the filter hides the row of " + name;
                    }
                    return "no such system row; the rows (module::name/phase) are: " + names;
                });
            control->registerViewCommand(
                "systemsMode", "editorSystems",
                "shows the systems panel's table (list) or the dependency graph of one stage (graph)",
                {requiredArg("mode", ArgKind::String, "list or graph", MODES)},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    setMode(args.string("mode").value_or("") == "graph");
                    return std::string{};
                });
            control->registerViewCommand(
                "systemsLane", "editorSystems", "chooses the stage the systems panel's graph shows",
                {requiredArg("stage", ArgKind::String,
                             "the stage's name (UPDATE) or number, as systemsView graph.lanes lists it"),
                 optionalArg("phase", ArgKind::String, "update or fixed", "the first phase that has the stage",
                             PHASES)},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    ensureGraph();
                    const std::optional<std::string> phase = args.string("phase");
                    const auto index = phase ? std::ranges::find(PHASES, *phase) - PHASES.begin() : -1;
                    if (!graphView->setLane(static_cast<i32>(index), args.string("stage").value_or("")))
                    {
                        return "no such stage in the " + std::string(WORLDS[world]) +
                            " world; the stages (phase/stage) are: " + graphView->lanesText();
                    }
                    return std::string{};
                });
            control->registerViewCommand(
                "systemsGraphView", "editorSystems",
                "sets the graph's zoom and the graph position at the canvas's top-left corner, or resets both",
                {optionalArg("zoom", ArgKind::Number, "0.1 to 2.5", "the current zoom"),
                 optionalArg("x", ArgKind::Number, "graph x of the left edge, in pixels at zoom 1", "the current x"),
                 optionalArg("y", ArgKind::Number, "graph y of the top edge", "the current y"),
                 optionalArg("reset", ArgKind::Boolean, "zoom 1 at the top-left; the other arguments are ignored",
                             "false")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    ensureGraph();
                    if (args.boolean("reset").value_or(false))
                    {
                        graphView->resetView();
                        return std::string{};
                    }
                    const auto number = [&args](const std::string_view key,
                                                const float fallback) -> std::optional<float>
                    {
                        const std::optional<std::string> text = args.json(key);
                        if (!text)
                        {
                            return fallback;
                        }
                        const Json value = Json::parse(*text, nullptr, false);
                        return value.is_number() && std::abs(value.get<double>()) <= std::numeric_limits<float>::max()
                            ? std::optional(value.get<float>())
                            : std::nullopt;
                    };
                    const std::optional<float> zoom = number("zoom", graphView->zoom());
                    const std::optional<float> x = number("x", graphView->origin().x);
                    const std::optional<float> y = number("y", graphView->origin().y);
                    if (!zoom || !x || !y)
                    {
                        return std::string("zoom, x and y are finite numbers");
                    }
                    if (*zoom < SystemsGraphView::ZOOM_MIN || *zoom > SystemsGraphView::ZOOM_MAX)
                    {
                        return std::format("zoom is from {} to {}", SystemsGraphView::ZOOM_MIN,
                                           SystemsGraphView::ZOOM_MAX);
                    }
                    graphView->setView(*zoom, *x, *y);
                    return std::string{};
                });
            control->registerQuery(
                "systemsView", "editorSystems",
                "the systems panel's rows in execution order (order, system, stage, phase, module, state, cost, "
                "costClass; name, moduleName, enabled, avgMs and maxMs: the scheduler's measured cost of the phase, "
                "unrounded, where the cost cell holds its value until it moves by 0.05 ms), its world, the refresh "
                "counter and detail (the selected system's line, and its sparkline: the average at each of the last "
                "120 reads, oldest first, and the top edge in ms), mode (list or graph) and graph (the "
                "shown stage's systems by column, their orderings, the write conflicts nothing orders, the "
                "selected system's neighbours and component access, the zoom and the nodes drawn)",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    out.writeString("world", WORLDS[world]);
                    out.writeString("mode", MODES[graphMode ? 1 : 0]);
                    ensureGraph(); // the one state change a query makes: idempotent, so a hidden panel answers current
                    out.beginObject("graph");
                    out.writeU64("builds", graphBuilds);
                    graphView->writeState(out);
                    out.endObject();
                    out.beginArray("rows", static_cast<u32>(rows.size()));
                    for (u32 i = 0; i < rows.size(); ++i)
                    {
                        out.beginObject("");
                        writeValue(SystemCells::spiteSchema(), &cells[i], out, FieldSet::All);
                        out.writeString("name", rows[i].name);
                        out.writeString("moduleName", rows[i].module);
                        out.writeBool("enabled", rows[i].enabled);
                        out.writeF32("avgMs", rows[i].avgMs);
                        out.writeF32("maxMs", rows[i].maxMs);
                        out.endObject();
                    }
                    out.endArray();
                    out.writeU64("refreshes", refreshes);
                    out.beginObject("detail");
                    out.writeString("text", detail.text());
                    out.writeBool("sparkline", sparkShown);
                    out.writeBool("warn", sparkShown && sparkWarn);
                    out.writeF32("top", sparkShown ? sparkTop : 0.0f);
                    const UiSeries* held = spark.series(series);
                    const u32 count = sparkShown && held ? held->count : 0;
                    out.beginArray("history", count);
                    for (u32 i = 0; i < count; ++i)
                    {
                        out.writeF32("", held->at(i));
                    }
                    out.endArray();
                    out.endObject();
                    list->writeView(out);
                    return std::string{};
                });
        }
    };

    EditorSystems::EditorSystems(Engine& engine, const EditorSettingsService& settings) :
        m_state(std::make_unique<State>(engine, settings))
    {
        State& state = *m_state;
        state.root = state.scope.createPanel("editor.panel", columnDesc());
        for (u32 i = 0; i < state.tabs.size(); ++i)
        {
            state.tabs[i] =
                state.density.track(state.scope.createButton("systems.tab", "", fittedButton()), HEIGHT_CONTROL);
            state.tabs[i].onClick(
                [&state, i]
                {
                    state.pending.push_back(
                        Json{{"command", "systemsWorld"}, {"args", {{"world", std::string(WORLDS[i])}}}}.dump());
                });
        }
        for (u32 i = 0; i < state.modes.size(); ++i)
        {
            state.modes[i] =
                state.density.track(state.scope.createButton("systems.mode", "", fittedButton()), HEIGHT_CONTROL);
            state.modes[i].onClick(
                [&state, i]
                {
                    state.pending.push_back(
                        Json{{"command", "systemsMode"}, {"args", {{"mode", std::string(MODES[i])}}}}.dump());
                });
        }
        state.groupGap = state.scope.createPanel("systems.bar.gap", barDesc(Vec2{}));
        state.groupGap.bindLayoutToken(UiLayoutField::MinWidth, InternedString(SPACE_LG));
        state.groupGap.setInteractable(false);
        state.graphView = std::make_unique<SystemsGraphView>(
            SystemsGraphHost{&state.scope, &state.density, state.styles,
                             [&state](std::string line) { state.pending.push_back(std::move(line)); }},
            state.graph);
        state.showTabs();
        state.list = std::make_unique<FilteredTable>(
            state.scope, *state.control,
            FilteredTableDesc{.id = "systems",
                              .owner = "editorSystems",
                              .nameKey = InternedString("editor-systems.table"),
                              .schema = &SystemCells::spiteSchema(),
                              .columns = {column("order", 72.0f), column("system", 220.0f), column("stage", 120.0f),
                                          column("phase", 72.0f), column("module", 120.0f),
                                          column("state", 110.0f, true), column("cost", 104.0f)},
                              .row = [&state](const u32 item) -> const void* { return &state.cells[item]; },
                              .key = [&state](const u32 item) { return state.keys[item]; },
                              .matches = [&state](const u32 item, const std::string_view needle)
                              { return state.matches(item, needle); },
                              .activate = [&state](const Entity key, const InternedString column)
                              { state.toggle(key, column); },
                              .headerHints = {{InternedString("state"), InternedString("editor-systems.hint.state")},
                                              {InternedString("cost"), InternedString("editor-systems.hint.cost")}}});
        state.list->table().onSelect([&state](const std::span<const Entity>) { state.detailStale = true; });
        UiRectDesc detailDesc = barDesc(Vec2{});
        detailDesc.layoutItem = fillItem();
        detailDesc.layoutItem->grow = DETAIL_TEXT_GROW;
        state.detail = state.scope.createLabel("systems.detail.text", "", detailDesc);
        state.detail.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SIZE_ROW));
        state.detail.setStyleClass(InternedString("list.filter.count"));
        UiRectDesc sparkDesc = barDesc(Vec2{});
        sparkDesc.layoutItem = fillItem();
        sparkDesc.layoutItem->grow = DETAIL_SPARK_GROW;
        state.spark = state.scope.createCanvas("systems.spark", UiCanvasDesc{.rect = sparkDesc});
        state.spark.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SIZE_ROW));
        state.spark.setInteractable(false);
        state.spark.setAccessibleNameKey(InternedString("editor-systems.spark"));
        state.spark.setVisible(false);
        state.scope.hierarchy().observeSize(state.spark.entity()); // its laid-out size is what the series is drawn over
        state.series = state.spark.createSeries(HISTORY_SAMPLES, UiSeriesRange{.tail = true, .min = 0.0f, .max = 1.0f});
        state.registerCommands();
        state.scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(SYSTEMS_BAR),
                      [&state](const Widget& slot)
                      {
                          for (Widget* item :
                               {static_cast<Widget*>(&state.tabs[0]), static_cast<Widget*>(&state.tabs[1]),
                                static_cast<Widget*>(&state.groupGap), static_cast<Widget*>(&state.modes[0]),
                                static_cast<Widget*>(&state.modes[1])})
                          {
                              putIn(state.scope, slot, *item);
                              if (item != &state.groupGap)
                              {
                                  setOwnClass(*item, slotFields(state.scope, slot).childClass);
                              }
                          }
                      });
        contract.slot(InternedString(SYSTEMS_FILTER), [&state](const Widget& slot) { state.list->fillFilter(slot); });
        contract.slot(InternedString(SYSTEMS_ROWS),
                      [&state](const Widget& slot)
                      {
                          state.list->fillRows(slot);
                          putIn(state.scope, slot, state.graphView->root());
                      });
        contract.slot(InternedString(SYSTEMS_DETAIL),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.detail);
                          putIn(state.scope, slot, state.spark);
                      });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                Panel toolbar = toolbarPanel(viewScope, SYSTEMS_TOOLBAR, root);
                fitBarPanel(viewScope, SYSTEMS_BAR, toolbar);
                filterSlotPanel(viewScope, SYSTEMS_FILTER, toolbar);
                fillSlotPanel(viewScope, SYSTEMS_ROWS, root);
                statusPanel(viewScope, SYSTEMS_DETAIL, root);
            });
        state.view = openView(engine, SYSTEMS_VIEW, "assets/ui/editor/systems.json", std::move(contract), state.root);
    }

    EditorSystems::~EditorSystems() { m_state->control->unregisterOwner("editorSystems"); }

    Widget EditorSystems::content() const { return m_state->root; }

    void EditorSystems::update()
    {
        State& state = *m_state;
        if (!state.pending.empty())
        {
            dispatch(*state.control, state.pending, "systems");
            state.commanded = true; // the person's own toggle shows at once
        }
        state.list->update();
        if (state.detailStale || state.list->filterText() != state.seenFilter)
        {
            state.syncDetail(false);
        }
        if (!shown(state.scope, state.root))
        {
            state.wasShown = false;
            return;
        }
        if (const bool playing = state.engine->playWorld() != nullptr; playing != state.playing)
        {
            state.playing = playing;
            state.world = playing ? state.world : 0;
            state.showTabs();
            state.wasShown = false;
        }
        // A command that is not this panel's own (a camera move, an undo) may have toggled a system: it is read
        // after at most COMMAND_REFRESH_FRAMES, not on every frame a busy session sends commands.
        const u64 command = state.control->commandGeneration();
        const u64 schedule = state.schedule();
        if (command != state.seenCommand)
        {
            state.seenCommand = command;
            state.stale = true;
        }
        ++state.sinceRefresh;
        ++state.sinceSample;
        if (state.sparkShown)
        {
            state.repaintSpark();
        }
        if (!state.wasShown || state.sinceSample >= COST_REFRESH_FRAMES || schedule != state.seenSchedule ||
            state.commanded || (state.stale && state.sinceRefresh >= State::COMMAND_REFRESH_FRAMES) ||
            (state.localization && state.localization->revision() != state.seenLocale))
        {
            state.wasShown = true;
            state.seenSchedule = schedule;
            state.commanded = false;
            state.stale = false;
            state.sinceRefresh = 0;
            state.refresh();
        }
        if (state.graphMode)
        {
            state.ensureGraph();
            state.graphView->update();
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "systems",
             .category = "world",
             .order = 140,
             .dock = {.target = "project"},
             .zone = "EditorSystems",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<EditorSystems>(c.engine, c.settings); }}};
    } // namespace
} // namespace spite
