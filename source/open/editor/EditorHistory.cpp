#include "EditorHistory.hpp"
#include "EditorCommandLog.hpp"
#include "EditorContext.hpp"
#include "EditorDialogs.hpp"
#include "EditorFeedback.hpp"
#include "EditorPanelList.hpp"
#include "EditorPanelRegistry.hpp"

#include <chrono>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/ServiceLocator.hpp"
#include "control/ControlServer.hpp"
#include "control/UndoStepRow.hpp"
#include "ecs/World.hpp"
#include "engine/Engine.hpp"
#include "engine/PlayWorld.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "serialization/FormatWalk.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        using namespace panelui;

        constexpr std::string_view HISTORY_VIEW = "editor.history";
        constexpr std::string_view HISTORY_TOOLBAR = "history.toolbar";
        constexpr std::string_view HISTORY_ROWS = "history.rows";
        constexpr std::string_view HISTORY_STATUS = "history.status";
        constexpr std::string_view OWNER = "editorHistory";
        constexpr std::string_view TABLE = "editor-history.";
        constexpr std::string_view CAPTURES_ROOT = "captures";
        constexpr sizet MAX_STEPS = 1000; // the newest steps the panel lists (the undo limit's default)
        constexpr sizet MAX_TEXT_BYTES = 160;
        constexpr u32 REFRESH_FRAMES = 20; // a stream of changes is read this often at most

        // One undo step as the panel keeps it.
        struct Step
        {
            u64 step = 0;
            bool done = false;
            std::string text;
            std::string time;
            bool operator==(const Step&) const = default;
        };

        // What the table shows of a step (the strings are localized when the row is built); stateClass names the state
        // cell's class suffix (<rowClass>.text.current|undone, none for a done step), not a column.
        struct HistoryCells
        {
            std::string state;
            std::string stateClass;
            std::string step;
            std::string text;
            std::string time;
            bool operator==(const HistoryCells&) const = default;
            SPITE_OBSERVED_SCHEMA(HistoryCells, state, stateClass, step, text, time)
        };

        UiTableColumn column(const std::string_view field, const float width)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString(std::string(TABLE) + "column." + std::string(field)),
                    .sortable = false,
                    .classField = field == "state" ? InternedString("stateClass") : InternedString{}};
        }
    } // namespace

    struct EditorHistory::State
    {
        State(Engine& engine, EditorFeedback& feedback, EditorDialogs& dialogs) :
            engine(&engine), control(&engine.control()), feedback(&feedback), dialogs(&dialogs),
            scope(editorUi(engine).ui()), density(scope),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        EditorFeedback* feedback;
        EditorDialogs* dialogs;
        UiScope scope;
        DensityFollower density;
        const Localization* localization; // not owned: the engine's service
        Panel root;
        Label status;
        Button save;
        Button reveal;
        std::unique_ptr<RowTable<HistoryCells>> rows;
        std::unique_ptr<UiDocumentView> view;
        std::vector<std::function<void()>> pending; // between frames: jumps and saves

        // the history as last read: the displayed world's steps, oldest first, the newest MAX_STEPS of them
        std::string world = "scene";
        std::string seenWorld;
        u64 seenRevision = ~0ull;
        u64 frameRevision = ~0ull; // the revision the last shown frame saw (read or not)
        u32 quiet = 0; // shown frames since the revision last moved
        u32 sinceRead = 0; // shown frames since the last read
        bool forceRead = false; // the panel's own jump ran: read this frame
        std::vector<Step> steps;
        u64 total = 0; // the steps the history holds
        sizet done = 0; // of the shown steps
        std::vector<u64> stepOf; // per table row, newest first: the step index, 0 for the initial state
        Entity currentKey = Entity(1u, 1u); // the row of the current position
        bool stale = true; // the history changed since the table was built
        bool wasShown = false;
        u64 seenLocale = ~0ull;
        u64 reads = 0; // table reads so far
        u64 walks = 0; // full walks of the steps table (each builds every row)
        u64 builds = 0; // row sets built from the steps

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

        // Reads the steps when the history's revision moved or the world changed (two compares otherwise: a view
        // command or a query moves neither); the query and the shown panel both come here, so whichever sees the
        // change first leaves it for the other.
        void pull()
        {
            PlayWorld* play = engine->playWorld();
            const std::string_view now = play ? "play" : "scene";
            const ControlServer::UndoDepth depth = control->undoDepth(now);
            if (depth.revision == seenRevision && now == seenWorld)
            {
                return;
            }
            seenRevision = depth.revision;
            seenWorld = std::string(now);
            world = seenWorld;
            SPITE_ZONE("EditorHistory.read");
            ++reads;
            World& source = play ? play->world() : engine->world();
            const u64 count = u64{depth.undoable} + depth.redoable;
            const u64 skip = count > MAX_STEPS ? count - MAX_STEPS : 0;
            std::vector<Step> next;
            next.reserve(static_cast<sizet>(count - skip));
            u64 at = 0;
            ++walks;
            source.services().queryTable(
                "CommandHistory", "steps",
                [&](const void* row)
                {
                    if (at++ < skip)
                    {
                        return;
                    }
                    const auto& logged = *static_cast<const UndoStepRow*>(row);
                    std::string text = cutText(
                        logged.label.empty() ? commandSentence(logged.command, logged.args, &source) : logged.label,
                        MAX_TEXT_BYTES);
                    if (logged.commands > 1 && logged.label.empty())
                    {
                        text += std::format(" (+{})", logged.commands - 1);
                    }
                    if (!logged.label.empty() && (logged.added || logged.moved || logged.deleted))
                    {
                        text += std::format(" (+{} ~{} -{})", logged.added, logged.moved, logged.deleted);
                    }
                    next.push_back({logged.step, logged.done, std::move(text), logged.time});
                });
            if (next != steps || count != total)
            {
                stale = true;
            }
            steps = std::move(next);
            total = count;
            done = static_cast<sizet>(std::ranges::count_if(steps, &Step::done));
            currentKey = Entity(static_cast<u32>(steps.size() - done) + 1u, 1u);
        }

        [[nodiscard]] std::vector<HistoryCells> cellsOf() const
        {
            const auto state = [this](const bool isDone, const bool current)
            {
                return std::pair{message(current      ? "state.current"
                                             : isDone ? "state.done"
                                                      : "state.undone"),
                                 std::string(current      ? "current"
                                                 : isDone ? ""
                                                          : "undone")};
            };
            std::vector<HistoryCells> cells;
            cells.reserve(steps.size() + 1);
            for (sizet i = steps.size(); i-- > 0;)
            {
                const Step& step = steps[i];
                auto [text, kind] = state(step.done, i + 1 == done);
                cells.push_back(
                    {std::move(text), std::move(kind), std::to_string(step.step), step.text, localClock(step.time)});
            }
            auto [text, kind] = state(true, done == 0);
            cells.push_back({std::move(text), std::move(kind), "", message("start"), ""});
            return cells;
        }

        [[nodiscard]] std::vector<u64> stepsOf() const
        {
            std::vector<u64> ids;
            ids.reserve(steps.size() + 1);
            for (sizet i = steps.size(); i-- > 0;)
            {
                ids.push_back(steps[i].step);
            }
            ids.push_back(0);
            return ids;
        }

        [[nodiscard]] std::string statusLine() const
        {
            const MessageArg args[]{{"steps", static_cast<std::int64_t>(total)},
                                    {"shown", static_cast<std::int64_t>(steps.size())},
                                    {"undone", static_cast<std::int64_t>(steps.size() - done)},
                                    {"world", std::string_view(world)}};
            return message(total == 0 ? "empty" : steps.size() < total ? "truncated" : "count", args);
        }

        void rebuild()
        {
            SPITE_ZONE("EditorHistory.rebuild");
            ++builds;
            stale = false;
            stepOf = stepsOf();
            const bool moved = rows->table().selection().empty() || rows->table().selection().front() != currentKey;
            rows->set(cellsOf());
            if (moved)
            {
                rows->table().scrollTo(currentKey);
            }
            if (const std::string line = statusLine(); status.text() != line)
            {
                status.setText(line);
            }
            seenLocale = localization ? localization->revision() : 0;
        }

        // The selection shows the current position: a jump the history refused, or a selection that moved by other
        // means, returns to it.
        void syncSelection()
        {
            const std::span<const Entity> selected = rows->table().selection();
            if (selected.size() != 1 || selected.front() != currentKey)
            {
                rows->table().setSelection(std::span<const Entity>(&currentKey, 1));
            }
        }

        // A selection by the user: go to that step, queued like every panel action.
        void selected(const std::span<const Entity> keys)
        {
            if (keys.empty() || keys.back() == currentKey)
            {
                return;
            }
            const u32 row = keys.back().index() - 1;
            if (row >= stepOf.size())
            {
                return;
            }
            pending.push_back(
                [this, to = stepOf[row], on = world]
                {
                    const Json reply =
                        request(*control, Json{{"command", "historyJump"}, {"args", {{"world", on}, {"to", to}}}});
                    if (!reply.value("ok", false))
                    {
                        const std::string error = reply.value("error", std::string());
                        const MessageArg args[]{{"error", std::string_view(error)}};
                        feedback->postToastKey(ToastLevel::Warn, std::string(TABLE) + "jumpFailed", args);
                    }
                });
        }

        void askSave()
        {
            const auto now = std::chrono::floor<std::chrono::seconds>(
                std::chrono::current_zone()->to_local(std::chrono::system_clock::now()));
            dialogs->pickFile(
                {.mode = EditorPickMode::SaveFile,
                 .title = message("save.title"),
                 .roots = {std::string(CAPTURES_ROOT)},
                 .name = std::format("history-{:%Y%m%d-%H%M%S}", now),
                 .extensions = "jsonl"},
                [this](const std::optional<EditorPicked> picked)
                {
                    if (picked)
                    {
                        pending.push_back(
                            [this, path = picked->path, on = world]
                            {
                                const Json reply = request(
                                    *control, Json{{"query", "saveLog"}, {"args", {{"world", on}, {"path", path}}}});
                                if (reply.value("ok", false))
                                {
                                    const std::string saved = reply["result"].value("path", path);
                                    const MessageArg args[]{
                                        {"count", static_cast<std::int64_t>(reply["result"].value("requests", 0u))},
                                        {"path", std::string_view(saved)}};
                                    feedback->postToastKey(ToastLevel::Info, std::string(TABLE) + "saved", args);
                                    return;
                                }
                                const std::string error = reply.value("error", std::string());
                                const MessageArg args[]{{"error", std::string_view(error)}};
                                feedback->postToastKey(ToastLevel::Error, std::string(TABLE) + "saveFailed", args);
                            });
                    }
                });
        }

        void registerQuery()
        {
            control->registerViewCommand(
                "historyReveal", OWNER, "selects live entities affected by an undo step and returns the selected count",
                {requiredArg("step", ArgKind::Integer, "history step id"),
                 optionalArg("world", ArgKind::String, "history world", "scene")},
                [this](const ControlArgs& args, IFormatWriter& out) -> std::string
                {
                    const std::string target = args.string("world").value_or("scene");
                    const World* source = control->world(target);
                    if (!source)
                        return control->unknownWorld(target);
                    const Json history = request(*control, Json{{"query", "history"}, {"args", {{"world", target}}}});
                    if (!history.value("ok", false) || !history.contains("result"))
                        return "cannot read history";
                    const auto& steps = history["result"]["steps"];
                    for (const auto& step : steps)
                    {
                        if (step.value("step", u64{0}) != args.integer("step").value_or(0))
                            continue;
                        Json ids = Json::array();
                        for (const auto& id : step["entities"])
                            if (id.is_number_unsigned() && source->isEntityAlive(Entity(id.get<u64>())))
                                ids.push_back(id);
                        if (ids.empty())
                            return "this step has no live affected entities";
                        const Json selected =
                            request(*control, Json{{"command", "select"}, {"args", {{"world", target}, {"ids", ids}}}});
                        if (!selected.value("ok", false))
                            return selected.value("error", std::string("cannot select affected entities"));
                        if (target == "scene")
                            (void)request(*control,
                                          Json{{"command", "historyHighlight"},
                                               {"args", {{"step", args.integer("step").value_or(0)}}}});
                        out.writeU32("selected", static_cast<u32>(ids.size()));
                        return {};
                    }
                    return "history step is no longer available";
                });
            control->registerQuery(
                "historyView", OWNER,
                "the History panel: the displayed world, how many undo steps its history holds, how many are undone, "
                "the row of the current position, the selected row, the rows (newest first, the initial state last) "
                "with the cells as the table shows them and the step each goes to, and the reads made",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    // from the history itself, not the table: a hidden panel builds none
                    pull();
                    const std::vector<HistoryCells> current = cellsOf();
                    const std::vector<u64> ids = stepsOf();
                    const std::span<const Entity> chosen = rows->table().selection();
                    out.writeString("world", world);
                    out.writeU64("steps", total);
                    out.writeU32("undone", static_cast<u32>(steps.size() - done));
                    out.writeBool("truncated", steps.size() < total);
                    out.writeU32("current", currentKey.index() - 1);
                    out.writeI32("selected", chosen.empty() ? -1 : static_cast<i32>(chosen.front().index()) - 1);
                    out.writeU64("reads", reads);
                    out.writeU64("walks", walks);
                    out.writeU64("builds", builds);
                    out.beginArray("rows", static_cast<u32>(current.size()));
                    for (sizet i = 0; i < current.size(); ++i)
                    {
                        out.beginObject("");
                        writeValue(HistoryCells::spiteSchema(), &current[i], out, FieldSet::All);
                        out.writeU64("stepId", ids[i]);
                        out.endObject();
                    }
                    out.endArray();
                    return std::string{};
                });
        }
    };

    EditorHistory::EditorHistory(Engine& engine, EditorFeedback& feedback, EditorDialogs& dialogs) :
        m_state(std::make_unique<State>(engine, feedback, dialogs))
    {
        State& state = *m_state;
        state.root = state.scope.createPanel("editor.panel", columnDesc());
        state.rows = std::make_unique<RowTable<HistoryCells>>(
            state.scope, "history.table", InternedString(std::string(TABLE) + "table"),
            std::vector{column("state", 72.0f), column("step", 48.0f), column("text", 420.0f), column("time", 84.0f)});
        state.rows->table().onSelect([&state](const std::span<const Entity> keys) { state.selected(keys); });

        UiRectDesc saveDesc = barDesc(Vec2{});
        saveDesc.contentSize = UiContentSizeComponent{
            .fitWidth = true, .fitHeight = false, .padding = Vec2{state.density.density().md, 0.0f}};
        state.save = state.density.track(state.scope.createButton("history.save", "", saveDesc), HEIGHT_BUTTON);
        bindFitPadding(state.save, SPACE_MD, SPACE_NONE);
        state.save.setLocalizedText(InternedString(std::string(TABLE) + "save"));
        state.save.setTooltip(InternedString(std::string(TABLE) + "save.tip"));
        state.save.onClick([&state] { state.askSave(); });
        state.reveal = state.density.track(state.scope.createButton("history.reveal", "", saveDesc), HEIGHT_BUTTON);
        bindFitPadding(state.reveal, SPACE_MD, SPACE_NONE);
        state.reveal.setLocalizedText(InternedString(std::string(TABLE) + "reveal"));
        state.reveal.setTooltip(InternedString(std::string(TABLE) + "reveal.tip"));
        state.reveal.onClick(
            [&state]
            {
                state.pull();
                if (state.done == 0 || state.done > state.steps.size())
                    return;
                const u64 step = state.steps[state.done - 1].step;
                const std::string world = state.world;
                state.pending.push_back(
                    [&state, step, world]
                    {
                        (void)request(*state.control,
                                      Json{{"command", "historyReveal"}, {"args", {{"step", step}, {"world", world}}}});
                    });
            });

        // as high as a row of the density, so the status bar (size.row) holds it at any density
        UiRectDesc statusDesc = barDesc(Vec2{});
        statusDesc.layoutItem = fillItem();
        state.status = state.scope.createLabel("history.count", "", statusDesc);
        state.status.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SIZE_ROW));
        state.status.setStyleClass(InternedString("list.filter.count"));
        state.registerQuery();
        state.scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(HISTORY_TOOLBAR),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.save);
                          putIn(state.scope, slot, state.reveal);
                          state.save.setStyleClass(slotFields(state.scope, slot).childClass);
                          state.reveal.setStyleClass(slotFields(state.scope, slot).childClass);
                      });
        contract.slot(InternedString(HISTORY_ROWS),
                      [&state](const Widget& slot) { state.rows->fill(state.scope, slot); });
        contract.slot(InternedString(HISTORY_STATUS),
                      [&state](const Widget& slot) { putIn(state.scope, slot, state.status); });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                toolbarPanel(viewScope, HISTORY_TOOLBAR, root, SPACE_XS, UiLayoutAlign::Center);
                fillSlotPanel(viewScope, HISTORY_ROWS, root);
                statusPanel(viewScope, HISTORY_STATUS, root);
            });
        state.view = openView(engine, HISTORY_VIEW, "assets/ui/editor/history.json", std::move(contract), state.root);
    }

    EditorHistory::~EditorHistory() { m_state->control->unregisterOwner(OWNER); }

    Widget EditorHistory::content() const { return m_state->root; }

    void EditorHistory::update()
    {
        State& state = *m_state;
        if (!state.pending.empty())
        {
            std::vector<std::function<void()>> actions;
            actions.swap(state.pending);
            for (const std::function<void()>& action : actions)
            {
                action();
            }
            state.forceRead = true;
        }
        if (!shown(state.scope, state.root))
        {
            state.wasShown = false; // a hidden panel reads nothing; showing it catches up
            return;
        }
        // The adapter's own guard is the history's revision and the displayed world. A stream of changes (a gizmo
        // drag folds into the newest step every frame) is read after a quiet frame or REFRESH_FRAMES at most; a
        // first show, another world and the panel's own jump read at once, so a click is never shown stale.
        const std::string_view now = state.engine->playWorld() ? "play" : "scene";
        const u64 revision = state.control->undoDepth(now).revision;
        state.quiet = revision == state.frameRevision ? state.quiet + 1 : 0;
        state.frameRevision = revision;
        ++state.sinceRead;
        if ((revision != state.seenRevision || now != state.seenWorld) &&
            (state.forceRead || !state.wasShown || now != state.seenWorld || state.quiet >= 1 ||
             state.sinceRead >= REFRESH_FRAMES))
        {
            state.pull();
            state.sinceRead = 0;
        }
        state.forceRead = false;
        const bool changed = state.stale || !state.wasShown ||
            (state.localization && state.localization->revision() != state.seenLocale);
        state.wasShown = true;
        if (changed)
        {
            state.rebuild();
        }
        state.syncSelection();
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "history",
             .category = "tools",
             .order = 195,
             .dock = {.target = "project"},
             .zone = "EditorHistory",
             .make = [](EditorPanelContext& c)
             { return makeEditorPanel<EditorHistory>(c.engine, c.feedback, c.dialogs); }}};
    } // namespace
} // namespace spite
