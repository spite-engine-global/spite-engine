#include "EditorPasses.hpp"
#include "EditorContext.hpp"
#include "EditorDialogs.hpp"
#include "EditorPanelList.hpp"
#include "EditorPanelRegistry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "rendering/IRenderer.hpp"
#include "rendering/RenderGraph.hpp"
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

        constexpr std::string_view PASSES_VIEW = "editor.passes";
        constexpr std::string_view PASSES_BAR = "passes.bar";
        constexpr std::string_view PASSES_ROWS = "passes.rows";

        constexpr std::array<std::string_view, 2> PASS_TABS{"editor", "game"};
        constexpr std::array<std::string_view, 3> PASS_TAB_FILTERS{"editor", "game", "all"};

        constexpr std::string_view PASSES_FILTER = "passes.filter";
        constexpr std::string_view PASSES_TOOLBAR = "passes.toolbar";
        constexpr std::string_view PASSES_ACTIONS = "passes.actions";
        constexpr std::string_view PASSES_DETAIL = "passes.detail";
        constexpr std::string_view PASSES_BODY = "passes.body";
        constexpr std::string_view TABLE = "editor-passes.";
        constexpr float TAB_WIDTH = 84.0f;
        constexpr u32 COMMAND_REFRESH_FRAMES = 20; // another client's commands re-read the graph this often at most
        constexpr u32 GPU_REFRESH_FRAMES = 30; // the selected pass's GPU time is re-read this often
        constexpr u64 GPU_STALE_FRAMES = 8; // a timed frame this far behind the recorded ones is not current
        constexpr sizet CONFIRM_NAMES = 5; // dependents the confirm lists before "and N more"
        constexpr std::chrono::milliseconds DOUBLE_CLICK{
            400}; // the OS default is 500; the input layer has no click count

        // A pass row with the panel tab it belongs to and the world its view draws.
        struct PassEntry
        {
            RenderGraphPassRow pass;
            u32 tab = 0; // index into PASS_TABS
            std::string world;
            u32 order = 0; // in the graph
        };

        // What the table shows of an entry (the strings are localized when it is built); stateClass names the state
        // cell's class suffix (<rowClass>.text.warn for a pass a command turned off), not a column.
        struct PassCells
        {
            std::string pass;
            std::string state;
            std::string stateClass;
            std::string stage;
            u32 order = 0;
            std::string view;
            std::string world;
            std::string type;
            std::string owner;
            SPITE_OBSERVED_SCHEMA(PassCells, pass, state, stateClass, stage, order, view, world, type, owner)
        };

        // One line of the selected pass's detail: what it reads, writes or waits on.
        struct DetailCells
        {
            std::string kind;
            std::string resource;
            std::string info;
            bool operator==(const DetailCells&) const = default;
            SPITE_OBSERVED_SCHEMA(DetailCells, kind, resource, info)
        };

        // The selected pass as the detail shows it. Counts are of the rows: reads, writes (an attachment or storage
        // write) and the barriers recorded for the pass.
        struct PassDetail
        {
            std::string pass;
            u32 reads = 0;
            u32 writes = 0;
            u32 barriers = 0;
            std::vector<std::string> dependents;
            std::vector<DetailCells> rows;
        };

        // The pass's GPU time from the last timed frame. state: timed, none (no timers or timestamps), idle (the
        // newest timed frame is old) or notRun (the pass was not in that frame).
        struct GpuReading
        {
            std::string_view state = "none";
            double ms = 0.0;
            u64 frame = 0;
        };

        UiTableColumn column(const std::string_view field, const float width, const bool activates = false)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString(std::string(TABLE) + "column." + std::string(field)),
                    .activates = activates,
                    .classField = field == "state" ? InternedString("stateClass") : InternedString{}};
        }

        UiTableColumn detailColumn(const std::string_view field, const float width)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString(std::string(TABLE) + "detail.column." + std::string(field)),
                    .sortable = false};
        }

        // The pass family: the name without its view suffix, up to the first underscore, without trailing digits
        // (Shadow2_Raster#1 -> Shadow).
        std::string stageOf(std::string_view pass)
        {
            pass = pass.substr(0, pass.find('#'));
            const std::string_view family = pass.substr(0, pass.find('_'));
            sizet end = family.size();
            while (end > 0 && std::isdigit(static_cast<unsigned char>(family[end - 1])))
            {
                --end;
            }
            return std::string(end > 0 ? family.substr(0, end) : pass);
        }
    } // namespace

    // -- passes -------------------------------------------------------------------------

    struct EditorPasses::State
    {
        State(Engine& engine, EditorDialogs& dialogs) :
            engine(&engine), control(&engine.control()), dialogs(&dialogs), scope(editorUi(engine).ui()),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        EditorDialogs* dialogs;
        UiScope scope;
        DensityFollower density{scope};
        const Localization* localization; // not owned: the engine's service
        Panel root;
        std::array<Button, PASS_TABS.size()> tabs;
        Label badge; // how many passes a command turned off
        Button enableAll;
        Label detailHead;
        Label detailNote;
        Widget detailSlot; // hidden while no single pass is selected
        std::unique_ptr<FilteredTable> list;
        std::unique_ptr<RowTable<DetailCells>> detail;
        std::unique_ptr<UiDocumentView> view;
        std::vector<PassEntry> all; // every pass of the graph
        std::vector<PassEntry> rows; // the shown tab's
        std::vector<PassCells> cells; // parallel to rows
        std::vector<Entity> keys; // parallel to rows
        std::unordered_map<std::string, u32> keyIndex; // "pass/view" -> key, stable for the session
        std::vector<std::string> pending;
        Entity selectedKey = Entity::undefined(); // the one selected row's, else undefined
        std::string selectedPass;
        PassDetail shownDetail; // what the detail labels hold
        u32 tab = 0;
        u32 turnedOff = 0; // passes a command turned off, every tab
        u32 sinceRefresh = 0;
        u32 sinceGpu = 0;
        bool wasShown = false;
        bool dirty = true; // this panel's own command ran: read now
        bool stale = false; // another client's command ran: read within COMMAND_REFRESH_FRAMES
        bool recheck = false; // a compile after a change may move the culled verdicts
        u64 seenCommand = 0;
        u64 ownCommands = 0; // the panel's own view commands that ran: they change nothing a refresh reads
        u64 seenGraph = 0;
        u64 seenLocale = 0;
        u64 refreshes = 0;
        Entity lastClickToggle = Entity::undefined();
        std::chrono::steady_clock::time_point lastClickAt;

        [[nodiscard]] std::string message(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            const std::string full = std::string(TABLE) + std::string(key);
            if (!localization)
            {
                return full;
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(full), args, scratch));
        }

        [[nodiscard]] Entity keyOf(const PassEntry& entry)
        {
            const std::string id =
                entry.pass.pass + (entry.pass.global ? "/global" : "/" + std::to_string(entry.pass.view));
            const auto [it, added] = keyIndex.try_emplace(id, static_cast<u32>(keyIndex.size() + 1));
            return Entity(it->second, 1u);
        }

        [[nodiscard]] PassCells cellsOf(const PassEntry& entry) const
        {
            const RenderGraphPassRow& pass = entry.pass;
            return {pass.pass,
                    message(!pass.active      ? "state.off"
                                : pass.culled ? "state.culled"
                                              : "state.on"),
                    pass.turnedOff ? "warn" : "",
                    stageOf(pass.pass),
                    entry.order,
                    pass.global ? message("view.global") : std::to_string(pass.view),
                    entry.world,
                    pass.type,
                    pass.owner};
        }

        [[nodiscard]] u64 graphGeneration() const
        {
            return engine->services().contains<RenderGraphGenerationSingleton>()
                ? engine->services().read<RenderGraphGenerationSingleton>()->get()
                : 0;
        }

        // A global pass belongs to no view: it is listed once, on the editor tab. The game tab is the game view's
        // passes; every other view is the editor's.
        [[nodiscard]] PassEntry entryOf(RenderGraphPassRow row) const
        {
            const u32 gameView = engine->gameView();
            PassEntry entry;
            if (row.global)
            {
                entry.world = "-";
            }
            else
            {
                entry.tab = row.view == gameView ? 1 : 0;
                entry.world = row.view == gameView || engine->viewFollowsGame(row.view)
                    ? std::string(engine->gameWorldName())
                    : "scene";
            }
            entry.pass = std::move(row);
            return entry;
        }

        // The graph's passes now, in graph order.
        [[nodiscard]] std::vector<PassEntry> read() const
        {
            std::vector<PassEntry> entries;
            engine->services().queryTable("RenderGraph", "passes",
                                          [&](const void* row)
                                          {
                                              entries.push_back(entryOf(*static_cast<const RenderGraphPassRow*>(row)));
                                              entries.back().order = static_cast<u32>(entries.size() - 1);
                                          });
            return entries;
        }

        [[nodiscard]] bool matches(const u32 item, const std::string_view needle) const
        {
            const PassCells& row = cells[item];
            return containsIgnoringCase(row.pass, needle) || containsIgnoringCase(row.stage, needle) ||
                containsIgnoringCase(row.world, needle) || containsIgnoringCase(row.type, needle) ||
                containsIgnoringCase(row.owner, needle);
        }

        // -- the detail: what the selected pass reads, writes and waits on, its dependents and GPU time

        [[nodiscard]] PassDetail detailOf(const std::string& pass) const
        {
            PassDetail detail{.pass = pass};
            if (!engine->services().contains<RenderGraph>())
            {
                return detail;
            }
            const auto graph = engine->services().read<RenderGraph>();
            for (const RenderGraphAccessRow& access : graph->accessRows())
            {
                if (access.pass != pass)
                {
                    continue;
                }
                const bool depthRead = access.access == "depth" && !access.write;
                detail.reads += access.write ? 0 : 1;
                detail.writes += access.write ? 1 : 0;
                detail.rows.push_back(
                    {message(access.write ? "detail.kind.write" : "detail.kind.read"), access.resource,
                     message(depthRead ? "detail.access.depthRead" : "detail.access." + access.access)});
            }
            for (const RenderGraphBarrierRow& barrier : graph->barrierRows())
            {
                if (barrier.pass != pass)
                {
                    continue;
                }
                const std::string from = barrier.srcStage + " " + barrier.srcAccess;
                const std::string to = barrier.dstStage + " " + barrier.dstAccess;
                const MessageArg args[]{{"list", std::string_view(barrier.list)},
                                        {"from", std::string_view(from)},
                                        {"to", std::string_view(to)}};
                std::string info = message("detail.barrier", args);
                if (!barrier.oldLayout.empty())
                {
                    const MessageArg layout[]{{"old", std::string_view(barrier.oldLayout)},
                                              {"new", std::string_view(barrier.newLayout)}};
                    info += " (" + message("detail.layout", layout) + ")";
                }
                ++detail.barriers;
                detail.rows.push_back({message("detail.kind.barrier"),
                                       barrier.resource.empty() ? message("detail.memory") : barrier.resource,
                                       std::move(info)});
            }
            detail.dependents = graph->passDependents(InternedString(pass));
            return detail;
        }

        // The last collected GPU frame, if it is current and holds the pass. The renderer's timestamps come back when
        // a frame slot is reused, so a few frames of lag are current; a timed frame older than that is not.
        [[nodiscard]] GpuReading gpuOf(const std::string_view pass) const
        {
            if (!engine->services().contains<IRenderer>())
            {
                return {};
            }
            const auto renderer = engine->services().read<IRenderer>();
            const GpuFrameTimings& timings = renderer->lastGpuFrameTimings();
            if (timings.passes.empty())
            {
                return {};
            }
            if (renderer->recordedFrames() > timings.frame + GPU_STALE_FRAMES)
            {
                return {.state = "idle", .frame = timings.frame};
            }
            const auto timed = std::ranges::find(timings.passes, pass,
                                                 [](const GpuPassTiming& timing) { return timing.pass.toView(); });
            return timed == timings.passes.end()
                ? GpuReading{.state = "notRun", .frame = timings.frame}
                : GpuReading{.state = "timed", .ms = timed->ms, .frame = timings.frame};
        }

        [[nodiscard]] std::string gpuText(const GpuReading& gpu) const
        {
            if (gpu.state != "timed")
            {
                return message("gpu." + std::string(gpu.state));
            }
            const std::string ms = std::format("{:.3f}", gpu.ms);
            const MessageArg args[]{{"ms", std::string_view(ms)}, {"frame", static_cast<std::int64_t>(gpu.frame)}};
            return message("gpu.value", args);
        }

        [[nodiscard]] std::string neededText(const std::vector<std::string>& dependents) const
        {
            if (dependents.empty())
            {
                return message("needed.none");
            }
            std::string names;
            for (const std::string& name : dependents)
            {
                names += (names.empty() ? "" : ", ") + name;
            }
            const MessageArg args[]{{"passes", std::string_view(names)}};
            return message("needed.by", args);
        }

        void showNote()
        {
            const std::string gpu = gpuText(gpuOf(selectedPass));
            const std::string needed = neededText(shownDetail.dependents);
            const MessageArg args[]{{"gpu", std::string_view(gpu)}, {"needed", std::string_view(needed)}};
            if (const std::string note = message("detail.note", args); detailNote.text() != note)
            {
                detailNote.setText(note);
            }
        }

        void showDetail()
        {
            const bool selected = !selectedPass.empty();
            if (detailSlot.isValid() && detailSlot.isVisible() != selected)
            {
                detailSlot.setVisible(selected);
            }
            if (!selected)
            {
                return;
            }
            shownDetail = detailOf(selectedPass);
            const MessageArg args[]{{"pass", std::string_view(selectedPass)},
                                    {"reads", static_cast<std::int64_t>(shownDetail.reads)},
                                    {"writes", static_cast<std::int64_t>(shownDetail.writes)},
                                    {"barriers", static_cast<std::int64_t>(shownDetail.barriers)}};
            if (const std::string head = message("detail.head", args); detailHead.text() != head)
            {
                detailHead.setText(head);
            }
            showNote();
            detail->set(shownDetail.rows);
            sinceGpu = 0;
        }

        // The selection as the table holds it: one row, else none.
        void followSelection()
        {
            const std::span<const Entity> chosen = list->table().selection();
            const Entity key = chosen.size() == 1 ? chosen.front() : Entity::undefined();
            if (key == selectedKey)
            {
                return;
            }
            selectedKey = key;
            const auto row = std::ranges::find(keys, key);
            selectedPass = key != Entity::undefined() && row != keys.end() ? rows[row - keys.begin()].pass.pass : "";
            showDetail();
        }

        void showBadge()
        {
            const bool any = turnedOff > 0;
            if (any)
            {
                const MessageArg args[]{{"count", static_cast<std::int64_t>(turnedOff)}};
                if (const std::string text = message("badge", args); badge.text() != text)
                {
                    badge.setText(text);
                }
            }
            if (badge.isVisible() != any)
            {
                badge.setVisible(any);
                enableAll.setVisible(any);
            }
        }

        void refresh()
        {
            SPITE_ZONE("EditorPasses.refresh");
            ++refreshes;
            sinceRefresh = 0;
            all = read();
            turnedOff =
                static_cast<u32>(std::ranges::count_if(all, [](const PassEntry& e) { return e.pass.turnedOff; }));
            rows.clear();
            cells.clear();
            keys.clear();
            for (const PassEntry& entry : all)
            {
                if (entry.tab == tab)
                {
                    rows.push_back(entry);
                    cells.push_back(cellsOf(entry));
                    keys.push_back(keyOf(entry));
                }
            }
            seenLocale = localization ? localization->revision() : 0;
            showTab();
            showBadge();
            list->setItems(static_cast<u32>(rows.size()));
            // the table pruned its selection to the shown rows: the detail follows it, and reads the graph again
            selectedKey = Entity::undefined();
            selectedPass.clear();
            followSelection();
            if (selectedPass.empty())
            {
                showDetail();
            }
        }

        void showTab()
        {
            for (u32 i = 0; i < tabs.size(); ++i)
            {
                const std::string title = message(i == 0 ? "tab.editor" : "tab.game");
                if (tabs[i].text() != title)
                {
                    tabs[i].setText(title);
                    tabs[i].setAccessibleName(title);
                }
                showSelected(tabs[i], i == tab);
            }
        }

        void setTab(const u32 index)
        {
            tab = index;
            refresh();
        }

        void queueEnabled(const std::string& pass, const bool enabled)
        {
            pending.push_back(
                Json{{"command", "setPassEnabled"}, {"args", {{"pass", pass}, {"enabled", enabled}}}}.dump());
        }

        // Turning a pass off that live passes read from asks first: they get nothing from it until it is back.
        void turnOff(const std::string& pass)
        {
            std::vector<std::string> dependents;
            if (engine->services().contains<RenderGraph>())
            {
                dependents = engine->services().read<RenderGraph>()->passDependents(InternedString(pass));
            }
            if (dependents.empty())
            {
                queueEnabled(pass, false);
                return;
            }
            std::string names;
            for (sizet i = 0; i < std::min(dependents.size(), CONFIRM_NAMES); ++i)
            {
                names += (names.empty() ? "" : ", ") + dependents[i];
            }
            if (dependents.size() > CONFIRM_NAMES)
            {
                const MessageArg more[]{{"passes", std::string_view(names)},
                                        {"count", static_cast<std::int64_t>(dependents.size() - CONFIRM_NAMES)}};
                names = message("confirm.more", more);
            }
            const MessageArg title[]{{"pass", std::string_view(pass)}};
            const MessageArg body[]{{"count", static_cast<std::int64_t>(dependents.size())},
                                    {"passes", std::string_view(names)}};
            dialogs->confirm({.title = message("confirm.title", title),
                              .body = message("confirm.body", body),
                              .confirmLabel = message("confirm.accept"),
                              .destructive = true},
                             [this, pass](const bool confirmed)
                             {
                                 if (confirmed)
                                 {
                                     queueEnabled(pass, false);
                                 }
                             });
        }

        // Enter on a row, or a click on the state of an already selected row (`column` set): the pass toggles (a
        // command, so it undoes). Only a shown row toggles; clicks of one row within DOUBLE_CLICK of each other count
        // once, so a double-click flips the pass once instead of twice.
        void toggle(const Entity key, const InternedString column)
        {
            const auto row = static_cast<u32>(std::ranges::find(keys, key) - keys.begin());
            if (row >= rows.size() || !std::ranges::contains(list->shown(), row))
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
            // the graph now: another client's command may have moved it since the panel last read
            const std::string& pass = rows[row].pass.pass;
            const bool active = [&]
            {
                if (!engine->services().contains<RenderGraph>())
                {
                    return rows[row].pass.active;
                }
                const auto graph = engine->services().read<RenderGraph>();
                const RGPass* live = graph->findPass(InternedString(pass));
                return live ? live->isActive() : rows[row].pass.active;
            }();
            if (active)
            {
                turnOff(pass);
            }
            else
            {
                queueEnabled(pass, true);
            }
        }

        // passesSelect: the pass's tab shows and its row is the selection (the filter must show it).
        std::string select(const std::string& pass)
        {
            if (pass.empty())
            {
                list->table().setSelection({});
                followSelection();
                return {};
            }
            const std::vector<PassEntry> now = read();
            const auto found = std::ranges::find(now, pass, [](const PassEntry& entry) { return entry.pass.pass; });
            if (found == now.end())
            {
                return "no pass '" + pass + "'; the passesView query lists them (tab=all)";
            }
            if (found->tab != tab)
            {
                setTab(found->tab);
            }
            const Entity key = keyOf(*found);
            const auto row = static_cast<u32>(std::ranges::find(keys, key) - keys.begin());
            if (row >= rows.size() || !std::ranges::contains(list->shown(), row))
            {
                return "the filter hides '" + pass + "'";
            }
            list->table().setSelection(std::span<const Entity>(&key, 1));
            followSelection();
            return {};
        }

        // The selected pass and how many passes a command turned off, from the graph now; the detail of the selected
        // pass with its dependents, GPU time and rows.
        void writeDetail(IFormatWriter& out, const std::vector<PassEntry>& now) const
        {
            out.writeString("selectedPass", selectedPass);
            out.writeU32("turnedOff",
                         static_cast<u32>(
                             std::ranges::count_if(now, [](const PassEntry& entry) { return entry.pass.turnedOff; })));
            if (selectedPass.empty())
            {
                return;
            }
            const PassDetail current = detailOf(selectedPass);
            const GpuReading gpu = gpuOf(selectedPass);
            out.beginObject("detail");
            out.writeString("pass", selectedPass);
            out.writeU32("reads", current.reads);
            out.writeU32("writes", current.writes);
            out.writeU32("barriers", current.barriers);
            out.beginArray("dependents", static_cast<u32>(current.dependents.size()));
            for (const std::string& name : current.dependents)
            {
                out.writeString("", name);
            }
            out.endArray();
            out.beginObject("gpu");
            out.writeString("state", gpu.state);
            out.writeF64("ms", gpu.ms);
            out.writeU64("frame", gpu.frame);
            out.writeString("text", gpuText(gpu));
            out.endObject();
            out.beginArray("rows", static_cast<u32>(current.rows.size()));
            for (const DetailCells& row : current.rows)
            {
                out.beginObject("");
                writeValue(DetailCells::spiteSchema(), &row, out, FieldSet::All);
                out.endObject();
            }
            out.endArray();
            out.endObject();
        }

        void registerCommands()
        {
            control->registerViewCommand(
                "passesTab", "editorPasses", "sets the passes panel's tab: the editor's views or the game view",
                {requiredArg("tab", ArgKind::String, "editor or game", PASS_TABS)},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    ++ownCommands;
                    setTab(static_cast<u32>(std::ranges::find(PASS_TABS, args.string("tab").value_or("")) -
                                            PASS_TABS.begin()));
                    return std::string{};
                });
            control->registerViewCommand(
                "passesSelect", "editorPasses",
                "selects a pass in the passes panel (its tab shows) so the detail lists its reads, writes, barriers, "
                "dependents and GPU time; no pass clears the selection",
                {optionalArg("pass", ArgKind::String, "the pass, as the passes table names it", "none: clear")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const std::string error = select(args.string("pass").value_or(""));
                    ownCommands += error.empty() ? 1 : 0; // a refusal bumps nothing
                    return error;
                });
            control->registerQuery(
                "passesView", "editorPasses",
                "the passes panel's rows: the shown tab's, or tab=all/editor/game, narrowed by view and drawsWorld; "
                "each pass with its view, world (scene or play; - for a global pass), tab, global and turnedOff (a "
                "command switched it off), read from the graph now, also while the panel is hidden; selectedPass is "
                "the "
                "pass the detail shows, with its detail: reads, writes, barriers, dependents (live passes that read "
                "what it writes), gpu {state timed|none|idle|notRun, ms, frame, text} and the detail rows",
                {optionalArg("tab", ArgKind::String, "editor, game or all", "the shown tab", PASS_TAB_FILTERS),
                 optionalArg("view", ArgKind::Integer, "only this render view's passes"),
                 optionalArg("drawsWorld", ArgKind::String, "only passes whose view draws this world (scene, play)")},
                [this](const ControlArgs& args, IFormatWriter& out)
                {
                    const std::string filter = args.string("tab").value_or(std::string(PASS_TABS[tab]));
                    const std::optional<u64> view = args.integer("view");
                    const std::string world = args.string("drawsWorld").value_or("");
                    const std::vector<PassEntry> now = read();
                    std::vector<const PassEntry*> match;
                    for (const PassEntry& entry : now)
                    {
                        if ((filter == "all" || filter == PASS_TABS[entry.tab]) &&
                            (!view || entry.pass.view == *view) && (world.empty() || entry.world == world))
                        {
                            match.push_back(&entry);
                        }
                    }
                    out.writeString("tab", PASS_TABS[tab]);
                    out.beginArray("rows", static_cast<u32>(match.size()));
                    for (const PassEntry* entry : match)
                    {
                        out.beginObject("");
                        writeValue(RenderGraphPassRow::spiteSchema(), &entry->pass, out, FieldSet::All);
                        out.writeString("world", entry->world);
                        out.writeString("tab", PASS_TABS[entry->tab]);
                        out.endObject();
                    }
                    out.endArray();
                    out.writeU64("refreshes", refreshes);
                    writeDetail(out, now);
                    list->writeView(out);
                    return std::string{};
                });
        }
    };

    EditorPasses::EditorPasses(Engine& engine, EditorDialogs& dialogs) :
        m_state(std::make_unique<State>(engine, dialogs))
    {
        State& state = *m_state;
        state.root = state.scope.createPanel("editor.panel", columnDesc());
        for (u32 i = 0; i < state.tabs.size(); ++i)
        {
            state.tabs[i] = state.density.track(
                state.scope.createButton("passes.tab", "", barDesc(Vec2{TAB_WIDTH, 0.0f})), HEIGHT_CONTROL);
            state.tabs[i].onClick(
                [&state, i]
                {
                    state.pending.push_back(
                        Json{{"command", "passesTab"}, {"args", {{"tab", std::string(PASS_TABS[i])}}}}.dump());
                });
        }
        const auto hint = [](const std::string_view field)
        { return std::pair{InternedString(field), InternedString(std::string(TABLE) + "hint." + std::string(field))}; };
        state.list = std::make_unique<FilteredTable>(
            state.scope, *state.control,
            FilteredTableDesc{.id = "passes",
                              .owner = "editorPasses",
                              .nameKey = InternedString(std::string(TABLE) + "table"),
                              .schema = &PassCells::spiteSchema(),
                              .columns = {column("pass", 280.0f), column("state", 84.0f, true), column("stage", 96.0f),
                                          column("order", 64.0f), column("view", 64.0f), column("world", 64.0f),
                                          column("type", 84.0f), column("owner", 140.0f)},
                              .row = [&state](const u32 item) -> const void* { return &state.cells[item]; },
                              .key = [&state](const u32 item) { return state.keys[item]; },
                              .matches = [&state](const u32 item, const std::string_view needle)
                              { return state.matches(item, needle); },
                              .activate = [&state](const Entity key, const InternedString column)
                              { state.toggle(key, column); },
                              .headerHints = {hint("stage"), hint("state"), hint("order"), hint("view"), hint("world"),
                                              hint("owner")}});
        state.detail = std::make_unique<RowTable<DetailCells>>(
            state.scope, "passes.detail.table", InternedString(std::string(TABLE) + "detail.table"),
            std::vector{detailColumn("kind", 72.0f), detailColumn("resource", 240.0f), detailColumn("info", 520.0f)});

        const Density density = state.density.density();
        state.badge = state.density.track(
            state.scope.createLabel("passes.badge", "", fittedText(density.control, density.xs)), HEIGHT_CONTROL);
        bindFitPadding(state.badge, SPACE_XS, SPACE_NONE);
        state.badge.setStyleClass(InternedString("form.badge"));
        state.badge.setTooltip(InternedString(std::string(TABLE) + "badge.tip"));
        state.badge.setVisible(false);
        UiRectDesc enableDesc = barDesc(Vec2{});
        enableDesc.contentSize =
            UiContentSizeComponent{.fitWidth = true, .fitHeight = false, .padding = Vec2{density.md, 0.0f}};
        state.enableAll =
            state.density.track(state.scope.createButton("passes.enableAll", "", enableDesc), HEIGHT_BUTTON);
        bindFitPadding(state.enableAll, SPACE_MD, SPACE_NONE);
        state.enableAll.setLocalizedText(InternedString(std::string(TABLE) + "enableAll"));
        state.enableAll.setTooltip(InternedString(std::string(TABLE) + "enableAll.tip"));
        state.enableAll.setVisible(false);
        state.enableAll.onClick(
            [&state]
            { state.pending.push_back(Json{{"command", "setPassesEnabled"}, {"args", {{"enabled", true}}}}.dump()); });
        const auto noteLabel = [&state](const std::string_view id)
        {
            UiRectDesc desc = barDesc(Vec2{});
            desc.layoutItem = fillItem();
            desc.layoutItem->grow = 0.0f;
            Label label = state.scope.createLabel(InternedString(id), "", desc);
            label.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SIZE_ROW));
            label.setStyleClass(InternedString("list.filter.count"));
            return label;
        };
        state.detailHead = noteLabel("passes.detail.head");
        state.detailNote = noteLabel("passes.detail.note");
        state.showTab();
        state.registerCommands();
        state.scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(PASSES_BAR),
                      [&state](const Widget& slot)
                      {
                          for (Button& tab : state.tabs)
                          {
                              putIn(state.scope, slot, tab);
                              setOwnClass(tab, slotFields(state.scope, slot).childClass);
                          }
                      });
        contract.slot(InternedString(PASSES_ACTIONS),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.badge);
                          putIn(state.scope, slot, state.enableAll);
                      });
        contract.slot(InternedString(PASSES_FILTER), [&state](const Widget& slot) { state.list->fillFilter(slot); });
        contract.slot(InternedString(PASSES_ROWS), [&state](const Widget& slot) { state.list->fillRows(slot); });
        contract.slot(InternedString(PASSES_DETAIL),
                      [&state](const Widget& slot)
                      {
                          state.detailSlot = slot;
                          putIn(state.scope, slot, state.detailHead);
                          putIn(state.scope, slot, state.detailNote);
                          state.detail->fill(state.scope, slot);
                          state.detailSlot.setVisible(!state.selectedPass.empty());
                      });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                Panel toolbar = toolbarPanel(viewScope, PASSES_TOOLBAR, root);
                const Density density = Density::read(viewScope);
                Panel tabs = viewScope.createPanel(InternedString(PASSES_BAR),
                                                   barDesc(Vec2{2.0f * TAB_WIDTH + density.xs, density.control},
                                                           stack(UiLayoutDirection::Row, density.xs)));
                bindGap(tabs, SPACE_XS);
                tabs.setParent(toolbar);
                fitBarPanel(viewScope, PASSES_ACTIONS, toolbar);
                filterSlotPanel(viewScope, PASSES_FILTER, toolbar);
                // the list and the selected pass's detail side by side: the dock is wide and short
                Panel body = stackPanel(viewScope, PASSES_BODY, Vec2{}, UiLayoutDirection::Row, UiLayoutAlign::Stretch);
                body.setLayoutItem(fillItem());
                body.setParent(root);
                UiLayoutItemComponent wide = fillItem();
                wide.grow = 3.0f;
                UiLayoutItemComponent narrow = fillItem();
                narrow.grow = 2.0f;
                fillSlotPanel(viewScope, PASSES_ROWS, body).setLayoutItem(wide);
                Panel detail = fillSlotPanel(viewScope, PASSES_DETAIL, body);
                detail.setLayoutItem(narrow);
                detail.setVisible(false); // until a pass is selected
            });
        state.view = openView(engine, PASSES_VIEW, "assets/ui/editor/passes.json", std::move(contract), state.root);
    }

    EditorPasses::~EditorPasses() { m_state->control->unregisterOwner("editorPasses"); }

    Widget EditorPasses::content() const { return m_state->root; }

    void EditorPasses::update()
    {
        State& state = *m_state;
        if (!state.pending.empty())
        {
            dispatch(*state.control, state.pending, "passes");
            state.dirty = true; // its own commands: read at once
            state.seenCommand = state.control->commandGeneration();
        }
        const u64 before = state.control->commandGeneration();
        state.list->update();
        if (state.seenCommand == before) // its own filter and sort commands change nothing a refresh reads
        {
            state.seenCommand = state.control->commandGeneration();
        }
        if (!shown(state.scope, state.root))
        {
            state.wasShown = false;
            return;
        }
        if (const u64 command = state.control->commandGeneration(); command != state.seenCommand)
        {
            // another client's command: read after at most COMMAND_REFRESH_FRAMES (this panel's own view commands
            // read at once, in their handlers)
            state.stale |= command - state.seenCommand > state.ownCommands;
            state.seenCommand = command;
            state.ownCommands = 0;
        }
        ++state.sinceRefresh;
        const u64 graph = state.graphGeneration();
        const bool changed = !state.wasShown || state.dirty || graph != state.seenGraph ||
            (state.localization && state.localization->revision() != state.seenLocale) ||
            (state.stale && state.sinceRefresh >= COMMAND_REFRESH_FRAMES);
        if (changed || state.recheck)
        {
            state.recheck = changed;
            state.wasShown = true;
            state.dirty = false;
            state.stale = false;
            state.seenGraph = graph;
            state.refresh();
            return;
        }
        state.followSelection();
        if (!state.selectedPass.empty() && ++state.sinceGpu >= GPU_REFRESH_FRAMES)
        {
            state.sinceGpu = 0;
            state.showNote();
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "passes",
             .category = "debug",
             .order = 90,
             .dock = {.target = "project"},
             .zone = "EditorPasses",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<EditorPasses>(c.engine, c.dialogs); }}};
    } // namespace
} // namespace spite
