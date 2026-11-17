#include "EditorComponents.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorPanelList.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/ControlServer.hpp"
#include "ecs/ComponentRegistry.hpp"
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

        constexpr std::string_view COMPONENTS_VIEW = "editor.components";
        constexpr std::string_view COMPONENTS_BAR = "components.bar";
        constexpr std::string_view COMPONENTS_FILTER = "components.filter";
        constexpr std::string_view COMPONENTS_ROWS = "components.rows";
        constexpr std::string_view COMPONENTS_FIELDS = "components.fields";
        constexpr std::string_view COMPONENTS_STATUS = "components.status";
        constexpr std::string_view COMPONENTS_TOOLBAR = "components.toolbar";
        constexpr float BUTTON_PAD = 16.0f; // a button is as wide as its text and this much

        UiRectDesc fittedButton()
        {
            UiRectDesc desc = barDesc(Vec2{});
            desc.contentSize =
                UiContentSizeComponent{.fitWidth = true, .fitHeight = false, .padding = Vec2{BUTTON_PAD, 0.0f}};
            return desc;
        }
        constexpr u32 MAX_FOUND = 10000;

        struct ComponentCells
        {
            std::string component;
            std::string category;
            u64 size = 0;
            u64 fields = 0;
            std::string entities; // the selected component's count; a dash for the others
            SPITE_OBSERVED_SCHEMA(ComponentCells, component, category, size, fields, entities)
        };

        struct FieldCells
        {
            std::string field;
            std::string type;
            std::string flags;
            bool operator==(const FieldCells&) const = default;
            SPITE_OBSERVED_SCHEMA(FieldCells, field, type, flags)
        };

        struct Component
        {
            std::string name;
            std::string category;
            std::vector<FieldCells> fields;
            bool loaded = true; // false: its module unloaded and the registry kept the entry
        };

        UiTableColumn column(const std::string_view field, const float width)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString("editor-components.column." + std::string(field))};
        }
    } // namespace

    struct EditorComponents::State
    {
        explicit State(Engine& engine) :
            engine(&engine), control(&engine.control()), scope(editorUi(engine).ui()),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        UiScope scope;
        DensityFollower density{scope};
        const Localization* localization;
        Panel root;
        Button show;
        Button refreshButton;
        Label note;
        std::unique_ptr<FilteredTable> list;
        std::unique_ptr<RowTable<FieldCells>> fields;
        std::unique_ptr<UiDocumentView> view;
        std::vector<Component> components;
        std::vector<ComponentCells> cells; // parallel to components
        std::vector<Entity> keys; // parallel to components
        std::unordered_map<std::string, u32> keyIndex; // name -> key, stable for the session
        std::vector<std::string> pending;
        std::string signature;
        std::string noteShown;
        std::string showName; // componentsShow's component, run by update
        std::string usageName; // the component whose entity count is read
        std::string shownFor; // the component whose fields the table holds
        std::string actionNote; // what Show Entities found, until the selection moves
        std::string usageWorld; // the world the usage was counted in
        u64 usage = 0;
        u64 seenLocale = 0;
        u64 seenCount = 0;
        u64 refreshes = 0;
        Entity selected = Entity::undefined();
        bool wasShown = false;
        bool due = false;
        bool usageDue = false;
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

        [[nodiscard]] const Component* current() const
        {
            const auto at = std::ranges::find(keys, selected);
            return at == keys.end() ? nullptr : &components[static_cast<u32>(at - keys.begin())];
        }

        [[nodiscard]] std::string entitiesText(const Component& component) const
        {
            return !component.loaded          ? message("editor-components.unloaded")
                : component.name == usageName ? std::to_string(usage)
                                              : message("editor-components.unread");
        }

        [[nodiscard]] ComponentCells cellsOf(const Component& component, const u64 size) const
        {
            return {component.name, message("editor-components.category." + component.category), size,
                    component.fields.size(), entitiesText(component)};
        }

        [[nodiscard]] std::string signatureOf() const
        {
            std::string text;
            for (u32 i = 0; i < cells.size(); ++i)
            {
                const ComponentCells& c = cells[i];
                text += std::format("{}|{}|{}|{}|{}|{}|", c.component, c.category, c.size, c.fields, c.entities,
                                    keys[i].id());
                text += '\n';
            }
            return text;
        }

        void refresh()
        {
            SPITE_ZONE("EditorComponents.refresh");
            ++refreshes;
            due = false;
            const Json reply = read("components", {{"counts", false}});
            components.clear();
            cells.clear();
            keys.clear();
            // listed by category, then name: the registry order is the order modules happened to load in
            std::vector<Json> entries;
            for (const Json& entry : reply.value("components", Json::array()))
            {
                entries.push_back(entry);
            }
            std::ranges::stable_sort(entries,
                                     [](const Json& a, const Json& b)
                                     {
                                         const std::string ca = a.value("category", std::string{});
                                         const std::string cb = b.value("category", std::string{});
                                         return ca != cb
                                             ? ca < cb
                                             : a.value("name", std::string{}) < b.value("name", std::string{});
                                     });
            for (const Json& entry : entries)
            {
                Component component{entry.value("name", std::string{}), entry.value("category", std::string{}), {}};
                component.loaded = entry.value("loaded", true);
                for (const Json& field : entry.value("fields", Json::array()))
                {
                    std::string type =
                        field.contains("type") ? field["type"].get<std::string>() : field.value("kind", std::string{});
                    if (field.value("count", 0u) != 0)
                    {
                        type += "[" + std::to_string(field.value("count", 0u)) + "]";
                    }
                    std::string flags;
                    for (const Json& flag : field.value("flags", Json::array()))
                    {
                        flags += (flags.empty() ? "" : ", ") + flag.get<std::string>();
                    }
                    component.fields.push_back({field.value("name", std::string{}), std::move(type), std::move(flags)});
                }
                cells.push_back(cellsOf(component, entry.value("size", u64{0})));
                const auto [it, added] = keyIndex.try_emplace(component.name, static_cast<u32>(keyIndex.size() + 1));
                keys.push_back(Entity(it->second, 1u));
                components.push_back(std::move(component));
            }
            seenLocale = localization ? localization->revision() : 0;
            seenCount = getRegisteredComponentCount();
            // a read that found nothing changed writes nothing
            if (std::string next = signatureOf(); next != signature)
            {
                signature = std::move(next);
                list->setItems(static_cast<u32>(components.size()));
            }
            showFields();
            noteDirty = true;
        }

        // the selected component's usage: one counted read of that component
        void readUsage()
        {
            usageDue = false;
            const Component* component = current();
            const std::string name = component ? component->name : std::string{};
            if (name.empty())
            {
                return;
            }
            usageWorld = std::string(engine->gameWorldName());
            const Json reply = read("components", {{"name", name}, {"world", usageWorld}});
            const Json& rows = reply.value("components", Json::array());
            usage = rows.empty() ? 0 : rows[0].value("entities", u64{0});
            usageName = name;
            bool changed = false;
            for (u32 i = 0; i < components.size(); ++i)
            {
                const std::string text = entitiesText(components[i]);
                if (cells[i].entities != text)
                {
                    cells[i].entities = text;
                    changed = true;
                }
            }
            if (changed)
            {
                signature = signatureOf();
                list->setItems(static_cast<u32>(components.size()));
            }
            noteDirty = true;
        }

        void showFields()
        {
            const Component* component = current();
            fields->set(component ? component->fields : std::vector<FieldCells>{});
            // no component picked: no empty table with bare headers, the note says to pick one
            if (fields->table().isVisible() != (component != nullptr))
            {
                fields->table().setVisible(component != nullptr);
            }
        }

        void showNote()
        {
            noteDirty = false;
            std::string text;
            const Component* component = current();
            if (!actionNote.empty())
            {
                text = actionNote;
            }
            else if (component)
            {
                const MessageArg args[]{{"name", std::string_view(component->name)},
                                        {"fields", static_cast<std::int64_t>(component->fields.size())},
                                        {"entities", static_cast<std::int64_t>(usage)}};
                text = message(component->loaded ? "editor-components.note.selected"
                                                 : "editor-components.note.selectedUnloaded",
                               args);
            }
            else
            {
                const MessageArg args[]{{"components", static_cast<std::int64_t>(components.size())}};
                text = message("editor-components.note", args);
            }
            if (text != noteShown)
            {
                noteShown = std::move(text);
                note.setText(noteShown);
            }
            const bool usable = component != nullptr && component->loaded;
            if (show.isInteractable() != usable)
            {
                show.setInteractable(usable);
            }
        }

        // find with=<component>, then select: the Hierarchy reveals them
        void showEntities(const std::string& name)
        {
            const std::string world(engine->gameWorldName());
            const Json found = read("find", {{"with", name}, {"world", world}, {"limit", MAX_FOUND}});
            Json ids = Json::array();
            for (const Json& entity : found.value("entities", Json::array()))
            {
                ids.push_back(entity.value("id", u64{0}));
            }
            const u64 total = found.value("total", static_cast<u64>(ids.size()));
            const MessageArg args[]{{"name", std::string_view(name)},
                                    {"entities", static_cast<std::int64_t>(ids.size())},
                                    {"shown", static_cast<std::int64_t>(ids.size())},
                                    {"total", static_cast<std::int64_t>(total)}};
            if (!ids.empty())
            {
                pending.push_back(Json{{"command", "select"}, {"args", {{"world", world}, {"ids", ids}}}}.dump());
            }
            actionNote = message(ids.empty()              ? "editor-components.note.none"
                                     : total > ids.size() ? "editor-components.note.shownPartial"
                                                          : "editor-components.note.shown",
                                 args);
            noteDirty = true;
        }

        void registerCommands()
        {
            control->registerViewCommand(
                "componentsSelect", "editorComponents", "selects a component's row in the components panel",
                {requiredArg("name", ArgKind::String, "a component name, as components lists it")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const std::string name = args.string("name").value_or("");
                    const auto at = std::ranges::find(components, name, &Component::name);
                    if (at == components.end())
                    {
                        return "no component '" + name + "'";
                    }
                    const Entity key = keys[static_cast<u32>(at - components.begin())];
                    list->table().setSelection(std::span(&key, 1));
                    list->table().scrollTo(key);
                    selected = key;
                    usageDue = true;
                    actionNote.clear();
                    noteDirty = true;
                    showFields();
                    return std::string{};
                });
            control->registerViewCommand(
                "componentsShow", "editorComponents",
                "selects the entities that have a component, which the Hierarchy reveals",
                {optionalArg("name", ArgKind::String, "a component name", "the selected component")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const std::string name = args.string("name").value_or(current() ? current()->name : std::string{});
                    const auto at = std::ranges::find(components, name, &Component::name);
                    if (at == components.end())
                    {
                        return name.empty() ? std::string("no component selected") : "no component '" + name + "'";
                    }
                    if (!at->loaded)
                    {
                        return "the module of '" + name + "' is unloaded";
                    }
                    showName = name;
                    return std::string{};
                });
            control->registerViewCommand("componentsRefresh", "editorComponents",
                                         "reads the component list and the selected usage again", {},
                                         [this](const ControlArgs&, IFormatWriter&)
                                         {
                                             due = usageDue = true;
                                             return std::string{};
                                         });
            control->registerQuery(
                "componentsView", "editorComponents",
                "the components panel: rows (component, category, size, fields, entities), the selected component, its "
                "fields (field, type, flags), the usage read, the note, the refresh counter and the list's filter and "
                "sort",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    out.writeString("component", current() ? current()->name : std::string{});
                    out.beginArray("rows", static_cast<u32>(cells.size()));
                    for (const ComponentCells& row : cells)
                    {
                        out.beginObject("");
                        writeValue(ComponentCells::spiteSchema(), &row, out, FieldSet::All);
                        out.endObject();
                    }
                    out.endArray();
                    out.beginArray("fields", static_cast<u32>(fields->rows().size()));
                    for (const FieldCells& row : fields->rows())
                    {
                        out.beginObject("");
                        writeValue(FieldCells::spiteSchema(), &row, out, FieldSet::All);
                        out.endObject();
                    }
                    out.endArray();
                    out.writeU64("usage", usage);
                    out.writeString("usageName", usageName);
                    out.writeString("note", noteShown);
                    out.writeU64("refreshes", refreshes);
                    list->writeView(out);
                    return std::string{};
                });
        }
    };

    EditorComponents::EditorComponents(Engine& engine) : m_state(std::make_unique<State>(engine))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.root = scope.createPanel("editor.panel", columnDesc());
        state.show = scope.createButton("components.show", "", fittedButton());
        state.density.track(state.show, HEIGHT_CONTROL);
        state.show.setLocalizedText(InternedString("editor-components.show"));
        state.show.onClick(
            [&state]
            { state.pending.push_back(Json{{"command", "componentsShow"}, {"args", Json::object()}}.dump()); });
        state.refreshButton = scope.createButton("components.refresh", "", fittedButton());
        state.density.track(state.refreshButton, HEIGHT_CONTROL);
        state.refreshButton.setLocalizedText(InternedString("editor-components.refresh"));
        state.refreshButton.onClick([&state]
                                    { state.pending.push_back(Json{{"command", "componentsRefresh"}}.dump()); });
        UiRectDesc noteDesc = barDesc(Vec2{});
        noteDesc.layoutItem = fillItem();
        noteDesc.layoutItem->alignSelf = UiLayoutAlign::Stretch; // as high as the status row, whatever its size.row
        state.note = scope.createLabel("components.note", "", noteDesc);
        state.note.setStyleClass(InternedString("components.note"));

        state.list = std::make_unique<FilteredTable>(
            scope, *state.control,
            FilteredTableDesc{
                .id = "components",
                .owner = "editorComponents",
                .nameKey = InternedString("editor-components.table"),
                .schema = &ComponentCells::spiteSchema(),
                .columns = {column("component", 260.0f), column("category", 120.0f), column("size", 100.0f),
                            column("fields", 84.0f), column("entities", 112.0f)},
                .row = [&state](const u32 item) -> const void* { return &state.cells[item]; },
                .key = [&state](const u32 item) { return state.keys[item]; },
                .matches =
                    [&state](const u32 item, const std::string_view needle)
                {
                    return containsIgnoringCase(state.cells[item].component, needle) ||
                        containsIgnoringCase(state.cells[item].category, needle);
                },
                .headerHints = {{InternedString("size"), InternedString("editor-components.hint.size")},
                                {InternedString("entities"), InternedString("editor-components.hint.entities")}}});
        state.list->table().onSelect(
            [&state](const std::span<const Entity> picked)
            {
                state.selected = picked.empty() ? Entity::undefined() : picked[0];
                state.usageDue = true;
                state.actionNote.clear();
                state.noteDirty = true;
                state.showFields();
            });
        state.fields = std::make_unique<RowTable<FieldCells>>(
            scope, "components.fields.table", InternedString("editor-components.table.fields"),
            std::vector{column("field", 180.0f), column("type", 180.0f), column("flags", 200.0f)});
        state.registerCommands();
        scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(COMPONENTS_BAR),
                      [&state](const Widget& slot)
                      {
                          for (Button* button : {&state.show, &state.refreshButton})
                          {
                              putIn(state.scope, slot, *button);
                              button->setStyleClass(slotFields(state.scope, slot).childClass);
                          }
                      });
        contract.slot(InternedString(COMPONENTS_FILTER),
                      [&state](const Widget& slot) { state.list->fillFilter(slot); });
        contract.slot(InternedString(COMPONENTS_ROWS), [&state](const Widget& slot) { state.list->fillRows(slot); });
        contract.slot(InternedString(COMPONENTS_FIELDS),
                      [&state](const Widget& slot) { state.fields->fill(state.scope, slot); });
        contract.slot(InternedString(COMPONENTS_STATUS),
                      [&state](const Widget& slot) { putIn(state.scope, slot, state.note); });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                Panel toolbar = toolbarPanel(viewScope, COMPONENTS_TOOLBAR, root);
                fitBarPanel(viewScope, COMPONENTS_BAR, toolbar);
                filterSlotPanel(viewScope, COMPONENTS_FILTER, toolbar);
                const auto share = [](Panel panel, const float grow)
                {
                    UiLayoutItemComponent item = fillItem();
                    item.grow = grow;
                    panel.setLayoutItem(item);
                };
                share(fillSlotPanel(viewScope, COMPONENTS_ROWS, root), 3.0f);
                share(fillSlotPanel(viewScope, COMPONENTS_FIELDS, root), 2.0f);
                statusPanel(viewScope, COMPONENTS_STATUS, root);
            });
        state.view =
            openView(engine, COMPONENTS_VIEW, "assets/ui/editor/components.json", std::move(contract), state.root);
    }

    EditorComponents::~EditorComponents() { m_state->control->unregisterOwner("editorComponents"); }

    Widget EditorComponents::content() const { return m_state->root; }

    void EditorComponents::update()
    {
        State& state = *m_state;
        if (!state.pending.empty())
        {
            dispatch(*state.control, state.pending, "components");
        }
        state.list->update();
        if (!shown(state.scope, state.root))
        {
            state.wasShown = false;
            return;
        }
        if (!state.wasShown || state.due || getRegisteredComponentCount() != state.seenCount ||
            (state.localization && state.localization->revision() != state.seenLocale))
        {
            state.wasShown = true;
            state.refresh();
            state.usageDue = state.usageDue || !state.usageName.empty();
        }
        // Play and Stop move the editor's world: the count follows it, as Show Entities does
        if (!state.usageName.empty() && state.usageWorld != state.engine->gameWorldName())
        {
            state.usageDue = true;
        }
        if (state.usageDue)
        {
            state.readUsage();
        }
        if (!state.showName.empty())
        {
            state.showEntities(std::exchange(state.showName, {}));
        }
        if (state.noteDirty)
        {
            state.showNote();
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "components",
             .category = "world",
             .order = 160,
             .dock = {.target = "scene"},
             .zone = "EditorComponents",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<EditorComponents>(c.engine); }}};
    } // namespace
} // namespace spite
