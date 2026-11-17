#include "EditorActivity.hpp"
#include "EditorCommandLog.hpp"
#include "EditorContext.hpp"
#include "EditorPanelList.hpp"
#include "EditorPanelRegistry.hpp"

#include <ctime>
#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/ServiceLocator.hpp"
#include "control/ControlServer.hpp"
#include "control/CommandLogRow.hpp"
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

        constexpr std::string_view ACTIVITY_VIEW = "editor.activity";
        constexpr std::string_view ACTIVITY_ROWS = "activity.rows";
        constexpr std::string_view ACTIVITY_STATUS = "activity.status";
        constexpr std::string_view OWNER = "editorActivity";
        constexpr std::string_view TABLE = "editor-activity.";

        // What the table shows of a command (the strings are localized when the row is built); sourceClass names the
        // source cell's class suffix (<rowClass>.text.you|agent|host), not a column.
        struct ActivityCells
        {
            std::string source;
            std::string sourceClass;
            std::string client;
            std::string time;
            std::string text;
            std::string result;
            bool operator==(const ActivityCells&) const = default;
            SPITE_OBSERVED_SCHEMA(ActivityCells, source, sourceClass, client, time, text, result)
        };

        UiTableColumn column(const std::string_view field, const float width)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString(std::string(TABLE) + "column." + std::string(field)),
                    .sortable = false,
                    .classField = field == "source" ? InternedString("sourceClass") : InternedString{}};
        }

        // the editor's own requests are YOU; a connection's (pipe or stdio) are AGENT; other in-process callers HOST
        std::string_view classOf(const std::string_view source)
        {
            return source == "editor" ? "you" : source == "host" ? "host" : "agent";
        }
    } // namespace

    struct EditorActivity::State
    {
        explicit State(Engine& engine) :
            engine(&engine), control(&engine.control()), scope(editorUi(engine).ui()), log(engine),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        UiScope scope;
        EditorCommandLog log;
        const Localization* localization; // not owned: the engine's service
        Panel root;
        Label status;
        Label empty; // the body's note while the log holds no command; the count line takes its place after
        std::unique_ptr<RowTable<ActivityCells>> rows;
        std::unique_ptr<UiDocumentView> view;
        bool wasShown = false;
        bool stale = true; // the log changed since the table was built
        u64 seenLocale = ~0ull;
        u64 builds = 0; // row sets built from the log

        [[nodiscard]] std::string message(const std::string_view key, const std::vector<MessageArg>& args = {}) const
        {
            const std::string full = std::string(TABLE) + std::string(key);
            if (!localization)
            {
                return full;
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(full), args, scratch));
        }

        // reads the log when a command ran or the world changed (two integer compares otherwise); the query and the
        // shown tab both come here, so whichever sees the change first leaves it for the other
        void pull()
        {
            if (log.refresh())
            {
                stale = true;
            }
        }

        [[nodiscard]] std::vector<ActivityCells> cellsOf() const
        {
            const std::string you = message("source.you");
            const std::string agent = message("source.agent");
            const std::string host = message("source.host");
            const std::string ok = message("result.ok");
            std::vector<ActivityCells> cells;
            cells.reserve(log.entries().size());
            for (const CommandLogEntry& entry : log.entries())
            {
                const std::string_view kind = classOf(entry.source);
                cells.push_back({kind == "you"        ? you
                                     : kind == "host" ? host
                                                      : agent,
                                 std::string(kind), entry.client, localClock(entry.time), entry.text,
                                 entry.failed ? entry.result : ok});
            }
            return cells;
        }

        void rebuild()
        {
            SPITE_ZONE("EditorActivity.rebuild");
            ++builds;
            stale = false;
            std::vector<ActivityCells> cells = cellsOf();
            const bool atEnd = rows->table().atEnd();
            if (rows->set(std::move(cells)) && atEnd)
            {
                rows->table().scrollToEnd(); // the newest row stays in view while it was
            }
            const bool none = log.entries().empty();
            rows->setHeaderOnly(none);
            empty.setVisible(none);
            status.setVisible(!none);
            if (none)
            {
                if (const std::string line = message("empty"); empty.text() != line)
                {
                    empty.setText(line);
                }
            }
            else if (const std::string line = message("count",
                                                      {{"shown", static_cast<std::int64_t>(log.entries().size())},
                                                       {"total", static_cast<std::int64_t>(log.total())},
                                                       {"world", log.world()}});
                     status.text() != line)
            {
                status.setText(line);
            }
            seenLocale = localization ? localization->revision() : 0;
        }

        void registerQuery()
        {
            control->registerQuery(
                "activityView", OWNER,
                "the Activity tab: the displayed world, how many commands its log holds, the rows shown (newest "
                "last) with the cells as the table shows them, and the reads made",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    // from the log itself, not the table: a hidden tab builds no table
                    pull();
                    const std::vector<ActivityCells> current = cellsOf();
                    out.writeString("world", log.world());
                    out.writeU64("total", log.total());
                    out.writeU64("reads", log.reads());
                    out.writeU64("builds", builds);
                    out.beginArray("rows", static_cast<u32>(current.size()));
                    for (const ActivityCells& cells : current)
                    {
                        out.beginObject("");
                        writeValue(ActivityCells::spiteSchema(), &cells, out, FieldSet::All);
                        out.endObject();
                    }
                    out.endArray();
                    return std::string{};
                });
        }
    };

    EditorActivity::EditorActivity(Engine& engine) : m_state(std::make_unique<State>(engine))
    {
        State& state = *m_state;
        state.root = state.scope.createPanel("editor.panel", columnDesc());
        state.rows = std::make_unique<RowTable<ActivityCells>>(
            state.scope, "activity.table", InternedString(std::string(TABLE) + "table"),
            std::vector{column("source", 72.0f), column("client", 120.0f), column("time", 84.0f),
                        column("text", 460.0f), column("result", 200.0f)});
        // the source is what the request reported: the header says so
        state.rows->table()
            .headerCell(InternedString("source"))
            .setTooltip(InternedString(std::string(TABLE) + "hint.source"));
        // as high as a row of the density, so the status bar (size.row) holds it at any density
        UiRectDesc statusDesc = barDesc(Vec2{});
        statusDesc.layoutItem = fillItem();
        state.status = state.scope.createLabel("activity.count", "", statusDesc);
        state.status.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SIZE_ROW));
        state.status.setStyleClass(InternedString("list.filter.count"));
        state.empty = emptyNote(state.scope, "activity.empty");
        state.registerQuery();
        state.scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(ACTIVITY_ROWS),
                      [&state](const Widget& slot)
                      {
                          state.rows->fill(state.scope, slot);
                          putIn(state.scope, slot, state.empty);
                      });
        contract.slot(InternedString(ACTIVITY_STATUS),
                      [&state](const Widget& slot) { putIn(state.scope, slot, state.status); });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                fillSlotPanel(viewScope, ACTIVITY_ROWS, root);
                statusPanel(viewScope, ACTIVITY_STATUS, root);
            });
        state.view = openView(engine, ACTIVITY_VIEW, "assets/ui/editor/activity.json", std::move(contract), state.root);
    }

    EditorActivity::~EditorActivity() { m_state->control->unregisterOwner(OWNER); }

    Widget EditorActivity::content() const { return m_state->root; }

    void EditorActivity::update()
    {
        State& state = *m_state;
        if (!shown(state.scope, state.root))
        {
            state.wasShown = false; // a hidden panel reads nothing; showing it catches up
            return;
        }
        // the adapter's own guard is the command generation and the displayed world
        state.pull();
        const bool changed = state.stale || !state.wasShown ||
            (state.localization && state.localization->revision() != state.seenLocale);
        state.wasShown = true;
        if (changed)
        {
            state.rebuild();
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "activity",
             .category = "tools",
             .order = 190,
             .dock = {.target = "project"},
             .zone = "EditorActivity",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<EditorActivity>(c.engine); }}};
    } // namespace
} // namespace spite
