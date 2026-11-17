#include "EditorEcs.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "editor/EditorModel.hpp"
#include "EditorPanelList.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "serialization/FormatWalk.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using namespace panelui;

        constexpr std::string_view ECS_VIEW = "editor.ecs";
        constexpr std::string_view ECS_BAR = "ecs.bar";
        constexpr std::string_view ECS_FILTER = "ecs.filter";
        constexpr std::string_view ECS_ROWS = "ecs.rows";
        constexpr std::string_view ECS_STATUS = "ecs.status";
        constexpr std::string_view ECS_TOOLBAR = "ecs.toolbar";
        constexpr std::string_view ECS_FILTERBAR = "ecs.filterbar";

        enum Page : u32
        {
            Worlds,
            Archetypes,
            Services,
            PlayMap,
            PageCount
        };
        constexpr std::array<std::string_view, PageCount> PAGES{"worlds", "archetypes", "services", "playMap"};
        constexpr float TAB_PAD = 16.0f; // a tab and Refresh are as wide as their text and this much
        // bytes as whole KiB, rounded up (a non-empty chunk never reads 0): the memory columns stay numbers that sort
        constexpr u64 kib(const u64 bytes) { return (bytes + 1023) / 1024; }

        UiRectDesc fittedButton()
        {
            UiRectDesc desc = barDesc(Vec2{});
            desc.contentSize =
                UiContentSizeComponent{.fitWidth = true, .fitHeight = false, .padding = Vec2{TAB_PAD, 0.0f}};
            return desc;
        }
        constexpr u32 MAP_ROWS = 200;
        constexpr u32 MAP_SELECTED = 32; // selected entities looked up one by one

        struct WorldCells
        {
            std::string world;
            std::string shown;
            std::string state;
            u64 entities = 0;
            u64 archetypes = 0;
            u64 chunks = 0;
            u64 memory = 0;
            u64 wasted = 0;
            bool operator==(const WorldCells&) const = default;
            SPITE_OBSERVED_SCHEMA(WorldCells, world, shown, state, entities, archetypes, chunks, memory, wasted)
        };

        struct ArchetypeCells
        {
            std::string components;
            u64 entities = 0;
            u64 chunks = 0;
            u64 memory = 0;
            u64 wasted = 0;
            u64 fill = 0;
            SPITE_OBSERVED_SCHEMA(ArchetypeCells, components, entities, chunks, memory, wasted, fill)
        };

        struct ServiceCells
        {
            std::string service;
            std::string scope;
            std::string tables;
            bool operator==(const ServiceCells&) const = default;
            SPITE_OBSERVED_SCHEMA(ServiceCells, service, scope, tables)
        };

        struct MapCells
        {
            u64 edit = 0;
            u64 play = 0;
            bool operator==(const MapCells&) const = default;
            SPITE_OBSERVED_SCHEMA(MapCells, edit, play)
        };

        UiTableColumn column(const std::string_view field, const float width)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString("editor-ecs.column." + std::string(field))};
        }

        template <class Cells>
        void writeRows(IFormatWriter& out, const char* key, const std::vector<Cells>& rows)
        {
            out.beginArray(key, static_cast<u32>(rows.size()));
            for (const Cells& row : rows)
            {
                out.beginObject("");
                writeValue(Cells::spiteSchema(), &row, out, FieldSet::All);
                out.endObject();
            }
            out.endArray();
        }
    } // namespace

    struct EditorEcs::State
    {
        State(Engine& engine, const EditorModel& editorModel) :
            engine(&engine), control(&engine.control()), scope(editorUi(engine).ui()), editorModel(&editorModel),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        UiScope scope;
        DensityFollower density{scope};
        const EditorModel* editorModel;
        const Localization* localization;
        Panel root;
        std::array<Button, PageCount> tabs;
        Button refreshButton;
        Label note;
        std::unique_ptr<FilteredTable> archetypes;
        std::unique_ptr<RowTable<WorldCells>> worlds;
        std::unique_ptr<RowTable<ServiceCells>> services;
        std::unique_ptr<RowTable<MapCells>> playMap;
        std::unique_ptr<UiDocumentView> view;
        std::vector<ArchetypeCells> archetypeRows;
        std::vector<Entity> archetypeKeys; // parallel to archetypeRows
        std::unordered_map<std::string, u32> keyIndex; // components -> key, stable for the session
        std::vector<std::string> pending;
        std::vector<Entity> seenSelection;
        std::string archetypeSignature;
        std::string noteShown;
        std::string world = "scene"; // the world the Archetypes and Services tabs inspect
        u32 page = Worlds;
        u64 mapTotal = 0;
        u64 seenLocale = 0;
        u64 refreshes = 0;
        Entity selectedArchetype = Entity::undefined();
        bool wasShown = false;
        bool playing = false;
        bool due = false; // a refresh was asked for
        u64 seenCommand = 0;
        bool noteDirty = true;

        [[nodiscard]] std::string message(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            std::string scratch;
            return localization ? std::string(localization->resolve(InternedString(key), args, scratch))
                                : std::string(key);
        }

        [[nodiscard]] Json read(const char* name, const Json& args = Json::object()) const
        {
            return request(*control, Json{{"query", name}, {"args", args}}).value("result", Json::object());
        }

        void readWorlds()
        {
            std::vector<WorldCells> rows;
            const std::string_view working = engine->gameWorldName();
            for (const Json& entry : read("worlds").value("worlds", Json::array()))
            {
                const Json stats = entry.value("stats", Json::object());
                const std::string name = entry.value("name", std::string{});
                rows.push_back(
                    {name, name == working ? message("editor-ecs.shown") : std::string{},
                     message(entry.value("paused", false) ? "editor-ecs.state.paused" : "editor-ecs.state.running"),
                     stats.value("activeEntityCount", u64{0}), stats.value("archetypeCount", u64{0}),
                     stats.value("totalChunkCount", u64{0}), kib(stats.value("totalChunkMemoryBytes", u64{0})),
                     kib(stats.value("totalWastedMemoryUnusedSlots", u64{0}))});
            }
            if (worlds->set(std::move(rows)) || worlds->table().selection().empty())
            {
                const auto& shownRows = worlds->rows();
                const auto at = std::ranges::find(shownRows, world, &WorldCells::world);
                const Entity key = Entity(static_cast<u32>(at - shownRows.begin()) + 1, 1u);
                if (at != shownRows.end())
                {
                    worlds->table().setSelection(std::span(&key, 1));
                }
            }
        }

        void readArchetypes()
        {
            const Json reply = read(
                "tableRows", {{"service", "EcsTables"}, {"table", "archetypes"}, {"world", world}, {"limit", 10000}});
            archetypeRows.clear();
            archetypeKeys.clear();
            std::string signature;
            for (const Json& row : reply.value("rows", Json::array()))
            {
                const std::string components = row.value("components", std::string{});
                std::string text;
                for (const char c : components)
                {
                    text += c == ',' ? std::string(", ") : std::string(1, c);
                }
                archetypeRows.push_back({text.empty() ? message("editor-ecs.noComponents") : text,
                                         row.value("entityCount", u64{0}), row.value("chunkCount", u64{0}),
                                         kib(row.value("memoryBytes", u64{0})), kib(row.value("wastedBytes", u64{0})),
                                         static_cast<u64>(std::llround(row.value("utilization", 0.0) * 100.0))});
                const auto [it, added] = keyIndex.try_emplace(components, static_cast<u32>(keyIndex.size() + 1));
                archetypeKeys.push_back(Entity(it->second, 1u));
                const ArchetypeCells& c = archetypeRows.back();
                signature += std::format("{}|{}|{}|{}|{}|{}|{}\n", c.components, c.entities, c.chunks, c.memory,
                                         c.wasted, c.fill, it->second);
            }
            // a read that found nothing changed writes nothing
            if (signature != archetypeSignature)
            {
                archetypeSignature = std::move(signature);
                archetypes->setItems(static_cast<u32>(archetypeRows.size()));
            }
        }

        void readServices()
        {
            std::vector<ServiceCells> rows;
            for (const Json& entry : read("services", {{"world", world}}).value("services", Json::array()))
            {
                std::string tables;
                for (const Json& table : entry.value("tables", Json::array()))
                {
                    tables += (tables.empty() ? "" : ", ") + table.get<std::string>();
                }
                rows.push_back(
                    {entry.value("name", std::string{}),
                     message(entry.value("depth", 0u) == 0 ? "editor-ecs.scope.world" : "editor-ecs.scope.root"),
                     std::move(tables)});
            }
            services->set(std::move(rows));
        }

        void readPlayMap()
        {
            std::vector<MapCells> rows;
            mapTotal = 0;
            if (engine->playWorld())
            {
                const std::span<const Entity> selection = editorModel->selection();
                if (selection.empty())
                {
                    const Json reply = read("playMap", {{"limit", MAP_ROWS}});
                    mapTotal = reply.value("total", u64{0});
                    for (const Json& pair : reply.value("entities", Json::array()))
                    {
                        rows.push_back({pair.value("edit", u64{0}), pair.value("play", u64{0})});
                    }
                }
                const bool selectedInPlay = editorModel->selectionWorld() == "play";
                for (u32 i = 0; i < selection.size() && i < MAP_SELECTED; ++i)
                {
                    const Json reply = request(
                        *control,
                        Json{{"query", "playMap"}, {"args", {{selectedInPlay ? "play" : "edit", selection[i].id()}}}});
                    for (const Json& pair : reply.value("result", Json::object()).value("entities", Json::array()))
                    {
                        rows.push_back({pair.value("edit", u64{0}), pair.value("play", u64{0})});
                    }
                }
                mapTotal = selection.empty() ? mapTotal : rows.size();
            }
            playMap->set(std::move(rows));
        }

        void showNote()
        {
            noteDirty = false;
            std::string text;
            const auto count = [](const auto& rows) { return static_cast<std::int64_t>(rows.size()); };
            const std::string_view name = world;
            if (page == Worlds)
            {
                const MessageArg args[]{{"worlds", count(worlds->rows())}, {"world", name}};
                text = message("editor-ecs.note.worlds", args);
            }
            else if (page == Archetypes)
            {
                const auto at = std::ranges::find(archetypeKeys, selectedArchetype);
                if (at != archetypeKeys.end() && archetypes->table().selection().size() == 1)
                {
                    const ArchetypeCells& row = archetypeRows[static_cast<u32>(at - archetypeKeys.begin())];
                    const MessageArg args[]{{"components", std::string_view(row.components)},
                                            {"entities", static_cast<std::int64_t>(row.entities)},
                                            {"chunks", static_cast<std::int64_t>(row.chunks)},
                                            {"memory", static_cast<std::int64_t>(row.memory)},
                                            {"wasted", static_cast<std::int64_t>(row.wasted)},
                                            {"fill", static_cast<std::int64_t>(row.fill)}};
                    text = message("editor-ecs.note.archetype", args);
                }
                else
                {
                    const MessageArg args[]{{"archetypes", count(archetypeRows)}, {"world", name}};
                    text = message("editor-ecs.note.archetypes", args);
                }
            }
            else if (page == Services)
            {
                const MessageArg args[]{{"services", count(services->rows())}, {"world", name}};
                text = message("editor-ecs.note.services", args);
            }
            else if (!engine->playWorld())
            {
                text = message("editor-ecs.note.notPlaying");
            }
            else
            {
                const MessageArg args[]{{"pairs", count(playMap->rows())},
                                        {"total", static_cast<std::int64_t>(mapTotal)},
                                        {"selected", static_cast<std::int64_t>(editorModel->selection().size())}};
                text = message(editorModel->selection().empty() ? "editor-ecs.note.map" : "editor-ecs.note.mapSelected",
                               args);
            }
            if (text != noteShown)
            {
                noteShown = std::move(text);
                note.setText(noteShown);
            }
        }

        // reads the shown tab's rows
        void refresh()
        {
            SPITE_ZONE("EditorEcs.refresh");
            ++refreshes;
            due = false;
            playing = engine->playWorld() != nullptr;
            if (!control->world(world)) // the play world after Stop, any world that was removed
            {
                world = "scene";
            }
            switch (page)
            {
            case Worlds:
                readWorlds();
                break;
            case Archetypes:
                readArchetypes();
                break;
            case Services:
                readServices();
                break;
            default:
                readPlayMap();
                seenSelection.assign(editorModel->selection().begin(), editorModel->selection().end());
                break;
            }
            seenLocale = localization ? localization->revision() : 0;
            showNote();
        }

        void showPage()
        {
            for (u32 i = 0; i < PageCount; ++i)
            {
                tabs[i].setLocalizedText(InternedString("editor-ecs.tab." + std::string(PAGES[i])));
                showSelected(tabs[i], i == page);
            }
            archetypes->setVisible(page == Archetypes);
            for (const auto& [index, table] : {std::pair{u32(Worlds), worlds->table()},
                                               {u32(Services), services->table()},
                                               {u32(PlayMap), playMap->table()}})
            {
                if (table.isVisible() != (page == index))
                {
                    Widget(table).setVisible(page == index);
                }
            }
        }

        void setPage(const u32 next)
        {
            page = next;
            showPage();
            due = true;
        }

        void registerCommands()
        {
            control->registerViewCommand("ecsPage", "editorEcs", "sets the tab the ECS panel shows",
                                         {requiredArg("page", ArgKind::String, "the tab", PAGES)},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         {
                                             const auto at = std::ranges::find(PAGES, args.string("page").value_or(""));
                                             if (at == PAGES.end())
                                             {
                                                 return std::string("unknown page");
                                             }
                                             setPage(static_cast<u32>(at - PAGES.begin()));
                                             return std::string{};
                                         });
            control->registerViewCommand("ecsWorld", "editorEcs",
                                         "sets the world the ECS panel's archetypes and services show",
                                         {requiredArg("world", ArgKind::String, "a world's name, as worlds lists it")},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         {
                                             const std::string next = args.string("world").value_or("");
                                             if (!control->world(next))
                                             {
                                                 return control->unknownWorld(next);
                                             }
                                             world = next;
                                             due = true;
                                             return std::string{};
                                         });
            control->registerViewCommand("ecsRefresh", "editorEcs", "reads the shown tab of the ECS panel again", {},
                                         [this](const ControlArgs&, IFormatWriter&)
                                         {
                                             due = true;
                                             return std::string{};
                                         });
            control->registerQuery(
                "ecsView", "editorEcs",
                "the ECS panel: its tab and inspected world, the rows of each tab read so far (worlds, archetypes, "
                "services, playMap), the note, the refresh counter and the archetype list's filter, sort and cells",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    out.writeString("page", PAGES[page]);
                    out.writeString("world", world);
                    writeRows(out, "worlds", worlds->rows());
                    writeRows(out, "archetypes", archetypeRows);
                    writeRows(out, "services", services->rows());
                    writeRows(out, "playMap", playMap->rows());
                    out.writeU64("mapTotal", mapTotal);
                    out.writeString("note", noteShown);
                    out.writeU64("refreshes", refreshes);
                    archetypes->writeView(out);
                    return std::string{};
                });
        }
    };

    EditorEcs::EditorEcs(Engine& engine, const EditorModel& editorModel) :
        m_state(std::make_unique<State>(engine, editorModel))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.root = scope.createPanel("editor.panel", columnDesc());
        for (u32 i = 0; i < PageCount; ++i)
        {
            state.tabs[i] = scope.createButton("ecs.tab", "", fittedButton());
            state.density.track(state.tabs[i], HEIGHT_CONTROL);
            state.tabs[i].setAccessibleNameKey(InternedString("editor-ecs.tab." + std::string(PAGES[i])));
            state.tabs[i].onClick(
                [&state, i]
                {
                    state.pending.push_back(
                        Json{{"command", "ecsPage"}, {"args", {{"page", std::string(PAGES[i])}}}}.dump());
                });
        }
        state.refreshButton = scope.createButton("ecs.refresh", "", fittedButton());
        state.density.track(state.refreshButton, HEIGHT_CONTROL);
        state.refreshButton.setLocalizedText(InternedString("editor-ecs.refresh"));
        state.refreshButton.onClick([&state] { state.pending.push_back(Json{{"command", "ecsRefresh"}}.dump()); });
        UiRectDesc noteDesc = barDesc(Vec2{});
        noteDesc.layoutItem = fillItem();
        state.note = scope.createLabel("ecs.note", "", noteDesc);
        state.density.track(state.note, HEIGHT_ROW);
        state.note.setStyleClass(InternedString("ecs.note"));

        state.archetypes = std::make_unique<FilteredTable>(
            scope, *state.control,
            FilteredTableDesc{.id = "ecs",
                              .owner = "editorEcs",
                              .nameKey = InternedString("editor-ecs.table.archetypes"),
                              .schema = &ArchetypeCells::spiteSchema(),
                              .columns = {column("components", 280.0f), column("entities", 112.0f),
                                          column("chunks", 88.0f), column("memory", 120.0f), column("wasted", 120.0f),
                                          column("fill", 112.0f)},
                              .row = [&state](const u32 item) -> const void* { return &state.archetypeRows[item]; },
                              .key = [&state](const u32 item) { return state.archetypeKeys[item]; },
                              .matches = [&state](const u32 item, const std::string_view needle)
                              { return containsIgnoringCase(state.archetypeRows[item].components, needle); },
                              .headerHints = {{InternedString("wasted"), InternedString("editor-ecs.hint.wasted")},
                                              {InternedString("fill"), InternedString("editor-ecs.hint.fill")}}});
        state.archetypes->table().onSelect(
            [&state](const std::span<const Entity> keys)
            {
                state.selectedArchetype = keys.empty() ? Entity::undefined() : keys[0];
                state.noteDirty = true;
            });
        state.worlds = std::make_unique<RowTable<WorldCells>>(
            scope, "ecs.worlds.table", InternedString("editor-ecs.table.worlds"),
            std::vector{column("world", 120.0f), column("shown", 96.0f), column("state", 96.0f),
                        column("entities", 112.0f), column("archetypes", 128.0f), column("chunks", 88.0f),
                        column("memory", 120.0f), column("wasted", 120.0f)});
        state.worlds->table().onSelect(
            [&state](const std::span<const Entity> keys)
            {
                const auto& rows = state.worlds->rows();
                if (!keys.empty() && keys[0].id() >= 1 && keys[0].id() <= rows.size() &&
                    rows[keys[0].id() - 1].world != state.world)
                {
                    state.pending.push_back(
                        Json{{"command", "ecsWorld"}, {"args", {{"world", rows[keys[0].id() - 1].world}}}}.dump());
                }
            });
        state.services = std::make_unique<RowTable<ServiceCells>>(
            scope, "ecs.services.table", InternedString("editor-ecs.table.services"),
            std::vector{column("service", 200.0f), column("scope", 84.0f), column("tables", 320.0f)});
        state.playMap =
            std::make_unique<RowTable<MapCells>>(scope, "ecs.playMap.table", InternedString("editor-ecs.table.playMap"),
                                                 std::vector{column("edit", 140.0f), column("play", 140.0f)});
        state.registerCommands();
        state.showPage();
        scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(ECS_BAR),
                      [&state](const Widget& slot)
                      {
                          for (Button& tab : state.tabs)
                          {
                              putIn(state.scope, slot, tab);
                              setOwnClass(tab, slotFields(state.scope, slot).childClass);
                          }
                      });
        contract.slot(InternedString(ECS_FILTER), [&state](const Widget& slot) { state.archetypes->fillFilter(slot); });
        contract.slot(InternedString(ECS_ROWS),
                      [&state](const Widget& slot)
                      {
                          state.worlds->fill(state.scope, slot);
                          state.archetypes->fillRows(slot);
                          state.services->fill(state.scope, slot);
                          state.playMap->fill(state.scope, slot);
                      });
        contract.slot(InternedString(ECS_STATUS),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.note);
                          putIn(state.scope, slot, state.refreshButton);
                      });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                Panel toolbar = toolbarPanel(viewScope, ECS_TOOLBAR, root);
                fitBarPanel(viewScope, ECS_BAR, toolbar);
                Panel filterBar = toolbarPanel(viewScope, ECS_FILTERBAR, root);
                filterSlotPanel(viewScope, ECS_FILTER, filterBar);
                fillSlotPanel(viewScope, ECS_ROWS, root);
                toolbarPanel(viewScope, ECS_STATUS, root);
            });
        state.view = openView(engine, ECS_VIEW, "assets/ui/editor/ecs.json", std::move(contract), state.root);
    }

    EditorEcs::~EditorEcs() { m_state->control->unregisterOwner("editorEcs"); }

    Widget EditorEcs::content() const { return m_state->root; }

    void EditorEcs::update()
    {
        State& state = *m_state;
        if (!state.pending.empty())
        {
            dispatch(*state.control, state.pending, "ecs");
        }
        state.archetypes->update();
        if (!shown(state.scope, state.root))
        {
            state.wasShown = false;
            return;
        }
        const bool playing = state.engine->playWorld() != nullptr;
        bool due = !state.wasShown || state.due || playing != state.playing ||
            (state.localization && state.localization->revision() != state.seenLocale);
        // the selection changes only through commands (select, play, stop): compared after one ran, not every frame
        if (const u64 command = state.control->commandGeneration(); command != state.seenCommand)
        {
            state.seenCommand = command;
            if (state.page == PlayMap && !due)
            {
                due = !std::ranges::equal(state.editorModel->selection(), state.seenSelection);
            }
        }
        if (due)
        {
            state.wasShown = true;
            state.refresh();
        }
        if (state.noteDirty)
        {
            state.showNote();
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "ecs",
             .category = "world",
             .order = 150,
             .dock = {.target = "scene"},
             .zone = "EditorEcs",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<EditorEcs>(c.engine, c.model); }}};
    } // namespace
} // namespace spite
