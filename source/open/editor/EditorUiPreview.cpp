#include "EditorContext.hpp"
#include "EditorDialogs.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorPanelUi.hpp"
#include "EditorPageScroll.hpp"
#include "base/ContentRoot.hpp"
#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "engine/PlayWorld.hpp"
#include "localization/Localization.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiEvents.hpp"
#include "ui/UiForm.hpp"
#include "uiview/UiStyleSheet.hpp"
#include "uiview/UiViewContext.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <memory>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

namespace spite
{
    namespace
    {
        using namespace panelui;
        constexpr auto OWNER = "editorUiPreview";
        constexpr auto PANEL = "uiPreview";
        constexpr auto VIEW = "__projectUiPreview";
        class UiPreview
        {
        public:
            UiPreview(Engine& engine, const DockSpace& dock) : engine(engine), dock(dock), scope(editorUi(engine).ui())
            {
                root = scope.createPanel("uiPreview.root", columnDesc());
                title = scope.createLabel("uiPreview.name", "", barDesc(Vec2{0, 24}));
                title.setParent(root);
                status = scope.createLabel("uiPreview.status", "", barDesc(Vec2{0, 24}));
                status.setParent(root);
                detail = scope.createLabel("uiPreview.detail", "", barDesc(Vec2{0, 24}));
                detail.setParent(root);
                detail.setVisible(false);
                viewport = scope.createPanel("uiPreview.viewport", columnDesc());
                viewport.setLayoutItem(fillItem());
                viewport.setParent(root);
                scope.world().getComponent<UiStateComponent>(viewport.entity()).clipsChildren = true;
                scope.commit();
                engine.control().registerViewCommand(
                    "openUiPreview", OWNER, "Open an isolated UI document preview",
                    {requiredArg("name", ArgKind::String, "document view name")},
                    [this](const ControlArgs& args, IFormatWriter&)
                    {
                        const auto name = args.string("name");
                        if (!name || name->empty() || *name == VIEW)
                            return std::string("document name required");
                        const UiDocumentView* source = nullptr;
                        for (UiContext* ui : {&editorUi(this->engine).ui(), &this->engine.sceneUi()})
                        {
                            const auto* found = ui->documents().find(*name);
                            if (found && !found->path().empty())
                            {
                                source = found;
                                break;
                            }
                        }
                        if (!source)
                            return std::string("document not found");
                        path = source->path();
                        title.setText(*name);
                        clear();
                        if (!this->dock.isDetached(PANEL))
                            this->dock.apply(UiDockOp{.kind = UiDockOpKind::Float,
                                                      .panel = PANEL,
                                                      .position = {80, 60},
                                                      .size = {1100, 700},
                                                      .detached = true});
                        this->dock.apply(UiDockOp{.kind = UiDockOpKind::Activate, .panel = PANEL});
                        return std::string{};
                    });
            }
            ~UiPreview()
            {
                engine.control().unregisterOwner(OWNER);
                clear();
            }
            Widget content() const { return root; }
            void update()
            {
                if (!shown(scope, root))
                {
                    clear();
                    return;
                }
                if (path.empty() || attempted)
                    return;
                attempted = true;
                view =
                    std::make_unique<UiDocumentView>(editorUi(engine).ui(), VIEW, UiViewContract{}, viewport.entity());
                const bool ok = view->load(path);
                view->watch(false);
                status.setVisible(true);
                status.setLocalizedText(ok ? "editor-uiPreview.static" : "editor-uiPreview.failed");
                detail.setVisible(!ok && !view->problems().empty());
                if (!ok)
                {
                    if (!view->problems().empty())
                    {
                        const auto& problem = view->problems().front();
                        detail.setText(problem.path + ": " + problem.message);
                    }
                    view.reset();
                    return;
                }
                auto& hierarchy = scope.hierarchy();
                Widget(&view->scope(), view->root(), view->scope().generation()).setLayoutItem(fillItem());
                hierarchy.setHitTransparent(view->root(), true);
                std::vector<Entity> nodes{view->root()};
                for (sizet i = 0; i < nodes.size(); ++i)
                {
                    const auto children = hierarchy.childrenOf(nodes[i]);
                    nodes.insert(nodes.end(), children.begin(), children.end());
                    if (auto* state = scope.world().tryGetComponent<UiStateComponent>(nodes[i]))
                        state->tabStop = false;
                }
            }

        private:
            void clear()
            {
                if (view)
                {
                    scope.hierarchy().setHitTransparent(view->root(), false);
                    view.reset();
                }
                attempted = false;
            }
            Engine& engine;
            DockSpace dock;
            UiScope scope;
            Panel root, viewport;
            Label title, status, detail;
            std::unique_ptr<UiDocumentView> view;
            std::filesystem::path path;
            bool attempted = false;
        };
        class GameUiEditor
        {
            using Json = nlohmann::ordered_json;
            static constexpr auto PANEL_NAME = "gameUiEditor";
            static constexpr auto DRAFT_NAME = "__gameUiEditorDraft";
            static constexpr auto STYLE_LAYER = "gameUiEditor.document";
            static constexpr auto HOVER_LAYER = "gameUiEditor.hover";
            static constexpr Vec2 MIN_ARTBOARD{1024.0f, 576.0f};
            struct Draft
            {
                std::unique_ptr<UiDocumentView> view;
                std::filesystem::path path;
                std::string name, saved;
                Json model;
                std::vector<std::string> protectedIds, mountSlots;
                bool knowsBindings = false;
                bool mounted = false;
                bool fillWidth = false;
                bool previewVisible = true;
            };
            struct Undo
            {
                Draft* document;
                std::string text;
            };
            struct Surface
            {
                std::unique_ptr<UiScope> scope;
                Panel panel;
            };

        public:
            GameUiEditor(Engine& engine, const DockSpace& dock, EditorDialogs& dialogs) :
                engine(engine), dock(dock), dialogs(dialogs), scope(editorUi(engine).ui())
            {
                root = scope.createPanel("gameUiEditor.root", columnDesc());
                bindGap(root, SPACE_XS);
                bindPadding(root, SPACE_SM, SPACE_SM, SPACE_SM, SPACE_SM);
                Panel bar = wrapRowPanel(scope, "gameUiEditor.toolbar", root);
                visibilityButton = button(bar, "gameUiEditor.hiddenViews", "editor-gameUiEditor.showHidden",
                                          [this]
                                          {
                                              showHidden = !showHidden;
                                              syncControls();
                                          });
                button(bar, "gameUiEditor.open", "editor-gameUiEditor.open", [this] { chooseFile(); });
                importButton = button(bar, "gameUiEditor.import", "editor-gameUiEditor.import",
                                      [this] { chooseSubtreeFile(false); });
                exportButton = button(bar, "gameUiEditor.export", "editor-gameUiEditor.export",
                                      [this] { chooseSubtreeFile(true); });
                saveButton = button(bar, "gameUiEditor.save", "editor-gameUiEditor.save", [this] { save(); });
                undoButton = button(bar, "gameUiEditor.undo", "editor-gameUiEditor.undo", [this] { undo(); });
                addPanelButton = button(bar, "gameUiEditor.addPanel", "editor-gameUiEditor.addPanel",
                                        [this] { reportOpen(addWidget("panel")); });
                addLabelButton = button(bar, "gameUiEditor.addLabel", "editor-gameUiEditor.addLabel",
                                        [this] { reportOpen(addWidget("label")); });
                addButtonButton = button(bar, "gameUiEditor.addButton", "editor-gameUiEditor.addButton",
                                         [this] { reportOpen(addWidget("button")); });
                duplicateButton = button(bar, "gameUiEditor.duplicate", "editor-gameUiEditor.duplicate",
                                         [this] { reportOpen(duplicateWidget()); });
                moveButton = button(bar, "gameUiEditor.move", "editor-gameUiEditor.move", [this] { armMove(); });
                deleteButton = button(bar, "gameUiEditor.delete", "editor-gameUiEditor.delete",
                                      [this] { reportOpen(deleteWidget()); });
                earlier = button(bar, "gameUiEditor.earlier", "editor-gameUiEditor.earlier", [this] { reorder(-1); });
                later = button(bar, "gameUiEditor.later", "editor-gameUiEditor.later", [this] { reorder(1); });
                title = scope.createLabel("gameUiEditor.document", "", barDesc({0.0f, 24.0f}));
                title.setParent(root);
                status = scope.createLabel("gameUiEditor.status", "", barDesc({0.0f, 24.0f}));
                status.setParent(root);
                status.setLocalizedText("editor-gameUiEditor.empty");

                UiRectDesc bodyDesc = topLeft();
                bodyDesc.layoutItem = fillItem();
                bodyDesc.layout = stack(UiLayoutDirection::Row, 4.0f, {}, UiLayoutAlign::Stretch);
                Panel body = scope.createPanel("gameUiEditor.body", bodyDesc);
                bindGap(body, SPACE_SM);
                body.setParent(root);
                UiRectDesc sidebarDesc = columnDesc();
                sidebarDesc.layoutItem = basisItem(210.0f);
                Panel sidebar = scope.createPanel("gameUiEditor.sidebar", sidebarDesc);
                sidebar.setParent(body);
                UiTreeDesc treeDesc;
                treeDesc.rect = topLeft();
                treeDesc.rect.layoutItem = fillItem();
                treeDesc.rowHeight = density.density().row;
                treeDesc.rowClass = "hierarchy.row";
                treeDesc.markSpace = 42.0f;
                treeDesc.markInset = 12.0f;
                treeDesc.children = [this](Entity entity) -> std::span<const Entity>
                {
                    if (!entity.isValid())
                        return treeRoots;
                    const auto it = treeChildren.find(entity);
                    return it == treeChildren.end() ? std::span<const Entity>{} : std::span<const Entity>(it->second);
                };
                treeDesc.parent = [this](Entity entity)
                {
                    const auto parent = scope.hierarchy().parentOf(entity);
                    return std::ranges::contains(nodes, parent) ? parent : Entity::undefined();
                };
                treeDesc.label = [this](Entity entity) { return idOf(entity); };
                if (engine.services().contains<Localization>())
                {
                    auto& locale = engine.services().get<Localization>();
                    std::string scratch;
                    requiredMark = locale.resolve("editor-gameUiEditor.requiredMark", {}, scratch);
                    slotMark = locale.resolve("editor-gameUiEditor.slotMark", {}, scratch);
                }
                treeDesc.mark = [this](Entity entity)
                {
                    const auto* document = documentOf(entity);
                    const auto id = idOf(entity);
                    if (document && std::ranges::contains(document->mountSlots, id))
                    {
                        return UiTreeMark{.text = slotMark};
                    }
                    if (document && std::ranges::contains(document->protectedIds, id))
                    {
                        return UiTreeMark{.text = requiredMark};
                    }
                    return UiTreeMark{};
                };
                tree = scope.createTree("gameUiEditor.hierarchy", std::move(treeDesc));
                tree.setAccessibleNameKey(InternedString("editor-gameUiEditor.hierarchy"));
                tree.setParent(sidebar);
                tree.onSelect(
                    [this](std::span<const Entity> keys)
                    {
                        if (refreshingRows)
                        {
                            return;
                        }
                        chooseWidget(keys.empty() ? Entity::undefined() : keys.front());
                    });
                tree.onDrop(
                    [this](std::span<const Entity> moved, Entity parent, u32 index)
                    {
                        if (moved.size() != 1)
                        {
                            reportOpen("Move one widget at a time.");
                            return;
                        }
                        const Entity source = moved.front();
                        queued = [this, source, parent, index] { reportOpen(treeMove(source, parent, index)); };
                    });
                tree.onDropRefused([this] { status.setLocalizedText("editor-gameUiEditor.dropRefused"); });

                UiRectDesc propertiesDesc = columnDesc();
                propertiesDesc.layoutItem = basisItem(320.0f);
                Panel inspectorHost = scope.createPanel("gameUiEditor.properties", propertiesDesc);
                inspectorPage =
                    PageScroll(scope, "gameUiEditor.propertyScroll", "editor-gameUiEditor.properties", inspectorHost);
                Panel properties = scope.createPanel("gameUiEditor.propertyPage", columnDesc());
                properties.setParent(inspectorPage.page());
                inspector = scope.createLabel("gameUiEditor.inspector", "", barDesc({0.0f, 30.0f}));
                inspector.setParent(properties);
                bounds = scope.createLabel("gameUiEditor.bounds", "", barDesc({0.0f, 24.0f}));
                bounds.setParent(properties);
                widgetClass = scope.createLabel("gameUiEditor.class", "", barDesc({0.0f, 24.0f}));
                widgetClass.setLocalizedText("editor-gameUiEditor.class");
                widgetClass.setParent(properties);
                textInput = scope.createTextInput("gameUiEditor.text", barDesc({0.0f, 30.0f}));
                textInput.setAccessibleNameKey("editor-gameUiEditor.text");
                textInput.setParent(properties);
                textInput.onSubmit([this](std::string_view value)
                                   { queueWidgetEdit([this, text = std::string(value)] { editText(text); }); });
                actionLabel = scope.createLabel("gameUiEditor.action", "", barDesc({0.0f, 28.0f}));
                actionLabel.setParent(properties);
                actionArgs = scope.createLabel("gameUiEditor.actionArgs", "", barDesc({0.0f, 24.0f}));
                actionArgs.setParent(properties);
                hoverLabel = scope.createLabel("gameUiEditor.hoverLabel", "", barDesc({0.0f, 24.0f}));
                hoverLabel.setLocalizedText("editor-gameUiEditor.hoverLabel");
                hoverLabel.setParent(properties);
                hoverColor = scope.createTextInput("gameUiEditor.hoverColor", barDesc({0.0f, 30.0f}));
                hoverColor.setAccessibleNameKey("editor-gameUiEditor.hoverColor");
                hoverColor.setTooltip(InternedString("editor-gameUiEditor.hoverColor"));
                hoverColor.setParent(properties);
                hoverColor.onSubmit([this](std::string_view value)
                                    { queueWidgetEdit([this, color = std::string(value)] { editHover(color); }); });
                hoverButton = button(properties, "gameUiEditor.hover", "editor-gameUiEditor.previewHover",
                                     [this]
                                     {
                                         previewHover = !previewHover;
                                         syncControls();
                                     });
                layoutTitle = scope.createLabel("gameUiEditor.layoutTitle", "", barDesc({0.0f, 26.0f}));
                layoutTitle.setLocalizedText("editor-gameUiEditor.layout");
                layoutTitle.setParent(properties);
                layoutTokens = scope.createLabel("gameUiEditor.layoutTokens", "", barDesc({0.0f, 26.0f}));
                layoutTokens.setLocalizedText("editor-gameUiEditor.layoutTokens");
                layoutTokens.setParent(properties);
                buildLayoutForm(properties);

                UiRectDesc canvasDesc = topLeft();
                canvasDesc.layoutItem = fillItem();
                viewport = scope.createScrollView("gameUiEditor.viewport", canvasDesc, MIN_ARTBOARD);
                viewport.setAccessibleNameKey("editor-gameUiEditor.canvas");
                viewport.setParent(body);
                inspectorHost.setParent(body);
                canvas = scope.createPanel("gameUiEditor.canvas", topLeft({}, MIN_ARTBOARD));
                canvas.setParent(viewport.content());
                canvas.setAccessibleNameKey("editor-gameUiEditor.canvas");
                scope.world().getComponent<UiStateComponent>(canvas.entity()).clipsChildren = true;
                outline = scope.createPanel("gameUiEditor.selection", topLeft());
                outline.setStyleClass("uidocs.highlight");
                outline.setInteractable(false);
                outline.setParent(canvas);
                outline.setZOffset(1);
                outline.setVisible(false);
                handle = scope.createButton("gameUiEditor.resize", "", topLeft({}, {18.0f, 18.0f}));
                handle.setLocalizedText("editor-gameUiEditor.handle");
                handle.setAccessibleNameKey("editor-gameUiEditor.resize");
                handle.setTooltip(InternedString("editor-gameUiEditor.resize"));
                handle.setParent(canvas);
                handle.setZOffset(2);
                handle.setVisible(false);

                canvas.onPress(
                    [this](const u8 pointer, const Vec2 point)
                    {
                        if (pointer != 0)
                        {
                            return;
                        }
                        auto& hierarchy = scope.hierarchy();
                        Entity hit = Entity::undefined();
                        for (const Entity entity : nodes)
                        {
                            if (hierarchy.isVisibleOf(entity) && hierarchy.absoluteRectOf(entity).contains(point))
                            {
                                hit = entity;
                            }
                        }
                        const bool choosingDestination = moveArmed;
                        chooseWidget(hit);
                        dragPoint = point;
                        dragOffset = {};
                        panning = !choosingDestination && !hit.isValid();
                        dragSelection = !choosingDestination && hit.isValid();
                    });
                scope.setReaction(canvas.entity(), UiEventKind::DragStart,
                                  [this](const void* event)
                                  {
                                      const auto& drag = *static_cast<const UiDragStartEvent*>(event);
                                      dragCanvas(drag.delta);
                                  });
                canvas.onDrag([this](const Vec2 delta) { dragCanvas(delta); });
                canvas.onDragEnd(
                    [this]
                    {
                        if (dragSelection)
                        {
                            queueDrop();
                        }
                        panning = false;
                        dragSelection = false;
                    });
                handle.onPress(
                    [this](const u8 pointer, Vec2)
                    {
                        const Vec2 size = editableSize();
                        if (pointer == 0 && (size.x >= 0.0f || size.y >= 0.0f))
                        {
                            dragSize = size;
                            resizeCheckpoint = true;
                        }
                    });
                const auto dragResize = [this](const Vec2 delta)
                {
                    if (dragSize)
                    {
                        *dragSize += delta / scope.hierarchy().layoutScale();
                        const Vec2 allowed = editableSize();
                        pendingSize = Vec2{allowed.x < 0.0f ? -1.0f : std::max(1.0f, std::round(dragSize->x)),
                                           allowed.y < 0.0f ? -1.0f : std::max(1.0f, std::round(dragSize->y))};
                    }
                };
                scope.setReaction(handle.entity(), UiEventKind::DragStart, [dragResize](const void* event)
                                  { dragResize(static_cast<const UiDragStartEvent*>(event)->delta); });
                handle.onDrag(dragResize);
                handle.onDragEnd([this] { dragSize.reset(); });
                scope.commit();
                registerCommands();
                syncControls();
            }

            ~GameUiEditor()
            {
                clearDrafts();
                engine.control().unregisterOwner(PANEL_NAME);
            }

            Widget content() const { return root; }

            void update()
            {
                tree.advance(static_cast<float>(engine.lastFrameSeconds()));
                if (!pendingEdits.empty())
                {
                    std::vector<std::function<void()>> edits;
                    edits.swap(pendingEdits);
                    for (auto& edit : edits)
                    {
                        edit();
                    }
                }
                if (queued)
                {
                    auto action = std::move(queued);
                    action();
                }
                if (pendingSize)
                {
                    if (editableSize() != *pendingSize && resize(*pendingSize, resizeCheckpoint).empty())
                    {
                        resizeCheckpoint = false;
                    }
                    pendingSize.reset();
                }
                if (pendingMove)
                {
                    reorder(pendingMove);
                    pendingMove = 0;
                }
                if (pendingOffset)
                {
                    offsetWidget(*pendingOffset);
                    pendingOffset.reset();
                }
                if (shown(scope, root))
                {
                    tree.advance(static_cast<float>(engine.lastFrameSeconds()));
                    inspectorPage.fit(density.density());
                    const Vec2 room = resolvedSize(scope, viewport);
                    const Vec2 artboard = composition
                        ? designSize
                        : Vec2{std::max(MIN_ARTBOARD.x, room.x), std::max(MIN_ARTBOARD.y, room.y)};
                    if (viewport.contentSize() != artboard)
                    {
                        viewport.setContentSize(artboard);
                        canvas.setSize(artboard);
                    }
                    if (surface.panel.isValid() && surface.panel.size() != artboard)
                    {
                        surface.panel.setSize(artboard);
                    }
                    for (const Draft& document : drafts)
                    {
                        UiDocumentView& view = *document.view;
                        if (document.fillWidth)
                        {
                            Widget widget(&view.scope(), view.root(), view.scope().generation());
                            const auto& hierarchy = scope.hierarchy();
                            const float width = document.mounted
                                ? hierarchy.absoluteRectOf(hierarchy.parentOf(view.root())).size.x /
                                    hierarchy.layoutScale()
                                : artboard.x;
                            if (width > 0.0f && widget.size().x != width)
                            {
                                widget.setSize({width, widget.size().y});
                            }
                        }
                    }
                    revealCanvasSelection();
                    syncOutline();
                }
            }

        private:
            void buildLayoutForm(const Widget& parent)
            {
                const auto field = [&](const char* id, const UiFormKind kind, const UiFormValue value,
                                       const double min = -1.0e7, const double max = 1.0e7, const double step = 1.0)
                {
                    UiFormField desc;
                    desc.id = InternedString(id);
                    desc.labelKey = InternedString(std::string("editor-gameUiEditor.field.") + id);
                    desc.kind = kind;
                    desc.defaultValue = value;
                    desc.min = min;
                    desc.max = max;
                    desc.step = step;
                    layoutFields.push_back(std::move(desc));
                };
                const auto choice = [&](const char* id, const double value, std::initializer_list<const char*> names)
                {
                    field(id, UiFormKind::Enum, UiFormValue::ofNumber(value));
                    for (const char* name : names)
                    {
                        layoutFields.back().choices.push_back(
                            {.id = InternedString(name),
                             .key = InternedString(std::string("editor-gameUiEditor.choice.") + name)});
                    }
                };
                field("anchor", UiFormKind::Vec2, {}, -1.0e7, 1.0e7, 0.01);
                field("pivot", UiFormKind::Vec2, UiFormValue::ofVector(0.5, 0.5), -1.0e7, 1.0e7, 0.01);
                field("offset", UiFormKind::Vec2, {});
                field("size", UiFormKind::Vec2, {}, 0.0);
                field("fit.width", UiFormKind::Bool, {});
                field("fit.height", UiFormKind::Bool, {});
                field("fit.padding", UiFormKind::Vec2, {}, 0.0);
                choice("layout.direction", 0, {"free", "row", "column"});
                field("layout.gap", UiFormKind::Number, {}, 0.0);
                field("layout.padding", UiFormKind::Vec4, {}, 0.0);
                choice("layout.align", 1, {"auto", "start", "center", "end", "stretch"});
                choice("layout.justify", 0, {"start", "center", "end", "spaceBetween"});
                field("layout.wrap", UiFormKind::Bool, {});
                field("item.grow", UiFormKind::Number, {}, 0.0, 1.0e7, 0.1);
                field("item.shrink", UiFormKind::Number, UiFormValue::ofNumber(1.0), 0.0, 1.0e7, 0.1);
                field("item.basis", UiFormKind::Number, UiFormValue::ofNumber(-1.0), -1.0);
                field("item.min", UiFormKind::Vec2, {}, 0.0);
                field("item.max", UiFormKind::Vec2, {}, 0.0);
                choice("item.alignSelf", 0, {"auto", "start", "center", "end", "stretch"});

                UiFormDesc desc;
                desc.rect = barDesc({});
                desc.fields = layoutFields;
                desc.rowHeight = density.density().control;
                desc.controlGrow = 4.0f;
                layoutForm = std::make_unique<PropertyForm>(scope, "gameUiEditor.layout", std::move(desc));
                layoutForm->root().setParent(parent);
                layoutForm->onEdit(
                    [this](const UiFormEdit& edit)
                    {
                        const Entity target = selected;
                        queued = [this, edit, target]
                        {
                            if (!std::ranges::contains(nodes, target))
                            {
                                return;
                            }
                            const Entity nextSelection = selected;
                            if (target != selected)
                            {
                                select(target);
                            }
                            const auto error = editLayout(edit.field, edit.value);
                            layoutForm->setError(edit.field, error);
                            if (nextSelection != target)
                            {
                                select(nextSelection);
                            }
                        };
                    });
            }

            static std::pair<std::string, std::string> splitField(const InternedString field)
            {
                const std::string_view name = field.toView();
                const sizet dot = name.find('.');
                return dot == std::string_view::npos
                    ? std::pair{std::string{}, std::string(name)}
                    : std::pair{std::string(name.substr(0, dot)), std::string(name.substr(dot + 1))};
            }

            const Json* authoredField(const Json& node, const InternedString field) const
            {
                const auto [group, key] = splitField(field);
                const Json* owner = &node;
                if (!group.empty())
                {
                    const auto found = node.find(group);
                    if (found == node.end())
                    {
                        return nullptr;
                    }
                    owner = &*found;
                }
                const auto found = owner->find(key);
                return found == owner->end() ? nullptr : &*found;
            }

            static u32 vectorSize(const UiFormKind kind)
            {
                return kind == UiFormKind::Vec4 ? 4u : kind == UiFormKind::Vec2 ? 2u : 0u;
            }

            static std::optional<UiFormValue> formValue(const UiFormField& field, const Json& json)
            {
                if (field.kind == UiFormKind::Bool)
                {
                    return json.is_boolean() ? std::optional(UiFormValue::ofBool(json.get<bool>())) : std::nullopt;
                }
                if (field.kind == UiFormKind::Enum)
                {
                    const auto at = std::ranges::find_if(
                        field.choices, [&](const UiChoiceItem& choice)
                        { return json.is_string() && json.get_ref<const std::string&>() == choice.id.toView(); });
                    return at == field.choices.end()
                        ? std::nullopt
                        : std::optional(UiFormValue::ofNumber(static_cast<double>(at - field.choices.begin())));
                }
                if (const u32 count = vectorSize(field.kind))
                {
                    if (!json.is_array() || json.size() != count)
                    {
                        return std::nullopt;
                    }
                    UiFormValue value;
                    for (u32 i = 0; i < count; ++i)
                    {
                        if (!json[i].is_number())
                        {
                            return std::nullopt;
                        }
                        value.number[i] = json[i].get<double>();
                    }
                    return value;
                }
                return json.is_number() ? std::optional(UiFormValue::ofNumber(json.get<double>())) : std::nullopt;
            }

            bool fieldApplies(const Json& node, const InternedString field) const
            {
                const auto [group, key] = splitField(field);
                if (group == "layout")
                {
                    return documentKind(node) == "panel" && (key == "direction" || node.contains("layout"));
                }
                if (group == "item")
                {
                    return !containerDirection().empty();
                }
                if (group == "fit" && key == "padding")
                {
                    return node.contains("fit");
                }
                return true;
            }

            void refreshLayoutForm(const Json* node)
            {
                layoutTitle.setVisible(node != nullptr);
                layoutForm->root().setVisible(node != nullptr);
                bool usesTokens = false;
                for (const auto& field : layoutFields)
                {
                    const bool applies = node && fieldApplies(*node, field.id);
                    layoutForm->row(field.id).setVisible(applies);
                    if (!applies)
                    {
                        continue;
                    }
                    auto value = field.defaultValue;
                    const Json* authored = authoredField(*node, field.id);
                    bool token = false;
                    if (authored)
                    {
                        if (const auto parsed = formValue(field, *authored))
                        {
                            value = *parsed;
                        }
                        else
                        {
                            token = true;
                        }
                    }
                    else if (field.id == InternedString("layout.direction") && node->contains("layout"))
                    {
                        value = UiFormValue::ofNumber(1);
                    }
                    else if (field.kind == UiFormKind::Bool && field.id.toView().starts_with("fit.") &&
                             node->contains("fit"))
                    {
                        value = UiFormValue::ofBool(true);
                    }
                    layoutForm->setReadOnly(field.id, token);
                    layoutForm->setValue(field.id, value);
                    layoutForm->setError(field.id, token ? authored->dump() : std::string{});
                    usesTokens |= token;
                }
                layoutTokens.setVisible(usesTokens);
            }

            std::string editLayout(const InternedString id, const UiFormValue& value)
            {
                const auto field = std::ranges::find(layoutFields, id, &UiFormField::id);
                Json next = active ? active->model : Json{};
                Json* node = active ? nodeAt(next["root"], selectedPath()) : nullptr;
                if (!node || field == layoutFields.end() || !fieldApplies(*node, id))
                {
                    return "Select a widget with that layout property.";
                }
                if (const Json* old = authoredField(*node, id); old && !formValue(*field, *old))
                {
                    return "Edit the source token to change this property.";
                }
                Json authored;
                if (field->kind == UiFormKind::Enum)
                {
                    const double number = value.number[0];
                    if (!std::isfinite(number) || number < 0 || number >= field->choices.size() ||
                        std::floor(number) != number)
                    {
                        return "Select one of the available values.";
                    }
                    authored = field->choices[static_cast<sizet>(number)].id.toView();
                }
                else if (field->kind == UiFormKind::Bool)
                {
                    authored = value.number[0] != 0.0;
                }
                else
                {
                    const u32 count = vectorSize(field->kind);
                    for (u32 i = 0; i < std::max(count, 1u); ++i)
                    {
                        if (!std::isfinite(value.number[i]) || value.number[i] < field->min ||
                            value.number[i] > field->max)
                        {
                            return "Enter a value in the field's supported range.";
                        }
                    }
                    if (count)
                    {
                        authored = Json::array();
                        for (u32 i = 0; i < count; ++i)
                        {
                            authored.push_back(value.number[i]);
                        }
                    }
                    else
                    {
                        authored = value.number[0];
                    }
                }
                const auto [group, key] = splitField(id);
                if (group == "layout" && key == "direction" && authored == "free")
                {
                    node->erase("layout");
                }
                else if (group.empty())
                {
                    if (key == "size" && active->fillWidth && selected == draft()->root())
                    {
                        const auto previous = node->value("size", std::array<float, 2>{});
                        authored[0] = previous[0];
                    }
                    (*node)[key] = std::move(authored);
                }
                else
                {
                    if (group == "fit" && !node->contains(group))
                    {
                        (*node)[group] = {{"width", false}, {"height", false}};
                    }
                    (*node)[group][key] = std::move(authored);
                }
                return apply(std::move(next));
            }

            void queueWidgetEdit(std::function<void()> action)
            {
                const Entity target = selected;
                pendingEdits.push_back(
                    [this, target, action = std::move(action)]
                    {
                        if (!std::ranges::contains(nodes, target))
                        {
                            return;
                        }
                        const Entity previous = selected;
                        if (previous != target)
                        {
                            select(target);
                        }
                        action();
                        if (previous != target)
                        {
                            select(std::ranges::contains(nodes, previous) ? previous : Entity::undefined());
                        }
                    });
            }

            void clearDrafts()
            {
                if (draftStyles)
                {
                    editorUi(engine).uiView().setSubtreeStylesheet(Entity::undefined(), nullptr);
                    draftStyles.reset();
                }
                for (auto& document : drafts)
                {
                    scope.hierarchy().setHitTransparent(document.view->root(), false);
                }
                history.clear();
                moveArmed = false;
                active = nullptr;
                selected = Entity::undefined();
                nodes.clear();
                drafts.clear();
                surface = {};
            }

            Surface makeSurface()
            {
                Surface next;
                next.scope = std::make_unique<UiScope>(editorUi(engine).ui());
                next.panel = next.scope->createPanel("gameUiEditor.artboard", topLeft({}, viewport.contentSize()));
                next.panel.setParent(canvas);
                next.panel.setInteractable(false);
                next.scope->commit();
                return next;
            }

            UiDocumentView* draft() const { return active ? active->view.get() : nullptr; }

            bool dirty() const
            {
                return styleText != savedStyle ||
                    std::ranges::any_of(drafts, [](const Draft& document)
                                        { return document.view->documentText() != document.saved; });
            }

            void reportOpen(const std::string& error)
            {
                if (!error.empty())
                {
                    status.setText(error);
                }
            }

            void dragCanvas(const Vec2 delta)
            {
                if (panning)
                {
                    viewport.scrollBy(-delta / scope.hierarchy().layoutScale());
                }
                else
                {
                    dragPoint += delta;
                    dragOffset += delta;
                }
            }

            void chooseFile()
            {
                std::string title = "Open game UI";
                if (engine.services().contains<Localization>())
                {
                    std::string scratch;
                    title = engine.services().get<Localization>().resolve("editor-gameUiEditor.openTitle", {}, scratch);
                }
                dialogs.pickFile({.title = std::move(title), .roots = {"ui"}, .root = "ui", .extensions = "json"},
                                 [this](std::optional<EditorPicked> file)
                                 {
                                     if (file)
                                     {
                                         const auto error = openFile(file->path);
                                         if (!error.empty())
                                         {
                                             status.setText(error);
                                         }
                                     }
                                 });
            }

            void chooseSubtreeFile(const bool exporting)
            {
                if (!active || !selected.isValid())
                {
                    return;
                }
                const Entity target = selected;
                const std::string original = active->view->documentText();
                std::string caption = exporting ? "Export subtree" : "Import subtree";
                if (engine.services().contains<Localization>())
                {
                    std::string scratch;
                    const auto key = exporting ? "editor-gameUiEditor.exportTitle" : "editor-gameUiEditor.importTitle";
                    caption = engine.services().get<Localization>().resolve(InternedString(key), {}, scratch);
                }
                dialogs.pickFile({.mode = exporting ? EditorPickMode::SaveFile : EditorPickMode::OpenFile,
                                  .title = std::move(caption),
                                  .roots = {"ui"},
                                  .root = "ui",
                                  .name = exporting ? "subtree.json" : "",
                                  .extensions = "json"},
                                 [this, target, original, exporting](std::optional<EditorPicked> file)
                                 {
                                     if (!file)
                                     {
                                         return;
                                     }
                                     if (selected != target || !active || active->view->documentText() != original)
                                     {
                                         status.setLocalizedText("editor-gameUiEditor.selectionChanged");
                                         return;
                                     }
                                     reportOpen(exporting ? exportSubtree(file->path, true)
                                                          : importSubtree(file->path));
                                 });
            }

            std::string subtreeFile(const std::string& name, std::filesystem::path& file) const
            {
                if (auto error = engine.control().resolveFile("ui", name, file); !error.empty())
                {
                    return error;
                }
                return file.extension() == ".json" && !name.ends_with(".composition.json")
                    ? std::string{}
                    : "Choose a .json UI document.";
            }

            std::string exportSubtree(const std::string& name, const bool overwrite)
            {
                const Json* node = active ? nodeAt(active->model.at("root"), selectedPath()) : nullptr;
                if (!node)
                {
                    return "Select a document widget to export.";
                }
                std::filesystem::path file;
                if (auto error = subtreeFile(name, file); !error.empty())
                {
                    return error;
                }
                const auto same = [&](const std::filesystem::path& other)
                {
                    std::error_code error;
                    return !other.empty() && std::filesystem::equivalent(file, other, error);
                };
                if (same(stylePath) || same(compositionPath) ||
                    std::ranges::any_of(drafts, [&](const Draft& document) { return same(document.path); }))
                {
                    return "Export to a separate file. Use Save to update the open documents.";
                }
                std::error_code existsError;
                if (std::filesystem::exists(file, existsError) && !overwrite)
                {
                    return "The export file exists. Confirm replacement to overwrite it.";
                }
                const std::string text = Json{{"version", UI_DOCUMENT_VERSION}, {"root", *node}}.dump(2) + "\n";
                if (existsError || !ensureDirectory(file.parent_path()) || !writeTextFileAtomically(file, text))
                {
                    return "Could not write the exported UI document.";
                }
                status.setLocalizedText("editor-gameUiEditor.exported");
                return {};
            }

            static std::unordered_map<std::string, u32> idCounts(const Json& root)
            {
                std::unordered_map<std::string, u32> counts;
                for (std::vector<const Json*> stack{&root}; !stack.empty();)
                {
                    const Json* node = stack.back();
                    stack.pop_back();
                    ++counts[node->value("id", std::string{})];
                    if (const auto children = node->find("children"); children != node->end())
                    {
                        for (const Json& child : *children)
                        {
                            stack.push_back(&child);
                        }
                    }
                }
                return counts;
            }

            static std::string_view documentKind(const Json& node)
            {
                const auto kind = node.find("kind");
                return kind == node.end() ? std::string_view("panel")
                                          : std::string_view(kind->get_ref<const std::string&>());
            }

            std::string changedBinding(const Json& before, const Json* after) const
            {
                const auto id = before.value("id", std::string{});
                const bool slot = before.contains("slot") || std::ranges::contains(active->mountSlots, id);
                if (slot || before.contains("bind") || std::ranges::contains(active->protectedIds, id))
                {
                    if (!after || documentKind(before) != documentKind(*after) ||
                        before.value("bind", Json{}) != after->value("bind", Json{}) ||
                        (slot &&
                         (before.value("slot", Json{}) != after->value("slot", Json{}) ||
                          !after->value("children", Json::array()).empty())))
                    {
                        return id;
                    }
                }
                if (const auto children = before.find("children"); children != before.end())
                {
                    for (const Json& child : *children)
                    {
                        const auto index = after ? childIndex(*after, child.value("id", std::string{})) : std::nullopt;
                        if (auto changed = changedBinding(child, index ? &(*after)["children"][*index] : nullptr);
                            !changed.empty())
                        {
                            return changed;
                        }
                    }
                }
                return {};
            }

            std::string importSubtree(const std::string& name)
            {
                const auto path = selectedPath();
                Json next = active ? active->model : Json{};
                Json* target = active ? nodeAt(next["root"], path) : nullptr;
                if (!target || !active->knowsBindings)
                {
                    return "Select a widget in a composition or a live document with a known contract.";
                }
                std::filesystem::path file;
                if (auto error = subtreeFile(name, file); !error.empty())
                {
                    return error;
                }
                std::string text, why;
                if (!readUiDocumentFile(file, text, why))
                {
                    return why;
                }
                {
                    auto scratch = makeSurface();
                    scratch.panel.setVisible(false);
                    UiDocumentView check(editorUi(engine).ui(), "__gameUiSubtreeImport", {}, scratch.panel.entity());
                    if (!check.apply(text))
                    {
                        const auto problem = std::ranges::find_if(check.problems(), &UiDocumentProblem::error);
                        return problem == check.problems().end() ? "Invalid SPITE UI document." : problem->message;
                    }
                }
                Json imported = Json::parse(text);
                const Json& subtree = imported.at("root");
                if (target->at("id") != subtree.at("id") || documentKind(*target) != documentKind(subtree))
                {
                    return "The imported root must have the selected widget's ID and kind.";
                }
                if (const auto id = changedBinding(*target, &subtree); !id.empty())
                {
                    return std::format("Import would change the protected widget or mount '{}'.", id);
                }
                const auto before = idCounts(next["root"]);
                *target = subtree;
                for (const auto& [id, count] : idCounts(next["root"]))
                {
                    const auto old = before.find(id);
                    if (count > 1 && (old == before.end() || count > old->second))
                    {
                        return std::format("Import would add a duplicate widget ID '{}'.", id);
                    }
                }
                if (const auto error = applyAndSelect(std::move(next), path.back()); !error.empty())
                {
                    return error;
                }
                status.setLocalizedText("editor-gameUiEditor.imported");
                return {};
            }

            Button button(const Widget& parent, InternedString id, InternedString label, std::function<void()> fn)
            {
                Button result = density.track(scope.createButton(id, "", fitWidthDesc()), HEIGHT_CONTROL);
                result.setLocalizedText(label);
                bindFitPadding(result, SPACE_MD, SPACE_NONE);
                result.setParent(parent);
                result.onClick([this, fn = std::move(fn)] { queued = fn; });
                return result;
            }

            std::string_view idOf(const Entity entity) const
            {
                const auto* identity = scope.world().isEntityAlive(entity)
                    ? scope.world().tryGetConstComponent<UiIdentityComponent>(entity)
                    : nullptr;
                return identity ? identity->id.toView() : std::string_view{};
            }

            Draft* documentOf(const Entity entity)
            {
                for (Entity parent = entity; parent.isValid(); parent = scope.hierarchy().parentOf(parent))
                {
                    const auto owner = std::ranges::find_if(drafts, [parent](const Draft& document)
                                                            { return document.view->root() == parent; });
                    if (owner != drafts.end())
                    {
                        return &*owner;
                    }
                }
                return nullptr;
            }

            std::vector<std::string> pathOf(const Entity widget) const
            {
                std::vector<std::string> pathIds;
                if (draft() && widget.isValid() && scope.hierarchy().isElement(widget))
                {
                    for (Entity entity = widget; entity.isValid(); entity = scope.hierarchy().parentOf(entity))
                    {
                        pathIds.emplace_back(idOf(entity));
                        if (entity == draft()->root())
                        {
                            std::ranges::reverse(pathIds);
                            return pathIds;
                        }
                    }
                }
                return {};
            }

            std::vector<std::string> selectedPath() const { return pathOf(selected); }

            static std::optional<sizet> childIndex(const Json& node, const std::string_view id)
            {
                const auto children = node.find("children");
                if (children == node.end() || !children->is_array())
                {
                    return std::nullopt;
                }
                std::optional<sizet> index;
                for (sizet i = 0; i < children->size(); ++i)
                {
                    if ((*children)[i].value("id", std::string{}) == id)
                    {
                        if (index)
                        {
                            return std::nullopt;
                        }
                        index = i;
                    }
                }
                return index;
            }

            template <typename Node>
            static Node* nodeAt(Node& rootNode, std::span<const std::string> pathIds)
            {
                Node* node = &rootNode;
                for (sizet level = 1; level < pathIds.size(); ++level)
                {
                    const auto index = childIndex(*node, pathIds[level]);
                    if (!index)
                    {
                        return nullptr;
                    }
                    node = &(*node)["children"][*index];
                }
                return pathIds.empty() ? nullptr : node;
            }

            Vec2 editableSize() const
            {
                if (!draft() || !selected.isValid())
                {
                    return {-1.0f, -1.0f};
                }
                const Json* node = nodeAt(active->model.at("root"), selectedPath());
                if (!node || !node->contains("size") || !(*node)["size"].is_array() || (*node)["size"].size() != 2)
                {
                    return {-1.0f, -1.0f};
                }
                const auto fit = node->value("fit", Json::object());
                const auto& size = (*node)["size"];
                const bool fillsParent = active->fillWidth && selected == draft()->root();
                return {size[0].is_number() && !fit.value("width", false) && !fillsParent ? size[0].get<float>()
                                                                                          : -1.0f,
                        size[1].is_number() && !fit.value("height", false) ? size[1].get<float>() : -1.0f};
            }

            void select(const Entity entity)
            {
                const bool changed = selected != entity;
                revealPending |= changed;
                selected = entity;
                active = documentOf(entity);
                if (active && composition && !active->previewVisible)
                {
                    showHidden = true;
                }
                const auto found = std::ranges::find(nodes, entity);
                refreshingRows = true;
                if (found == nodes.end())
                {
                    tree.setSelection({});
                }
                else
                {
                    for (Entity parent = scope.hierarchy().parentOf(entity); std::ranges::contains(nodes, parent);
                         parent = scope.hierarchy().parentOf(parent))
                    {
                        if (!tree.isExpanded(parent))
                        {
                            tree.setExpanded(parent, true);
                        }
                    }
                    tree.setSelection(std::span(&entity, 1));
                    if (changed)
                    {
                        tree.scrollTo(entity);
                    }
                }
                refreshingRows = false;
                syncControls();
            }

            Entity uniqueWidget(const std::string_view id) const
            {
                Entity found = Entity::undefined();
                for (Entity entity : nodes)
                {
                    if (idOf(entity) == id)
                    {
                        if (found.isValid())
                        {
                            return Entity::undefined();
                        }
                        found = entity;
                    }
                }
                return found;
            }

            void chooseWidget(const Entity entity)
            {
                if (moveArmed && entity.isValid())
                {
                    queued = [this, entity]
                    {
                        moveArmed = false;
                        reportOpen(moveWidget(entity));
                        syncControls();
                    };
                    return;
                }
                if (!moveArmed)
                {
                    select(entity);
                }
            }

            void rebuildRows()
            {
                nodes.clear();
                const auto previousRoots = std::move(treeRoots);
                treeRoots.clear();
                treeChildren.clear();
                const auto append = [&](const auto& self, const Entity entity) -> void
                {
                    if (idOf(entity).empty())
                    {
                        return;
                    }
                    nodes.push_back(entity);
                    for (const Entity child : scope.hierarchy().childrenOf(entity))
                    {
                        if (!idOf(child).empty())
                        {
                            treeChildren[entity].push_back(child);
                            self(self, child);
                        }
                    }
                };
                for (const Draft& document : drafts)
                {
                    if (!document.mounted)
                    {
                        treeRoots.push_back(document.view->root());
                        append(append, document.view->root());
                    }
                }
                refreshingRows = true;
                tree.refresh();
                for (Entity node : treeRoots)
                {
                    if (!std::ranges::contains(previousRoots, node))
                    {
                        tree.setExpanded(node, true);
                    }
                }
                refreshingRows = false;
                select(std::ranges::contains(nodes, selected) ? selected : Entity::undefined());
            }

            void syncControls()
            {
                const Vec2 size = editableSize();
                canResize = size.x >= 0.0f || size.y >= 0.0f;
                saveButton.setInteractable(dirty());
                visibilityButton.setVisible(
                    composition &&
                    std::ranges::any_of(drafts, [](const Draft& document) { return !document.previewVisible; }));
                if (composition)
                {
                    visibilityButton.setLocalizedText(showHidden ? "editor-gameUiEditor.hideHidden"
                                                                 : "editor-gameUiEditor.showHidden");
                    for (const Draft& document : drafts)
                    {
                        if (!document.previewVisible)
                        {
                            UiDocumentView& view = *document.view;
                            Widget panel(&view.scope(), view.root(), view.scope().generation());
                            panel.setVisible(showHidden);
                        }
                    }
                }
                undoButton.setInteractable(!history.empty());
                const bool child = draft() && selected.isValid() && selected != draft()->root();
                const bool reorderable = child && !containerDirection().empty();
                earlier.setInteractable(reorderable);
                later.setInteractable(reorderable);
                const std::string width = size.x >= 0.0f ? std::format("{:.0f}", size.x) : "auto";
                const std::string height = size.y >= 0.0f ? std::format("{:.0f}", size.y) : "auto";
                inspector.setText(canResize ? std::format("{}  ·  {} × {}", idOf(selected), width, height)
                                            : idOf(selected));
                Json* node = active ? nodeAt(active->model["root"], selectedPath()) : nullptr;
                importButton.setInteractable(node && active->knowsBindings);
                exportButton.setInteractable(node != nullptr);
                refreshLayoutForm(node);
                const bool canAdd = acceptsChildren(node);
                addPanelButton.setInteractable(canAdd);
                addLabelButton.setInteractable(canAdd);
                addButtonButton.setInteractable(canAdd);
                duplicateButton.setInteractable(child && node);
                deleteButton.setInteractable(child && node);
                moveButton.setInteractable(child && node);
                moveButton.setLocalizedText(moveArmed ? "editor-gameUiEditor.cancelMove" : "editor-gameUiEditor.move");
                const std::string styleClass = node ? node->value("class", std::string{}) : std::string{};
                widgetClass.setVisible(!styleClass.empty());
                if (!styleClass.empty())
                {
                    widgetClass.setLocalizedArg("name", std::string_view(styleClass));
                }
                const bool literal =
                    node && node->contains("text") && (*node)["text"].is_string() && !node->contains("textKey");
                textInput.setVisible(literal);
                if (literal)
                {
                    textInput.setText((*node)["text"].get<std::string>());
                }
                syncAppearance();
                syncOutline();
            }

            void syncOutline()
            {
                auto& hierarchy = scope.hierarchy();
                const bool visible = selected.isValid() && hierarchy.isVisibleOf(selected);
                outline.setVisible(visible);
                handle.setVisible(visible && canResize);
                bounds.setVisible(visible);
                if (visible)
                {
                    const UiRect rect = hierarchy.absoluteRectOf(selected);
                    const UiRect origin = hierarchy.absoluteRectOf(canvas.entity());
                    const float scale = hierarchy.layoutScale();
                    const Vec2 offset = (rect.position - origin.position) / scale;
                    const Vec2 size = rect.size / scale;
                    if (offset != boundsOffset || size != boundsSize)
                    {
                        boundsOffset = offset;
                        boundsSize = size;
                        bounds.setLocalizedText("editor-gameUiEditor.bounds");
                        bounds.setLocalizedArg("x", static_cast<std::int64_t>(std::lround(offset.x)));
                        bounds.setLocalizedArg("y", static_cast<std::int64_t>(std::lround(offset.y)));
                        bounds.setLocalizedArg("width", static_cast<std::int64_t>(std::lround(size.x)));
                        bounds.setLocalizedArg("height", static_cast<std::int64_t>(std::lround(size.y)));
                    }
                    place(outline, offset, size);
                    handle.setOffset(offset + Vec2{size.x - 9.0f, size.y - 3.0f});
                }
            }

            void revealCanvasSelection()
            {
                if (!revealPending)
                {
                    return;
                }
                if (!selected.isValid())
                {
                    revealPending = false;
                    return;
                }
                const auto& hierarchy = scope.hierarchy();
                if (!hierarchy.isVisibleOf(selected))
                {
                    return;
                }
                const UiRect target = hierarchy.absoluteRectOf(selected);
                const UiRect area = hierarchy.absoluteRectOf(viewport.entity());
                if (target.size.x <= 0.0f || target.size.y <= 0.0f || area.size.x <= 0.0f || area.size.y <= 0.0f)
                {
                    return;
                }
                const auto delta = [](const float at, const float size, const float start, const float room)
                {
                    if (at < start)
                    {
                        return at - start;
                    }
                    return std::max(0.0f, at + std::min(size, room) - (start + room));
                };
                const Vec2 move{delta(target.position.x, target.size.x, area.position.x, area.size.x),
                                delta(target.position.y, target.size.y, area.position.y, area.size.y)};
                if (move != Vec2{})
                {
                    viewport.scrollBy(move / hierarchy.layoutScale());
                }
                revealPending = false;
            }

            std::string fail(const char* key, const char* error)
            {
                status.setLocalizedText(InternedString(key));
                return error;
            }

            void rememberUndo(Draft* document, const std::string& text)
            {
                constexpr sizet LIMIT = 32;
                if (history.size() == LIMIT)
                {
                    history.erase(history.begin());
                }
                history.push_back({document, text});
            }

            void showDirty()
            {
                status.setLocalizedText(dirty() ? "editor-gameUiEditor.unsaved" : "editor-gameUiEditor.ready");
            }

            static Json rgba(const u32 value)
            {
                Json color = Json::array();
                for (u32 channel = 0; channel < 4; ++channel)
                {
                    color.push_back(static_cast<double>((value >> (channel * 8)) & 0xff) / 255.0);
                }
                return color;
            }

            void syncAppearance()
            {
                const std::string id = composition ? std::string(idOf(selected)) : std::string{};
                const bool styling = !id.empty() && !stylePath.empty();
                hoverLabel.setVisible(styling);
                hoverColor.setVisible(styling);
                hoverButton.setVisible(styling);
                hoverButton.setLocalizedText(previewHover ? "editor-gameUiEditor.previewNormal"
                                                          : "editor-gameUiEditor.previewHover");

                const auto action =
                    std::ranges::find_if(actions, [&](const UiCompositionAction& item)
                                         { return active && item.view == active->name && item.id == id; });
                actionLabel.setVisible(!id.empty());
                actionArgs.setVisible(action != actions.end());
                if (action != actions.end())
                {
                    const std::string& event = action->event;
                    actionLabel.setText(std::format("{} → {}", event == "click" ? "Click" : event, action->command));
                    std::string args;
                    const Json declaredArgs = Json::parse(action->args);
                    for (const auto& [name, value] : declaredArgs.items())
                    {
                        args += std::format("{}{}={}", args.empty() ? "" : "  ", name,
                                            value.is_string() ? value.get<std::string>() : value.dump());
                    }
                    actionArgs.setText(args);
                }
                else
                {
                    actionLabel.setLocalizedText("editor-gameUiEditor.noAction");
                }

                if (id == appearanceId && previewHover == appearanceHover && !appearanceChanged)
                {
                    return;
                }
                if (!draftStyles)
                {
                    return;
                }
                auto& sheet = *draftStyles;
                if (appearanceHover && !appearanceId.empty())
                {
                    sheet.unloadLayer(HOVER_LAYER);
                }
                appearanceId = id;
                appearanceHover = previewHover;
                appearanceChanged = false;
                if (!styling)
                {
                    return;
                }
                const auto& identity = scope.world().getConstComponent<UiIdentityComponent>(selected);
                const u32 color =
                    sheet.resolve(identity.id, identity.kind, UiInteractionState::Hover, identity.styleClass)
                        .backgroundColor;
                const u32 rgb = ((color & 0xff) << 16) | (color & 0xff00) | ((color >> 16) & 0xff);
                hoverColor.setText(std::format("#{:06X}{:02X}", rgb, color >> 24));
                if (previewHover)
                {
                    const Json overlay{{"elements", {{id, {{"background", {{"color", rgba(color)}}}}}}}};
                    (void)sheet.loadLayerText(HOVER_LAYER, overlay.dump());
                }
            }

            std::string editHover(std::string_view hex)
            {
                const std::string id(idOf(selected));
                if (stylePath.empty() || id.empty())
                {
                    return "select a widget on the composed HUD";
                }
                if (std::ranges::count_if(nodes, [&](Entity entity) { return idOf(entity) == id; }) != 1)
                {
                    return "widget id must be unique on the canvas to edit its stylesheet rule";
                }
                if (!hex.empty() && hex.front() == '#')
                {
                    hex.remove_prefix(1);
                }
                if (hex.size() != 6 && hex.size() != 8)
                {
                    return fail("editor-gameUiEditor.invalidColor", "use #RRGGBB or #RRGGBBAA");
                }
                u32 value = 0;
                const auto parsed = std::from_chars(hex.data(), hex.data() + hex.size(), value, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != hex.data() + hex.size())
                {
                    return fail("editor-gameUiEditor.invalidColor", "use #RRGGBB or #RRGGBBAA");
                }
                if (hex.size() == 6)
                {
                    value = (value << 8) | 0xff;
                }
                Json next = Json::parse(styleText);
                next["elements"][id]["states"]["hover"]["background"]["color"] =
                    rgba(((value >> 24) & 0xff) | ((value >> 8) & 0xff00) | ((value << 8) & 0xff0000) | (value << 24));
                const std::string text = next.dump(2) + "\n";
                auto& sheet = *draftStyles;
                if (auto loaded = sheet.loadLayerText(STYLE_LAYER, text); !loaded)
                {
                    return fail("editor-gameUiEditor.refused", "stylesheet refused the edit");
                }
                rememberUndo(nullptr, styleText);
                styleText = text;
                previewHover = true;
                appearanceChanged = true;
                showDirty();
                syncControls();
                return {};
            }

            std::string apply(Json next, const bool remember = true)
            {
                if (next == active->model)
                {
                    return {};
                }
                const std::string before = draft()->documentText();
                if (!draft()->apply(next.dump(2) + "\n"))
                {
                    return fail("editor-gameUiEditor.refused", "draft refused the edit");
                }
                if (remember)
                {
                    rememberUndo(active, before);
                }
                active->model = std::move(next);
                showDirty();
                rebuildRows();
                return {};
            }

            bool acceptsChildren(const Json* node) const
            {
                return node && active && documentKind(*node) == "panel" && !node->value("opaque", false) &&
                    !node->contains("slot") &&
                    !std::ranges::contains(active->mountSlots, node->value("id", std::string{}));
            }

            std::unordered_set<std::string> reservedIds() const
            {
                std::unordered_set<std::string> ids;
                for (Entity entity : nodes)
                {
                    ids.emplace(idOf(entity));
                }
                for (const auto& document : drafts)
                {
                    ids.insert(document.protectedIds.begin(), document.protectedIds.end());
                }
                return ids;
            }

            static std::string freshId(const std::string_view base, std::unordered_set<std::string>& ids)
            {
                const auto prefix = base.size() + 12 < UI_DOCUMENT_MAX_ID ? base : std::string_view("widget");
                for (u32 suffix = 1;; ++suffix)
                {
                    std::string id = std::format("{}.{}", prefix, suffix);
                    if (ids.insert(id).second)
                    {
                        return id;
                    }
                }
            }

            static bool duplicateIds(Json& node, std::unordered_set<std::string>& ids)
            {
                if (node.value("opaque", false))
                {
                    return false;
                }
                node["id"] = freshId(node.value("id", std::string("widget")), ids);
                node.erase("bind");
                node.erase("slot");
                if (node.contains("children"))
                {
                    for (auto& child : node["children"])
                    {
                        if (!duplicateIds(child, ids))
                        {
                            return false;
                        }
                    }
                }
                return true;
            }

            std::string applyAndSelect(Json next, const std::string& id)
            {
                const auto error = apply(std::move(next));
                if (error.empty())
                {
                    select(uniqueWidget(id));
                }
                return error;
            }

            std::string addWidget(const std::string& kind)
            {
                if (kind != "panel" && kind != "label" && kind != "button")
                {
                    return "kind must be panel, label or button";
                }
                Json next = active ? active->model : Json{};
                Json* parent = next.contains("root") ? nodeAt(next["root"], selectedPath()) : nullptr;
                if (!acceptsChildren(parent))
                {
                    return "select a panel to add a widget";
                }
                auto ids = reservedIds();
                const std::string id = freshId(kind, ids);
                Json child{{"id", id},
                           {"kind", kind},
                           {"pivot", {0, 0}},
                           {"size", kind == "panel" ? Json::array({140, 80}) : Json::array({100, 30})}};
                if (kind != "panel")
                {
                    child["text"] = kind == "label" ? "Label" : "Button";
                }
                if (!parent->contains("children"))
                {
                    (*parent)["children"] = Json::array();
                }
                (*parent)["children"].push_back(std::move(child));
                return applyAndSelect(std::move(next), id);
            }

            std::string duplicateWidget()
            {
                const auto path = selectedPath();
                Json next = active ? active->model : Json{};
                Json* parent = path.size() > 1 ? nodeAt(next["root"], std::span(path).first(path.size() - 1)) : nullptr;
                if (!acceptsChildren(parent))
                {
                    return "select a non-root widget to duplicate";
                }
                const auto index = childIndex(*parent, path.back());
                if (!index)
                {
                    return "selected widget id is ambiguous";
                }
                auto& children = (*parent)["children"];
                Json copy = children[*index];
                auto ids = reservedIds();
                if (!duplicateIds(copy, ids))
                {
                    return "this subtree contains a widget that cannot be duplicated";
                }
                const std::string id = copy["id"].get<std::string>();
                children.insert(children.begin() + *index + 1, std::move(copy));
                return applyAndSelect(std::move(next), id);
            }

            std::string protectedWidget(const Json& node) const
            {
                const std::string id = node.value("id", std::string{});
                if (node.contains("bind") || std::ranges::contains(active->protectedIds, id))
                {
                    return id;
                }
                if (const auto children = node.find("children"); children != node.end())
                {
                    for (const auto& child : *children)
                    {
                        if (auto binding = protectedWidget(child); !binding.empty())
                        {
                            return binding;
                        }
                    }
                }
                return {};
            }

            std::string checkStructuralEdit(const Json& node) const
            {
                if (!active->knowsBindings)
                {
                    return "Open the module composition to check bindings before deleting or reparenting.";
                }
                if (const auto id = protectedWidget(node); !id.empty())
                {
                    return std::format("'{}' is protected by a module requirement, binding, action, or mount.", id);
                }
                return {};
            }

            std::string deleteWidget()
            {
                const auto path = selectedPath();
                if (path.size() < 2)
                {
                    return "Select a widget below the document root to delete.";
                }
                Json next = active->model;
                Json* parent = nodeAt(next["root"], std::span(path).first(path.size() - 1));
                const auto index = parent ? childIndex(*parent, path.back()) : std::nullopt;
                if (!index)
                {
                    return "The selected widget ID is ambiguous.";
                }
                auto& children = (*parent)["children"];
                if (const auto error = checkStructuralEdit(children[*index]); !error.empty())
                {
                    return error;
                }
                children.erase(children.begin() + *index);
                return applyAndSelect(std::move(next), path[path.size() - 2]);
            }

            void armMove()
            {
                if (moveArmed)
                {
                    moveArmed = false;
                    showDirty();
                }
                else
                {
                    const auto path = selectedPath();
                    const Json* node = active ? nodeAt(active->model.at("root"), path) : nullptr;
                    if (!node || path.size() < 2)
                    {
                        return;
                    }
                    const auto error = checkStructuralEdit(*node);
                    if (!error.empty())
                    {
                        reportOpen(error);
                        return;
                    }
                    moveArmed = true;
                    status.setLocalizedText("editor-gameUiEditor.moveDestination");
                }
                syncControls();
            }

            std::string treeMove(const Entity source, const Entity destination, u32 index)
            {
                if (!std::ranges::contains(nodes, source))
                {
                    return "The dragged widget is no longer in the draft.";
                }
                moveArmed = false;
                select(source);
                const auto from = selectedPath();
                if (from.size() < 2 || !destination.isValid() || documentOf(destination) != active)
                    return "Choose a destination panel in the same document.";
                const auto to = pathOf(destination);
                if (to.size() >= from.size() && std::equal(from.begin(), from.end(), to.begin()))
                    return "A widget cannot move into itself or its own children.";
                Json next = active->model;
                Json* oldParent = nodeAt(next["root"], std::span(from).first(from.size() - 1));
                Json* target = nodeAt(next["root"], to);
                if (!oldParent || !acceptsChildren(target))
                    return "Choose a destination panel that can contain widgets.";
                const auto oldIndex = childIndex(*oldParent, from.back());
                if (!oldIndex)
                    return "The selected widget ID is ambiguous.";
                const bool sameParent = std::equal(to.begin(), to.end(), from.begin(), from.end() - 1);
                auto& oldChildren = (*oldParent)["children"];
                if (!sameParent)
                    if (const auto error = checkStructuralEdit(oldChildren[*oldIndex]); !error.empty())
                        return error;
                Json widget = std::move(oldChildren[*oldIndex]);
                oldChildren.erase(oldChildren.begin() + *oldIndex);
                target = nodeAt(next["root"], to);
                auto& children = (*target)["children"];
                if (children.is_null())
                    children = Json::array();
                index = std::min(index, static_cast<u32>(children.size()));
                children.insert(children.begin() + index, std::move(widget));
                return applyAndSelect(std::move(next), from.back());
            }

            std::string moveWidget(const Entity destination) { return treeMove(selected, destination, ~0u); }

            std::string containerDirection() const
            {
                const auto path = selectedPath();
                const Json* parent = path.size() > 1
                    ? nodeAt(active->model.at("root"), std::span(path).first(path.size() - 1))
                    : nullptr;
                if (!parent)
                {
                    return {};
                }
                const auto layout = parent->find("layout");
                return layout == parent->end() ? std::string{} : layout->value("direction", std::string("row"));
            }

            std::string offsetWidget(const Vec2 delta)
            {
                if (!active || (selected == draft()->root() && active->mounted) || !containerDirection().empty() ||
                    !std::isfinite(delta.x) || !std::isfinite(delta.y))
                {
                    return "Select an anchored widget to move on the canvas.";
                }
                Json next = active->model;
                Json* node = nodeAt(next["root"], selectedPath());
                if (!node)
                {
                    return "Select an unambiguous widget to move on the canvas.";
                }
                const auto offset = node->value("offset", std::array<float, 2>{});
                (*node)["offset"] = {offset[0] + delta.x, offset[1] + delta.y};
                return apply(std::move(next));
            }

            std::string resize(const Vec2 size, const bool remember = true)
            {
                const Vec2 allowed = editableSize();
                if ((size.x != -1.0f && (allowed.x < 0.0f || !std::isfinite(size.x) || size.x < 1.0f)) ||
                    (size.y != -1.0f && (allowed.y < 0.0f || !std::isfinite(size.y) || size.y < 1.0f)))
                {
                    return fail("editor-gameUiEditor.noSize", "select a widget with an editable size");
                }
                if (size == Vec2{-1.0f, -1.0f})
                {
                    return {};
                }
                Json next = active->model;
                auto& fields = (*nodeAt(next["root"], selectedPath()))["size"];
                if (size.x >= 0.0f)
                {
                    fields[0] = size.x;
                }
                if (size.y >= 0.0f)
                {
                    fields[1] = size.y;
                }
                return apply(std::move(next), remember);
            }

            std::string editText(std::string_view value)
            {
                Json next = active ? active->model : Json{};
                Json* node = next.contains("root") ? nodeAt(next["root"], selectedPath()) : nullptr;
                if (!node || !node->contains("text") || !(*node)["text"].is_string() || node->contains("textKey"))
                {
                    return fail("editor-gameUiEditor.noText", "select a widget with literal text");
                }
                (*node)["text"] = value;
                return apply(std::move(next));
            }

            std::string reorder(const int steps)
            {
                const auto pathIds = selectedPath();
                Json next = active ? active->model : Json{};
                Json* parent =
                    pathIds.size() > 1 ? nodeAt(next["root"], std::span(pathIds).first(pathIds.size() - 1)) : nullptr;
                const auto index = parent ? childIndex(*parent, pathIds.back()) : std::nullopt;
                if (!index || containerDirection().empty())
                {
                    return fail("editor-gameUiEditor.noContainer", "select a widget in a row or column");
                }
                auto& children = (*parent)["children"];
                const int from = static_cast<int>(*index);
                const int to = std::clamp(from + steps, 0, static_cast<int>(children.size()) - 1);
                if (from == to)
                {
                    return {};
                }
                if (to > from)
                {
                    std::rotate(children.begin() + from, children.begin() + from + 1, children.begin() + to + 1);
                }
                else
                {
                    std::rotate(children.begin() + to, children.begin() + from, children.begin() + from + 1);
                }
                return apply(std::move(next));
            }

            void queueDrop()
            {
                if (!draft() || !selected.isValid())
                {
                    return;
                }
                const std::string direction = containerDirection();
                if (direction.empty())
                {
                    if (selected != draft()->root() || !active->mounted)
                    {
                        pendingOffset = dragOffset / scope.hierarchy().layoutScale();
                    }
                    return;
                }
                const bool column = direction == "column";
                auto& hierarchy = scope.hierarchy();
                const auto siblings = hierarchy.childrenOf(hierarchy.parentOf(selected));
                int current = 0;
                int target = 0;
                bool passed = false;
                for (const Entity sibling : siblings)
                {
                    if (!std::ranges::contains(nodes, sibling))
                    {
                        continue;
                    }
                    if (sibling == selected)
                    {
                        passed = true;
                        continue;
                    }
                    current += !passed;
                    const UiRect rect = hierarchy.absoluteRectOf(sibling);
                    target += column ? dragPoint.y >= rect.position.y + rect.size.y * 0.5f
                                     : dragPoint.x >= rect.position.x + rect.size.x * 0.5f;
                }
                pendingMove = target - current;
            }

            std::string undo()
            {
                if (history.empty())
                {
                    return "nothing to undo";
                }
                const auto& [document, previous] = history.back();
                if (document)
                {
                    if (!document->view->apply(previous))
                    {
                        return fail("editor-gameUiEditor.refused", "could not restore previous draft");
                    }
                    document->model = Json::parse(previous);
                }
                else
                {
                    auto& sheet = *draftStyles;
                    if (!sheet.loadLayerText(STYLE_LAYER, previous))
                    {
                        return fail("editor-gameUiEditor.refused", "could not restore previous stylesheet");
                    }
                    styleText = previous;
                    appearanceChanged = true;
                }
                history.pop_back();
                showDirty();
                rebuildRows();
                return {};
            }

            std::string save()
            {
                if (!dirty())
                {
                    return {};
                }
                struct SaveFile
                {
                    const std::filesystem::path* path;
                    std::string* saved;
                    const std::string* text;
                };
                std::vector<SaveFile> files;
                for (auto& document : drafts)
                {
                    const auto& text = document.view->documentText();
                    if (text != document.saved)
                    {
                        files.push_back({&document.path, &document.saved, &text});
                    }
                }
                if (styleText != savedStyle)
                {
                    files.push_back({&stylePath, &savedStyle, &styleText});
                }
                for (const auto& file : files)
                {
                    const auto onDisk = readTextFile(*file.path);
                    if (!onDisk || *onDisk != *file.saved)
                    {
                        return fail("editor-gameUiEditor.conflict", "source changed since opening or saving the draft");
                    }
                }
                for (const auto& file : files)
                {
                    if (!writeTextFileAtomically(*file.path, *file.text))
                    {
                        syncControls();
                        return fail("editor-gameUiEditor.writeFailed", "could not save document");
                    }
                    *file.saved = *file.text;
                }
                status.setLocalizedText("editor-gameUiEditor.saved");
                syncControls();
                return {};
            }

            bool show()
            {
                if (!dock.isOpen(PANEL_NAME))
                {
                    if (const auto home = dock.reopenOp(PANEL_NAME); home && dock.apply(*home))
                    {
                        return true;
                    }
                    return dock.apply(UiDockOp{.kind = UiDockOpKind::Float,
                                               .panel = PANEL_NAME,
                                               .position = {24.0f, 24.0f},
                                               .size = {1660.0f, 800.0f},
                                               .detached = true});
                }
                return dock.apply(UiDockOp{.kind = UiDockOpKind::Activate, .panel = PANEL_NAME});
            }

            std::string open(const std::string& name)
            {
                PlayWorld* play = engine.playWorld();
                const UiDocumentView* source = play ? play->ui().documents().find(name) : nullptr;
                if (!source || !source->watching() || source->path().empty() || source->documentText().empty())
                {
                    return "no watched game document in Play";
                }
                return openDraft(source->path(), name, source);
            }

            std::string openFile(const std::string& name)
            {
                std::filesystem::path file;
                if (const auto error = engine.control().resolveFile("ui", name, file); !error.empty())
                {
                    return error;
                }
                if (file.extension() != ".json")
                {
                    return "select a JSON UI document";
                }
                if (name.ends_with(".composition.json"))
                {
                    return openComposition(file);
                }
                return openDraft(file, name, nullptr);
            }

            std::string buildDraft(Draft& next, const std::filesystem::path& file, const Entity parent,
                                   const UiDocumentView* source = nullptr)
            {
                std::string why;
                if (!readUiDocumentFile(file, next.saved, why))
                {
                    return "cannot read game UI document: " + why;
                }
                if (source && next.saved != source->documentText())
                {
                    return "the game document has changed; wait for Play to reload it";
                }
                UiViewContract contract;
                for (const auto& slot : next.mountSlots)
                {
                    contract.slot(InternedString(slot), {});
                }
                if (source)
                {
                    next.knowsBindings = true;
                    for (const auto& entry : source->contract().elements())
                    {
                        if (entry.required)
                        {
                            next.protectedIds.emplace_back(entry.id.toView());
                        }
                        if (entry.slot)
                        {
                            next.mountSlots.emplace_back(entry.id.toView());
                            contract.slot(entry.id, {});
                        }
                        else if (entry.required)
                        {
                            contract.require(entry.id, entry.kind);
                        }
                        else
                        {
                            contract.optional(entry.id, entry.kind);
                        }
                    }
                }
                next.path = file;
                next.view = std::make_unique<UiDocumentView>(
                    editorUi(engine).ui(), std::format("{}{}", DRAFT_NAME, ++draftNumber), std::move(contract), parent);
                if (!next.view->apply(next.saved))
                {
                    const auto error = std::ranges::find_if(next.view->problems(), &UiDocumentProblem::error);
                    return "cannot build document draft: " + utf8Path(file.filename()) +
                        (error == next.view->problems().end() ? std::string{} : ": " + error->message);
                }
                next.model = Json::parse(next.saved);
                return {};
            }

            std::string install(Surface nextSurface, std::vector<Draft> next, const std::string& name,
                                const std::filesystem::path& stylesheet, const Vec2 size = {},
                                std::filesystem::path compositionFile = {},
                                std::vector<UiCompositionAction> bindings = {},
                                std::unique_ptr<UiStyleSheet> nextStyles = {}, std::string nextStyle = {})
            {
                appearanceId.clear();
                previewHover = appearanceHover = false;
                appearanceChanged = true;
                clearDrafts();
                surface = std::move(nextSurface);
                draftStyles = std::move(nextStyles);
                if (draftStyles)
                {
                    editorUi(engine).uiView().setSubtreeStylesheet(surface.panel.entity(), draftStyles.get());
                }
                drafts = std::move(next);
                for (const auto& document : drafts)
                {
                    if (!document.mounted)
                    {
                        scope.hierarchy().setHitTransparent(document.view->root(), true);
                    }
                }
                compositionPath = std::move(compositionFile);
                composition = !compositionPath.empty();
                showHidden = false;
                designSize = size;
                stylePath = stylesheet;
                savedStyle = styleText = std::move(nextStyle);
                actions = std::move(bindings);
                viewport.setScrollOffset({});
                title.setText(name);
                showDirty();
                rebuildRows();
                return show() ? std::string{} : "could not show UI Editor";
            }

            std::string openDraft(const std::filesystem::path& file, const std::string& name,
                                  const UiDocumentView* source)
            {
                std::error_code pathError;
                const bool sameFile =
                    drafts.size() == 1 && std::filesystem::equivalent(drafts.front().path, file, pathError);
                if (dirty())
                {
                    return sameFile ? (show() ? std::string{} : "could not show UI Editor")
                                    : "save the current draft before opening another document";
                }
                auto nextSurface = makeSurface();
                std::vector<Draft> next(1);
                if (auto error = buildDraft(next.front(), file, nextSurface.panel.entity(), source); !error.empty())
                {
                    return error;
                }
                return install(std::move(nextSurface), std::move(next), name, {});
            }

            std::string openComposition(const std::filesystem::path& file)
            {
                if (compositionPath == file)
                {
                    return show() ? std::string{} : "could not show UI Editor";
                }
                if (dirty())
                {
                    return "save the current draft before opening another composition";
                }
                auto spec = readUiComposition(file);
                if (!spec)
                {
                    return std::string(spec.error().message);
                }
                const auto resolveAsset = [&](const std::string& asset, std::filesystem::path& path)
                {
                    constexpr std::string_view prefix = "assets/ui/";
                    return engine.control().resolveFile("ui", std::string_view(asset).substr(prefix.size()), path);
                };
                std::filesystem::path baseStyle, stylesheet;
                if (auto error = resolveAsset(spec->baseStyle, baseStyle); !error.empty())
                {
                    return error;
                }
                auto nextStyles = std::make_unique<UiStyleSheet>();
                if (const auto loaded = nextStyles->load(baseStyle); !loaded)
                {
                    return std::string(loaded.error().message);
                }
                if (!spec->theme.empty())
                {
                    std::filesystem::path theme;
                    if (auto error = resolveAsset(spec->theme, theme); !error.empty())
                    {
                        return error;
                    }
                    if (const auto loaded = nextStyles->setTheme("composition", std::span(&theme, 1)); !loaded)
                    {
                        return std::string(loaded.error().message);
                    }
                }
                std::string nextStyle;
                if (!spec->style.empty())
                {
                    if (auto error = resolveAsset(spec->style, stylesheet); !error.empty())
                    {
                        return error;
                    }
                    auto text = readTextFile(stylesheet);
                    if (!text)
                    {
                        return std::string(text.error().message);
                    }
                    if (const auto loaded = nextStyles->loadLayerText(STYLE_LAYER, *text); !loaded)
                    {
                        return std::string(loaded.error().message);
                    }
                    nextStyle = std::move(*text);
                }
                auto nextSurface = makeSurface();
                auto& editorView = editorUi(engine).uiView();
                struct RestoreStyles
                {
                    UiViewContext& view;
                    Entity root;
                    UiStyleSheet* sheet;
                    bool restore = true;
                    ~RestoreStyles()
                    {
                        if (restore)
                        {
                            view.setSubtreeStylesheet(root, sheet);
                        }
                    }
                } previousStyles{editorView, draftStyles ? surface.panel.entity() : Entity::undefined(),
                                 draftStyles.get()};
                // Token bindings are evaluated during document apply. Give the staging surface its own
                // table before building it, and restore the current draft's sheet if any view is refused.
                editorView.setSubtreeStylesheet(nextSurface.panel.entity(), nextStyles.get());
                std::vector<Draft> next;
                next.reserve(spec->views.size());
                for (const auto& entry : spec->views)
                {
                    Entity mount = nextSurface.panel.entity();
                    if (!entry.parent.empty())
                    {
                        const auto parentDocument = std::ranges::find_if(next, [&](const Draft& document)
                                                                         { return document.name == entry.parent; });
                        mount = parentDocument == next.end()
                            ? Entity::undefined()
                            : parentDocument->view->element(InternedString(entry.slot)).entity();
                        if (!mount.isValid())
                        {
                            return "composition mount slot is missing: " + entry.slot;
                        }
                    }
                    std::filesystem::path path;
                    if (auto error = resolveAsset(entry.path, path); !error.empty())
                    {
                        return error;
                    }
                    Draft& document = next.emplace_back();
                    for (const auto& child : spec->views)
                    {
                        if (child.parent == entry.name && !std::ranges::contains(document.mountSlots, child.slot))
                        {
                            document.mountSlots.push_back(child.slot);
                        }
                    }
                    if (auto error = buildDraft(document, path, mount); !error.empty())
                    {
                        return error;
                    }
                    document.name = entry.name;
                    document.mounted = !entry.parent.empty();
                    document.knowsBindings = true;
                    document.protectedIds = spec->protectedIds(entry.name);
                    std::unordered_map<std::string, u32> counts;
                    for (std::vector<const Json*> stack{&document.model.at("root")}; !stack.empty();)
                    {
                        const Json* node = stack.back();
                        stack.pop_back();
                        ++counts[node->value("id", std::string{})];
                        if (const auto children = node->find("children"); children != node->end())
                        {
                            for (const Json& child : *children)
                            {
                                stack.push_back(&child);
                            }
                        }
                    }
                    for (const auto& id : document.protectedIds)
                    {
                        if (counts[id] != 1)
                        {
                            return std::format("View '{}' needs exactly one protected widget '{}'.", entry.name, id);
                        }
                    }
                    document.fillWidth = entry.fillWidth;
                    document.previewVisible = entry.previewVisible;
                }
                previousStyles.restore = false;
                return install(std::move(nextSurface), std::move(next), spec->name, stylesheet, spec->screen, file,
                               std::move(spec->actions), std::move(nextStyles), std::move(nextStyle));
            }

            void registerCommands()
            {
                auto& control = engine.control();
                control.registerViewCommand(
                    "openGameUiEditor", PANEL_NAME, "open a Play document in the visual UI Editor",
                    {requiredArg("name", ArgKind::String, "registered Play document name")},
                    [this](const ControlArgs& args, IFormatWriter&) { return open(args.string("name").value_or("")); });
                control.registerViewCommand("openGameUiEditorFile", PANEL_NAME,
                                            "open a UI document without starting Play",
                                            {requiredPath("path", "ui", "JSON document under assets/ui")},
                                            [this](const ControlArgs& args, IFormatWriter&)
                                            { return openFile(args.string("path").value_or("")); });
                control.registerViewCommand("showGameUiEditor", PANEL_NAME,
                                            "show the visual UI Editor without starting Play", {},
                                            [this](const ControlArgs&, IFormatWriter&)
                                            { return show() ? std::string{} : "could not show UI Editor"; });
                control.registerViewCommand(
                    "openGameUiComposition", PANEL_NAME, "open a module UI composition without loading the module",
                    {requiredPath("path", "ui", "composition file under assets/ui")},
                    [this](const ControlArgs& args, IFormatWriter&)
                    {
                        std::filesystem::path file;
                        const auto error = engine.control().resolveFile("ui", args.string("path").value_or(""), file);
                        return error.empty() ? openComposition(file) : error;
                    });
                control.registerViewCommand("gameUiSelect", PANEL_NAME, "select a widget in the visual draft by id",
                                            {requiredArg("id", ArgKind::String, "unique document widget id")},
                                            [this](const ControlArgs& args, IFormatWriter&)
                                            {
                                                const std::string id = args.string("id").value_or("");
                                                const Entity found = uniqueWidget(id);
                                                if (!found.isValid())
                                                {
                                                    return std::string("name an unambiguous draft widget");
                                                }
                                                moveArmed = false;
                                                select(found);
                                                return std::string{};
                                            });
                control.registerViewCommand(
                    "gameUiLayout", PANEL_NAME, "edit an authored field in the selected widget's Layout inspector",
                    {requiredArg("field", ArgKind::String, "layout field, e.g. anchor or layout.padding"),
                     requiredArg("value", ArgKind::Json, "the field's native JSON value")},
                    [this](const ControlArgs& args, IFormatWriter&)
                    {
                        const InternedString id(args.string("field").value_or(""));
                        const auto field = std::ranges::find(layoutFields, id, &UiFormField::id);
                        if (field == layoutFields.end())
                        {
                            return std::string("unknown layout field");
                        }
                        const auto json = Json::parse(args.json("value").value_or(""), nullptr, false);
                        const auto value = formValue(*field, json);
                        return value ? editLayout(id, *value) : std::string("value does not match the layout field");
                    });
                control.registerViewCommand("gameUiImportSubtree", PANEL_NAME,
                                            "replace the selected draft subtree from a native UI document",
                                            {requiredPath("path", "ui", "SPITE UI document under assets/ui")},
                                            [this](const ControlArgs& args, IFormatWriter&)
                                            { return importSubtree(args.string("path").value_or("")); });
                control.registerViewCommand(
                    "gameUiExportSubtree", PANEL_NAME, "write the selected draft subtree as a native UI document",
                    {requiredPath("path", "ui", "export file under assets/ui"),
                     optionalArg("overwrite", ArgKind::Boolean, "replace an existing file", "false")},
                    [this](const ControlArgs& args, IFormatWriter&)
                    {
                        return exportSubtree(args.string("path").value_or(""),
                                             args.boolean("overwrite").value_or(false));
                    });
                control.registerViewCommand("gameUiAdd", PANEL_NAME, "add a widget to the selected panel",
                                            {requiredArg("kind", ArgKind::String, "panel, label or button")},
                                            [this](const ControlArgs& args, IFormatWriter&)
                                            { return addWidget(args.string("kind").value_or("")); });
                control.registerViewCommand("gameUiDuplicate", PANEL_NAME, "duplicate the selected widget subtree", {},
                                            [this](const ControlArgs&, IFormatWriter&) { return duplicateWidget(); });
                control.registerViewCommand("gameUiDelete", PANEL_NAME, "delete the selected unbound widget subtree",
                                            {}, [this](const ControlArgs&, IFormatWriter&) { return deleteWidget(); });
                control.registerViewCommand("gameUiMove", PANEL_NAME,
                                            "move the selected unbound widget into another panel in its document",
                                            {requiredArg("parent", ArgKind::String, "destination panel id")},
                                            [this](const ControlArgs& args, IFormatWriter&)
                                            { return moveWidget(uniqueWidget(args.string("parent").value_or(""))); });
                control.registerViewCommand("gameUiOffset", PANEL_NAME,
                                            "move the selected anchored widget by a canvas delta",
                                            {requiredArg("x", ArgKind::Number, "horizontal delta in UI pixels"),
                                             requiredArg("y", ArgKind::Number, "vertical delta in UI pixels")},
                                            [this](const ControlArgs& args, IFormatWriter&)
                                            {
                                                const auto x = Json::parse(args.json("x").value_or(""), nullptr, false);
                                                const auto y = Json::parse(args.json("y").value_or(""), nullptr, false);
                                                return x.is_number() && y.is_number()
                                                    ? offsetWidget({x.get<float>(), y.get<float>()})
                                                    : "enter a numeric x and y delta";
                                            });
                control.registerViewCommand("gameUiResize", PANEL_NAME, "set the selected draft widget width or height",
                                            {optionalArg("width", ArgKind::Number, "new width in UI pixels"),
                                             optionalArg("height", ArgKind::Number, "new height in UI pixels")},
                                            [this](const ControlArgs& args, IFormatWriter&)
                                            {
                                                const auto dimension = [&](const char* name)
                                                {
                                                    const auto field = args.json(name);
                                                    if (!field)
                                                    {
                                                        return -1.0f;
                                                    }
                                                    const auto value = Json::parse(*field, nullptr, false);
                                                    return value.is_number() && value.get<float>() >= 1.0f
                                                        ? value.get<float>()
                                                        : 0.0f;
                                                };
                                                return resize({dimension("width"), dimension("height")});
                                            });
                control.registerViewCommand("gameUiReorder", PANEL_NAME,
                                            "move the selected draft widget within its row or column",
                                            {requiredArg("direction", ArgKind::String, "earlier or later")},
                                            [this](const ControlArgs& args, IFormatWriter&)
                                            {
                                                const auto direction = args.string("direction").value_or("");
                                                if (direction != "earlier" && direction != "later")
                                                {
                                                    return std::string("direction must be earlier or later");
                                                }
                                                return reorder(direction == "earlier" ? -1 : 1);
                                            });
                control.registerViewCommand("gameUiText", PANEL_NAME, "set the selected draft widget's literal text",
                                            {requiredArg("text", ArgKind::String, "new literal text")},
                                            [this](const ControlArgs& args, IFormatWriter&)
                                            { return editText(args.string("text").value_or("")); });
                control.registerViewCommand("gameUiUndo", PANEL_NAME, "undo a draft edit", {},
                                            [this](const ControlArgs&, IFormatWriter&) { return undo(); });
                control.registerViewCommand("gameUiSave", PANEL_NAME, "save the draft source for hot reload", {},
                                            [this](const ControlArgs&, IFormatWriter&) { return save(); });
                control.registerViewCommand("gameUiHoverColor", PANEL_NAME,
                                            "preview a selected widget's hover background override",
                                            {requiredArg("color", ArgKind::String, "#RRGGBB or #RRGGBBAA")},
                                            [this](const ControlArgs& args, IFormatWriter&)
                                            { return editHover(args.string("color").value_or("")); });
                control.registerQuery("gameUiEditor", PANEL_NAME, "visual UI Editor selection and draft state", {},
                                      [this](const ControlArgs&, IFormatWriter& out)
                                      {
                                          out.writeString("selected",
                                                          selected.isValid() ? idOf(selected) : std::string_view{});
                                          out.writeBool("dirty", dirty());
                                          out.writeBool("composition", composition);
                                          out.writeU64("documents", drafts.size());
                                          out.writeU64("undo", history.size());
                                          return std::string{};
                                      });
            }

            Engine& engine;
            DockSpace dock;
            EditorDialogs& dialogs;
            UiScope scope;
            DensityFollower density{scope};
            PageScroll inspectorPage;
            ScrollView viewport;
            Panel root, canvas, outline;
            Label title, status, inspector, widgetClass, actionLabel, actionArgs, hoverLabel, bounds;
            Label layoutTitle, layoutTokens;
            Button saveButton, undoButton, earlier, later, handle, hoverButton, visibilityButton;
            Button importButton, exportButton;
            Button addPanelButton, addLabelButton, addButtonButton, duplicateButton, moveButton, deleteButton;
            TextInput textInput, hoverColor;
            TreeView tree;
            std::vector<UiFormField> layoutFields;
            std::unique_ptr<PropertyForm> layoutForm;
            Vec2 boundsOffset{}, boundsSize{-1.0f, -1.0f};
            std::string requiredMark = "Bound";
            std::string slotMark = "Slot";
            std::vector<Entity> treeRoots;
            std::unordered_map<Entity, std::vector<Entity>> treeChildren;
            Surface surface;
            std::unique_ptr<UiStyleSheet> draftStyles;
            std::vector<Draft> drafts;
            Draft* active = nullptr;
            std::vector<Undo> history;
            std::filesystem::path stylePath, compositionPath;
            std::string styleText, savedStyle, appearanceId;
            std::vector<UiCompositionAction> actions;
            Vec2 designSize{};
            u64 draftNumber = 0;
            std::vector<Entity> nodes;
            Entity selected = Entity::undefined();
            std::function<void()> queued;
            std::vector<std::function<void()>> pendingEdits;
            std::optional<Vec2> dragSize, pendingSize, pendingOffset;
            Vec2 dragPoint{}, dragOffset{};
            int pendingMove = 0;
            bool resizeCheckpoint = false;
            bool refreshingRows = false;
            bool canResize = false;
            bool composition = false;
            bool showHidden = false;
            bool previewHover = false;
            bool appearanceHover = false;
            bool appearanceChanged = false;
            bool panning = false;
            bool dragSelection = false;
            bool moveArmed = false;
            bool revealPending = false;
        };
        const EditorPanelRegistrar GAME_EDITOR_REGISTRATION{
            {.name = "gameUiEditor",
             .category = "project",
             .order = 209,
             .minSize = {420.0f, 280.0f},
             .zone = "GameUiEditor",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<GameUiEditor>(c.engine, c.dock, c.dialogs); },
             .openByDefault = false}};
        const EditorPanelRegistrar REGISTRATION{
            {.name = "uiPreview",
             .category = "project",
             .order = 208,
             .zone = "EditorUiPreview",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<UiPreview>(c.engine, c.dock); },
             .openByDefault = false}};
    } // namespace
} // namespace spite
