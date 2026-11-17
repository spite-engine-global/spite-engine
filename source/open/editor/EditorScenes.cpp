#include "EditorScenes.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorSceneFlows.hpp"
#include "EditorFeedback.hpp"
#include "EditorPanelList.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <optional>
#include <string>
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

        constexpr std::string_view SCENES_VIEW = "editor.scenes";
        constexpr std::string_view SCENES_BAR = "scenes.bar";
        constexpr std::string_view SCENES_FILTER = "scenes.filter";
        constexpr std::string_view SCENES_ROWS = "scenes.rows";
        constexpr std::string_view SCENES_RECENT = "scenes.recent";
        constexpr std::string_view SCENES_STATUS = "scenes.status";
        constexpr std::string_view SCENES_TOOLBAR = "scenes.toolbar";
        constexpr std::string_view SCENES_CONTENT = "scenes.content";
        constexpr std::string_view LAYER_TAG = "SceneLayerComponent";
        constexpr float OPEN_WIDTH = 56.0f;

        enum Action : u32
        {
            Load,
            Unload,
            SetActive,
            Save,
            ActionCount
        };
        constexpr std::array<std::string_view, ActionCount> ACTION_IDS{"scenes.load", "scenes.unload", "scenes.active",
                                                                       "scenes.save"};
        constexpr std::array<std::string_view, ActionCount> ACTION_KEYS{"load", "unload", "setActive", "save"};

        struct Layer
        {
            u32 id = 0;
            std::string path;
            bool active = false;
            bool dirty = false;
            u64 entities = 0;
        };

        struct LayerCells
        {
            u32 layer = 0;
            std::string scene;
            std::string active;
            std::string modified;
            u64 entities = 0;
            SPITE_OBSERVED_SCHEMA(LayerCells, layer, scene, active, modified, entities)
        };

        UiTableColumn column(const std::string_view field, const float width)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString("editor-scenes.column." + std::string(field))};
        }

        Entity keyOf(const u32 layer) { return Entity(layer + 1, 1u); }

        UiLayoutItemComponent shareItem(const float grow)
        {
            UiLayoutItemComponent item = fillItem();
            item.grow = grow;
            return item;
        }
    } // namespace

    struct EditorScenes::State
    {
        State(Engine& engine, const EditorModel& model, EditorFeedback& feedback,
              std::function<void(std::string_view)> openScene) :
            engine(&engine), control(&engine.control()), scope(editorUi(engine).ui()),
            flows(&EditorModelAccess::flows(model)), feedback(&feedback), openScene(std::move(openScene)),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        UiScope scope;
        DensityFollower density{scope};
        EditorSceneFlows* flows;
        EditorFeedback* feedback;
        std::function<void(std::string_view)> openScene;
        const Localization* localization;
        Panel root;
        std::array<Button, ActionCount> buttons;
        Label recentTitle;
        Label recentEmpty;
        Label summary;
        Label warning;
        std::unique_ptr<FilteredTable> list;
        std::unique_ptr<Table> recentList;
        std::unique_ptr<UiDocumentView> view;
        std::vector<Layer> layers;
        std::vector<LayerCells> cells; // parallel to layers
        std::vector<std::string> recent;
        std::vector<std::string> unsaved;
        std::vector<std::function<void()>> pending;
        std::string shownSignature;
        u64 worldEntities = 0;
        u64 seenCommand = 0;
        u64 seenLocale = 0;
        u64 refreshes = 0;
        Entity seenSelection = Entity::undefined();
        Entity selectedKey = Entity::undefined(); // from onSelect: reading the table's selection marks it modified
        static constexpr u32 COMMAND_REFRESH_FRAMES = 20;
        bool wasShown = false;
        bool recheck = false;
        bool commanded = false; // this panel ran a command: read at once
        bool stale = false; // a command ran since the last read
        u32 sinceRefresh = 0;
        u32 fast = 0; // frames that read every frame, after the File menu's flow was asked (it may forget a file)

        [[nodiscard]] std::string message(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            std::string scratch;
            return localization ? std::string(localization->resolve(InternedString(key), args, scratch))
                                : std::string(key);
        }

        void toast(const ToastLevel level, const std::string_view key, const std::span<const MessageArg> args = {})
        {
            feedback->postToastKey(level, std::string("editor-scenes.") + std::string(key), args);
        }

        // Runs a command now; whether it was accepted (a refusal posts an error toast), and its result.
        bool run(const std::string_view command, const Json& args, Json* result = nullptr)
        {
            commanded = true;
            const Json reply = request(*control, Json{{"command", command}, {"args", args}});
            if (reply.value("ok", false))
            {
                if (result && reply.contains("result"))
                {
                    *result = reply["result"];
                }
                return true;
            }
            const std::string error = reply.contains("error") && reply["error"].is_string()
                ? reply["error"].get<std::string>()
                : std::string("refused");
            const MessageArg args2[]{{"error", std::string_view(error)}};
            toast(ToastLevel::Error, "error", args2);
            return false;
        }

        [[nodiscard]] const Layer* selected() const
        {
            const auto it =
                std::ranges::find_if(layers, [&](const Layer& layer) { return keyOf(layer.id) == selectedKey; });
            return it == layers.end() ? nullptr : &*it;
        }

        void refresh()
        {
            SPITE_ZONE("EditorScenes.refresh");
            ++refreshes;
            const Json info = request(*control, Json{{"query", "sceneInfo"}}).value("result", Json::object());
            const Json state = request(*control, Json{{"query", "sceneLayers"}}).value("result", Json::object());
            const Json recents = request(*control, Json{{"query", "recentScenes"}}).value("result", Json::object());
            worldEntities = info.value("worldEntityCount", u64{0});
            layers.clear();
            cells.clear();
            for (const Json& row : state.value("layers", Json::array()))
            {
                Layer layer{row.value("id", 0u), row.value("path", std::string{}), row.value("active", false),
                            row.value("dirty", false), row.value("entityCount", u64{0})};
                cells.push_back({layer.id, layer.path.empty() ? message("editor-scenes.untitled") : layer.path,
                                 layer.active ? message("editor-scenes.active") : std::string{},
                                 layer.dirty ? message("editor-scenes.modified") : std::string{}, layer.entities});
                layers.push_back(std::move(layer));
            }
            const auto strings = [](const Json& array)
            {
                std::vector<std::string> out;
                for (const Json& entry : array)
                {
                    out.push_back(entry.is_string() ? entry.get<std::string>() : std::string{});
                }
                return out;
            };
            unsaved = strings(state.value("unsavedUnloads", Json::array()));
            recent = strings(recents.value("scenes", Json::array()));
            seenLocale = localization ? localization->revision() : 0;
            // a read that found nothing changed writes nothing
            std::string signature = std::format("{}\n", worldEntities);
            for (const LayerCells& c : cells)
            {
                signature += std::format("{}|{}|{}|{}|{}\n", c.layer, c.scene, c.active, c.modified, c.entities);
            }
            for (const std::vector<std::string>* strings : {&unsaved, &recent})
            {
                for (const std::string& text : *strings)
                {
                    signature += text + "\n";
                }
                signature += "--\n";
            }
            if (signature == shownSignature)
            {
                return;
            }
            shownSignature = std::move(signature);
            list->setItems(static_cast<u32>(layers.size()));
            recentList->setRowCount(static_cast<u32>(recent.size()));
            if (recentEmpty.isVisible() != recent.empty())
            {
                recentEmpty.setVisible(recent.empty());
            }
            recentList->rebindVisible();
            showStatus();
            showButtons();
        }

        void readNow()
        {
            wasShown = true;
            commanded = false;
            stale = false;
            sinceRefresh = 0;
            refresh();
        }

        void showStatus()
        {
            summary.setLocalizedArg("layers", static_cast<std::int64_t>(layers.size()));
            summary.setLocalizedArg("entities", static_cast<std::int64_t>(worldEntities));
            std::string joined;
            for (const std::string& path : unsaved)
            {
                joined += (joined.empty() ? "" : ", ") + path;
            }
            if (!joined.empty())
            {
                warning.setLocalizedArgText("paths", joined);
            }
            if (warning.isVisible() == unsaved.empty())
            {
                warning.setVisible(!unsaved.empty());
            }
        }

        // the buttons that act on the selected layer
        void showButtons()
        {
            const Layer* layer = selected();
            const std::array<bool, ActionCount> usable{true, layer != nullptr, layer && !layer->active,
                                                       layer != nullptr};
            for (u32 i = 0; i < ActionCount; ++i)
            {
                if (buttons[i].isInteractable() != usable[i])
                {
                    buttons[i].setInteractable(usable[i]);
                }
            }
        }

        // The selected layer's roots are selected, which the Hierarchy reveals. A layer's roots carry the tag; the open
        // scene's are the untagged roots, whose rows say layer 0.
        void highlight(const u32 layer)
        {
            if (engine->playWorld())
            {
                return;
            }
            Json ids = Json::array();
            u64 after = 0;
            while (true)
            {
                Json args{{"limit", 10000}, {"after", after}};
                args[layer == 0 ? "without" : "with"] = std::string(LAYER_TAG);
                const Json found =
                    request(*control, Json{{"query", "find"}, {"args", args}}).value("result", Json::object());
                for (const Json& entity : found.value("entities", Json::array()))
                {
                    if (!entity.contains("parent") && entity.value("layer", 0u) == layer)
                    {
                        ids.push_back(entity.value("id", u64{0}));
                    }
                }
                if (!found.contains("next"))
                {
                    break;
                }
                after = found["next"].get<u64>();
            }
            if (!ids.empty())
            {
                run("select", {{"ids", ids}});
            }
        }

        void act(const Action action)
        {
            // the live counter: a command may have landed since update() last compared it
            if (const u64 live = control->commandGeneration(); stale || live != seenCommand)
            {
                seenCommand = live;
                readNow();
            }
            const Layer* layer = selected();
            if (action == Load)
            {
                flows->request(EditorSceneFlows::Op::Open);
            }
            else if (layer && action == Unload)
            {
                flows->requestLayer(EditorSceneFlows::Op::Close, layer->id);
            }
            else if (layer && action == SetActive)
            {
                run("setActiveSaveTarget", {{"layer", layer->id}});
            }
            else if (layer && action == Save)
            {
                flows->requestLayer(EditorSceneFlows::Op::Save, layer->id);
            }
        }

        void registerCommands()
        {
            control->registerViewCommand(
                "scenesSelect", "editorScenes", "selects a layer's row in the scenes panel",
                {requiredArg("layer", ArgKind::Integer, "a layer id (sceneLayers)")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const u64 id = args.integer("layer").value_or(0);
                    if (!std::ranges::any_of(layers, [id](const Layer& l) { return l.id == id; }))
                    {
                        return std::string("no scene layer ") + std::to_string(id);
                    }
                    const Entity key = keyOf(static_cast<u32>(id));
                    selectedKey = key;
                    list->table().setSelection(std::span(&key, 1));
                    list->table().scrollTo(key);
                    return std::string{};
                });
            control->registerQuery(
                "scenesView", "editorScenes",
                "the scenes panel: rows (layer, scene, active, modified, entities), the selected layer, the world's "
                "entity count, the recent scenes, the layers unloaded unsaved and the refresh counter",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    const Layer* layer = selected();
                    out.writeI32("selectedLayer", layer ? static_cast<i32>(layer->id) : -1);
                    out.writeU64("worldEntityCount", worldEntities);
                    out.beginArray("rows", static_cast<u32>(cells.size()));
                    for (const LayerCells& row : cells)
                    {
                        out.beginObject("");
                        writeValue(LayerCells::spiteSchema(), &row, out, FieldSet::All);
                        out.endObject();
                    }
                    out.endArray();
                    for (const auto& [key, strings] : {std::pair{"recent", &recent}, {"unsavedUnloads", &unsaved}})
                    {
                        out.beginArray(key, static_cast<u32>(strings->size()));
                        for (const std::string& text : *strings)
                        {
                            out.writeString("", text);
                        }
                        out.endArray();
                    }
                    out.writeBool("warning", warning.isVisible());
                    out.writeU64("refreshes", refreshes);
                    list->writeView(out);
                    return std::string{};
                });
        }
    };

    EditorScenes::EditorScenes(Engine& engine, const EditorModel& model, EditorFeedback& feedback,
                               std::function<void(std::string_view)> openScene) :
        m_state(std::make_unique<State>(engine, model, feedback, std::move(openScene)))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.root = scope.createPanel("editor.panel", columnDesc());
        for (u32 i = 0; i < ActionCount; ++i)
        {
            state.buttons[i] = state.density.track(
                scope.createButton(InternedString(ACTION_IDS[i]), "", fitWidthDesc()), HEIGHT_CONTROL);
            bindFitPadding(state.buttons[i], SPACE_MD, SPACE_NONE);
            state.buttons[i].setLocalizedText(InternedString("editor-scenes." + std::string(ACTION_KEYS[i])));
            state.buttons[i].onClick([&state, i] { state.pending.push_back([&state, i] { state.act(Action(i)); }); });
        }
        state.recentTitle =
            state.density.track(scope.createLabel("scenes.recentTitle", "", barDesc(Vec2{})), HEIGHT_ROW);
        state.recentTitle.setLocalizedText(InternedString("editor-scenes.recent"));
        state.recentEmpty =
            state.density.track(scope.createLabel("scenes.recentEmpty", "", barDesc(Vec2{})), HEIGHT_ROW);
        state.recentEmpty.setLocalizedText(InternedString("editor-scenes.recent.empty"));
        UiRectDesc summaryDesc = fitWidthDesc();
        summaryDesc.layoutItem->alignSelf = UiLayoutAlign::Stretch; // as high as the status row, whatever its size.row
        state.summary = scope.createLabel("scenes.summary", "", summaryDesc);
        state.summary.setLocalizedText(InternedString("editor-scenes.summary"));
        UiRectDesc warningDesc = topLeft();
        warningDesc.layoutItem = fillItem();
        state.warning = state.density.track(scope.createLabel("scenes.warning", "", warningDesc), HEIGHT_ROW);
        state.warning.setLocalizedText(InternedString("editor-scenes.unsaved"));
        state.warning.setStyleClass(InternedString("scenes.warning"));
        state.warning.setTooltip(InternedString("editor-scenes.unsaved.hint"));
        state.warning.setVisible(false);

        state.list = std::make_unique<FilteredTable>(
            scope, *state.control,
            FilteredTableDesc{
                .id = "scenes",
                .owner = "editorScenes",
                .nameKey = InternedString("editor-scenes.table"),
                .schema = &LayerCells::spiteSchema(),
                .columns = {column("layer", 72.0f), column("entities", 96.0f), column("active", 84.0f),
                            column("modified", 96.0f), column("scene", 220.0f)},
                .row = [&state](const u32 item) -> const void* { return &state.cells[item]; },
                .key = [&state](const u32 item) { return keyOf(state.layers[item].id); },
                .matches = [&state](const u32 item, const std::string_view needle)
                { return containsIgnoringCase(state.cells[item].scene, needle); },
                .headerHints = {{InternedString("active"), InternedString("editor-scenes.hint.active")}}});
        state.list->table().onSelect([&state](const std::span<const Entity> keys)
                                     { state.selectedKey = keys.empty() ? Entity::undefined() : keys[0]; });
        state.recentList = std::make_unique<Table>(
            scope,
            TableDesc{.id = "scenes.recentList",
                      .cells = 1,
                      .buttonWidth = OPEN_WIDTH,
                      .read =
                          [&state](const u32 row, const std::span<std::string> cells, std::string& button)
                      {
                          cells[0] = state.recent[row];
                          button = state.message("editor-scenes.open");
                      },
                      .press =
                          [&state](const u32 row)
                      {
                          state.pending.push_back(
                              [&state, path = state.recent[row]]
                              {
                                  if (state.openScene)
                                  {
                                      state.openScene(path);
                                      state.fast = 6; // the File menu's flow answers within a few frames
                                  }
                              });
                      },
                      .buttonName = [&state](const u32 row)
                      { return state.message("editor-scenes.open") + " " + state.recent[row]; }});
        state.recentList->list.setLayoutItem(fillItem());
        state.registerCommands();
        scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(SCENES_BAR),
                      [&state](const Widget& slot)
                      {
                          for (Button& button : state.buttons)
                          {
                              putIn(state.scope, slot, button);
                              button.setStyleClass(slotFields(state.scope, slot).childClass);
                          }
                      });
        contract.slot(InternedString(SCENES_FILTER), [&state](const Widget& slot) { state.list->fillFilter(slot); });
        contract.slot(InternedString(SCENES_ROWS), [&state](const Widget& slot) { state.list->fillRows(slot); });
        contract.slot(InternedString(SCENES_RECENT),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.recentTitle);
                          putIn(state.scope, slot, state.recentEmpty);
                          fillList(state.scope, slot, *state.recentList);
                      });
        contract.slot(InternedString(SCENES_STATUS),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.summary);
                          putIn(state.scope, slot, state.warning);
                      });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                Panel toolbar = toolbarPanel(viewScope, SCENES_TOOLBAR, root);
                fitBarPanel(viewScope, SCENES_BAR, toolbar);
                filterSlotPanel(viewScope, SCENES_FILTER, toolbar);
                Panel content =
                    stackPanel(viewScope, SCENES_CONTENT, Vec2{}, UiLayoutDirection::Row, UiLayoutAlign::Stretch);
                content.setLayoutItem(fillItem());
                content.setParent(root);
                Panel rows = fillSlotPanel(viewScope, SCENES_ROWS, content);
                rows.setLayoutItem(shareItem(3.0f));
                Panel recent = fillSlotPanel(viewScope, SCENES_RECENT, content);
                recent.setLayoutItem(shareItem(2.0f));
                statusPanel(viewScope, SCENES_STATUS, root);
            });
        state.view = openView(engine, SCENES_VIEW, "assets/ui/editor/scenes.json", std::move(contract), state.root);
    }

    EditorScenes::~EditorScenes() { m_state->control->unregisterOwner("editorScenes"); }

    Widget EditorScenes::content() const { return m_state->root; }

    void EditorScenes::update()
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
        }
        state.list->update();
        if (!shown(state.scope, state.root))
        {
            state.wasShown = false;
            return;
        }
        const u64 command = state.control->commandGeneration();
        if (command != state.seenCommand)
        {
            state.seenCommand = command;
            state.stale = true;
        }
        ++state.sinceRefresh;
        const bool own = state.commanded;
        const bool due = !state.wasShown || own || state.recheck || state.fast > 0 ||
            (state.stale && state.sinceRefresh >= State::COMMAND_REFRESH_FRAMES) ||
            (state.localization && state.localization->revision() != state.seenLocale);
        if (state.fast > 0)
        {
            --state.fast;
        }
        if (due)
        {
            state.readNow();
            state.recheck = own;
        }
        const Layer* layer = state.selected();
        const Entity key = layer ? keyOf(layer->id) : Entity::undefined();
        if (key != state.seenSelection)
        {
            state.seenSelection = key;
            state.showButtons();
            if (layer)
            {
                state.highlight(layer->id);
            }
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "scenes",
             .category = "project",
             .order = 130,
             .dock = {.target = "project"},
             .zone = "EditorScenes",
             .make = [](EditorPanelContext& c)
             { return makeEditorPanel<EditorScenes>(c.engine, c.model, c.feedback, c.openScene); }}};
    } // namespace
} // namespace spite
