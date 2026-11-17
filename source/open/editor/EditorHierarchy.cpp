#include "EditorHierarchy.hpp"
#include "EditorAgentLayer.hpp"
#include "EditorPanelList.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorEntityLabel.hpp"
#include "EditorFeedback.hpp"
#include "EditorSystemCost.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/ContentRoot.hpp"
#include "base/Logging.hpp"
#include "control/ControlServer.hpp"
#include "editor/EditorModel.hpp"
#include "EditorSceneView.hpp"
#include "EditorSceneFlows.hpp"
#include "EditorPanelUi.hpp"
#include "engine/Engine.hpp"
#include "engine/SceneCommands.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "ecs/Chunk.hpp"
#include "engine/PlayWorld.hpp"
#include "localization/Localization.hpp"
#include "engine/PrimitiveMesh.hpp"
#include <spite/profiling/Zone.hpp>
#include "serialization/FormatWalk.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiDragDrop.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiScope.hpp"
#include "uiview/UiStyleSheet.hpp"
#include "world/EntityName.hpp"
#include "world/TransformComponents.hpp"
#include "world/EntityTransformManager.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;

        constexpr std::string_view OWNER = "editorHierarchy";
        constexpr std::string_view SCENE_WORLD = "scene";
        constexpr std::string_view PLAY_WORLD = "play";
        // the nesting step (the spec's 16), also the width of the caret and of the type icon
        float indentOf(const panelui::Density& density) { return density.xl; }
        constexpr float THUMB = 10.0f; // kept clear for the scroll thumb
        constexpr float BAR_STEPS = 16.0f; // the readout bar's fill moves in these steps: a jitter rebinds nothing
        constexpr float MARK_SPACE = 64.0f; // the width of an AGENT or EDITED tag at a row's end
        constexpr float MARK_SHORT_SPACE = 24.0f; // and of its one-letter form, once the name needs the room
        constexpr std::string_view TABLE = "editor-hierarchy.";
        constexpr u8 SECONDARY = 1;
        constexpr std::string_view ROW_CLASS = "hierarchy.row"; // the rows' class while the slot names none

        // the panel as a UI document: the document owns the panel's containers, the code the rows slot
        constexpr std::string_view VIEW = "hierarchy.panel";
        constexpr std::string_view ROWS_SLOT = "hierarchy.rows";
        constexpr std::string_view DOCUMENT = "assets/ui/editor/hierarchy.json";
        // the toolbar's slots: the search box, the actions and the filter's chip
        constexpr std::string_view FILTER_SLOT = "hierarchy.filter";
        constexpr std::string_view ACTIONS_SLOT = "hierarchy.actions";
        constexpr std::string_view SCOPE_SLOT = "hierarchy.scope"; // the Scene / UI / both choice
        constexpr std::string_view STATUS_SLOT = "hierarchy.status";
        constexpr Entity UI_SECTION = HIERARCHY_UI_SECTION;
        constexpr u32 SCENE_HEADER_GENERATION = 0xFFFFFFFEu;
        Entity sceneHeader(const u32 layer) { return Entity(layer, SCENE_HEADER_GENERATION); }
        bool isSceneHeader(const Entity node) { return node.isValid() && node.generation() == SCENE_HEADER_GENERATION; }
        using Scope = HierarchyScope;
        constexpr u32 FILTER_CAP = HIERARCHY_FILTER_CAP;
        constexpr u32 RETURN_KEY = 40; // HID
        constexpr u32 SPACE_KEY = 44;
        constexpr u8 PRIMARY = 0;
        // the search box's least and the chip's greatest width, in control heights (size.control): they follow the
        // density
        constexpr float SEARCH_MIN_CONTROLS = 2.5f;
        constexpr float CHIP_MAX_CONTROLS = 7.0f;

        // menu item ids
        constexpr std::string_view CREATE_ITEM = "hierarchy.create";
        constexpr std::string_view CREATE_CHILD_ITEM = "hierarchy.createChild";
        constexpr std::string_view DUPLICATE_ITEM = "hierarchy.duplicate";
        constexpr std::string_view RENAME_ITEM = "hierarchy.rename";
        constexpr std::string_view DELETE_ITEM = "hierarchy.delete";
        // the Create Primitive submenu: its parent item and, before the kind, each child's id
        constexpr std::string_view PRIMITIVE_ITEM = "hierarchy.primitive";
        constexpr std::string_view PRIMITIVE_PREFIX = "hierarchy.primitive.";

        UiRectDesc topLeft(const Vec2 offset = {}, const Vec2 size = {})
        {
            UiRectDesc desc;
            desc.offset = offset;
            desc.size = size;
            desc.pivot = Vec2{0.0f, 0.0f};
            return desc;
        }

        // what a row shows of its node: the name (invalid: unnamed), the primary component and its enabled and active
        // state (active = enabled and no disabled ancestor); a UI row (an element or the section) shows the
        // element's id as its name and its widget kind when it has none
        struct RowLook
        {
            InternedString name;
            ComponentID primary = INVALID_COMPONENT_ID;
            bool enabled = true;
            bool active = true;
            bool ui = false;
            UiWidgetKind kind = UiWidgetKind::Panel;
            bool operator==(const RowLook&) const = default;
        };

        // a UI hierarchy at one structure generation: what the UI rows were read from
        struct UiSeen
        {
            const UiHierarchyManager* manager = nullptr;
            u64 generation = 0;
            bool operator==(const UiSeen&) const = default;
        };

        struct HierarchyStats
        {
            u64 rebuilds = 0; // the model dropped: the structure changed
            u64 refreshes = 0; // the tree re-read its visible nodes
            u64 childLists = 0; // children lists read from the transform manager
            u64 labels = 0; // row labels read (one per bound row)
            u64 rebinds = 0; // the visible rows rebound in place: a cost moved, the pointer changed row
        };
    } // namespace

    struct EditorHierarchy::State
    {
        // A request line; selectCreated: select the entities its reply created.
        struct Request
        {
            std::string line;
            bool selectCreated = false;
        };

        State(Engine& engine, const EditorModel& editorModel, const EditorAgentLayer& agent, EditorFeedback& feedback) :
            engine(&engine), control(&engine.control()), editorModel(&editorModel), agent(&agent), feedback(&feedback),
            filter(EditorModelAccess::hierarchyFilter(editorModel)),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr),
            scope(editorUi(engine).ui())
        {
        }

        Engine* engine;
        ControlServer* control;
        const EditorModel* editorModel;
        const EditorAgentLayer* agent;
        EditorFeedback* feedback;
        const Localization* localization; // not owned: the engine's service
        UiScope scope;
        panelui::DensityFollower density{scope};
        Panel host; // the docked content: the document view's root goes in it
        TreeView tree;
        TextInput rename;
        Menu menu;
        std::unique_ptr<UiDocumentView> view;

        // -- toolbar --
        // The search and the scope are the editor model's (HierarchyFilter): the rows listed are the matches
        // of its find query and their ancestors. This panel reads it, asks it to refresh and settles its rows when its
        // revision moves; the model answers the query while the panel is hidden.
        const HierarchyFilter& filter;
        bool scopeOpened = false; // the UI scope opened the section: a person's collapse stays until the scope changes
        u64 seenFilter = 0; // the revision the rows show
        Scope seenScope = Scope::Both;
        std::string revealedFor; // the text whose matches were revealed
        std::unordered_set<Entity> revealed; // and which of them: a later match is revealed once, as it appears
        std::vector<Entity> autoExpanded; // nodes the filter expanded: collapsed again when it clears
        u64 modelFilter = 0; // the revision the model was built from
        sizet modelNodes = 0; // the hierarchy's entities
        TextInput search;
        Label searchIcon; // the magnifier before the field
        Button clear;
        Button create;
        Button collapse;
        Button locate;
        Button pick;
        Button chip;
        Label count;
        Label tag; // the tab bar's tag
        // what the tag shows: 0 hidden, 1 the readouts' unit, 2 the rows over the budget; with the count and budget
        struct TagLook
        {
            u8 kind = 0;
            u32 over = 0;
            float budgetMs = 0.0f;
            bool operator==(const TagLook&) const = default;
        };
        TagLook tagLook;
        u32 tagRows = 0; // the rows listed when the tag was counted
        Dropdown scopeChoice; // shown while the world has UI elements or the scope narrows the rows
        PopupList picker;
        std::vector<std::string> pickNames; // the picker's components by item
        std::vector<Entity> pickSelection; // the selection the last pick left, sorted: the note lasts while it stands
        std::string pickNote; // what that pick did, shown in place of the count
        std::string shownNote;
        std::string chipText;
        u8 countKind = 0; // 0 hidden, 1 count, 2 more, 3 none, 4 note
        u64 countedShown = ~0ull;
        u64 countedTotal = ~0ull;
        bool canLocate = false;
        std::optional<std::string> typedText; // the search box's text, not yet sent as hierarchyFilter

        std::vector<Entity> roots;
        struct SceneRow
        {
            u32 id;
            std::string path;
            bool dirty;
        };
        std::vector<SceneRow> scenes;
        std::unordered_map<u32, std::vector<Entity>> sceneRoots;
        u64 sceneState = ~u64{0};
        u64 ownershipRevision = 0;
        u64 sectionsRevision = 0;
        u64 modelSections = 0;
        u64 seenSections = 0;
        u32 activeScene = 0;
        std::optional<u32> focusedScene;
        u64 focusRevision = 0;
        u64 seenFocus = 0;
        std::unordered_map<Entity, std::vector<Entity>> children; // read lazily, dropped with the model
        // The UI section's and the UI elements' children lists: read lazily too, and dropped as soon as the
        // UI hierarchy's structure moved, so a section opened after a change reads it fresh; nothing follows the UI
        // while the section is closed.
        std::unordered_map<Entity, std::vector<Entity>> uiChildren;
        UiSeen listsUi;
        UiSeen seenUi; // the UI structure the panel follows
        bool modelSection = false; // whether the model lists the UI section
        InternedString sectionName; // its row's label, localized
        std::string sectionText;
        std::unordered_map<Entity, RowLook> labelled; // what bound rows show, since the last refresh
        std::string labelBuffer; // the fallback label of the row being read, reused: no allocation per row
        const EntityTransformManager* modelTransforms = nullptr; // what the model was built from; null: dropped
        u64 modelStructure = 0; // and at which structure generation
        const World* seenWorld = nullptr;
        u64 seenStructure = 0;
        u64 seenCommand = 0;
        u64 seenAgent = 0; // the agent layer's revision the rows show
        u64 seenActivity = ~0ull; // the transform manager's activity generation the rows show
        u64 seenLocale = ~0ull;
        std::string agentText; // the AGENT and EDITED tags, localized
        std::string activeSceneText;
        std::string editedText;
        std::string agentShort; // what each shrinks to beside a name that needs the row's width
        std::string editedShort;
        bool synced = false;

        // -- subtree costs --
        // A row's cost is the summed average of the distinct (system, phase) pairs that touch any entity of its subtree
        // (systemcost::touches): what the schedule would spend on those entities' archetypes, not a per-entity cost,
        // for systems run over chunks. The pairs and their shown costs are read from the scheduler when the schedule
        // changed and every systemcost::REFRESH_FRAMES while the panel shows, never per frame.
        struct Phase
        {
            std::vector<ComponentID> touched;
            sizet slot = 0; // its place in World::readSystemTimings
            float heldMs = -1.0f; // what shows: the average held until it moved by systemcost::STEP_MS
        };
        struct AspectMatch
        {
            std::vector<ComponentID> ids;
            std::vector<u64> bits; // the phases that touch an entity of this aspect
        };
        struct NodeCost
        {
            float ms = 0.0f;
            u32 systems = 0;
            bool warn = false; // over the budget (released at systemcost::WARN_RELEASE of it)
        };
        struct DetailLook
        {
            std::string text;
            bool warn = false;
            float bar = -1.0f; // the subtree's share of every phase's cost, in 1/BAR_STEPS; negative: no readout
        };
        bool costOn = false; // once the shell gave the telemetry setting
        bool telemetryFull = false;
        float budgetMs = 2.0f;
        std::vector<Phase> phases;
        const World* phasesWorld = nullptr;
        u64 seenSchedule = 0;
        float totalMs = 0.0f; // every phase's shown cost: no subtree can be over the budget below it
        std::unordered_map<u64, AspectMatch> aspects; // by the hash of the component ids, valid for `phases`
        std::unordered_map<Entity, NodeCost> costs; // per subtree, dropped when a structure, command or cost moved
        std::unordered_set<Entity> over; // the nodes in warning, kept across drops for the release margin
        std::unordered_map<Entity, DetailLook> detailed; // what bound rows show, since the last refresh
        std::vector<u64> scratchBits;
        std::vector<float> timings; // the scheduler's averages, reused: a poll allocates nothing
        std::string costText;
        bool costsStale = true; // the panel was hidden (or nothing read yet): read at once when shown
        bool costsMoved = false; // a held cost moved, or the pointer changed row, since the rows were bound
        u32 sinceCosts = 0;
        Entity hoverEntity = Entity::undefined(); // the element under the pointer, and the row node it is
        Entity hoverNode = Entity::undefined();
        Entity menuNode = Entity::undefined(); // the row the context menu opened on
        Entity renaming = Entity::undefined();
        bool selectAllOnEdit = false;
        struct ModelDrop
        {
            bool active = false;
            Entity parent = Entity::undefined();
        } modelDrop; // the Project model drag the indicator shows
        std::vector<Entity> dropped; // the nodes of a drop sent and not yet shown: opened to and scrolled to after it
        std::vector<Request> pending;
        HierarchyStats stats;

        // the play world while playing, else the scene world
        [[nodiscard]] std::string_view worldName() const { return engine->playWorld() ? PLAY_WORLD : SCENE_WORLD; }
        [[nodiscard]] World& world() const
        {
            PlayWorld* play = engine->playWorld();
            return play ? play->world() : engine->world();
        }
        [[nodiscard]] EntityTransformManager& transforms() const
        {
            PlayWorld* play = engine->playWorld();
            return play ? play->transforms() : engine->transforms();
        }

        // the UI layer of the shown world: the play world's while playing, else the scene's
        [[nodiscard]] const UiHierarchyManager& uiHierarchy() const
        {
            PlayWorld* play = engine->playWorld();
            return (play ? play->ui() : engine->sceneUi()).hierarchy();
        }
        [[nodiscard]] UiSeen uiNow() const
        {
            const UiHierarchyManager& ui = uiHierarchy();
            return {&ui, ui.structureGeneration()};
        }

        // a row of the scene's hierarchy, as against a UI element or the UI section
        [[nodiscard]] bool isScene(const Entity node) const
        {
            return node != UI_SECTION && !isSceneHeader(node) && !uiHierarchy().isElement(node);
        }

        u32 rootScene(const Entity root) const
        {
            const auto* tag = world().tryGetConstComponent<SceneLayerComponent>(root);
            return tag ? tag->layer : 0;
        }

        void readScenes()
        {
            const u64 state = control->stateGeneration();
            if (sceneState == state && (!scenes.empty() == !engine->playWorld()))
                return;
            sceneState = state;
            std::vector<SceneRow> next;
            u64 revision = 0;
            const u32 previousActive = activeScene;
            if (!engine->playWorld())
            {
                const Json result =
                    Json::parse(control->handle(R"({"query":"sceneLayers","args":{"counts":false}})"), nullptr, false);
                if (result.is_object() && result.value("ok", false))
                {
                    const auto& rows = result["result"];
                    revision = rows.value("ownershipRevision", u64{0});
                    focusRevision = rows.value("focusRevision", u64{0});
                    activeScene = rows.at("active").get<u32>();
                    for (const auto& row : rows.at("layers"))
                        next.push_back(
                            {row.at("id").get<u32>(), row.value("path", std::string{}), row.at("dirty").get<bool>()});
                }
            }
            if (revision != ownershipRevision || activeScene != previousActive ||
                !std::ranges::equal(next, scenes, {}, &SceneRow::id, &SceneRow::id))
                ++sectionsRevision;
            ownershipRevision = revision;
            scenes = std::move(next);
        }

        [[nodiscard]] bool isShown() const
        {
            return scope.hierarchy().layout().isVisible(tree.entity()) && tree.size().x > 0.0f && tree.size().y > 0.0f;
        }

        // -- model -----------------------------------------------------------------

        // Built lazily and dropped here whenever the structure moved since (or
        // Play/Stop switched the world), so a query between frames, hidden or
        // in the frame of a structural command, never reads stale rows.
        void ensureModel()
        {
            EntityTransformManager& manager = transforms();
            filter.refresh(false);
            readScenes();
            if (modelTransforms == &manager && modelStructure == manager.structureGeneration() &&
                modelFilter == filter.revision() && modelSection == filter.sectionListed() &&
                modelSections == sectionsRevision)
            {
                return;
            }
            SPITE_ZONE("EditorHierarchy.ensureModel");
            modelTransforms = &manager;
            modelSections = sectionsRevision;
            modelStructure = manager.structureGeneration();
            modelFilter = filter.revision();
            modelSection = filter.sectionListed();
            children.clear();
            uiChildren.clear();
            roots.clear();
            sceneRoots.clear();
            modelNodes = 0;
            if (filter.scope() != Scope::Ui)
            {
                manager.compile();
                const std::span<const Entity> order = manager.compiledOrder();
                modelNodes = order.size();
                for (sizet i = 0; i < order.size(); i += manager.subtreeOf(order[i]).size())
                {
                    if (filter.lists(order[i]))
                    {
                        roots.push_back(order[i]);
                    }
                }
                for (const Chunk* chunk : world().getChunksContaining(Aspect{}))
                {
                    if (chunk->hasComponent(getComponentId<UiIdentityComponent>()))
                    {
                        continue;
                    }
                    for (const Entity entity : chunk->entities())
                    {
                        if (!manager.isInHierarchy(entity))
                        {
                            ++modelNodes;
                            if (filter.lists(entity))
                            {
                                roots.push_back(entity);
                            }
                        }
                    }
                }
                std::ranges::sort(roots, {}, &Entity::id);
                if (!scenes.empty())
                {
                    for (const Entity root : roots)
                        sceneRoots[rootScene(root)].push_back(root);
                    roots.clear();
                    for (const SceneRow& scene : scenes)
                    {
                        const bool hasRoots = sceneRoots.contains(scene.id);
                        const bool placeholder =
                            scene.id == 0 && scene.id != activeScene && scene.path.empty() && !scene.dirty && !hasRoots;
                        if (!placeholder && (!filter.filtering() || hasRoots))
                            roots.push_back(sceneHeader(scene.id));
                    }
                }
            }
            if (filter.scope() != Scope::Scene)
            {
                modelNodes += uiHierarchy().elementCount();
            }
            if (modelSection)
            {
                roots.push_back(UI_SECTION);
            }
        }

        // the UI section's children: the UI hierarchy's roots, in entity order
        void readUiRoots(std::vector<Entity>& out) const
        {
            const UiHierarchyManager& ui = uiHierarchy();
            const std::span<const Entity> order = ui.compiledOrder();
            for (sizet i = 0; i < order.size(); i += ui.subtreeOf(order[i]).size())
            {
                if (filter.lists(order[i]))
                {
                    out.push_back(order[i]);
                }
            }
        }

        // node's children in the compiled model's order (entity index; none: the roots)
        void readChildren(const Entity node, std::vector<Entity>& out)
        {
            ensureModel();
            if (!node.isValid())
            {
                out.assign(roots.begin(), roots.end());
                return;
            }
            if (node == UI_SECTION)
            {
                readUiRoots(out);
                return;
            }
            if (isSceneHeader(node))
            {
                if (const auto it = sceneRoots.find(node.index()); it != sceneRoots.end())
                    out.assign(it->second.begin(), it->second.end());
                return;
            }
            if (uiHierarchy().isElement(node))
            {
                for (const Entity child : uiHierarchy().childrenOf(node))
                {
                    if (filter.lists(child))
                    {
                        out.push_back(child);
                    }
                }
                return;
            }
            const EntityTransformManager& manager = transforms();
            const std::span<const Entity> subtree = manager.subtreeOf(node);
            for (sizet i = 1; i < subtree.size(); i += manager.subtreeOf(subtree[i]).size())
            {
                if (filter.lists(subtree[i]))
                {
                    out.push_back(subtree[i]);
                }
            }
        }

        // The tree's: cached per model, counted.
        std::span<const Entity> childrenOf(const Entity node)
        {
            ensureModel();
            if (!node.isValid())
            {
                return roots;
            }
            const bool ui = !isScene(node) && !isSceneHeader(node);
            if (ui)
            {
                if (const UiSeen now = uiNow(); !(now == listsUi))
                {
                    uiChildren.clear(); // the UI changed while no row followed it
                    listsUi = now;
                }
            }
            auto [it, added] = (ui ? uiChildren : children).try_emplace(node);
            if (added)
            {
                ++stats.childLists;
                readChildren(node, it->second);
            }
            return it->second;
        }

        Entity parentOf(const Entity node)
        {
            ensureModel();
            if (isSceneHeader(node))
                return Entity::undefined();
            const Entity parent = filter.parentNow(node);
            return !parent.isValid() && !scenes.empty() && node.isValid() && isScene(node)
                ? sceneHeader(rootScene(node))
                : parent;
        }

        // a scene entity's name, else "Entity"; a UI element's id, else empty
        [[nodiscard]] InternedString nameOf(const Entity node) const
        {
            const World& scene = world();
            if (isSceneHeader(node))
                return lookOf(node).name;
            if (!isScene(node))
            {
                return node == UI_SECTION ? sectionName : lookOf(node).name;
            }
            const auto* name =
                scene.isEntityAlive(node) ? scene.tryGetConstComponent<EntityNameComponent>(node) : nullptr;
            return name && !name->name.toView().empty() ? name->name : InternedString(UNNAMED_ENTITY);
        }

        [[nodiscard]] ComponentID primaryOf(const Entity node) const
        {
            return isScene(node) ? primaryComponentOf(world(), node) : INVALID_COMPONENT_ID;
        }

        [[nodiscard]] RowLook lookOf(const Entity node) const
        {
            if (isSceneHeader(node))
            {
                const auto row = std::ranges::find(scenes, node.index(), &SceneRow::id);
                if (row == scenes.end())
                    return {};
                const auto slash = row->path.find_last_of("/\\");
                std::string label = row->path.empty()
                    ? tr("scene.untitled", {{"number", static_cast<std::int64_t>(row->id) + 1}})
                    : row->path.substr(slash == std::string::npos ? 0 : slash + 1);
                if (row->dirty)
                    label += " *";
                return {InternedString(label), INVALID_COMPONENT_ID, true, true};
            }
            if (node == UI_SECTION)
            {
                return {sectionName, INVALID_COMPONENT_ID, true, true, true, UiWidgetKind::Panel};
            }
            const World& scene = world();
            if (uiHierarchy().isElement(node))
            {
                const auto* identity =
                    scene.isEntityAlive(node) ? scene.tryGetConstComponent<UiIdentityComponent>(node) : nullptr;
                return {identity && !identity->id.toView().empty() ? identity->id : InternedString{},
                        INVALID_COMPONENT_ID,
                        true,
                        true,
                        true,
                        identity ? identity->kind : UiWidgetKind::Panel};
            }
            const auto* name =
                scene.isEntityAlive(node) ? scene.tryGetConstComponent<EntityNameComponent>(node) : nullptr;
            const EntityTransformManager& manager = transforms();
            return {name && !name->name.toView().empty() ? name->name : InternedString{}, primaryOf(node),
                    manager.isEntityEnabled(node), manager.isEntityActive(node)};
        }

        // The row label the panel and the query share (entityLabel); a fallback is valid until the next call.
        std::string_view textOf(const Entity node, const RowLook& look)
        {
            if (look.name.isValid())
            {
                return look.name.toView();
            }
            if (look.ui)
            {
                labelBuffer.clear();
                std::format_to(std::back_inserter(labelBuffer), "{} #{}", uiWidgetKindName(look.kind), node.index());
                return labelBuffer;
            }
            return unnamedEntityLabel(labelBuffer, look.primary, node);
        }

        std::string_view labelOf(const Entity node)
        {
            ++stats.labels;
            const RowLook look = lookOf(node);
            labelled[node] = look;
            return textOf(node, look);
        }

        // The row's Phosphor icon (the Forge look): a light, the camera or a mesh by the primary component, else a
        // folder for a parent, a shape for another component and a dot for a bare entity; the UI section stacks, a
        // UI element is a rectangle
        [[nodiscard]] static std::string_view iconFor(const Entity node, const RowLook& look, const bool parent)
        {
            if (isSceneHeader(node))
                return "folder";
            if (look.ui)
            {
                return node == UI_SECTION ? "stack" : "rectangle";
            }
            const ComponentID primary = look.primary;
            const std::string_view name =
                primary == INVALID_COMPONENT_ID ? std::string_view{} : componentDisplayName(primary);
            if (name == "DirectionalLight")
            {
                return "sun";
            }
            if (name == "PointLight")
            {
                return "lightbulb";
            }
            if (name == "SpotLight")
            {
                return "flashlight";
            }
            if (name == "AreaLight")
            {
                return "rectangle";
            }
            if (name == "Camera")
            {
                return "camera";
            }
            if (name == "Mesh")
            {
                return "cube";
            }
            return parent ? "folder" : primary == INVALID_COMPONENT_ID ? "circle" : "shapes";
        }

        [[nodiscard]] std::string_view iconOf(const Entity node)
        {
            return iconFor(node, lookOf(node), !childrenOf(node).empty());
        }

        // An inactive row is dimmed; the tooltip says whether it is disabled itself or by a parent.
        [[nodiscard]] UiTreeLook lookOfRow(const Entity node) const
        {
            const EntityTransformManager& manager = transforms();
            if (!isScene(node) || manager.isEntityActive(node))
            {
                return {};
            }
            return {true,
                    InternedString(manager.isEntityEnabled(node) ? "editor-hierarchy.inactive.parent"
                                                                 : "editor-hierarchy.inactive.self")};
        }

        // AGENT on the entity a client is editing, EDITED on the others with unreviewed changes, both washed
        [[nodiscard]] UiTreeMark markOf(const Entity node) const
        {
            if (isSceneHeader(node))
                return node.index() == activeScene ? UiTreeMark{activeSceneText, false, activeSceneText} : UiTreeMark{};
            if (node == UI_SECTION)
            {
                return {};
            }
            switch (agent->markOf(node))
            {
            case EditorAgentLayer::Mark::Agent:
                return {agentText, true, agentShort};
            case EditorAgentLayer::Mark::Edited:
                return {editedText, true, editedShort};
            case EditorAgentLayer::Mark::None:
                break;
            }
            return {};
        }

        void loadMarkTexts()
        {
            activeSceneText = tr("scene.active", {});
            const auto resolve = [this](const std::string_view key)
            {
                std::string scratch;
                return localization ? std::string(localization->resolve(InternedString(key), scratch))
                                    : std::string(key);
            };
            agentText = resolve("editor-hierarchy.mark.agent");
            editedText = resolve("editor-hierarchy.mark.edited");
            agentShort = resolve("editor-hierarchy.mark.agent.short");
            editedShort = resolve("editor-hierarchy.mark.edited.short");
            sectionText = resolve("editor-hierarchy.section");
            sectionName = InternedString(sectionText);
            seenLocale = localization ? localization->revision() : 0;
        }

        // -- subtree costs --

        [[nodiscard]] std::string tr(const std::string_view key, const std::vector<MessageArg>& args) const
        {
            const std::string full = std::string(TABLE) + std::string(key);
            if (!localization)
            {
                return full;
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(full), args, scratch));
        }

        // Reads the scheduler's averages into the phases (every phase again when the schedule or the world changed);
        // true when a shown cost moved, which drops the subtree costs and marks the rows stale.
        bool readPhases()
        {
            SPITE_ZONE("EditorHierarchy.costs");
            const World& target = world();
            const bool fresh = phasesWorld != &target || seenSchedule != target.systemRevision();
            if (fresh)
            {
                phases.clear();
                aspects.clear();
                phasesWorld = &target;
                seenSchedule = target.systemRevision();
                sizet slot = 0;
                for (const SystemDescriptor& system : target.getSystems())
                {
                    for (const std::optional<SystemPhaseDescriptor>& phase : {system.update, system.fixedUpdate})
                    {
                        if (phase && !systemcost::touchedOf(*phase).empty())
                        {
                            phases.push_back({systemcost::touchedOf(*phase), slot});
                        }
                        slot += phase ? 1 : 0;
                    }
                }
            }
            target.readSystemTimings(timings);
            bool moved = fresh;
            bool noWarning = false;
            for (Phase& phase : phases)
            {
                moved |= systemcost::hold(phase.heldMs, noWarning,
                                          phase.slot < timings.size() ? timings[phase.slot] : 0.0f, 0.0f);
            }
            if (moved)
            {
                totalMs = 0.0f;
                for (const Phase& phase : phases)
                {
                    totalMs += std::max(0.0f, phase.heldMs);
                }
                costs.clear();
                costsMoved = true;
            }
            return moved;
        }

        // The phases touching an entity of this one's aspect, as bits over `phases`; one entry per distinct aspect
        const std::vector<u64>& matchOf(const Entity entity)
        {
            std::array<ComponentID, 64> ids;
            const std::span<const ComponentID> aspect(
                ids.data(), std::min<u32>(world().getEntityAspectIds(entity, ids), static_cast<u32>(ids.size())));
            u64 hash = 14695981039346656037ull;
            for (const ComponentID id : aspect)
            {
                hash = (hash ^ static_cast<u64>(id)) * 1099511628211ull;
            }
            AspectMatch& match = aspects[hash];
            const sizet words = (phases.size() + 63) / 64;
            if (match.bits.size() != words || !std::ranges::equal(match.ids, aspect)) // new, or a hash collision
            {
                match.ids.assign(aspect.begin(), aspect.end());
                match.bits.assign(words, 0);
                for (sizet i = 0; i < phases.size(); ++i)
                {
                    if (systemcost::covers(phases[i].touched, aspect))
                    {
                        match.bits[i / 64] |= 1ull << (i % 64);
                    }
                }
            }
            return match.bits;
        }

        // The cost of the phases that touch any entity of the node's subtree, once per drop of the memo
        const NodeCost& costOf(const Entity node)
        {
            const auto [at, added] = costs.try_emplace(node);
            NodeCost& cost = at->second;
            if (!added)
            {
                return cost;
            }
            ensureModel();
            const sizet words = (phases.size() + 63) / 64;
            scratchBits.assign(words, 0);
            for (const Entity member : transforms().subtreeOf(node))
            {
                const std::vector<u64>& bits = matchOf(member);
                for (sizet word = 0; word < words; ++word)
                {
                    scratchBits[word] |= bits[word];
                }
            }
            for (sizet word = 0; word < words; ++word)
            {
                cost.systems += static_cast<u32>(std::popcount(scratchBits[word]));
                for (u64 bits = scratchBits[word]; bits != 0; bits &= bits - 1)
                {
                    cost.ms += std::max(0.0f, phases[word * 64 + std::countr_zero(bits)].heldMs);
                }
            }
            const bool was = over.contains(node);
            cost.warn = budgetMs > 0.0f && cost.systems > 0 &&
                (was ? cost.ms >= budgetMs * systemcost::WARN_RELEASE : cost.ms > budgetMs);
            if (cost.warn)
            {
                over.insert(node);
            }
            else
            {
                over.erase(node);
            }
            return cost;
        }

        // What the node's row shows of its cost: nothing without a matching system; quiet only over the budget,
        // selected or hovered; full always. A quiet panel whose phases sum to less than the budget reads no other
        // subtree.
        void detailFor(const Entity node, DetailLook& out)
        {
            std::string& text = out.text;
            text.clear();
            out.warn = false;
            out.bar = -1.0f;
            if (!costOn || !isScene(node)) // systems run over entities' archetypes: a UI row has no readout
            {
                return;
            }
            const bool picked = node == hoverNode || std::ranges::binary_search(tree.selection(), node);
            if (!telemetryFull && !picked && (budgetMs <= 0.0f || totalMs < budgetMs * systemcost::WARN_RELEASE))
            {
                return;
            }
            const NodeCost& cost = costOf(node);
            if (cost.systems == 0 || !(telemetryFull || picked || cost.warn))
            {
                return;
            }
            out.warn = cost.warn;
            out.bar =
                totalMs > 0.0f ? std::round(std::clamp(cost.ms / totalMs, 0.0f, 1.0f) * BAR_STEPS) / BAR_STEPS : 0.0f;
            text = tr(cost.warn ? "cost.over" : "cost",
                      {{"ms", std::round(static_cast<double>(cost.ms) * 100.0) / 100.0}});
        }

        // Whether a bound row would show another readout than it does
        bool detailsChanged()
        {
            DetailLook now;
            return std::ranges::any_of(detailed,
                                       [&](const auto& entry)
                                       {
                                           detailFor(entry.first, now);
                                           return now.text != entry.second.text || now.warn != entry.second.warn ||
                                               now.bar != entry.second.bar;
                                       });
        }

        // Quiet shows the hovered row's cost: the row under the pointer, compared per frame without a write.
        void followHover()
        {
            const Entity hovered = scope.router().hovered();
            if (hovered == hoverEntity)
            {
                return;
            }
            hoverEntity = hovered;
            Entity node = Entity::undefined();
            if (hovered.isValid() && scope.hierarchy().parentOf(hovered) == tree.content().entity())
            {
                if (const auto* row = scope.world().tryGetConstComponent<UiTreeRowComponent>(hovered))
                {
                    node = row->node;
                }
            }
            if (node == hoverNode)
            {
                return;
            }
            hoverNode = node;
            costsMoved = true;
        }

        // Per frame: the schedule's revision and the pointer's row; the costs themselves every REFRESH_FRAMES.
        void followCosts()
        {
            const World& target = world();
            if (costsStale || phasesWorld != &target || seenSchedule != target.systemRevision() ||
                ++sinceCosts >= systemcost::REFRESH_FRAMES)
            {
                sinceCosts = 0;
                costsStale = false;
                readPhases();
            }
            if (!telemetryFull)
            {
                followHover();
            }
        }

        // The listed rows (the roots, and the children of an expanded row) whose cost is in warning. A subtree costs
        // at least what any of its parts does (ms), but the warning has a release margin that each row holds for
        // itself, so a child may warn under a parent that does not: the walk goes on below any row within the margin
        // (ms at or over WARN_RELEASE of the budget), not only below the warning ones.
        u32 countOver(const Entity parent)
        {
            u32 total = 0;
            for (const Entity node : childrenOf(parent))
            {
                if (!isScene(node))
                {
                    continue;
                }
                const NodeCost cost = costOf(node);
                total += cost.warn ? 1 : 0;
                if ((cost.warn || cost.ms >= budgetMs * systemcost::WARN_RELEASE) && tree.isExpanded(node))
                {
                    total += countOver(node);
                }
            }
            return total;
        }

        // The tab-bar tag: the count of rows over the budget in warning, else the unit of the readouts under full
        // telemetry, else nothing (quiet until it matters). Written when what it shows changed.
        void updateTag()
        {
            tagRows = tree.rowCount();
            TagLook now;
            if (costOn && budgetMs > 0.0f && totalMs >= budgetMs * systemcost::WARN_RELEASE)
            {
                now.over = countOver(Entity::undefined());
            }
            now.kind = now.over > 0 ? 2 : (costOn && telemetryFull ? 1 : 0);
            now.budgetMs = now.kind == 2 ? budgetMs : 0.0f;
            if (now == tagLook)
            {
                return;
            }
            tagLook = now;
            tag.setVisible(false);
            if (now.kind == 0)
            {
                return;
            }
            tag.setStyleClass(InternedString(now.kind == 2 ? "hierarchy.tag.over" : "hierarchy.tag"));
            tag.setLocalizedText(InternedString(now.kind == 2 ? "editor-hierarchy.tag.over" : "editor-hierarchy.tag"));
            tag.setLocalizedArg("count", static_cast<std::int64_t>(now.over));
            tag.setLocalizedArg("ms", std::round(static_cast<double>(now.budgetMs) * 100.0) / 100.0);
        }

        // The telemetry setting and the system budget in force (the shell, at start and at every editorSettings).
        void setTelemetry(const bool full, const float budget)
        {
            if (costOn && full == telemetryFull && budget == budgetMs)
            {
                return;
            }
            costOn = true;
            telemetryFull = full;
            budgetMs = budget;
            over.clear();
            costs.clear();
            hoverEntity = Entity::undefined();
            hoverNode = Entity::undefined();
            readPhases();
            costsStale = false;
            sinceCosts = 0;
            costsMoved = false;
            ++stats.rebinds;
            detailed.clear();
            tree.refreshRows();
            updateTag();
        }

        void refresh()
        {
            SPITE_ZONE("EditorHierarchy.refresh");
            ++stats.refreshes;
            labelled.clear();
            detailed.clear();
            tree.refresh();
            if (renaming.isValid() && !placeRename())
            {
                endRename(); // the renamed node is gone (or no longer shows)
            }
        }

        // The structure changed: drop the model, re-read the visible nodes.
        void rebuild()
        {
            ++stats.rebuilds;
            modelTransforms = nullptr;
            refresh();
        }

        // The selection's nodes without those whose ancestor is selected.
        std::vector<Entity> topMost()
        {
            const std::span<const Entity> selected = tree.selection(); // sorted
            std::vector<Entity> top;
            for (const Entity node : selected)
            {
                bool nested = false;
                for (Entity at = parentOf(node); at.isValid() && !nested; at = parentOf(at))
                {
                    nested = std::ranges::binary_search(selected, at);
                }
                if (!nested && isScene(node) && world().isEntityAlive(node)) // UI rows are read-only
                {
                    top.push_back(node);
                }
            }
            return top;
        }

        // The visible rows in order, as the tree shows them. Reads children
        // past the tree's cache, so the query and rename leave the stats alone.
        template <typename Fn>
        void forEachRow(Fn&& fn)
        {
            struct Frame
            {
                std::vector<Entity> nodes;
                sizet next = 0;
            };
            std::vector<Frame> stack(1);
            readChildren(Entity::undefined(), stack.back().nodes);
            while (!stack.empty())
            {
                Frame& top = stack.back();
                if (top.next == top.nodes.size())
                {
                    stack.pop_back();
                    continue;
                }
                const Entity node = top.nodes[top.next++];
                const auto depth = static_cast<u32>(stack.size() - 1);
                fn(node, depth);
                if (tree.isExpanded(node))
                {
                    readChildren(node, stack.emplace_back().nodes);
                }
            }
        }

        // -- search --

        // hierarchyFilter or a locate set the filter: the rows settle at once, a query between frames lists them
        void filterChanged()
        {
            if (filter.revision() != seenFilter)
            {
                settleFilter();
                syncSelection(); // follow() sees the revision settled and would leave the highlight to a later event
            }
            showToolbar();
        }

        // hierarchyLocate: the way to the row opens and the tree scrolls to it
        void locateNode(const Entity node)
        {
            reveal(node);
            tree.scrollTo(node);
        }

        // hierarchyPick selected `count` of `total` entities
        void picked(const u64 count, const u64 total)
        {
            pickSelection = sortedSelection();
            pickNote = tr(total > count ? "pick.capped" : "pick.done",
                          {{"count", static_cast<std::int64_t>(count)}, {"total", static_cast<std::int64_t>(total)}});
            showToolbar();
        }

        // The rows to list moved: a new text reveals its matches, a cleared one closes what the filter opened, and
        // the tree re-reads its rows.
        void settleFilter()
        {
            seenFilter = filter.revision();
            if (filter.scope() != seenScope)
            {
                seenScope = filter.scope();
                scopeOpened = false;
            }
            if (!filter.filtering())
            {
                for (const Entity node : autoExpanded)
                {
                    tree.setExpanded(node, false);
                }
                autoExpanded.clear();
                revealedFor.clear();
                revealed.clear();
            }
            else
            {
                // a new text reveals every match, a later change (a rename, a component, a new scene) only the matches
                // it brought: one the person closed stays closed
                if (filter.text() != revealedFor)
                {
                    revealedFor = filter.text();
                    revealed.clear();
                }
                for (const Entity match : filter.matches())
                {
                    if (!revealed.insert(match).second)
                    {
                        continue;
                    }
                    for (Entity at = parentOf(match); at.isValid(); at = parentOf(at))
                    {
                        if (!tree.isExpanded(at))
                        {
                            tree.setExpanded(at, true);
                            autoExpanded.push_back(at);
                        }
                    }
                }
                // the UI scope alone lists the section: it opens once, and a person's collapse stays
                if (filter.scope() == Scope::Ui && !scopeOpened)
                {
                    scopeOpened = true;
                    if (!tree.isExpanded(UI_SECTION))
                    {
                        tree.setExpanded(UI_SECTION, true);
                        autoExpanded.push_back(UI_SECTION);
                    }
                }
            }
            rebuild();
        }

        // Opens the ancestors of node, top-down, so its row shows.
        void reveal(const Entity node)
        {
            std::vector<Entity> ancestors;
            for (Entity at = parentOf(node); at.isValid(); at = parentOf(at))
            {
                ancestors.push_back(at);
            }
            for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it)
            {
                tree.setExpanded(*it, true);
            }
        }

        std::string collapseAll()
        {
            tree.collapseAll();
            autoExpanded.clear();
            if (renaming.isValid() && !placeRename())
            {
                endRename();
            }
            return {};
        }

        // Submits a creating command for a new entity under parent (a scene header: in its layer) and selects the
        // result.
        void createUnder(const Entity parent, const char* command, Json args)
        {
            if (filter.filtering())
            {
                pending.push_back({Json{{"command", "hierarchyFilter"}}.dump()}); // the new entity matches nothing
            }
            if (isSceneHeader(parent))
                args["layer"] = parent.index();
            // a UI row selected: the entity goes to the root, UI elements take no scene children
            if (parent.isValid() && isScene(parent) && world().isEntityAlive(parent))
            {
                args["parent"] = parent.id();
            }
            submit({Json{{"command", command}, {"args", std::move(args)}}}, true);
        }

        void createEntity(const Entity parent) { createUnder(parent, "createEntity", Json{{"name", UNNAMED_ENTITY}}); }

        void createPrimitive(const Entity parent, const std::string_view kind)
        {
            createUnder(parent, "createPrimitive", Json{{"kind", std::string(kind)}});
        }

        std::vector<UiMenuItem> primitiveItems(const std::string_view idPrefix)
        {
            std::vector<UiMenuItem> items;
            for (const std::string_view kind : primitiveKinds())
                items.push_back({.id = InternedString(std::string(idPrefix).append(kind).c_str()),
                                 .label = tr(std::format("primitive.{}", kind), {})});
            return items;
        }

        // The component list: those some entity of the world has, game and engine ones.
        void openPicker()
        {
            const Json reply =
                panelui::request(*control, Json{{"query", "components"}, {"args", {{"world", worldName()}}}});
            std::vector<std::pair<std::string, std::string>> entries; // label, registered name
            if (reply.value("ok", false))
            {
                for (const Json& row : reply["result"].value("components", Json::array()))
                {
                    const std::string category = row.value("category", std::string{});
                    const u64 entities = row.value("entities", u64{0});
                    if (entities == 0 || category == "runtimeDerived" || category == "ui")
                    {
                        continue;
                    }
                    const std::string name = row.value("name", std::string{});
                    const ComponentID id = getComponentIdByName(name.c_str());
                    entries.emplace_back(
                        std::format("{} ({})",
                                    id == INVALID_COMPONENT_ID ? std::string_view(name) : componentDisplayName(id),
                                    entities),
                        name);
                }
            }
            std::ranges::sort(entries);
            std::vector<UiChoiceItem> items;
            pickNames.clear();
            for (const auto& [label, name] : entries)
            {
                items.push_back({.id = InternedString(name), .label = label});
                pickNames.push_back(name);
            }
            picker.setItems(items);
            picker.setSelected(-1);
            picker.open(pick);
        }

        [[nodiscard]] std::vector<Entity> sortedSelection() const
        {
            std::vector<Entity> held(editorModel->selection().begin(), editorModel->selection().end());
            std::ranges::sort(held);
            return held;
        }

        // The dropdown shows while there is UI to choose or a scope that narrows the rows (to get out of it); its slot
        // fits its height, so a hidden one takes no room. The dropdown is the code's: a document apply never writes it.
        void showScope()
        {
            if (scopeChoice.selected() != static_cast<i32>(filter.scope()))
            {
                scopeChoice.setSelected(static_cast<i32>(filter.scope()));
            }
            const bool wanted = filter.scope() != Scope::Both || uiHierarchy().elementCount() > 0;
            if (scopeChoice.isVisible() != wanted)
            {
                scopeChoice.setVisible(wanted);
            }
        }

        // The toolbar shows the filter and the last pick; it writes only what differs.
        void showToolbar()
        {
            showScope();
            const bool active = filter.searching();
            if (clear.isVisible() != active)
            {
                clear.setVisible(active);
            }
            const bool chipShown = active && pickNote.empty(); // a pick's note takes the whole row
            if (chip.isVisible() != chipShown)
            {
                chip.setVisible(chipShown);
            }
            if (active && filter.text() != chipText)
            {
                chipText = filter.text();
                chip.setLocalizedArgText("text", chipText);
            }
            if (search.value() != filter.text())
            {
                search.setText(filter.text());
            }
            ensureModel();
            const u64 shownCount = filter.matches().size();
            const u8 kind = !pickNote.empty() ? 4
                : !active                     ? 0
                : shownCount == 0             ? 3
                : filter.found() > FILTER_CAP ? 2
                                              : 1;
            if (kind != countKind)
            {
                countKind = kind;
                countedShown = countedTotal = ~0ull;
                shownNote.clear();
                count.setVisible(kind != 0);
                if (kind != 0 && kind != 4)
                {
                    count.setLocalizedText(InternedString(kind == 3       ? "editor-hierarchy.count.none"
                                                              : kind == 2 ? "editor-hierarchy.count.more"
                                                                          : "editor-hierarchy.count"));
                }
            }
            if (kind == 1 || kind == 2)
            {
                if (countedShown != shownCount)
                {
                    countedShown = shownCount;
                    count.setLocalizedArg("shown", shownCount);
                }
                if (countedTotal != modelNodes)
                {
                    countedTotal = modelNodes;
                    count.setLocalizedArg("total", modelNodes);
                }
            }
            else if (kind == 4 && shownNote != pickNote)
            {
                shownNote = pickNote;
                count.setText(pickNote);
            }
            const bool locatable = filter.locateTarget().isValid();
            if (locatable != canLocate)
            {
                canLocate = locatable;
                locate.setInteractable(locatable);
            }
        }

        // -- requests --------------------------------------------------------------

        void submit(std::vector<Json> commands, const bool selectCreated = false)
        {
            if (commands.empty())
            {
                return;
            }
            Json request = commands.size() == 1
                ? std::move(commands.front())
                : Json{{"command", "batch"}, {"args", {{"commands", std::move(commands)}}}};
            request["args"]["world"] = worldName();
            pending.push_back({request.dump(), selectCreated});
        }

        void selectRequest(const std::span<const Entity> nodes)
        {
            Json ids = Json::array();
            for (const Entity node : nodes)
            {
                ids.push_back(node.id());
            }
            pending.push_back({Json{{"command", "select"}, {"args", {{"world", worldName()}, {"ids", ids}}}}.dump()});
        }

        void dispatch()
        {
            while (!pending.empty())
            {
                std::vector<Request> requests;
                requests.swap(pending);
                for (const Request& request : requests)
                {
                    const Json reply = Json::parse(control->handle(request.line), nullptr, false);
                    if (!reply.is_object() || !reply.value("ok", false))
                    {
                        SWARNING_LOG("hierarchy: {} -> {}", request.line, reply.dump());
                        continue;
                    }
                    if (!request.selectCreated)
                    {
                        continue;
                    }
                    const Json& result = reply["result"];
                    std::vector<Entity> created;
                    for (const Json& one : result.contains("results") ? result["results"] : Json::array({result}))
                    {
                        created.emplace_back(one.value("id", u64{0}));
                    }
                    selectRequest(created);
                }
            }
        }

        void activate(const InternedString item)
        {
            const std::string_view id = item.toView();
            const Entity node = menuNode;
            if (id.starts_with(PRIMITIVE_PREFIX))
            {
                createPrimitive(node, id.substr(PRIMITIVE_PREFIX.size()));
                return;
            }
            if (isSceneHeader(node))
            {
                using Op = EditorSceneFlows::Op;
                auto& flows = EditorModelAccess::flows(*editorModel);
                if (id == "scene.active")
                {
                    submit({Json{{"command", "setActiveSaveTarget"}, {"args", {{"layer", node.index()}}}}});
                    tree.setExpanded(node, true);
                }
                else if (id == "scene.create")
                    createEntity(node);
                else if (id == "scene.save")
                    flows.requestLayer(Op::Save, node.index());
                else if (id == "scene.close")
                    flows.requestLayer(Op::Close, node.index());
                return;
            }
            if (id == CREATE_ITEM || id == CREATE_CHILD_ITEM)
            {
                createEntity(id == CREATE_CHILD_ITEM ? node : Entity::undefined());
                return;
            }
            if (id == RENAME_ITEM)
            {
                startRename(node);
                return;
            }
            if (id == "saveEntity")
            {
                EditorModelAccess::flows(*editorModel).saveEntity(node);
                return;
            }
            const bool duplicate = id == DUPLICATE_ITEM;
            std::vector<Json> commands;
            for (const Entity top : topMost())
            {
                commands.push_back(
                    Json{{"command", duplicate ? "duplicateEntity" : "deleteEntity"}, {"args", {{"id", top.id()}}}});
            }
            submit(std::move(commands), duplicate);
        }

        void openMenu(const Entity node, const Vec2 position)
        {
            if (isSceneHeader(node))
            {
                menuNode = node;
                std::vector<UiMenuItem> items{
                    {.id = InternedString("scene.active"),
                     .label = tr("scene.makeActive", {}),
                     .enabled = node.index() != activeScene},
                    {.id = InternedString("scene.create"), .label = tr("scene.create", {})},
                    {.id = InternedString(PRIMITIVE_ITEM),
                     .label = tr("menu.primitive", {}),
                     .children = primitiveItems(PRIMITIVE_PREFIX)},
                    {.id = InternedString("scene.save"), .label = tr("scene.save", {})},
                    {.id = InternedString("scene.close"), .label = tr("scene.close", {})},
                };
                menu.open(items, position);
                return;
            }
            if (node.isValid() && !isScene(node))
            {
                return; // the UI rows are read-only: no action applies to them
            }
            menuNode = node;
            std::vector<UiMenuItem> items{{.id = InternedString(CREATE_ITEM), .label = tr("menu.create", {})},
                                          {.id = InternedString(PRIMITIVE_ITEM),
                                           .label = tr("menu.primitive", {}),
                                           .children = primitiveItems(PRIMITIVE_PREFIX)}};
            if (node.isValid())
            {
                items.push_back({.id = InternedString(CREATE_CHILD_ITEM), .label = tr("menu.createChild", {})});
                items.push_back({.separator = true});
                items.push_back({.id = InternedString(DUPLICATE_ITEM), .label = tr("menu.duplicate", {})});
                if (worldName() == "scene")
                {
                    items.push_back({.id = InternedString("saveEntity"), .label = tr("menu.save", {})});
                }
                items.push_back({.id = InternedString(RENAME_ITEM), .label = tr("menu.rename", {})});
                items.push_back({.id = InternedString(DELETE_ITEM), .label = tr("menu.delete", {})});
            }
            menu.open(items, position);
        }

        void drop(const std::span<const Entity> nodes, const Entity parent)
        {
            // the UI rows are read-only: they neither move nor take a drop
            if ((parent.isValid() && !isScene(parent) && !isSceneHeader(parent)) ||
                std::ranges::any_of(nodes, [this](const Entity node) { return !isScene(node); }))
            {
                feedback->postToastKey(ToastLevel::Warn, std::string(TABLE) + "dropRefusedUi");
                return;
            }
            std::vector<Json> commands;
            for (const Entity node : nodes)
            {
                if (parentOf(node) == parent)
                {
                    continue; // siblings have no order: nothing to move
                }
                Json args{{"id", node.id()}};
                if (isSceneHeader(parent))
                    args["layer"] = parent.index();
                else if (parent.isValid())
                {
                    if (!scenes.empty())
                    {
                        const auto sceneOf = [this](Entity entity)
                        {
                            Entity root = entity;
                            for (Entity at = parentOf(root); at.isValid() && !isSceneHeader(at); at = parentOf(root))
                                root = at;
                            return rootScene(root);
                        };
                        if (sceneOf(node) != sceneOf(parent))
                        {
                            feedback->postToastKey(ToastLevel::Warn, std::string(TABLE) + "scene.moveHint");
                            return;
                        }
                    }
                    args["parent"] = parent.id();
                }
                commands.push_back(Json{{"command", "reparent"}, {"args", std::move(args)}});
            }
            if (!commands.empty())
            {
                dropped.assign(nodes.begin(), nodes.end());
            }
            submit(std::move(commands));
        }

        // After the drop ran and the rows followed it: the dropped nodes show, whatever collapsed parent they went
        // into.
        void settleDrop()
        {
            const std::vector<Entity> nodes = std::move(dropped);
            dropped.clear();
            Entity first = Entity::undefined();
            for (const Entity node : nodes)
            {
                if (world().isEntityAlive(node) && transforms().isInHierarchy(node))
                {
                    reveal(node);
                    first = first.isValid() ? first : node;
                }
            }
            if (first.isValid())
            {
                tree.scrollTo(first);
            }
        }

        // -- model drop -----------------------------------------------------

        // A Project model dragged over the panel: the indicator marks its parent, and is gone with the drag. A quiet
        // panel reads the drag pointer and nothing else.
        void followModelDrop()
        {
            const UiDragDrop::Drag* drag = scope.dragDrop().current();
            HierarchyModelDrop target;
            if (drag && drag->accepting && drag->payload.type == InternedString(MODEL_DRAG_PAYLOAD) && isShown())
            {
                target = hierarchyModelDrop(*engine, scope.hierarchy(), host, drag->hit);
            }
            if (target.inside && target.accepts)
            {
                const Entity row = target.layer ? sceneHeader(*target.layer) : target.parent;
                modelDrop = {true, row};
                tree.showDropTarget(row);
            }
            else if (modelDrop.active)
            {
                modelDrop = {};
                tree.hideDropTarget();
            }
        }

        void refuseDrop() { feedback->postToastKey(ToastLevel::Warn, std::string(TABLE) + "dropRefused"); }

        // -- rename ----------------------------------------------------------------

        // Lays the rename field over the renamed node's row; false when no row shows it.
        bool placeRename()
        {
            u32 row = 0;
            u32 found = static_cast<u32>(-1);
            u32 depth = 0;
            forEachRow(
                [&](const Entity shown, const u32 shownDepth)
                {
                    if (shown == renaming)
                    {
                        found = row;
                        depth = shownDepth;
                    }
                    ++row;
                });
            if (found == static_cast<u32>(-1))
            {
                return false;
            }
            const float x = static_cast<float>(depth + 2) * tree.indent();
            rename.setOffset(Vec2{x, static_cast<float>(found) * tree.rowHeight()});
            rename.setSize(Vec2{std::max(0.0f, tree.size().x - x - THUMB), tree.rowHeight()});
            return true;
        }

        void startRename(const Entity node)
        {
            if (renaming == node || !isScene(node) || !world().isEntityAlive(node))
            {
                return; // F2 inside the open field, a UI row (read-only), or a node that died
            }
            renaming = node;
            if (!placeRename())
            {
                renaming = Entity::undefined();
                return;
            }
            tree.scrollTo(node);
            rename.setText(nameOf(node).toView());
            rename.setVisible(true);
            scope.router().requestFocus(rename.entity());
            selectAllOnEdit = true; // once focus started the edit, which puts the caret at the end
        }

        void commitRename(const std::string_view text)
        {
            const Entity node = renaming;
            if (!node.isValid() || text.empty() || !world().isEntityAlive(node) || text == nameOf(node).toView())
            {
                return;
            }
            // a set needs the component: an unnamed entity gets it with the name
            if (world().hasComponent<EntityNameComponent>(node))
            {
                submit({Json{{"command", "set"},
                             {"args", {{"id", node.id()}, {"path", "EntityNameComponent.name"}, {"value", text}}}}});
            }
            else
            {
                submit({Json{
                    {"command", "addComponent"},
                    {"args", {{"id", node.id()}, {"component", "EntityNameComponent"}, {"value", {{"name", text}}}}}}});
            }
        }

        void endRename()
        {
            if (!renaming.isValid())
            {
                return;
            }
            renaming = Entity::undefined();
            selectAllOnEdit = false;
            rename.setVisible(false);
            if (scope.router().focused() == rename.entity())
            {
                scope.router().requestFocus(tree.entity()); // F2 and the arrows keep working
            }
        }

        // -- toolbar widgets ---------------------------------------------------------

        // An icon button of the actions slot: the control's square, named and explained by one message.
        Button actionButton(const std::string_view id, const std::string_view icon, const std::string_view key)
        {
            const float side = density.density().control;
            Button button = scope.createButton(InternedString(id), "", panelui::fieldDesc(Vec2{side, side}));
            button.setIcon(icon);
            button.setAccessibleNameKey(InternedString(key));
            button.setTooltip(InternedString(key));
            density.apply([button](const panelui::Density& now) mutable
                          { button.setSize(Vec2{now.control, now.control}); });
            return button;
        }

        void buildToolbar()
        {
            const panelui::Density& now = density.density();
            UiRectDesc searchDesc = panelui::barDesc(Vec2{0.0f, now.control});
            searchDesc.layoutItem = panelui::fillItem();
            searchDesc.layoutItem->alignSelf = UiLayoutAlign::Center;
            search = scope.createTextInput("hierarchy.search", searchDesc);
            panelui::bindInputPadding(search);
            search.setAccessibleNameKey(InternedString("editor-hierarchy.search"));
            search.setTooltip(InternedString("editor-hierarchy.search"));
            search.setLocalizedPlaceholder(InternedString("editor-hierarchy.search.placeholder"));
            search.onChange(
                [this](const std::string_view typed)
                {
                    typedText = std::string(typed); // one command per frame, the latest text
                });

            searchIcon =
                scope.createLabel("hierarchy.search.icon", "", panelui::barDesc(Vec2{now.xl + now.md, now.control}));
            searchIcon.setIcon("magnifying-glass");
            searchIcon.setInteractable(false);
            searchIcon.setAccessibleRole(UiRole::Presentation);
            density.apply([this](const panelui::Density& next)
                          { searchIcon.setSize(Vec2{next.xl + next.md, next.control}); });

            const auto clearFilter = [this]
            {
                scope.router().requestFocus(search.entity());
                typedText = std::string{};
            };
            clear = actionButton("hierarchy.clear", "x", "engine.ui.list.clear");
            clear.onClick(clearFilter);
            clear.setVisible(false);

            create = actionButton("hierarchy.create", "plus", "editor-hierarchy.create");
            create.onClick(
                [this]
                {
                    const std::span<const Entity> selected = tree.selection();
                    createEntity(selected.size() == 1 ? selected.front() : Entity::undefined());
                });
            collapse = actionButton("hierarchy.collapse", "arrows-in-line-vertical", "editor-hierarchy.collapse");
            collapse.onClick([this] { pending.push_back({Json{{"command", "hierarchyCollapse"}}.dump()}); });
            locate = actionButton("hierarchy.locate", "crosshair", "editor-hierarchy.locate");
            locate.onClick([this] { pending.push_back({Json{{"command", "hierarchyLocate"}}.dump()}); });
            locate.setInteractable(false);
            pick = actionButton("hierarchy.pick", "stack", "editor-hierarchy.pick");
            pick.onPress(
                [this](const u8 button, Vec2)
                {
                    if (button == PRIMARY)
                    {
                        openPicker();
                    }
                });
            pick.onKeyDown(
                [this](const u32 scancode, u8, const bool repeat)
                {
                    if ((scancode == RETURN_KEY || scancode == SPACE_KEY) && !repeat)
                    {
                        openPicker();
                        return true;
                    }
                    return false;
                });
            picker = scope.createPopupList("hierarchy.pick.list", UiDropdownDesc{.maxRows = 12, .searchable = true});
            picker.setAccessibleNameKey(InternedString("editor-hierarchy.pick.list"));
            picker.onChoose(
                [this](const u32 index)
                {
                    if (index < pickNames.size())
                    {
                        pending.push_back(
                            {Json{{"command", "hierarchyPick"}, {"args", {{"component", pickNames[index]}}}}.dump()});
                    }
                });

            UiRectDesc countDesc = panelui::fittedText(now.control, now.xs);
            countDesc.layoutItem->shrink = 1.0f;
            count = scope.createLabel("hierarchy.count", "", countDesc);
            panelui::bindFitPadding(count, panelui::SPACE_XS, panelui::SPACE_NONE);
            count.setVisible(false);
            UiRectDesc chipDesc = panelui::fittedText(now.control, now.md);
            chipDesc.layoutItem = UiLayoutItemComponent{.alignSelf = UiLayoutAlign::Center};
            chip = scope.createButton("hierarchy.chip", "", chipDesc);
            panelui::bindFitPadding(chip, panelui::SPACE_MD, panelui::SPACE_NONE);
            chip.setLocalizedText(InternedString("engine.ui.list.chip"));
            chip.onClick(clearFilter);
            chip.setVisible(false);
            density.apply(
                [this](const panelui::Density& next)
                {
                    for (Widget field : {Widget(search), Widget(count), Widget(chip)})
                    {
                        field.setSize(Vec2{field.size().x, next.control});
                    }
                    UiLayoutItemComponent searchItem = panelui::fillItem();
                    searchItem.minSize = Vec2{SEARCH_MIN_CONTROLS * next.control, 0.0f};
                    searchItem.alignSelf = UiLayoutAlign::Center;
                    search.setLayoutItem(searchItem);
                    chip.setLayoutItem(UiLayoutItemComponent{.maxSize = Vec2{CHIP_MAX_CONTROLS * next.control, 0.0f},
                                                             .alignSelf = UiLayoutAlign::Center});
                });
        }

        // The scope's dropdown: its row is in the scope slot, shown while there is UI to choose
        void buildScope()
        {
            UiDropdownDesc desc;
            desc.rect = panelui::barDesc(Vec2{0.0f, density.density().control});
            desc.rect.layoutItem = panelui::fillItem();
            desc.rect.layoutItem->alignSelf = UiLayoutAlign::Center;
            scopeChoice = density.track(scope.createDropdown("hierarchy.scope.choice", desc), panelui::HEIGHT_CONTROL);
            scopeChoice.setAccessibleNameKey(InternedString("editor-hierarchy.scope"));
            scopeChoice.setTooltip(InternedString("editor-hierarchy.scope"));
            std::vector<UiChoiceItem> items;
            for (const std::string_view name : HIERARCHY_SCOPE_NAMES)
            {
                items.push_back({.id = InternedString(name),
                                 .label = std::string(name),
                                 .key = InternedString(std::string(TABLE) + "scope." + std::string(name))});
            }
            scopeChoice.setItems(items);
            scopeChoice.setSelected(static_cast<i32>(filter.scope()));
            scopeChoice.onChange(
                [this](const u32 index)
                {
                    if (index < HIERARCHY_SCOPE_NAMES.size())
                    {
                        pending.push_back({Json{{"command", "hierarchyFilter"},
                                                {"args", {{"scope", std::string(HIERARCHY_SCOPE_NAMES[index])}}}}
                                               .dump()});
                    }
                });
        }

        void fillScope(const Widget& slot) { panelui::putIn(scope, slot, scopeChoice); }

        void fillFilter(const Widget& slot)
        {
            const InternedString base = panelui::slotFields(scope, slot).childClass;
            const InternedString family = base.isValid() ? base : InternedString(panelui::LIST_FILTER_CLASS);
            panelui::putIn(scope, slot, searchIcon);
            panelui::putIn(scope, slot, search);
            panelui::putIn(scope, slot, clear);
            searchIcon.setStyleClass(panelui::classPart(family, ".searchIcon"));
            search.setStyleClass(panelui::classPart(family, ".search"));
            clear.setStyleClass(panelui::classPart(family, ".clear"));
        }

        void fillActions(const Widget& slot)
        {
            const InternedString base = panelui::slotFields(scope, slot).childClass;
            const InternedString family = base.isValid() ? base : InternedString(panelui::LIST_FILTER_CLASS);
            for (Button button : {create, collapse, locate, pick})
            {
                panelui::putIn(scope, slot, button);
                button.setStyleClass(panelui::classPart(family, ".clear"));
            }
        }

        // the count (or the pick's note), then the chip: both shorten before they push each other out
        void fillStatus(const Widget& slot)
        {
            const InternedString base = panelui::slotFields(scope, slot).childClass;
            const InternedString family = base.isValid() ? base : InternedString(panelui::LIST_FILTER_CLASS);
            panelui::putIn(scope, slot, count);
            panelui::putIn(scope, slot, chip);
            count.setStyleClass(panelui::classPart(family, ".count"));
            chip.setStyleClass(panelui::classPart(family, ".chip"));
        }

        // -- document ----------------------------------------------------------------

        // The rows slot bound a new element or its fields changed: the tree goes in it and honours them.
        void fill(const Widget& slot)
        {
            if (scope.hierarchy().parentOf(tree.entity()) != slot.entity())
            {
                tree.setParent(slot);
            }
            const UiSlotComponent& fields = scope.world().getConstComponent<UiSlotComponent>(slot.entity());
            tree.setScrollEnabled(fields.scroll);
            tree.setRowClass(fields.childClass.isValid() ? fields.childClass : InternedString(ROW_CLASS));
        }

        // -- follow ----------------------------------------------------------------

        // The tree shows the inspector's selection when it is in this world;
        // nodes selected from elsewhere are revealed.
        void syncSelection()
        {
            std::vector<Entity> wanted;
            if (editorModel->selectionWorld() == worldName())
            {
                for (const Entity node : editorModel->selection())
                {
                    if (filter.hasRow(node) && filter.scopeAllows(node))
                    {
                        wanted.push_back(node);
                    }
                }
            }
            std::vector<Entity> sorted = wanted;
            std::ranges::sort(sorted);
            // the section row may be the tree's own highlight (a click or an arrow on it): the inspector never holds it
            std::vector<Entity> shown(tree.selection().begin(), tree.selection().end());
            std::erase(shown, UI_SECTION);
            if (sorted == shown)
            {
                return;
            }
            for (const Entity node : wanted)
            {
                reveal(node);
            }
            tree.setSelection(wanted);
            if (!wanted.empty())
            {
                tree.scrollTo(wanted.back());
            }
        }

        void follow()
        {
            if (selectAllOnEdit && scope.world().getConstComponent<UiTextEditComponent>(rename.entity()).editing)
            {
                selectAllOnEdit = false;
                rename.apply(UiTextCommand::SelectAll);
            }
            if (renaming.isValid() && scope.router().focused() != rename.entity())
            {
                endRename(); // committed, cancelled or focus moved away
            }
            if (const World* shown = &world(); shown != seenWorld)
            {
                // Play or Stop: the other world's generations and rename mean nothing here
                seenWorld = shown;
                synced = false;
                endRename();
                revealedFor.clear();
                revealed.clear();
                autoExpanded.clear();
                scopeOpened = false;
            }
            if (!isShown())
            {
                costsStale = true; // the first frame shown reads the costs at once
                return; // the generations stay unseen, so showing catches up
            }
            const u64 structure = transforms().structureGeneration();
            const u64 command = control->stateGeneration();
            readScenes();
            const bool sectionsChanged = seenSections != sectionsRevision;
            const bool focusChanged = !scenes.empty() && (focusedScene != activeScene || seenFocus != focusRevision);
            const u64 agentRevision = agent->revision();
            const u64 activity = transforms().activityGeneration();
            if (costOn)
            {
                followCosts();
                if (tree.rowCount() != tagRows)
                {
                    costsMoved = true; // rows opened or closed: the tag counts the listed ones
                }
            }
            // a locale change rewrites the tags' texts, which only a row that shows one needs redrawn
            const bool localeChanged = localization && localization->revision() != seenLocale;
            if (localeChanged)
            {
                loadMarkTexts();
                pickNote.clear(); // resolved in the old language
            }
            const bool marksChanged = agentRevision != seenAgent || (localeChanged && agent->hasMarks());
            const bool commandChanged = command != seenCommand;
            filter.follow();
            // The UI follows only where rows show it: an open section re-reads at every structure change, a closed one
            // only when the section itself appears or goes (nothing else of it is listed)
            bool uiStructure = false;
            if (const UiSeen now = uiNow(); !(now == seenUi))
            {
                seenUi = now;
                uiStructure = filter.sectionListed() != modelSection || (modelSection && tree.isExpanded(UI_SECTION));
                showScope(); // the dropdown appears with the UI's first element, whatever the text matches
            }
            const bool filterMoved = filter.revision() != seenFilter;
            // the readouts are worked out again when anything they read may have moved, and rebind only if one differs
            const bool costsChanged = std::exchange(costsMoved, false) || localeChanged;
            const bool activityChanged = activity != seenActivity;
            if (synced && structure == seenStructure && !sectionsChanged && !focusChanged && !commandChanged &&
                !marksChanged && !costsChanged && !filterMoved && !activityChanged && !uiStructure)
            {
                return;
            }
            const bool structureChanged = !synced || structure != seenStructure || uiStructure || sectionsChanged;
            synced = true;
            seenSections = sectionsRevision;
            seenStructure = structure;
            seenCommand = command;
            seenAgent = agentRevision;
            seenActivity = activity;
            if (structureChanged || commandChanged)
            {
                costs.clear(); // a subtree or an entity's components moved
            }
            if (structureChanged || filterMoved)
            {
                over.clear();
                hoverEntity = Entity::undefined();
                hoverNode = Entity::undefined();
                if (filterMoved)
                {
                    settleFilter();
                }
                else
                {
                    rebuild();
                }
            }
            else if (marksChanged ||
                     std::ranges::any_of(labelled,
                                         [this](const auto& entry) { return lookOf(entry.first) != entry.second; }))
            {
                refresh();
            }
            else if ((commandChanged || costsChanged) && detailsChanged())
            {
                ++stats.rebinds;
                detailed
                    .clear(); // the rebind repopulates the bound rows only; a scrolled-off entry would differ forever
                tree.refreshRows();
            }
            SPITE_ZONE("EditorHierarchy.syncSelection");
            syncSelection();
            if (focusChanged)
            {
                focusedScene = activeScene;
                seenFocus = focusRevision;
                const Entity header = sceneHeader(activeScene);
                tree.setExpanded(header, true);
                if (filter.scope() != Scope::Ui)
                    tree.scrollTo(header);
            }
            updateTag(); // after the selection opened its parents
            if (!pickNote.empty() && sortedSelection() != pickSelection)
            {
                pickNote.clear(); // the selection moved on: the note no longer describes it
            }
            if (commandChanged || filterMoved || structureChanged || localeChanged)
            {
                showToolbar();
            }
        }

        // -- control -----------------------------------------------------------------

        void registerQuery()
        {
            control->registerViewCommand("hierarchyCollapse", OWNER, "collapses every node of the hierarchy", {},
                                         [this](const ControlArgs&, IFormatWriter&) { return collapseAll(); });
            control->registerQuery(
                "hierarchy", OWNER,
                "the hierarchy panel's visible rows (a UI row has ui true and its uiKind; the UI "
                "section's own row, id 0, has section true; scene headers have id 0, sceneLayer "
                "and activeScene), the filter (text, scope), selection, "
                "the drag in flight (drag {nodes, parent, index, valid}), stats and the tab-bar "
                "tag (tag: the text shown, empty while hidden; tagOver: the listed rows in warning)",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    filter.refresh(false, true); // current while hidden or between frames
                    out.writeString("world", worldName());
                    out.writeString("telemetry", telemetryFull ? "full" : "quiet");
                    out.writeF32("systemBudgetMs", budgetMs);
                    out.writeString("tag", tag.isVisible() ? tag.text() : std::string_view{});
                    out.writeU32("tagOver", tagLook.over);
                    if (costOn)
                    {
                        readPhases(); // current even while hidden
                    }
                    DetailLook detail;
                    std::vector<std::pair<Entity, u32>> rows;
                    forEachRow([&rows](const Entity node, const u32 depth) { rows.emplace_back(node, depth); });
                    const std::span<const Entity> selected = tree.selection();
                    out.beginArray("rows", static_cast<u32>(rows.size()));
                    std::vector<Entity> kids;
                    for (const auto& [node, depth] : rows)
                    {
                        kids.clear();
                        readChildren(node, kids);
                        out.beginObject("");
                        // the UI section is no entity: id 0 and `section`; a UI element carries `ui`
                        // and its widget kind
                        out.writeU64("id", node == UI_SECTION || isSceneHeader(node) ? 0 : node.id());
                        const RowLook look = lookOf(node);
                        const InternedString name = nameOf(node);
                        out.writeString("name", name.isValid() ? name.toView() : std::string_view{});
                        out.writeString("label", textOf(node, look));
                        out.writeBool("ui", look.ui);
                        out.writeBool("section", node == UI_SECTION);
                        if (isSceneHeader(node))
                        {
                            out.writeU32("sceneLayer", node.index());
                            out.writeBool("activeScene", node.index() == activeScene);
                        }
                        out.writeString(
                            "uiKind", look.ui && node != UI_SECTION ? uiWidgetKindName(look.kind) : std::string_view{});
                        out.writeString("primary",
                                        look.primary == INVALID_COMPONENT_ID ? std::string_view{}
                                                                             : componentDisplayName(look.primary));
                        out.writeU32("depth", depth);
                        out.writeU32("children", static_cast<u32>(kids.size()));
                        out.writeBool("expanded", tree.isExpanded(node));
                        out.writeBool("enabled", look.enabled);
                        out.writeBool("active", look.active);
                        out.writeBool("selected", std::ranges::binary_search(selected, node));
                        // the layer's own token, not the localized tag text
                        const EditorAgentLayer::Mark mark = node == UI_SECTION || isSceneHeader(node)
                            ? EditorAgentLayer::Mark::None
                            : agent->markOf(node);
                        out.writeString("mark",
                                        mark == EditorAgentLayer::Mark::Agent        ? "agent"
                                            : mark == EditorAgentLayer::Mark::Edited ? "edited"
                                                                                     : "");
                        // the summed average of the systems touching the subtree, and what the row
                        // shows of it now (localized; empty: nothing)
                        const NodeCost cost = costOn && isScene(node) ? costOf(node) : NodeCost{};
                        detailFor(node, detail);
                        out.writeF32("costMs", cost.ms);
                        out.writeU32("costSystems", cost.systems);
                        out.writeBool("costOver", cost.warn);
                        out.writeString("detail", detail.text);
                        out.writeF32("costBar", detail.bar);
                        out.writeString("icon", iconFor(node, look, !kids.empty()));
                        out.endObject();
                    }
                    out.endArray();
                    // the search: find's text= matches in the hierarchy, of `total` entities in it
                    // the search: find's text= matches in the hierarchy, of `total` entities in it
                    filter.write(out, modelNodes);
                    out.writeString("note", pickNote);
                    out.writeU64("renaming", renaming.id());
                    // a drag in flight: where it would land (parent 0: the roots) and whether it
                    // does
                    const auto& drag = scope.world().getConstComponent<UiTreeComponent>(tree.entity());
                    out.beginObject("drag");
                    out.writeU32("nodes", static_cast<u32>(drag.dragged.size()));
                    out.writeU64("parent", drag.dropParent.id());
                    out.writeU32("index", drag.dropIndex);
                    out.writeBool("valid", drag.dropValid);
                    out.endObject();
                    out.beginObject("modelDrop");
                    out.writeBool("active", modelDrop.active);
                    out.writeU64("parent", modelDrop.parent.id());
                    out.endObject();
                    out.beginObject("stats");
                    out.writeU64("rebuilds", stats.rebuilds);
                    out.writeU64("refreshes", stats.refreshes);
                    out.writeU64("childLists", stats.childLists);
                    out.writeU64("labels", stats.labels);
                    out.writeU64("rebinds", stats.rebinds);
                    out.writeU64("finds", filter.finds());
                    out.endObject();
                    return std::string{};
                });
        }
    };

    HierarchyModelDrop hierarchyModelDrop(Engine& engine, const UiHierarchyManager& editorUi, const Widget& content,
                                          const Entity hit)
    {
        PlayWorld* play = engine.playWorld();
        const World& world = play ? play->world() : engine.world();
        const UiHierarchyManager& sceneUi = (play ? play->ui() : engine.sceneUi()).hierarchy();
        Entity row = Entity::undefined();
        for (Entity at = hit; at.isValid(); at = editorUi.parentOf(at))
        {
            if (row == Entity::undefined())
            {
                if (const auto* treeRow = editorUi.world().tryGetConstComponent<UiTreeRowComponent>(at))
                {
                    row = treeRow->node;
                }
            }
            if (at == content.entity())
            {
                if (row == Entity::undefined())
                {
                    return {true, true, Entity::undefined()};
                }
                if (isSceneHeader(row) && !engine.playWorld())
                    return {.inside = true, .accepts = true, .layer = row.index()};
                const bool scene = row != UI_SECTION && !sceneUi.isElement(row) && world.isEntityAlive(row);
                return {true, scene, scene ? row : Entity::undefined()};
            }
        }
        return {};
    }

    EditorHierarchy::EditorHierarchy(Engine& engine, const EditorModel& editorModel, const EditorAgentLayer& agent,
                                     EditorFeedback& feedback) :
        m_state(std::make_unique<State>(engine, editorModel, agent, feedback))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.host = scope.createPanel("hierarchy.host", panelui::columnDesc());
        UiTreeDesc desc;
        desc.rect = topLeft();
        desc.rect.layoutItem = panelui::fillItem(); // fills the slot when the document lays the slot out
        desc.rowHeight = state.density.density().row;
        desc.indent = indentOf(state.density.density());
        desc.rowClass = InternedString(ROW_CLASS);
        desc.children = [&state](const Entity node) { return state.childrenOf(node); };
        desc.parent = [&state](const Entity node) { return state.parentOf(node); };
        desc.label = [&state](const Entity node) { return state.labelOf(node); };
        desc.icon = [&state](const Entity node) { return state.iconOf(node); };
        desc.mark = [&state](const Entity node) { return state.markOf(node); };
        desc.markSpace = MARK_SPACE;
        desc.markShortSpace = MARK_SHORT_SPACE;
        desc.markInset = THUMB;
        desc.look = [&state](const Entity node) { return state.lookOfRow(node); };
        desc.ordered = false; // children are listed by entity index: a drop that keeps the parent changes nothing
        state.loadMarkTexts();
        state.tree = scope.createTree("hierarchy", std::move(desc));
        state.tree.setAccessibleNameKey(InternedString("editor.access.hierarchy"));
        state.tree.onSelect(
            [&state](const std::span<const Entity> selection)
            {
                // the UI section is a heading, not an entity: the inspector never holds it
                std::vector<Entity> chosen(selection.begin(), selection.end());
                std::erase(chosen, UI_SECTION);
                std::erase_if(chosen, isSceneHeader);
                std::vector<Entity> held(state.editorModel->selection().begin(), state.editorModel->selection().end());
                std::ranges::sort(held);
                if (state.editorModel->selectionWorld() != state.worldName() || !std::ranges::equal(held, chosen))
                {
                    state.selectRequest(chosen);
                }
            });
        state.tree.onDrop([&state](const std::span<const Entity> nodes, const Entity parent, u32)
                          { state.drop(nodes, parent); });
        state.tree.onDropRefused([&state] { state.refuseDrop(); });
        state.tree.onRename([&state](const Entity node) { state.startRename(node); });
        state.tree.onContextMenu([&state](const Entity node, const Vec2 position) { state.openMenu(node, position); });
        // a secondary press on no row: the root menu
        const auto background = [&state](const u8 button, const Vec2 position)
        {
            if (button == SECONDARY)
            {
                state.openMenu(Entity::undefined(), position);
            }
        };
        state.tree.onPress(background);
        state.tree.content().onPress(background);

        state.tag = scope.createLabel("hierarchy.tag", "", panelui::fittedText(state.density.density().row, 0.0f));
        state.density.track(state.tag, panelui::HEIGHT_ROW);
        state.tag.setVisible(false);

        state.rename = scope.createTextInput("hierarchy.rename", topLeft());
        state.rename.setAccessibleNameKey(InternedString("editor.access.hierarchyRename"));
        state.rename.setParent(state.tree.content());
        state.rename.setZOffset(2); // over the rows and the drop indicator
        state.rename.setVisible(false);
        state.rename.onCommit([&state](const std::string_view text) { state.commitRename(text); });
        state.rename.onSubmit([&state](std::string_view) { state.endRename(); }); // Enter ends it, changed or not

        // a density change: the rows take the new height and indent, an open rename field follows its row
        state.density.apply(
            [&state](const panelui::Density& now)
            {
                state.tree.setMetrics(now.row, indentOf(now));
                if (state.renaming.isValid())
                {
                    state.placeRename();
                }
            });

        UiMenuDesc menuDesc;
        menuDesc.bounds = state.tree;
        state.menu = scope.createMenu("hierarchy.menu", menuDesc);
        state.menu.onActivate([&state](const InternedString item) { state.activate(item); });

        state.buildToolbar();
        state.buildScope();
        state.registerQuery();
        scope.commit();

        // the code's contract: the rows slot; without a readable document, the fallback builds the default panel
        UiViewContract contract;
        contract.slot(InternedString(FILTER_SLOT), [&state](const Widget& slot) { state.fillFilter(slot); });
        contract.slot(InternedString(ACTIONS_SLOT), [&state](const Widget& slot) { state.fillActions(slot); });
        contract.slot(InternedString(SCOPE_SLOT), [&state](const Widget& slot) { state.fillScope(slot); });
        contract.slot(InternedString(STATUS_SLOT), [&state](const Widget& slot) { state.fillStatus(slot); });
        contract.slot(InternedString(ROWS_SLOT), [&state](const Widget& slot) { state.fill(slot); });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                panelui::columnRoot(root);
                panelui::toolbarPanel(viewScope, FILTER_SLOT, root, panelui::SPACE_XS, UiLayoutAlign::Center);
                panelui::toolbarPanel(viewScope, ACTIONS_SLOT, root, panelui::SPACE_XS, UiLayoutAlign::Center);
                const panelui::Density density = panelui::Density::read(viewScope);
                // the scope row and the status line fit their height and are padded at the sides only
                for (const std::string_view id : {SCOPE_SLOT, STATUS_SLOT})
                {
                    UiRectDesc bar = panelui::barDesc(
                        Vec2{},
                        panelui::stack(UiLayoutDirection::Row, density.xs, Vec4{density.xs, 0.0f, density.xs, 0.0f}));
                    bar.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                    Panel barPanel = viewScope.createPanel(InternedString(id), bar);
                    panelui::bindGap(barPanel, panelui::SPACE_XS);
                    panelui::bindPadding(barPanel, panelui::SPACE_XS, panelui::SPACE_NONE, panelui::SPACE_XS,
                                         panelui::SPACE_NONE);
                    barPanel.setParent(root);
                }
                panelui::fillSlotPanel(viewScope, ROWS_SLOT, root);
            });
        state.view = std::make_unique<UiDocumentView>(editorUi(engine).ui(), std::string(VIEW), std::move(contract),
                                                      state.host.entity());
        if (!state.view->load(resolveContentPath(DOCUMENT)))
        {
            SWARNING_LOG("hierarchy: {} did not apply ({} problems): the default panel shows", DOCUMENT,
                         state.view->problems().size());
        }
        // the very last step: nothing after it can fail, so a constructor that throws leaves no hook with a dead
        // capture; the model runs these inside its commands and the destructor takes them out first
        EditorModelAccess::setHierarchyView(
            editorModel,
            {.changed = [&state] { state.filterChanged(); },
             .reveal = [&state](const Entity node) { state.locateNode(node); },
             .picked = [&state](const u64 count, const u64 total) { state.picked(count, total); }});
    }

    EditorHierarchy::~EditorHierarchy()
    {
        EditorModelAccess::setHierarchyView(*m_state->editorModel, {});
        m_state->control->unregisterOwner(OWNER);
    }

    Widget EditorHierarchy::content() const { return m_state->host; }

    Widget EditorHierarchy::tag() const { return m_state->tag; }

    void EditorHierarchy::setTelemetry(const bool full, const float systemBudgetMs)
    {
        m_state->setTelemetry(full, systemBudgetMs);
    }

    void EditorHierarchy::update()
    {
        m_state->tree.advance(static_cast<float>(m_state->engine->lastFrameSeconds()));
        {
            SPITE_ZONE("EditorHierarchy.dispatch");
            if (m_state->typedText)
            {
                const Json line{{"command", "hierarchyFilter"}, {"args", {{"text", *m_state->typedText}}}};
                m_state->typedText.reset();
                m_state->pending.push_back({line.dump()});
            }
            m_state->dispatch();
        }
        m_state->follow();
        m_state->followModelDrop();
        if (!m_state->dropped.empty())
        {
            m_state->settleDrop();
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "hierarchy",
             .order = 10,
             .dock = {.target = "scene", .zone = UiDockZone::Left, .ratio = 0.2f, .after = "game"},
             .minSize = {120.0f, 0.0f},
             .zone = "EditorHierarchy",
             .make = [](EditorPanelContext& c)
             { return makeEditorPanel<EditorHierarchy>(c.engine, c.model, c.agent, c.feedback); }}};
    } // namespace
} // namespace spite
