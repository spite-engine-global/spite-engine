#include "base/WritableLayout.hpp"
#include "editor/EditorShell.hpp"
#include "editor/EditorModel.hpp"
#include "EditorCaptureFiles.hpp"
#include "editor/EditorTestSeams.hpp"
#include "ui/UiContext.hpp"
#include "base/ContentRoot.hpp"

#include <spite/profiling/Zone.hpp>
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderingSingletons.hpp"
#include "rendering/SceneViewOverlay.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "application/Window.hpp"
#include "base/Assert.hpp"
#include "base/Logging.hpp"
#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "EditorBuildState.hpp"
#include "EditorContext.hpp"
#include "EditorActions.hpp"
#include "EditorAgentLayer.hpp"
#include "EditorCapture.hpp"
#include "EditorCommandPalette.hpp"
#include "EditorDialogs.hpp"
#include "EditorFeedback.hpp"
#include "EditorFileMenu.hpp"
#include "EditorGameToolbar.hpp"
#include "EditorGizmo.hpp"
#include "EditorHierarchy.hpp"
#include "EditorInspector.hpp"
#include "EditorLayouts.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorPanelUi.hpp"
#include "EditorProfiler.hpp"
#include "EditorProject.hpp"
#include "editor/EditorPreviewJobs.hpp"
#include "EditorSceneHud.hpp"
#include "EditorSceneToolbar.hpp"
#include "EditorSceneGrid.hpp"
#include "EditorSceneView.hpp"
#include "EditorSettings.hpp"
#include "EditorShellChrome.hpp"
#include "EditorFrameHeader.hpp"
#include "EditorStatusBar.hpp"
#include "engine/Engine.hpp"
#include "engine/PlayWorld.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiScope.hpp"
#include "renderruntime/RenderRuntime.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiTextMeasurer.hpp"
#include "uiview/UiFrame.hpp"
#include "uiview/UiViewContext.hpp"

namespace spite
{
    namespace
    {
        constexpr std::string_view OWNER = "editorShell";
        constexpr std::string_view VIEW = "editor.shell";
        // the document's slots (assets/ui/editor/shell.json)
        constexpr std::string_view MENUS_SLOT = "shell.toolbar.menus";
        constexpr std::string_view CENTER_SPACER = "shell.toolbar.centerSpacer";
        constexpr std::string_view TRANSPORT_SLOT = "shell.toolbar.transport";
        constexpr std::string_view END_SLOT = "shell.toolbar.end";
        constexpr std::string_view TINT_SLOT = "shell.toolbar.tint";
        constexpr std::string_view DOCK_SLOT = "shell.dock";
        constexpr std::string_view SHORTCUTS_ITEM = "help.shortcuts";
        constexpr std::string_view PALETTE_ITEM = "help.palette";
        constexpr std::string_view RESET_ITEM = "window.reset";
        constexpr std::string_view FULLSCREEN_ITEM = "window.fullscreen";
        constexpr std::string_view TIME_SCALE_ITEM = "window.timescale";
        constexpr std::string_view TIME_SCALE_PREFIX = "window.timescale.";
        // the Window > Layout submenu: a layout to load, one to delete, and the prompt that names a new one
        constexpr std::string_view LAYOUT_ITEM = "window.layout";
        constexpr std::string_view LAYOUT_DELETE_ITEM = "window.layout.delete";
        constexpr std::string_view LAYOUT_LOAD_PREFIX = "layout.load.";
        constexpr std::string_view LAYOUT_DELETE_PREFIX = "layout.delete.";
        constexpr std::string_view LAYOUT_SAVE_ITEM = "layout.save";
        using Json = nlohmann::json;
        // frames a view panel's size must hold, with no drag in progress, before its
        // view target is reallocated at it (a graph rebuild)
        constexpr u32 SETTLE_FRAMES = 6;
        constexpr u64 VIEW_IMAGE_SLOT = 0x5350495445564900ull; // + view: the image of a render view's target
        // the keys of editor.json (namespace "editor") for the move, rotate and scale buttons
        constexpr std::array<std::string_view, 3> GIZMO_LABELS{"move", "rotate", "scale"};
        constexpr std::array<std::string_view, 2> GIZMO_SPACE_ICONS{"globe-hemisphere-west", "cube"}; // world, local
        constexpr std::array<std::string_view, 3> GIZMO_ICONS{"arrows-out-cardinal", "arrow-clockwise",
                                                              "arrows-out-simple"};

        struct EditItem
        {
            std::string_view id; // the label is the editor key "menu.<id>"
            EditorActions::Action action;
        };

        constexpr std::array EDIT_ITEMS{
            EditItem{"edit.undo", EditorActions::Action::Undo},
            EditItem{"edit.redo", EditorActions::Action::Redo},
            EditItem{"edit.copy", EditorActions::Action::Copy},
            EditItem{"edit.paste", EditorActions::Action::Paste},
            EditItem{"edit.duplicate", EditorActions::Action::Duplicate},
            EditItem{"edit.delete", EditorActions::Action::Delete},
        };

        using panelui::topLeft;

        // a text's width in a monospace column: code points, not bytes
        sizet codepoints(const std::string_view text)
        {
            return static_cast<sizet>(std::ranges::count_if(
                text, [](const char c) { return (static_cast<unsigned char>(c) & 0xC0u) != 0x80u; }));
        }

        // indexed by UiDockOpKind and UiDockZone
        // the UiDockOpKind names, then "show": the shell's own kind, a panel back where it was
        constexpr std::array<std::string_view, 9> KIND_NAMES{"dock",   "float",    "move",    "close", "activate",
                                                             "resize", "maximize", "restore", "show"};
        constexpr u8 SHOW_KIND = 8;
        constexpr std::array<std::string_view, 5> ZONE_NAMES{"center", "left", "right", "top", "bottom"};

        UiDockOp dockOp(const UiDockOpKind kind, const std::string_view panel, const std::string_view target = {},
                        const UiDockZone zone = UiDockZone::Center, const float ratio = 0.5f)
        {
            return UiDockOp{.kind = kind,
                            .panel = InternedString(panel),
                            .target = target.empty() ? InternedString{} : InternedString(target),
                            .zone = zone,
                            .ratio = ratio};
        }

        std::string dockRequest(const UiDockOp& op)
        {
            return std::format(
                R"({{"command":"dock","args":{{"kind":"{}","panel":"{}","target":"{}","zone":"{}","ratio":{},"x":{},"y":{},"w":{},"h":{}{}{}}}}})",
                KIND_NAMES[static_cast<u8>(op.kind)], op.panel.toView(),
                op.target.isValid() ? op.target.toView() : std::string_view{}, ZONE_NAMES[static_cast<u8>(op.zone)],
                op.ratio, op.position.x, op.position.y, op.size.x, op.size.y,
                op.index == UiDockOp::LAST ? std::string{} : std::format(",\"index\":{}", op.index),
                op.detached ? ",\"detached\":true" : "");
        }

        template <std::size_t N>
        bool named(const std::array<std::string_view, N>& names, const std::optional<std::string>& text, u8& out)
        {
            const auto it = std::ranges::find(names, text.value_or(std::string{}));
            out = static_cast<u8>(it - names.begin());
            return it != names.end();
        }

        // A number member; absent keeps out, unparsable fails.
        bool number(const ControlArgs& args, const std::string_view key, float& out)
        {
            const std::optional<std::string> text = args.json(key);
            if (!text)
            {
                return true;
            }
            char* end = nullptr;
            const float value = std::strtof(text->c_str(), &end);
            if (end == text->c_str() || !std::isfinite(value))
            {
                return false;
            }
            out = value;
            return true;
        }
    } // namespace

    struct EditorShell::State
    {
        State(Engine& engine, EditorShellDesc desc) :
            model(engine, {.settingsFile = desc.settingsFile, .forgeTokensFile = desc.forgeTokensFile}), seams(engine),
            engine(&engine), desc(std::move(desc)), scope(editorUi(engine).ui()),
            settings(&EditorModelAccess::settings(model))
        {
            this->desc.layoutFile = resolveUserConfigPath(this->desc.layoutFile);
            layouts = std::make_unique<EditorLayoutStore>(this->desc.layoutFile.empty()
                                                              ? std::filesystem::path{}
                                                              : this->desc.layoutFile.parent_path() / "layouts",
                                                          resolveContentPath("assets/ui/editor/layouts"));
            this->desc.hotkeysFile = resolveContentPath(this->desc.hotkeysFile);
            this->desc.forgeTokensFile = resolveContentPath(this->desc.forgeTokensFile);
            if (!this->desc.settingsFile.empty())
            {
                this->desc.settingsFile = resolveUserConfigPath(this->desc.settingsFile);
            }
        }

        ~State()
        {
            if (resizeAxis != UiResizeAxis::None && engine->window())
            {
                engine->window()->setCursorShape(CursorShape::Default);
            }
            for (auto it = buildSequence.rbegin(); it != buildSequence.rend(); ++it)
            {
                panels[*it].panel = {};
            }
        }

        // What the shell adds to the engine: the Scene panel's GPU work (SceneViewOverlay, which the overlay passes
        // cull themselves without). Removed after every other member but the model released what it holds there.
        struct Seams
        {
            explicit Seams(Engine& engine) : engine(&engine), ui(&editorUi(engine))
            {
                engine.services().registerService(std::make_shared<SceneViewOverlay>());
                bumpGraphGeneration();
            }
            ~Seams()
            {
                engine->services().unregisterService<SceneViewOverlay>();
                bumpGraphGeneration();
            }
            Seams(const Seams&) = delete;
            Seams& operator=(const Seams&) = delete;

            // the overlay passes declare (or cull) themselves at graph compile: the next frame recompiles
            void bumpGraphGeneration() const
            {
                if (engine->services().contains<RenderGraphGenerationSingleton>())
                {
                    engine->services().get<RenderGraphGenerationSingleton>().bump("scene view overlay");
                }
            }

            Engine* engine;
            UiWorld* ui;
        };

        // the editor model: built before every part of the shell and destroyed after them all
        EditorModel model;
        Seams seams;
        Engine* engine;
        UiResizeAxis resizeAxis = UiResizeAxis::None;
        EditorShellDesc desc;
        UiScope scope;
        panelui::DensityFollower density{scope}; // the toolbar's control height and text padding follow the density
        Panel root; // the shell's size; the toasts, menus and drag preview sit on it above `host`
        Panel host; // holds the shell document view's root, sized to the root
        Panel barTint; // the toolbar's play-state strip, in the document's shell.toolbar.tint slot
        Widget centerSpacer;
        std::unique_ptr<UiDocumentView> view; // editor.shell: the chrome's containers, assets/ui/editor/shell.json
        Button file;
        Button window;
        Button edit;
        Button help;
        Button paletteField; // the header's Search commands field (EditorCommandPalette)
        Button play;
        Button pause;
        Button step;
        Button stop;
        Menu menu;
        u32 barMenu = 0; // the menu bar's menu the shared popup last opened (File, Edit, Window, Help)
        DockSpace dock;
        Panel dragPreview; // the editor UI's UiDragDrop preview, over everything
        EditorFeedback* feedback = nullptr; // the model's: before what reports to it
        std::unique_ptr<EditorPreviewJobs> previewJobs;
        std::unique_ptr<EditorInspector> inspector;
        std::unique_ptr<EditorStatusBar> statusBar; // after the inspector it reads
        std::unique_ptr<EditorFrameHeader> frameHeader; // after the status bar whose windows it follows
        std::unique_ptr<EditorDialogs> dialogs; // before the panels that ask
        std::unique_ptr<EditorActions> actions;
        u64 seenBindings = 0;
        std::unique_ptr<EditorFileMenu> fileMenu; // after the actions whose hotkeys it handles
        std::unique_ptr<EditorCommandPalette> palette; // after the actions, dialogs and feedback it uses
        float paletteWidth = 0.0f; // the field's natural width (UI px), measured by fitEnd
        std::unique_ptr<EditorGizmo> gizmo;
        EditorSettingsService* settings; // the model's
        std::vector<u32> applied; // the settings listeners the shell added, removed first in ~EditorShell
        std::unique_ptr<EditorAgentLayer> agent; // after the settings it reads, before the panels that show it
        // the registered panels by (order, name), and the indices in the order they are built and updated
        struct Built
        {
            EditorPanelDesc desc;
            EditorPanel panel;
        };
        std::vector<Built> panels;
        std::vector<u32> buildSequence;

        // the panel's least content: its fixed size and the bars and rows of the density
        void setPanelMin(const EditorPanelDesc& desc, const panelui::Density& now)
        {
            dock.setPanelMinSize(InternedString(desc.name),
                                 desc.minSize +
                                     Vec2{0.0f, desc.minBarsRows.x * now.bar + desc.minBarsRows.y * now.row});
        }
        std::array<Button, 3> gizmoModes; // move, rotate, scale
        Button gizmoSpace;
        Button gizmoSnap;
        NumberField gizmoStep; // the snap step of the chosen tool
        Label gizmoUnit;
        const Localization* localization = nullptr; // not owned: the engine's service
        u64 shownRevision = 0; // the localization revision the toolbar and tab titles show
        u32 gizmoShown = ~0u; // the settings the toolbar shows (mode | space << 2 | snap << 3)
        f32 stepShown = -1.0f; // the step the field shows
        float stepTextScale = -1.0f; // what the field's reserved width was measured for
        u64 endLayout = ~0ull; // what the end group's shedding was decided for
        u64 stepSheet = ~0ull;
        bool sceneDirty = false; // the Scene tab shows a `*` after its title while the open scene has unsaved changes
        std::unique_ptr<EditorSceneView> sceneView; // with the editor views
        std::string defaultLayout;
        std::unique_ptr<EditorLayoutStore> layouts; // the named layouts beside the layout file
        std::vector<std::string> pending; // request lines update() dispatches
        Vec2 size{0.0f, 0.0f};
        u32 yieldLevel = 0; // the wordmark yields before the end controls
        bool synced = false;
        std::vector<std::function<void()>> deferred; // what a menu item or dialog runs between frames, after them
        bool playing = false;
        bool paused = false;
        bool splitForPlay = false; // Play split Game off the Scene tab group; Stop tabs it back
        u32 gameTab = UiDockOp::LAST; // where Game's tab stood among its group's before Play split it off
        bool editorViews = false; // the engine renders the Scene and Game panels' views
        bool gameFocused = false;
        float zoomWheel = 0.0f;
        struct ViewImage
        {
            Panel host; // the panel's content: the room the view's target is shown in
            Image image; // the target, letterboxed in host while its extent lags the panel
            u32 view = 0;
            Extent2D pending{};
            u32 stableFrames = 0;
        };
        std::array<ViewImage, 2> views; // scene, game
        std::unique_ptr<EditorModeFrame> modeFrame; // on the Scene image: the Play and Pause border
        std::unique_ptr<EditorPlayCounter> playCounter; // the transport group's frame counter
        std::unique_ptr<EditorTimeScale> timeScale; // the transport group's time scale field and reset
        Label timeScaleMark; // the multiplication sign before the time scale: the field reads as a speed
        std::unique_ptr<EditorSceneHud> sceneHud; // the ruler, the HUD and the agent callout, over the same host
        std::unique_ptr<EditorSceneToolbar> sceneToolbar; // over the Scene panel's host (with the editor views)
        std::unique_ptr<EditorSceneGrid> sceneGrid; // the grid and background of view 0
        std::unique_ptr<EditorGameToolbar>
            gameToolbar; // around the Game host: the toolbar, the target's fit, the status

        // The editor's views: the Scene panel shows SCENE_VIEW through the editor camera
        // (RenderView::pose): it follows the game, so it draws the edit world, or the play world while playing; the
        // Game panel shows GAME_VIEW, the game view. Both render offscreen (NO_SWAPCHAIN_VIEW) and the swapchain
        // carries only the UI. Off (a standalone game): view 0 is the game view, on the swapchain. Needs
        // config.graphics.renderViewCount >= 2; false otherwise. Moves a running Play's binding to the new game view;
        // turning off stops rendering GAME_VIEW (RenderView::active).
        bool setEditorViews(const bool on)
        {
            RenderRuntime& render = engine->render();
            if (on && render.viewCount() < 2)
            {
                return false;
            }
            // the probe capture view is the bake's, not a panel's
            if (on &&
                (engine->config().graphics.isCaptureView(SCENE_VIEW) ||
                 engine->config().graphics.isCaptureView(GAME_VIEW)))
            {
                SERROR_LOG(
                    "EditorShell: graphics.probeCaptureView {} is a view the editor shows; the editor views stay "
                    "off",
                    engine->config().graphics.probeCaptureView);
                return false;
            }
            if (on == editorViews)
            {
                return true;
            }
            engine->setViewFollowsGame(SCENE_VIEW, on);
            (void)engine->setGameView(on ? GAME_VIEW : SCENE_VIEW);
            // the editor's game view renders only while the editor shows it
            render.view(GAME_VIEW).active = on;
            render.view(SCENE_VIEW).active = true;
            // the Scene view is the editor's tool: it renders at its panel's size; standalone, view 0 is the game view
            render.view(SCENE_VIEW).scalable = !on;
            render.view(SCENE_VIEW).renderScale = 1.0f;
            if (!on)
            {
                for (u32 view = 0; view < render.viewCount(); ++view)
                {
                    render.view(view).windowRect = {};
                    render.view(view).extent = {};
                }
            }
            (void)render.setSwapchainView(on ? NO_SWAPCHAIN_VIEW : SCENE_VIEW);
            engine->setSwapchainViewLock(on ? "the editor's views render offscreen into its panels: no swapchain view "
                                              "while the editor is open"
                                            : "");
            // screenshot panel=scene|game captures the panel's view
            seams.ui->setPanelView("scene", on ? std::optional(SCENE_VIEW) : std::nullopt);
            seams.ui->setPanelView("game", on ? std::optional(GAME_VIEW) : std::nullopt);
            editorViews = on;
            return true;
        }

        // The ScenePick, SceneGrid, SelectionOutline (with its args and mask passes) and TransformGizmo passes run
        // only while the SceneViewOverlay service has work for them; derived every frame: a graph recompile turns
        // every declared pass back on.
        void syncOverlayPasses() const
        {
            ServiceLocator& services = engine->services();
            if (!services.contains<RenderGraph>())
            {
                return;
            }
            static const InternedString PICK = toHashedString(SceneViewOverlay::PICK_PASS);
            static const InternedString GRID = toHashedString(SceneViewOverlay::GRID_PASS);
            static const InternedString OUTLINE_ARGS = toHashedString(SceneViewOverlay::OUTLINE_ARGS_PASS);
            static const InternedString OUTLINE_MASK = toHashedString(SceneViewOverlay::OUTLINE_MASK_PASS);
            static const InternedString OUTLINE = toHashedString(SceneViewOverlay::OUTLINE_PASS);
            static const InternedString GIZMO = toHashedString(SceneViewOverlay::GIZMO_PASS);
            const auto overlay = services.read<SceneViewOverlay>();
            auto graph = services.write<RenderGraph>();
            for (const auto& [name, wanted] :
                 {std::pair{PICK, overlay->pickWork()},
                  {GRID, overlay->gridWork()},
                  {OUTLINE_ARGS, !overlay->outlined.empty()},
                  {OUTLINE_MASK, !overlay->outlined.empty()},
                  {OUTLINE, !overlay->outlined.empty()},
                  {GIZMO, !overlay->gizmo.empty() || !overlay->marquee.empty() || !overlay->agentHighlight.empty()}})
            {
                if (const RGPass* pass = graph->findPass(name); pass && pass->isActive() != wanted)
                {
                    graph->setPassActive(name, wanted);
                }
            }
        }

        // Each view panel's image rect becomes its view's window rect at once and its extent follows the panel's
        // size once that settles; game input follows the Game panel's focus. The image keeps the aspect of the
        // target (its extent) and is centred in the panel, so a resizing panel letterboxes the image, never
        // stretches it, and the window rect is the rect actually drawn (picking maps through it).
        void syncViews()
        {
            const UiHierarchyManager& hierarchy = editorUi(*engine).ui().hierarchy();
            const UiRouter& router = editorUi(*engine).ui().router();
            const bool dragging = router.pressed().isValid();
            const float scale = std::max(hierarchy.layoutScale(), 1e-3f);
            for (ViewImage& entry : views)
            {
                RenderView& view = engine->render().view(entry.view);
                const bool panelVisible = hierarchy.isVisibleOf(entry.image.entity());
                view.active = panelVisible || engine->screenshotNeedsView(entry.view);
                if (!panelVisible)
                {
                    continue; // a hidden panel keeps its target and rect, including during a screenshot
                }
                const UiRect area = hierarchy.absoluteRectOf(entry.host.entity());
                const Extent2D room{std::max(1u, static_cast<u32>(std::max(0l, std::lround(area.size.x)))),
                                    std::max(1u, static_cast<u32>(std::max(0l, std::lround(area.size.y))))};
                // the Game panel's aspect or resolution makes the target smaller or other than the room
                const Extent2D wanted =
                    entry.view == GAME_VIEW && gameToolbar ? gameToolbar->targetIn(area.size) : room;
                if (wanted.width == view.extent.width && wanted.height == view.extent.height)
                {
                    entry.stableFrames = 0;
                }
                else
                {
                    if (wanted.width != entry.pending.width || wanted.height != entry.pending.height)
                    {
                        entry.pending = wanted;
                        entry.stableFrames = 0;
                    }
                    else if (!dragging)
                    {
                        ++entry.stableFrames;
                    }
                    if (view.extent.width == 0 || entry.stableFrames >= SETTLE_FRAMES)
                    {
                        view.extent = wanted;
                        entry.stableFrames = 0;
                    }
                }

                // the target keeps its own aspect while its extent lags the panel: fitted in the panel and centred
                UiRect shown = area;
                if (view.extent.width != room.width || view.extent.height != room.height)
                {
                    const float fit = std::min(area.size.x / static_cast<float>(view.extent.width),
                                               area.size.y / static_cast<float>(view.extent.height));
                    shown.size = Vec2{static_cast<float>(view.extent.width), static_cast<float>(view.extent.height)} *
                        std::max(fit, 0.0f);
                    shown.position = area.position + (area.size - shown.size) * 0.5f;
                }
                if (sceneToolbar && entry.view == SCENE_VIEW)
                {
                    sceneToolbar->setSize(area.size / scale);
                    sceneHud->setSize(area.size / scale);
                }
                if (entry.image.size() != shown.size / scale)
                {
                    entry.image.setSize(shown.size / scale);
                    if (modeFrame && entry.view == SCENE_VIEW)
                    {
                        modeFrame->setSize(shown.size / scale);
                    }
                }
                view.windowRect = {
                    {static_cast<i32>(std::lround(shown.position.x)), static_cast<i32>(std::lround(shown.position.y))},
                    {static_cast<u32>(std::max(0l, std::lround(shown.size.x))),
                     static_cast<u32>(std::max(0l, std::lround(shown.size.y)))}};
            }

            const Image& game = views[1].image;
            const Panel& gameHost = views[1].host;
            const auto elsewhere = [&game, &gameHost](const Entity element)
            { return element.isValid() && element != game.entity() && element != gameHost.entity(); };
            if (gameFocused &&
                (!hierarchy.isVisibleOf(game.entity()) || elsewhere(router.pressed()) || elsewhere(router.focused())))
            {
                gameFocused = false;
            }
            applyGameFocus();
        }

        void applyGameFocus()
        {
            // focused, the game view lets presses through to the game; unfocused, a press on it focuses it
            for (Widget game : {static_cast<Widget>(views[1].image), static_cast<Widget>(views[1].host)})
            {
                if (game.isInteractable() == gameFocused)
                {
                    game.setInteractable(!gameFocused);
                }
            }
            engine->setGameInputFocus(gameFocused);
        }

        std::string setGameFocus(const bool on)
        {
            if (!editorViews)
            {
                return std::string("the editor's views are off: the game takes input as a standalone game");
            }
            if (on && !editorUi(*engine).ui().hierarchy().isVisibleOf(views[1].image.entity()))
            {
                return std::string("the Game panel is not shown (closed, or behind another tab)");
            }
            if (on)
            {
                UiRouter& router = editorUi(*engine).ui().router();
                if (router.modalActive())
                {
                    return std::string("a modal dialog owns input");
                }
                router.clearFocus();
            }
            gameFocused = on;
            applyGameFocus();
            return std::string{};
        }

        // The message of a key; the key itself when missing.
        [[nodiscard]] std::string message(const std::string& key) const
        {
            if (!localization)
            {
                return key;
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(key), scratch));
        }

        [[nodiscard]] std::string message(const std::string& key, const std::vector<MessageArg>& args) const
        {
            if (!localization)
            {
                return key;
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(key), args, scratch));
        }

        // The message of an editor key ("bar.play" is "editor.bar.play").
        [[nodiscard]] std::string tr(const std::string_view key) const { return message("editor." + std::string(key)); }

        // A panel's tab and menu title: the key "title" of its table (assets/loc/<locale>/editor/<name>.json).
        [[nodiscard]] std::string title(const EditorPanelDesc& desc) const
        {
            return message("editor-" + desc.name + ".title");
        }

        // The tab's text: the title, and the Scene tab's carries the open scene's dirty marker.
        [[nodiscard]] std::string tabTitle(const EditorPanelDesc& desc) const
        {
            return title(desc) + (sceneDirty && desc.name == "scene" ? "*" : "");
        }

        void syncSceneTab(const bool dirty)
        {
            if (dirty == sceneDirty)
            {
                return;
            }
            sceneDirty = dirty;
            for (const Built& built : panels)
            {
                if (built.desc.name == "scene")
                {
                    dock.setPanelTitle(InternedString(built.desc.name), tabTitle(built.desc));
                }
            }
        }

        template <class T>
        [[nodiscard]] T* panel(const std::string_view name) const
        {
            const auto it =
                std::ranges::find(panels, name, [](const Built& built) { return std::string_view(built.desc.name); });
            if (it == panels.end() || !it->panel.owner)
            {
                SERROR_LOG("EditorShell: no built panel {}.", name);
                return nullptr;
            }
            return static_cast<T*>(it->panel.owner.get());
        }

        // A locale or table change: tab titles and the toolbar texts the shell sets itself follow; the buttons
        // with a localized text follow on their own.
        void syncLocale()
        {
            if (!localization || localization->revision() == shownRevision)
            {
                return;
            }
            shownRevision = localization->revision();
            for (const Built& built : panels)
            {
                dock.setPanelTitle(InternedString(built.desc.name), tabTitle(built.desc));
            }
            gizmoShown = ~0u;
            synced = false;
        }

        // The registered placements as dock ops, each after the panel it docks against and the one it names `after`.
        void buildDefaultLayout()
        {
            const auto apply = [this](const UiDockOp& op)
            {
                [[maybe_unused]] const bool applied = dock.apply(op);
                SASSERTM(applied, "EditorShell: a default layout op was refused.");
            };
            std::vector<bool> placed(panels.size());
            const auto place = [&](auto&& self, const std::size_t index) -> void
            {
                if (placed[index])
                {
                    return;
                }
                placed[index] = true;
                if (!panels[index].desc.openByDefault)
                {
                    return;
                }
                const EditorDockPlacement& at = panels[index].desc.dock;
                // a reference to a panel that is not registered is logged and dropped (returns true): `after` is
                // ignored, and a missing target docks the panel at the root's bottom edge
                const auto missing = [&](const std::string& dependency, const char* role)
                {
                    if (dependency.empty())
                    {
                        return false;
                    }
                    const auto it = std::ranges::find(panels, dependency, [](const Built& b) { return b.desc.name; });
                    if (it == panels.end())
                    {
                        SERROR_LOG("EditorShell: panel {} docks {} {}, which is not registered; ignored",
                                   panels[index].desc.name, role, dependency);
                        return true;
                    }
                    self(self, static_cast<std::size_t>(it - panels.begin()));
                    return false;
                };
                missing(at.after, "after");
                const bool noTarget = missing(at.target, "against");
                // the root takes a Center dock only while it is empty
                apply(noTarget ? dockOp(UiDockOpKind::Dock, panels[index].desc.name, {}, UiDockZone::Bottom, at.ratio)
                               : dockOp(UiDockOpKind::Dock, panels[index].desc.name, at.target, at.zone, at.ratio));
            };
            for (std::size_t i = 0; i < panels.size(); ++i)
            {
                place(place, i);
            }
            for (const Built& built : panels)
            {
                if (built.desc.openByDefault && built.desc.dock.activate)
                {
                    apply(dockOp(UiDockOpKind::Activate, built.desc.name));
                }
            }
        }

        std::vector<UiMenuItem> windowItems() const
        {
            std::vector<UiMenuItem> items;
            const auto panelItem = [this](const Built& built)
            {
                return UiMenuItem{.id = InternedString("window." + built.desc.name),
                                  .label = title(built.desc),
                                  .checkable = true,
                                  .checked = dock.isOpen(InternedString(built.desc.name))};
            };
            for (const Built& built : panels)
            {
                if (built.desc.category.empty())
                {
                    items.push_back(panelItem(built));
                }
            }
            for (const std::string_view category : {"project", "debug", "world", "tools"})
            {
                std::vector<UiMenuItem> children;
                for (const Built& built : panels)
                {
                    if (built.desc.category == category)
                    {
                        children.push_back(panelItem(built));
                    }
                }
                if (!children.empty())
                {
                    items.push_back(UiMenuItem{.id = InternedString("window.group." + std::string(category)),
                                               .label = tr("menu.window.group." + std::string(category)),
                                               .children = std::move(children)});
                }
            }
            items.push_back(UiMenuItem{.separator = true});
            items.push_back(UiMenuItem{
                .id = InternedString(LAYOUT_ITEM), .label = tr("menu.window.layout"), .children = layoutItems()});
            items.push_back(UiMenuItem{.id = InternedString(RESET_ITEM), .label = tr("menu.window.reset")});
            items.push_back(UiMenuItem{.id = InternedString(TIME_SCALE_ITEM),
                                       .label = tr("menu.window.timescale"),
                                       .enabled = engine->configStore() != nullptr,
                                       .children = timeScaleItems()});
            if (const Window* window = engine->window())
            {
                items.push_back(UiMenuItem{.id = InternedString(FULLSCREEN_ITEM),
                                           .label = tr("menu.window.fullscreen"),
                                           .checkable = true,
                                           .checked = window->isFullscreen()});
            }
            return items;
        }

        // The time scale presets, the one in force checked; 1x is the reset.
        std::vector<UiMenuItem> timeScaleItems() const
        {
            std::vector<UiMenuItem> items;
            const double now = engine->config().timing.timeScale;
            for (u32 i = 0; i < EditorTimeScale::PRESETS.size(); ++i)
            {
                const double preset = EditorTimeScale::PRESETS[i];
                items.push_back(UiMenuItem{.id = InternedString(std::string(TIME_SCALE_PREFIX) + std::to_string(i)),
                                           .label = preset == 0.0 ? tr("menu.window.timescale.frozen")
                                               : preset == 1.0    ? tr("menu.window.timescale.normal")
                                                                  : std::format("{:g}x", preset),
                                           .checkable = true,
                                           .checked = static_cast<float>(preset) == static_cast<float>(now)});
            }
            return items;
        }

        // Built-in and saved layouts, with save and delete actions.
        std::vector<UiMenuItem> layoutItems() const
        {
            std::vector<UiMenuItem> items;
            for (const std::string_view name : EditorLayoutStore::BUILT_IN)
            {
                std::string key(name);
                std::ranges::transform(key, key.begin(),
                                       [](const char c) { return static_cast<char>(std::tolower(c)); });
                items.push_back(UiMenuItem{.id = InternedString(std::string(LAYOUT_LOAD_PREFIX) + std::string(name)),
                                           .label = tr("menu.window.layout." + key)});
            }
            const std::vector<std::string> saved = savedLayouts();
            if (!saved.empty())
            {
                items.push_back(UiMenuItem{.separator = true});
            }
            for (const std::string& name : saved)
            {
                items.push_back(
                    UiMenuItem{.id = InternedString(std::string(LAYOUT_LOAD_PREFIX) + name), .label = name});
            }
            items.push_back(UiMenuItem{.separator = true});
            items.push_back(UiMenuItem{.id = InternedString(LAYOUT_SAVE_ITEM),
                                       .label = tr("menu.window.layout.save"),
                                       .enabled = !desc.layoutFile.empty()});
            UiMenuItem remove{.id = InternedString(LAYOUT_DELETE_ITEM),
                              .label = tr("menu.window.layout.delete"),
                              .enabled = !saved.empty()};
            for (const std::string& name : saved)
            {
                remove.children.push_back(
                    UiMenuItem{.id = InternedString(std::string(LAYOUT_DELETE_PREFIX) + name), .label = name});
            }
            items.push_back(std::move(remove));
            return items;
        }

        // Opens the bar's menu on the shared popup below its button: a press on the button, or Left and Right inside
        // an open menu, which walk the bar in order.
        void openBarMenu(const u32 index)
        {
            barMenu = index;
            switch (index)
            {
            case 0:
                menu.open(fileMenu->items(), file);
                break;
            case 1:
                menu.open(editItems(), edit);
                break;
            case 2:
                menu.open(windowItems(), window);
                break;
            default:
                menu.open(helpItems(), help);
                break;
            }
        }

        std::vector<UiMenuItem> editItems() const
        {
            // a label and its first chord in two columns, as the File menu's (the chord follows a rebinding)
            std::array<std::string, EDIT_ITEMS.size()> labels;
            std::array<std::string, EDIT_ITEMS.size()> chords;
            sizet column = 0;
            for (sizet i = 0; i < EDIT_ITEMS.size(); ++i)
            {
                labels[i] = tr("menu." + std::string(EDIT_ITEMS[i].id));
                if (const std::vector<std::string> found = actions->shortcuts(EDIT_ITEMS[i].action); !found.empty())
                {
                    chords[i] = found.front();
                    column = std::max(column, codepoints(labels[i]));
                }
            }
            std::vector<UiMenuItem> items;
            for (sizet i = 0; i < EDIT_ITEMS.size(); ++i)
            {
                if (EDIT_ITEMS[i].action == EditorActions::Action::Copy)
                {
                    items.push_back(UiMenuItem{.separator = true});
                }
                std::string label = std::move(labels[i]);
                if (!chords[i].empty())
                {
                    label.append(column - codepoints(label) + 3, ' ');
                    label += chords[i];
                }
                items.push_back(UiMenuItem{.id = InternedString(EDIT_ITEMS[i].id),
                                           .label = std::move(label),
                                           .enabled = actions->enabled(EDIT_ITEMS[i].action)});
            }
            return items;
        }

        std::vector<UiMenuItem> helpItems() const
        {
            return {UiMenuItem{.id = InternedString(PALETTE_ITEM), .label = tr("menu.help.palette")},
                    UiMenuItem{.id = InternedString(SHORTCUTS_ITEM), .label = tr("menu.help.shortcuts")}};
        }

        // the gizmo mode buttons' tooltips show their hotkeys; read again when the bindings change
        void showModeHints()
        {
            constexpr std::array<EditorActions::Action, 3> MODE_HOTKEYS{EditorActions::Action::GizmoTranslate,
                                                                        EditorActions::Action::GizmoRotate,
                                                                        EditorActions::Action::GizmoScale};
            seenBindings = actions->bindingRevision();
            for (u32 i = 0; i < gizmoModes.size(); ++i)
            {
                std::string chords;
                for (const std::string& chord : actions->shortcuts(MODE_HOTKEYS[i]))
                {
                    chords += (chords.empty() ? "" : ", ") + chord;
                }
                panelui::setTip(gizmoModes[i], chords);
            }
        }

        // The sheet of the editor's hotkeys: every action with a chord in the input system now (generated from the
        // hotkeys file, so a rebinding shows), in two monospace columns.
        void showShortcuts()
        {
            const auto width = codepoints;
            std::vector<std::pair<std::string, std::string>> rows;
            sizet column = 0;
            for (u32 i = 0; i < static_cast<u32>(EditorActions::Action::Count); ++i)
            {
                const std::vector<std::string> chords = actions->shortcuts(static_cast<EditorActions::Action>(i));
                if (chords.empty())
                {
                    continue;
                }
                std::string chordText;
                for (const std::string& chord : chords)
                {
                    chordText += (chordText.empty() ? "" : ", ") + chord;
                }
                std::string label = tr("shortcut." + std::string(EditorActions::ACTION_NAMES[i]));
                column = std::max(column, width(label));
                rows.emplace_back(std::move(label), std::move(chordText));
            }
            std::string body;
            for (const auto& [label, chords] : rows)
            {
                body += (body.empty() ? "" : "\n") + label + std::string(column - width(label) + 3, ' ') + chords;
            }
            dialogs->notice({.title = tr("menu.help.shortcuts"), .body = std::move(body)});
        }

        void activateItem(const InternedString item)
        {
            if (const auto edit = std::ranges::find(EDIT_ITEMS, item.toView(), &EditItem::id); edit != EDIT_ITEMS.end())
            {
                actions->request(edit->action);
                return;
            }
            if (fileMenu->activate(item))
            {
                return;
            }
            if (item.toView() == SHORTCUTS_ITEM)
            {
                showShortcuts();
                return;
            }
            if (item.toView() == PALETTE_ITEM)
            {
                palette->open();
                return;
            }
            activateWindowItem(item);
        }

        void activateWindowItem(const InternedString item)
        {
            if (activateLayoutItem(item.toView()))
            {
                return;
            }
            if (item.toView() == RESET_ITEM)
            {
                pending.emplace_back(R"({"command":"resetLayout"})");
                return;
            }
            if (item.toView() == FULLSCREEN_ITEM)
            {
                const bool fullscreen = engine->window() && engine->window()->isFullscreen();
                pending.push_back(std::format(R"({{"command":"setWindow","args":{{"fullscreen":{}}}}})", !fullscreen));
                return;
            }
            if (item.toView() == TIME_SCALE_ITEM)
            {
                return; // a submenu opens
            }
            if (item.toView().starts_with(TIME_SCALE_PREFIX))
            {
                if (const sizet preset = std::stoul(std::string(item.toView().substr(TIME_SCALE_PREFIX.size())));
                    preset < EditorTimeScale::PRESETS.size())
                {
                    timeScale->set(EditorTimeScale::PRESETS[preset]);
                }
                return;
            }
            const std::string_view panel = item.toView().substr(std::string_view("window.").size());
            pending.push_back(std::format(R"({{"command":"dock","args":{{"kind":"show","panel":"{}"}}}})", panel));
        }

        // the dock's detached floating windows, each in an OS window of its own that the editor opened for
        // it, its root surface bound to that window. Each frame the two follow each other: a dock op that moved the
        // window moves the OS window, the OS frame that moved or resized it moves the dock window (a dock op), and
        // closing it closes its panels. A window the platform refuses floats back into the dock space.
        struct DetachedWindow
        {
            Entity root;
            WindowId window = INVALID_WINDOW_ID;
            Vec2 position; // the dock window's rect as last synced, in surface units
            Vec2 size;
            WindowPosition osPosition; // the OS window's as last synced
            WindowExtent osExtent;
        };
        std::vector<DetachedWindow> detachedWindows;
        // while a tab drag would detach its panel: a borderless OS window where it would open, showing the ghost
        // root (editor.dock.ghost); closed when the drag lands or leaves the outside
        WindowId ghostWindow = INVALID_WINDOW_ID;
        Panel ghost;

        void syncDetached()
        {
            const Window* main = engine->window();
            UiHierarchyManager& hierarchy = editorUi(*engine).ui().hierarchy();
            const std::vector<UiDetachedWindow> now = dock.detachedWindows();
            for (auto it = detachedWindows.begin(); it != detachedWindows.end();)
            {
                if (std::ranges::any_of(now, [&](const UiDetachedWindow& window) { return window.root == it->root; }))
                {
                    ++it;
                    continue;
                }
                hierarchy.bindRootToWindow(it->root, 0);
                engine->closeWindow(it->window);
                it = detachedWindows.erase(it);
            }
            const float scale = hierarchy.layoutScale();
            const float pixels = main && main->getLogicalExtent().width > 0
                ? static_cast<float>(main->getFramebufferExtent().width) /
                    static_cast<float>(main->getLogicalExtent().width)
                : 1.0f;
            const WindowPosition origin = main ? main->getPosition() : WindowPosition{};
            // surface units to the desktop's window units and back
            const auto toDesktop = [&](const Vec2 at)
            {
                return WindowPosition{origin.x + static_cast<i32>(std::lround(at.x * scale / pixels)),
                                      origin.y + static_cast<i32>(std::lround(at.y * scale / pixels))};
            };
            const auto toExtent = [&](const Vec2 size)
            {
                return WindowExtent{static_cast<u32>(std::max(1l, std::lround(size.x * scale / pixels))),
                                    static_cast<u32>(std::max(1l, std::lround(size.y * scale / pixels)))};
            };
            const auto inSpace = [&](const UiDetachedWindow& window)
            {
                UiDockOp op = dockOp(UiDockOpKind::Float, window.panel.toView());
                op.position = Vec2{40.0f, 40.0f};
                op.size = window.size;
                pending.push_back(dockRequest(op));
            };
            syncGhost(hierarchy, toDesktop, toExtent, origin, pixels);
            for (const UiDetachedWindow& window : now)
            {
                auto it = std::ranges::find_if(detachedWindows,
                                               [&](const DetachedWindow& open) { return open.root == window.root; });
                if (it == detachedWindows.end())
                {
                    const WindowId id = main
                        ? engine->openWindow(SecondaryWindowDesc{.title = InternedString("SPITE"),
                                                                 .position = toDesktop(window.position),
                                                                 .extent = toExtent(window.size)})
                        : INVALID_WINDOW_ID;
                    if (id == INVALID_WINDOW_ID)
                    {
                        inSpace(window); // headless, or the platform or the GPU refused it
                        continue;
                    }
                    hierarchy.bindRootToWindow(window.root, id);
                    // synced as asked: where the OS put it elsewhere (a window kept on screen), the dock follows
                    detachedWindows.push_back({window.root, id, window.position, window.size,
                                               toDesktop(window.position), toExtent(window.size)});
                    it = std::prev(detachedWindows.end());
                }
                Window* os = engine->secondaryWindow(it->window);
                if (!os)
                {
                    continue;
                }
                if (os->isCloseRequested())
                {
                    for (const Built& built : panels)
                    {
                        const InternedString name(built.desc.name);
                        for (Entity host = dock.panelHost(name); host != Entity::undefined();
                             host = hierarchy.parentOf(host))
                        {
                            if (host == window.root)
                            {
                                pending.push_back(dockRequest(dockOp(UiDockOpKind::Close, name.toView())));
                                break;
                            }
                        }
                    }
                    continue;
                }
                // Minimized frames can report offscreen coordinates; preserve the dock's last rectangle.
                if (os->isMinimized())
                    continue;
                const WindowPosition at = os->getPosition();
                const WindowExtent extent = os->getLogicalExtent();
                if (window.position != it->position || window.size != it->size)
                {
                    // the dock moved it: the OS window follows
                    os->setPosition(toDesktop(window.position));
                    const WindowExtent wanted = toExtent(window.size);
                    os->setExtent(wanted.width, wanted.height);
                    it->position = window.position;
                    it->size = window.size;
                    it->osPosition = toDesktop(window.position);
                    it->osExtent = wanted;
                }
                else if (at.x != it->osPosition.x || at.y != it->osPosition.y || extent.width != it->osExtent.width ||
                         extent.height != it->osExtent.height)
                {
                    // the OS frame moved or resized it: the dock window follows
                    UiDockOp op = dockOp(UiDockOpKind::Move, window.panel.toView());
                    op.position = Vec2{static_cast<float>(at.x - origin.x), static_cast<float>(at.y - origin.y)} *
                        (pixels / scale);
                    op.size =
                        Vec2{static_cast<float>(extent.width), static_cast<float>(extent.height)} * (pixels / scale);
                    pending.push_back(dockRequest(op));
                    it->osPosition = at;
                    it->osExtent = extent;
                    it->position = op.position;
                    it->size = op.size;
                }
                const WindowExtent framebuffer = os->getFramebufferExtent();
                hierarchy.setWindowRect(
                    it->window,
                    UiRect{Vec2{static_cast<float>(at.x - origin.x), static_cast<float>(at.y - origin.y)} * pixels,
                           Vec2{static_cast<float>(framebuffer.width), static_cast<float>(framebuffer.height)}});
            }
        }

        template <class ToDesktop, class ToExtent>
        void syncGhost(UiHierarchyManager& hierarchy, const ToDesktop& toDesktop, const ToExtent& toExtent,
                       const WindowPosition origin, const float pixels)
        {
            const std::optional<UiRect> wanted = engine->window() ? dock.detachPreview() : std::nullopt;
            if (!wanted)
            {
                if (ghostWindow != INVALID_WINDOW_ID)
                {
                    hierarchy.bindRootToWindow(ghost.entity(), 0);
                    ghost.setVisible(false);
                    engine->closeWindow(ghostWindow);
                    ghostWindow = INVALID_WINDOW_ID;
                }
                return;
            }
            const WindowPosition at = toDesktop(wanted->position);
            const WindowExtent extent = toExtent(wanted->size);
            if (ghostWindow == INVALID_WINDOW_ID)
            {
                ghostWindow = engine->openWindow(SecondaryWindowDesc{
                    .title = InternedString("SPITE"), .position = at, .extent = extent, .ghost = true});
                if (ghostWindow == INVALID_WINDOW_ID)
                {
                    return;
                }
                if (!ghost.isValid())
                {
                    ghost = scope.createPanel("editor.dock.ghost", panelui::topLeft());
                    ghost.setInteractable(false);
                }
                ghost.setVisible(true);
                hierarchy.bindRootToWindow(ghost.entity(), ghostWindow);
            }
            Window* os = engine->secondaryWindow(ghostWindow);
            if (!os)
            {
                return;
            }
            if (const WindowPosition now = os->getPosition(); now.x != at.x || now.y != at.y)
            {
                os->setPosition(at);
            }
            if (const WindowExtent now = os->getLogicalExtent();
                now.width != extent.width || now.height != extent.height)
            {
                os->setExtent(extent.width, extent.height);
            }
            if (ghost.offset() != wanted->position || ghost.size() != wanted->size)
            {
                ghost.setOffset(wanted->position);
                ghost.setSize(wanted->size);
            }
            const WindowExtent framebuffer = os->getFramebufferExtent();
            hierarchy.setWindowRect(
                ghostWindow,
                UiRect{Vec2{static_cast<float>(at.x - origin.x), static_cast<float>(at.y - origin.y)} * pixels,
                       Vec2{static_cast<float>(framebuffer.width), static_cast<float>(framebuffer.height)}});
        }

        // Where a panel goes when it is shown: its tab if it is open, else the place it was closed from, else its
        // registered slot when the panel that slot docks against is open, else a window.
        UiDockOp showOp(const InternedString name) const
        {
            if (dock.isOpen(name))
            {
                return dockOp(UiDockOpKind::Activate, name.toView());
            }
            if (const std::optional<UiDockOp> home = dock.reopenOp(name))
            {
                return *home;
            }
            const auto registered =
                std::ranges::find(panels, name.toView(), [](const Built& b) { return std::string_view(b.desc.name); });
            if (registered != panels.end() && !registered->desc.dock.target.empty() &&
                dock.isOpen(InternedString(registered->desc.dock.target)))
            {
                const EditorDockPlacement& at = registered->desc.dock;
                return dockOp(UiDockOpKind::Dock, name.toView(), at.target, at.zone, at.ratio);
            }
            UiDockOp op = dockOp(UiDockOpKind::Float, name.toView());
            op.position = Vec2{120.0f, 80.0f};
            op.size = Vec2{420.0f, 320.0f};
            return op;
        }

        // The text of a named layout: Default is the registry's placements, Debug and Profiling assets, the rest the
        // user's. Applying it is the caller's: nothing here touches the layout in use.
        std::string layoutText(const std::string_view name, std::string& text) const
        {
            if (EditorLayoutStore::builtIn(name) == "Default")
            {
                text = defaultLayout;
                return {};
            }
            if (desc.layoutFile.empty() && EditorLayoutStore::builtIn(name).empty())
            {
                return "layout persistence is off (no layout file)";
            }
            return layouts->read(name, text);
        }

        std::vector<std::string> savedLayouts() const
        {
            return desc.layoutFile.empty() ? std::vector<std::string>{} : layouts->names();
        }

        std::string saveLayoutFile() const
        {
            if (desc.layoutFile.empty())
            {
                return "layout persistence is off (no layout file)";
            }
            if (desc.layoutFile.has_parent_path())
            {
                if (const auto made = ensureDirectory(desc.layoutFile.parent_path()); !made)
                {
                    return std::string(made.error().message.c_str());
                }
            }
            if (const auto written = writeTextFileAtomically(desc.layoutFile, dock.saveLayout()); !written)
            {
                return std::string(written.error().message.c_str());
            }
            return {};
        }

        // Runs a layout command by name and says how it went in a toast.
        void runLayoutCommand(const char* command, const std::string& name)
        {
            const Json reply =
                Json::parse(engine->control().handle(Json{{"command", command}, {"args", {{"name", name}}}}.dump(
                                -1, ' ', false, Json::error_handler_t::replace)),
                            nullptr, false);
            const bool ok = reply.is_object() && reply.value("ok", false);
            if (ok && std::string_view(command) == "loadLayout")
            {
                return; // the dock shows it
            }
            const std::string error = ok ? std::string{} : reply.value("error", std::string("refused"));
            feedback->postToastKey(ok ? ToastLevel::Info : ToastLevel::Error,
                                   ok ? std::string("editor.layout.") + command : "editor.layout.failed",
                                   std::vector<MessageArg>{{"name", name}, {"error", error}});
        }

        // The Layout submenu's items; false for any other. Nothing runs inside the menu's handler: a load waits for
        // update(), a save or delete for the person's answers.
        bool activateLayoutItem(const std::string_view id)
        {
            if (id == LAYOUT_ITEM || id == LAYOUT_DELETE_ITEM)
            {
                return true; // a submenu opens
            }
            if (id.starts_with(LAYOUT_LOAD_PREFIX))
            {
                deferred.push_back([this, name = std::string(id.substr(LAYOUT_LOAD_PREFIX.size()))]
                                   { runLayoutCommand("loadLayout", name); });
                return true;
            }
            if (id.starts_with(LAYOUT_DELETE_PREFIX))
            {
                const std::string name(id.substr(LAYOUT_DELETE_PREFIX.size()));
                const std::vector<MessageArg> args{{"name", name}};
                dialogs->confirm({.title = message("editor.layout.delete.title", args),
                                  .body = message("editor.layout.delete.body", args),
                                  .confirmLabel = tr("layout.delete.confirm"),
                                  .destructive = true},
                                 [this, name](const bool confirmed)
                                 {
                                     if (confirmed)
                                     {
                                         runLayoutCommand("deleteLayout", name);
                                     }
                                 });
                return true;
            }
            if (id != LAYOUT_SAVE_ITEM)
            {
                return false;
            }
            dialogs->prompt({.title = tr("layout.save.title"),
                             .body = message("editor.layout.save.body", {{"path", utf8Path(layouts->directory())}}),
                             .confirmLabel = tr("layout.save.confirm")},
                            [this](const std::optional<std::string>& text)
                            {
                                if (text)
                                {
                                    saveLayoutAs(*text);
                                }
                            });
            return true;
        }

        // A name the person typed: checked, then saved, after a confirmation when it replaces a layout.
        void saveLayoutAs(std::string name)
        {
            const auto blank = [](const char c) { return c == ' ' || c == '	'; };
            name.erase(name.begin(), std::ranges::find_if_not(name, blank));
            name.erase(std::ranges::find_if_not(name.rbegin(), name.rend(), blank).base(), name.end());
            if (const std::string error = EditorLayoutStore::checkName(name); !error.empty())
            {
                feedback->postToastKey(ToastLevel::Error, "editor.layout.failed",
                                       std::vector<MessageArg>{{"name", name}, {"error", error}});
                return;
            }
            const std::vector<std::string> saved = savedLayouts();
            const auto sameName = [&name](const std::string& other)
            {
                return std::ranges::equal(other, name,
                                          [](const char a, const char b)
                                          {
                                              return std::tolower(static_cast<unsigned char>(a)) ==
                                                  std::tolower(static_cast<unsigned char>(b));
                                          });
            };
            if (std::ranges::none_of(saved, sameName))
            {
                runLayoutCommand("saveLayout", name);
                return;
            }
            const std::vector<MessageArg> args{{"name", name}};
            dialogs->confirm({.title = message("editor.layout.replace.title", args),
                              .body = message("editor.layout.replace.body", args),
                              .confirmLabel = tr("layout.replace.confirm"),
                              .destructive = true},
                             [this, name](const bool confirmed)
                             {
                                 if (confirmed)
                                 {
                                     runLayoutCommand("saveLayout", name);
                                 }
                             });
        }

        void registerCommands()
        {
            ControlServer& control = engine->control();
            // the editor's panels send their requests in process
            control.setLocalSource(RequestSource::Editor);
            // not undoable: the layout is editor UI state, outside the undo
            // history and the command log (a splitter drag sends one per move)
            control.registerViewCommand(
                "dock", OWNER,
                "applies a dock op to an editor panel; float and move reply with the applied x, y, w, h and detached "
                "(whether the window is in an OS window of its own)",
                {requiredArg("kind", ArgKind::String,
                             "dock, float, move, close, activate, resize, maximize, restore or show (a closed panel "
                             "back where it was closed, else at its registered slot, else in a window; an open one "
                             "activates; the other args are ignored)",
                             KIND_NAMES),
                 requiredArg("panel", ArgKind::String, "the panel"),
                 optionalArg("target", ArgKind::String, "the panel docked against"),
                 optionalArg("zone", ArgKind::String, "center, left, right, top or bottom", "center", ZONE_NAMES),
                 optionalArg("ratio", ArgKind::Number, "a split's share"),
                 optionalArg("x", ArgKind::Number, "a floating panel's left (kept reachable in the dock space)"),
                 optionalArg("y", ArgKind::Number, "a floating panel's top (kept reachable in the dock space)"),
                 optionalArg("w", ArgKind::Number, "a floating panel's width (at most the dock space's)"),
                 optionalArg("h", ArgKind::Number, "a floating panel's height (at most the dock space's)"),
                 optionalArg("index", ArgKind::Number,
                             "a center dock's place among the target group's tabs (default: last)"),
                 optionalArg("detached", ArgKind::Boolean,
                             "float: the window goes into an OS window of its own (x, y in the UI's surface "
                             "units, not clamped to the dock space), or back into the dock space (false); move "
                             "keeps it as it is")},
                [this](const ControlArgs& args, IFormatWriter& out)
                {
                    UiDockOp op;
                    u8 kind = 0;
                    u8 zone = 0;
                    if (!named(KIND_NAMES, args.string("kind"), kind))
                    {
                        return std::string(
                            "dock needs kind dock|float|move|close|activate|resize|maximize|restore|show");
                    }
                    if (args.json("zone") && !named(ZONE_NAMES, args.string("zone"), zone))
                    {
                        return std::string("dock zone is center|left|right|top|bottom");
                    }
                    const std::optional<std::string> panel = args.string("panel");
                    if (!panel || panel->empty())
                    {
                        return std::string("dock needs panel");
                    }
                    op.kind = static_cast<UiDockOpKind>(kind);
                    op.zone = static_cast<UiDockZone>(zone);
                    op.panel = InternedString(*panel);
                    if (const std::optional<std::string> target = args.string("target"); target && !target->empty())
                    {
                        op.target = InternedString(*target);
                    }
                    if (!number(args, "ratio", op.ratio) || !number(args, "x", op.position.x) ||
                        !number(args, "y", op.position.y) || !number(args, "w", op.size.x) ||
                        !number(args, "h", op.size.y))
                    {
                        return std::string("dock ratio, x, y, w and h are numbers");
                    }
                    if (args.json("index"))
                    {
                        float index = -1.0f;
                        if (!number(args, "index", index) || !(index >= 0.0f) || index != std::floor(index) ||
                            index >= static_cast<float>(UiDockOp::LAST))
                        {
                            return std::string("dock index is a whole number from 0");
                        }
                        op.index = static_cast<u32>(index);
                    }
                    op.detached = args.boolean("detached").value_or(false);
                    if (kind == SHOW_KIND)
                    {
                        op = showOp(op.panel);
                    }
                    if (!dock.apply(op))
                    {
                        return std::string("dock op refused: unknown panel or target, closed "
                                           "target, malformed values or a no-op");
                    }
                    if (kind == SHOW_KIND)
                    {
                        const u64 window = scope.hierarchy().windowOf(dock.panelHost(op.panel));
                        if (Window* detached = engine->secondaryWindow(window))
                        {
                            if (detached->isMinimized())
                                detached->setState(WindowState::Normal);
                            detached->raise();
                        }
                    }
                    if (op.kind == UiDockOpKind::Float || op.kind == UiDockOpKind::Move)
                    {
                        const UiRect window = dock.floatingRect(op.panel); // as clamped to the dock space
                        out.writeF32("x", window.position.x);
                        out.writeF32("y", window.position.y);
                        out.writeF32("w", window.size.x);
                        out.writeF32("h", window.size.y);
                        out.writeBool("detached", dock.isDetached(op.panel));
                    }
                    return std::string{};
                });
            control.registerQuery("dockLayout", OWNER, "the dock layout and the file it saves to", {},
                                  [this](const ControlArgs&, IFormatWriter& out)
                                  {
                                      out.writeString("layout", dock.saveLayout());
                                      out.writeString("file", utf8Path(desc.layoutFile));
                                      return std::string{};
                                  });
            control.registerQuery(
                "editorPanels", OWNER,
                "the registered editor panels in menu order: name, order, title, whether the dock has it open and "
                "shows it (its tab is the active one), the default layout's placement (target, zone, ratio, after) "
                "and the render view it shows (-1: none)",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    const UiHierarchyManager& hierarchy = scope.hierarchy();
                    out.beginArray("panels", static_cast<u32>(panels.size()));
                    for (const Built& built : panels)
                    {
                        const InternedString name(built.desc.name);
                        const Entity host = dock.panelHost(name);
                        const bool open = dock.isOpen(name);
                        out.beginObject("");
                        out.writeString("name", built.desc.name);
                        out.writeI32("order", built.desc.order);
                        out.writeString("title", title(built.desc));
                        out.writeBool("open", open);
                        out.writeBool("visible", open && host.isValid() && hierarchy.isVisibleOf(host));
                        out.beginObject("dock");
                        out.writeString("target", built.desc.dock.target);
                        out.writeString("zone", ZONE_NAMES[static_cast<u8>(built.desc.dock.zone)]);
                        out.writeF32("ratio", built.desc.dock.ratio);
                        out.writeString("after", built.desc.dock.after);
                        out.endObject();
                        out.writeI32("view", built.desc.view ? static_cast<i32>(*built.desc.view) : -1);
                        out.endObject();
                    }
                    out.endArray();
                    return std::string{};
                });
            control.registerViewCommand(
                "saveLayout", OWNER,
                "writes the dock layout to its file, or under name to the layouts folder beside it (a built-in's name "
                "is refused; a user layout of that name is replaced)",
                {optionalArg("name", ArgKind::String, "a layout name", "the layout file")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const std::optional<std::string> name = args.string("name");
                    if (!name)
                    {
                        return saveLayoutFile();
                    }
                    return desc.layoutFile.empty() ? std::string("layout persistence is off (no layout file)")
                                                   : layouts->write(*name, dock.saveLayout());
                });
            control.registerViewCommand(
                "loadLayout", OWNER,
                "applies a dock layout: the layout file, a named layout (Default, Debug, Profiling or a saved one) or "
                "a layout JSON; a layout that is not one, or names no panel of the editor, leaves the layout as it is",
                {optionalArg("name", ArgKind::String, "a layout name, as the layouts query lists them",
                             "the layout file"),
                 optionalArg("layout", ArgKind::Json, "a layout, as dockLayout gives it", "the layout file")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const std::optional<std::string> name = args.string("name");
                    std::optional<std::string> text = args.string("layout");
                    if (!text)
                    {
                        text = args.json("layout");
                    }
                    if (name && text)
                    {
                        return std::string("loadLayout takes a name or a layout, not both");
                    }
                    if (name)
                    {
                        std::string preset;
                        if (std::string error = layoutText(*name, preset); !error.empty())
                        {
                            return error;
                        }
                        text = std::move(preset);
                    }
                    if (!text)
                    {
                        if (desc.layoutFile.empty())
                        {
                            return std::string("layout persistence is off (no layout file)");
                        }
                        Expected<std::string> file = readTextFile(desc.layoutFile);
                        if (!file)
                        {
                            return std::string(file.error().message.c_str());
                        }
                        text = std::move(*file);
                    }
                    return dock.loadLayout(*text)
                        ? std::string{}
                        : std::string(name ? *name + " is not a dock layout of this editor" : "not a dock layout");
                });
            control.registerViewCommand(
                "deleteLayout", OWNER, "deletes a saved layout; the built-in layouts stay",
                {requiredArg("name", ArgKind::String, "a saved layout, as the layouts query lists them")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const std::optional<std::string> name = args.string("name");
                    return desc.layoutFile.empty() ? std::string("layout persistence is off (no layout file)")
                                                   : layouts->remove(name.value_or(std::string{}));
                });
            control.registerQuery("layouts", OWNER,
                                  "the named layouts: the built-in ones, the saved ones and the folder they are in", {},
                                  [this](const ControlArgs&, IFormatWriter& out)
                                  {
                                      out.writeString("file", utf8Path(desc.layoutFile));
                                      out.writeString("directory", utf8Path(layouts->directory()));
                                      out.beginArray("builtIn", static_cast<u32>(EditorLayoutStore::BUILT_IN.size()));
                                      for (const std::string_view name : EditorLayoutStore::BUILT_IN)
                                      {
                                          out.writeString("", name);
                                      }
                                      out.endArray();
                                      const std::vector<std::string> saved = savedLayouts();
                                      out.beginArray("saved", static_cast<u32>(saved.size()));
                                      for (const std::string& name : saved)
                                      {
                                          out.writeString("", name);
                                      }
                                      out.endArray();
                                      return std::string{};
                                  });
            // not undoable: focus is editor UI state, outside the history and the log
            control.registerViewCommand(
                "gameFocus", OWNER,
                "gives or takes the game's input focus, as a press on the Game panel or elsewhere "
                "does",
                {requiredArg("on", ArgKind::Boolean, "true focuses the shown Game panel")},
                [this](const ControlArgs& args, IFormatWriter& out)
                {
                    const bool on = args.boolean("on").value_or(false);
                    if (std::string error = setGameFocus(on); !error.empty())
                    {
                        return error;
                    }
                    out.writeBool("on", on);
                    return std::string{};
                });
            control.registerViewCommand(
                "showGameUiPreview", OWNER, "shows the live Game view alongside the UI Documents panel during Play", {},
                [this](const ControlArgs&, IFormatWriter&)
                {
                    if (!editorViews || !engine->playWorld())
                    {
                        return std::string("Start Play with the editor views enabled to preview the game UI");
                    }
                    const UiHierarchyManager& hierarchy = scope.hierarchy();
                    const auto visible = [&](const InternedString panel)
                    {
                        const Entity host = dock.panelHost(panel);
                        return host.isValid() && hierarchy.isVisibleOf(host);
                    };
                    const InternedString documents("uidocuments");
                    const InternedString game("game");
                    if (!visible(documents))
                    {
                        return std::string("Show UI Documents before opening its live preview");
                    }
                    if (visible(game))
                    {
                        return std::string{};
                    }
                    if (dock.isMaximized(documents))
                    {
                        dock.apply(dockOp(UiDockOpKind::Restore, documents.toView()));
                    }
                    if (!dock.apply(showOp(game)))
                    {
                        return std::string("Could not show the Game panel");
                    }
                    if (hierarchy.parentOf(dock.panelHost(game)) == hierarchy.parentOf(dock.panelHost(documents)) &&
                        !dock.apply(dockOp(UiDockOpKind::Dock, game.toView(), documents.toView(), UiDockZone::Right)))
                    {
                        dock.apply(showOp(documents));
                        return std::string("Could not show Game beside UI Documents");
                    }
                    return std::string{};
                });
            control.registerViewCommand("resetLayout", OWNER, "applies the default dock layout", {},
                                        [this](const ControlArgs&, IFormatWriter&)
                                        {
                                            [[maybe_unused]] const bool loaded = dock.loadLayout(defaultLayout);
                                            SASSERTM(loaded, "EditorShell: the default layout does not load.");
                                            feedback->postToastKey(ToastLevel::Info, "editor.layout.reset", {}, 2.0);
                                            return std::string{};
                                        });
        }

        // The shell document view: the document owns the chrome's containers and where they sit, code the
        // slots' content. A slot's childClass styles the buttons in it; the rest of the content is placed by the
        // slot's container (the dock and the status bar fill it).
        void openView()
        {
            const auto buttons = [this](std::vector<Widget> items, const sizet icons = 0)
            {
                return [this, items = std::move(items), icons](const Widget& slot)
                {
                    const InternedString styleClass = panelui::slotFields(scope, slot).childClass;
                    for (sizet i = 0; i < items.size(); ++i)
                    {
                        Widget item = items[i];
                        panelui::putIn(scope, slot, item);
                        panelui::setOwnClass(item, i < icons ? InternedString("toolbar.icon") : styleClass);
                    }
                    synced = false;
                };
            };
            const auto content = [this](const Widget& item)
            { return [this, item](const Widget& slot) { panelui::putIn(scope, slot, item); }; };
            UiViewContract contract;
            contract.slot(InternedString(MENUS_SLOT), buttons({file, edit, window}));
            contract.optional(InternedString(CENTER_SPACER), UiWidgetKind::Panel,
                              [this](const Widget& spacer) { centerSpacer = spacer; });
            contract.slot(InternedString(TRANSPORT_SLOT),
                          [this, attach = buttons({play, pause, step, stop}, 4)](const Widget& slot)
                          {
                              attach(slot);
                              panelui::putIn(scope, slot, playCounter->root());
                              panelui::putIn(scope, slot, timeScaleMark);
                              panelui::putIn(scope, slot, timeScale->field());
                          });
            // the frame readout sits before Help
            contract.slot(InternedString(END_SLOT),
                          [this](const Widget& slot)
                          {
                              panelui::putIn(scope, slot, paletteField);
                              paletteField.setStyleClass(panelui::slotFields(scope, slot).childClass);
                              panelui::putIn(scope, slot, statusBar->jobs());
                              panelui::putIn(scope, slot, frameHeader->root());
                              panelui::putIn(scope, slot, help);
                              help.setStyleClass(InternedString("toolbar.menu"));
                              panelui::putIn(scope, slot, agent->chip());
                          });
            contract.slot(InternedString(TINT_SLOT), content(barTint));
            contract.slot(InternedString(DOCK_SLOT), content(dock));
            contract.fallback([](UiScope& viewScope, const Widget& shellRoot) { buildFallback(viewScope, shellRoot); });
            view = panelui::openView(*engine, VIEW, "assets/ui/editor/shell.json", std::move(contract), host);
        }

        // The shipped document's containers, for a document that is missing or refused.
        static void buildFallback(UiScope& scope, const Widget& shellRoot)
        {
            using panelui::stack;
            constexpr UiLayoutAlign STRETCH = UiLayoutAlign::Stretch;
            struct Spec
            {
                std::string_view id;
                Vec2 size{};
                std::optional<UiLayoutComponent> layout;
                UiLayoutItemComponent item = panelui::fixedItem();
                bool fitWidth = false;
                bool fitHeight = false;
                bool slot = false;
                bool interactable = true;
                std::string_view childClass;
            };
            const auto make = [&scope](const Widget& parent, Spec spec)
            {
                UiRectDesc desc = panelui::barDesc(spec.size, spec.layout);
                desc.layoutItem = spec.item;
                if (spec.fitWidth || spec.fitHeight)
                {
                    desc.contentSize = UiContentSizeComponent{.fitWidth = spec.fitWidth, .fitHeight = spec.fitHeight};
                }
                Panel panel = scope.createPanel(InternedString(spec.id), desc);
                panel.setParent(parent);
                panel.setInteractable(spec.interactable);
                if (spec.slot)
                {
                    UiSlotComponent slot;
                    slot.childClass = InternedString(spec.childClass);
                    scope.world().addComponent<UiSlotComponent>(panel.entity(), slot);
                }
                return panel;
            };
            const UiLayoutComponent column = stack(UiLayoutDirection::Column, 0.0f, {}, STRETCH);
            const UiLayoutItemComponent grow = panelui::fillItem();
            const UiLayoutComponent buttons = stack(UiLayoutDirection::Row, 4.0f);

            Widget root = shellRoot;
            root.setPivot(Vec2{0.0f, 0.0f});
            root.setInteractable(false);
            root.setLayout(column);
            root.setLayoutItem(grow);
            // the bar is as tall as its lines: the groups wrap, each whole, when the window or the text needs it
            Panel bar = make(root, {.id = "editor.bar", .layout = column, .fitHeight = true});
            bar.setLandmark(true);
            // the tint strip comes first: a control's focus ring (3 px outside it) then falls on the controls' own
            // line, below it, and over the strip above it, never under a later sibling
            make(bar, {.id = TINT_SLOT, .size = {0.0f, 3.0f}, .layout = column, .slot = true});
            UiLayoutComponent line = stack(UiLayoutDirection::Row, 0.0f, Vec4{0.0f, 0.0f, 0.0f, 4.0f}, STRETCH);
            line.wrap = true;
            const Panel controls = make(bar, {.id = "editor.bar.controls", .layout = line, .fitHeight = true});
            panelui::bindGap(controls, panelui::SPACE_NONE);
            panelui::bindPadding(controls, panelui::SPACE_NONE, panelui::SPACE_NONE, panelui::SPACE_NONE,
                                 panelui::SPACE_XS);
            // the wordmark: the accent block (as high as a control) and the name, centred on the line
            const Panel brand = make(
                controls,
                {.id = "shell.toolbar.brand",
                 .layout = stack(UiLayoutDirection::Row, 6.0f, Vec4{4.0f, 1.0f, 12.0f, 0.0f}, UiLayoutAlign::Center),
                 .fitWidth = true,
                 .fitHeight = true});
            panelui::bindGap(brand, panelui::SPACE_MD);
            panelui::bindPadding(brand, panelui::SPACE_XS, panelui::SPACE_HAIRLINE, panelui::SPACE_XL,
                                 panelui::SPACE_NONE);
            UiLayoutItemComponent markItem = panelui::fixedItem();
            markItem.minSize = Vec2{8.0f, 30.0f};
            Panel mark = make(brand, {.id = "shell.toolbar.brand.mark", .item = markItem});
            mark.setStyleClass(InternedString("brand.mark"));
            mark.bindLayoutToken(UiLayoutField::MinWidth, InternedString(panelui::SPACE_LG));
            mark.bindLayoutToken(UiLayoutField::MinHeight, InternedString(panelui::SIZE_CONTROL));
            UiRectDesc nameDesc = panelui::barDesc(Vec2{});
            nameDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
            Label name = scope.createLabel("shell.toolbar.brand.name", "", nameDesc);
            name.setParent(brand);
            name.setLocalizedText(InternedString("editor.bar.brand"));
            name.setStyleClass(InternedString("brand.name"));
            // the groups are items of the one wrapping line, each whole: the menus, the gizmo group (its gap before
            // it), the transport and the end group, so what the end group holds never moves the groups before it
            const auto group = [&](const std::string_view id, const bool spaced, const std::string_view childClass = {})
            {
                const Vec4 padding{id == TRANSPORT_SLOT ? 0.0f : spaced ? 16.0f : 4.0f, 1.0f, 0.0f, 0.0f};
                const Panel panel = make(controls,
                                         {.id = id,
                                          .layout = stack(UiLayoutDirection::Row, 4.0f, padding),
                                          .fitWidth = true,
                                          .fitHeight = true,
                                          .slot = true,
                                          .childClass = childClass});
                panelui::bindGap(panel, id == TRANSPORT_SLOT ? panelui::SPACE_SM : panelui::SPACE_XS);
                panelui::bindPadding(panel,
                                     id == TRANSPORT_SLOT ? panelui::SPACE_NONE
                                         : spaced         ? panelui::SPACE_XL
                                                          : panelui::SPACE_XS,
                                     panelui::SPACE_HAIRLINE, panelui::SPACE_NONE, panelui::SPACE_NONE);
            };
            group(MENUS_SLOT, false, "toolbar.menu");
            UiLayoutItemComponent spacerItem;
            spacerItem.basis = 0.0f;
            spacerItem.shrink = 0.0f;
            make(controls, {.id = CENTER_SPACER, .item = spacerItem, .interactable = false});
            group(TRANSPORT_SLOT, false);
            UiLayoutComponent end = buttons;
            end.justify = UiLayoutJustify::End;
            end.padding = Vec4{0.0f, 1.0f, 4.0f, 0.0f};
            UiLayoutItemComponent endItem = grow;
            endItem.basis = -1.0f;
            const Panel endGroup = make(
                controls,
                {.id = END_SLOT, .layout = end, .item = endItem, .fitWidth = true, .fitHeight = true, .slot = true});
            panelui::bindGap(endGroup, panelui::SPACE_XS);
            panelui::bindPadding(endGroup, panelui::SPACE_NONE, panelui::SPACE_HAIRLINE, panelui::SPACE_XS,
                                 panelui::SPACE_NONE);
            make(root, {.id = DOCK_SLOT, .layout = column, .item = grow, .slot = true, .interactable = false});
        }

        // A toolbar toggle shows its state as the shared selected class (panelui::showSelected) and as the selected
        // state uiTree reads, never in the text.
        void showToggle(Widget button, const bool on) { panelui::showSelected(button, on); }

        // The step field has a fixed width, that of a typical reading (three integer digits and three decimals; the
        // range reaches 1e6, which clips), measured again when the text scale or the stylesheet changes, so editing it
        // does not move the buttons after it.
        void fitStepField()
        {
            const UiTextMeasurer* measurer = scope.hierarchy().textMeasurer();
            const float textScale = engine->uiDisplay().textScale;
            const u64 sheet = editorUi(*engine).uiView().stylesheet().generation();
            if (!measurer || (textScale == stepTextScale && sheet == stepSheet))
            {
                return;
            }
            stepTextScale = textScale;
            stepSheet = sheet;
            UiLayoutItemComponent item = scope.world().getConstComponent<UiLayoutItemComponent>(gizmoStep.entity());
            item.minSize.x =
                std::ceil(measurer->measureText(gizmoStep.entity(), "000.000").x) + 2.0f * density.density().xs;
            item.maxSize.x = item.minSize.x;
            gizmoStep.setLayoutItem(item);
        }

        void syncGizmoBar()
        {
            fitStepField();
            const GizmoSettings& chosen = EditorModelAccess::gizmo(model);
            gizmoStep.setVisible(chosen.snap);
            gizmoUnit.setVisible(chosen.snap);
            const u32 shown = static_cast<u32>(chosen.mode) | static_cast<u32>(chosen.space) << 2 |
                static_cast<u32>(chosen.snap) << 3;
            const f32 step = chosen.step(chosen.mode);
            if (shown == gizmoShown && step == stepShown)
            {
                return;
            }
            gizmoShown = shown;
            stepShown = step;
            gizmoStep.setNumber(step);
            gizmoStep.setAccessibleNameKey(
                InternedString(std::format("editor.gizmo.step.{}", GIZMO_LABELS[static_cast<u32>(chosen.mode)])));
            gizmoUnit.setLocalizedText(
                InternedString(std::format("editor.gizmo.unit.{}", GIZMO_LABELS[static_cast<u32>(chosen.mode)])));
            for (u32 i = 0; i < gizmoModes.size(); ++i)
            {
                showToggle(gizmoModes[i], i == static_cast<u32>(chosen.mode));
            }
            const bool local = chosen.space == GizmoSpace::Local;
            gizmoSpace.setIcon(GIZMO_SPACE_ICONS[local ? 1 : 0]);
            showToggle(gizmoSpace, local);
            showToggle(gizmoSnap, chosen.snap);
        }

        // The panel's place among its tab group's tabs; LAST while it is closed.
        u32 tabIndexOf(const std::string_view panel) const
        {
            const Entity host = dock.panelHost(InternedString(panel));
            if (!host.isValid())
            {
                return UiDockOp::LAST;
            }
            const World& world = editorUi(*engine).world();
            const auto* group = world.tryGetConstComponent<UiDockNodeComponent>(scope.hierarchy().parentOf(host));
            if (!group || !dock.isOpen(InternedString(panel)))
            {
                return UiDockOp::LAST;
            }
            return static_cast<u32>(std::ranges::find(group->tabs, host) - group->tabs.begin());
        }

        // Width needed by the end group, including its right padding and gaps between visible items.
        float endWidth(const bool search, const float scale, const float gap) const
        {
            const UiHierarchyManager& hierarchy = scope.hierarchy();
            float width = hierarchy.absoluteRectOf(help.entity()).size.x / scale;
            width += hierarchy.absoluteRectOf(agent->chip().entity()).size.x / scale;
            u32 items = 2;
            if (statusBar->frameOver())
            {
                width += frameHeader->width(false);
                ++items;
            }
            if (statusBar->jobs().isVisible())
            {
                width += hierarchy.absoluteRectOf(statusBar->jobs().entity()).size.x / scale;
                ++items;
            }
            if (search)
            {
                width += paletteWidth;
                ++items;
            }
            return width + static_cast<float>(items) * gap;
        }

        // Fit the end group against the room it would have without the centering spacer. Search is sacrificed only
        // when normal flow cannot hold it; centering uses the same budget, so it cannot hide Search by itself.
        void fitEnd()
        {
            const UiHierarchyManager& hierarchy = scope.hierarchy();
            if (hierarchy.layoutGeneration() == endLayout)
            {
                return;
            }
            // the room is what the row leaves after the item before the end group, whatever the document puts there
            const Entity endGroup = hierarchy.parentOf(help.entity());
            const Entity controls = endGroup.isValid() ? hierarchy.parentOf(endGroup) : Entity::undefined();
            if (!controls.isValid() || !hierarchy.isVisibleOf(help.entity()) ||
                hierarchy.paintIndexOf(controls) == UiHierarchyManager::NO_INDEX ||
                hierarchy.paintIndexOf(help.entity()) == UiHierarchyManager::NO_INDEX ||
                (statusBar->jobs().isVisible() &&
                 hierarchy.paintIndexOf(statusBar->jobs().entity()) == UiHierarchyManager::NO_INDEX))
            {
                return;
            }
            const float scale = hierarchy.layoutScale();
            float beforeWidth = 0.0f;
            const Entity spacer = centerSpacer.isValid() ? centerSpacer.entity() : Entity::undefined();
            for (const Entity child : hierarchy.childrenOf(controls))
            {
                if (child == endGroup)
                    break;
                if (child != spacer && hierarchy.isVisibleOf(child))
                {
                    if (hierarchy.paintIndexOf(child) == UiHierarchyManager::NO_INDEX)
                        return;
                    beforeWidth += hierarchy.absoluteRectOf(child).size.x / scale;
                }
            }
            endLayout = hierarchy.layoutGeneration();
            const UiRect row = hierarchy.absoluteRectOf(controls);
            const World& world = scope.world();
            // what the header gives back, level by level, counted from the items' own sizes so the room can be read
            // as it would be with everything shown (the decision then does not flip with the layout it caused)
            Entity brand = Entity::undefined();
            Entity brandName = Entity::undefined();
            float markWidth = 0.0f;
            float gains[3] = {0.0f, 0.0f, 0.0f};
            for (const Entity child : hierarchy.childrenOf(controls))
            {
                if (world.getConstComponent<UiIdentityComponent>(child).id.toView() == "shell.toolbar.brand")
                {
                    brand = child;
                    break;
                }
            }
            if (brand.isValid())
            {
                const UiLayoutComponent& brandLayout = world.getConstComponent<UiLayoutComponent>(brand);
                const UiTextMeasurer* measurer = hierarchy.textMeasurer();
                for (const Entity part : hierarchy.childrenOf(brand))
                {
                    const std::string_view id = world.getConstComponent<UiIdentityComponent>(part).id.toView();
                    if (id == "shell.toolbar.brand.name")
                    {
                        brandName = part;
                    }
                    else if (id == "shell.toolbar.brand.mark")
                    {
                        markWidth = world.getConstComponent<UiLayoutItemComponent>(part).minSize.x;
                    }
                }
                const float nameGain = brandName.isValid() && measurer
                    ? std::ceil(
                          measurer->measureText(brandName, world.getConstComponent<UiLabelComponent>(brandName).text)
                              .x) +
                        brandLayout.gap
                    : 0.0f;
                gains[1] = nameGain;
                gains[2] = gains[1] + markWidth + brandLayout.padding.x + brandLayout.padding.z;
            }
            const float room = row.size.x / scale - beforeWidth - gains[yieldLevel];
            const float gap = panelui::Density::read(scope).xs; // between the items, and after the last
            // the field's own width (its text and padding), not its rect: a squeezed row shrinks the rect
            if (const UiTextMeasurer* measurer = hierarchy.textMeasurer())
            {
                paletteWidth = std::ceil(measurer->measureText(paletteField.entity(), paletteField.text()).x) +
                    2.0f * gap + 2.0f * panelui::Density::read(scope).hairline;
            }
            // The wordmark yields before actionable end items; hysteresis prevents flicker at the boundary.
            const float floorNeed = endWidth(false, scale, gap);
            u32 level = 0;
            while (level < 2 && floorNeed + 0.5f + (level < yieldLevel ? 2.0f * gap : 0.0f) > room + gains[level])
            {
                ++level;
            }
            if (level != yieldLevel)
            {
                yieldLevel = level;
                if (brandName.isValid())
                {
                    Widget(&scope, brandName, scope.generation()).setVisible(level < 1);
                }
                if (brand.isValid())
                {
                    Widget(&scope, brand, scope.generation()).setVisible(level < 2);
                }
            }
            const float available = room + gains[level];
            frameHeader->setSparklineShown(false);
            // the Search commands field takes what the others leave, never their room: Ctrl+K and Help > Command
            // Palette open the list without it
            const bool search = endWidth(true, scale, gap) + 0.5f <= available;
            if (paletteField.isVisible() != search)
            {
                paletteField.setVisible(search);
            }
        }

        void centerTransport()
        {
            if (!centerSpacer.isValid())
                return;
            const UiHierarchyManager& hierarchy = scope.hierarchy();
            const Entity controls = hierarchy.parentOf(centerSpacer.entity());
            const Entity transport = hierarchy.parentOf(play.entity());
            if (!controls.isValid() || !transport.isValid() ||
                hierarchy.paintIndexOf(controls) == UiHierarchyManager::NO_INDEX ||
                hierarchy.paintIndexOf(transport) == UiHierarchyManager::NO_INDEX ||
                hierarchy.paintIndexOf(help.entity()) == UiHierarchyManager::NO_INDEX ||
                (statusBar->jobs().isVisible() &&
                 hierarchy.paintIndexOf(statusBar->jobs().entity()) == UiHierarchyManager::NO_INDEX))
                return;
            const float scale = hierarchy.layoutScale();
            const float width = hierarchy.absoluteRectOf(controls).size.x / scale;
            const float transportWidth = hierarchy.absoluteRectOf(transport).size.x / scale;
            if (width <= 0.0f || transportWidth <= 0.0f)
                return;
            float left = 0.0f;
            for (const Entity child : hierarchy.childrenOf(controls))
            {
                if (child == centerSpacer.entity())
                    break;
                if (hierarchy.isVisibleOf(child))
                {
                    if (hierarchy.paintIndexOf(child) == UiHierarchyManager::NO_INDEX)
                        return;
                    left += hierarchy.absoluteRectOf(child).size.x / scale;
                }
            }
            const float gap = panelui::Density::read(scope).xs;
            const float right = endWidth(paletteField.isVisible(), scale, gap);
            const float desired = (width - transportWidth) * 0.5f - left;
            const float spacer = desired >= 0.0f && left + desired + transportWidth + right <= width ? desired : 0.0f;
            UiLayoutItemComponent item = scope.world().getConstComponent<UiLayoutItemComponent>(centerSpacer.entity());
            if (std::abs(item.basis - spacer) > 0.5f)
            {
                item.basis = spacer;
                centerSpacer.setLayoutItem(item);
            }
        }

        void syncToolbar()
        {
            syncGizmoBar();
            PlayWorld* world = engine->playWorld();
            const bool nowPlaying = world != nullptr;
            const bool nowPaused = nowPlaying && world->world().simulationState().paused;
            frameHeader->setShown(statusBar->frameOver());
            fitEnd();
            play.setVisible(!nowPlaying);
            pause.setVisible(nowPlaying);
            step.setVisible(nowPaused);
            stop.setVisible(nowPlaying);
            const bool showTimeScale = nowPlaying || engine->config().timing.timeScale != 1.0f;
            timeScaleMark.setVisible(showTimeScale);
            timeScale->field().setVisible(showTimeScale);
            centerTransport();
            const u64 simulated = nowPlaying ? world->world().simulationState().simulatedFrames : 0;
            const EditorModeFrame::Mode mode = !nowPlaying ? EditorModeFrame::Mode::Edit
                : nowPaused                                ? EditorModeFrame::Mode::Paused
                                                           : EditorModeFrame::Mode::Playing;
            if (modeFrame)
            {
                modeFrame->setMode(mode);
            }
            playCounter->update(mode, simulated, engine->lastFrameSeconds());
            timeScale->update();
            if (synced && nowPlaying == playing && nowPaused == paused)
            {
                return;
            }
            synced = true;
            // Play shows and focuses the Game panel; Stop, however it came, takes the focus back
            if (editorViews && nowPlaying != playing)
            {
                const auto& shown = editorUi(*engine).ui().hierarchy();
                if (nowPlaying)
                {
                    // a Scene tab hiding behind Game would hide the live play world: show them side by side
                    if (shown.isVisibleOf(views[0].image.entity()) && !shown.isVisibleOf(views[1].image.entity()))
                    {
                        gameTab = tabIndexOf("game");
                        splitForPlay = dock.apply(dockOp(UiDockOpKind::Dock, "game", "scene", UiDockZone::Right));
                    }
                    (void)dock.apply(dockOp(UiDockOpKind::Activate, "game"));
                }
                else if (splitForPlay)
                {
                    splitForPlay = false;
                    UiDockOp back = dockOp(UiDockOpKind::Dock, "game", "scene");
                    back.index = gameTab;
                    (void)dock.apply(back);
                    (void)dock.apply(dockOp(UiDockOpKind::Activate, "scene"));
                }
                gameFocused = nowPlaying;
                applyGameFocus();
            }
            playing = nowPlaying;
            paused = nowPaused;
            barTint.setStyleClass(InternedString("toolbar.tint." +
                                                 std::string(!playing     ? "edit"
                                                                 : paused ? "paused"
                                                                          : "playing")));
            showToggle(play, playing);
            showToggle(pause, paused);
            play.setInteractable(!playing);
            pause.setInteractable(playing);
            step.setInteractable(paused);
            stop.setInteractable(playing);
            pause.setAccessibleNameKey(InternedString(paused ? "editor.bar.resume" : "editor.bar.pause"));
            pause.setIcon(paused ? "play" : "pause");
        }
    };

    EditorShell::EditorShell(Engine& engine, EditorShellDesc desc) :
        m_state(std::make_unique<State>(engine, std::move(desc)))
    {
        State& state = *m_state;
        for (u32 i = 2; i < 6 && i < engine.render().viewCount(); ++i)
            if (!engine.config().graphics.isCaptureView(i))
                engine.render().view(i).active = false;
        UiViewContext& view = state.seams.ui->uiView();
        // JetBrainsMono first: a style naming a font that is not registered falls back to the first one
        static constexpr std::pair<std::string_view, std::string_view> FONTS[] = {
            {"JetBrainsMono", "JetBrainsMonoRegularFont.ttf"},
            {"ChakraPetch", "ChakraPetch-Bold.ttf"},
            {"Geist", "Geist-Regular.ttf"},
            {"GeistMono", "GeistMono-Regular.ttf"},
            {"GeistMonoMedium", "GeistMono-Medium.ttf"},
            // the Phosphor icon weights; engine.json's fontWeights maps "icons" to them
            {"icons-thin", "Phosphor-Thin.ttf"},
            {"icons-light", "Phosphor-Light.ttf"},
            {"icons", "Phosphor-Regular.ttf"},
            {"icons-bold", "Phosphor-Bold.ttf"},
            {"icons-fill", "Phosphor-Fill.ttf"}};
        for (const auto& [name, file] : FONTS)
        {
            if (const auto font =
                    view.registerFont(InternedString(name), resolveContentPath(std::format("assets/fonts/{}", file)));
                !font)
            {
                SWARNING_LOG("editor shell: {}", font.error().message);
            }
        }
        // the model loaded the stylesheet and its settings applied the theme, the scales, reduced motion and the locale
        view.stylesheet().watch(true);

        UiScope& scope = state.scope;
        // the root and the shell's containers take no input: what no panel covers reaches the game UI and gameplay
        state.root = scope.createPanel("editor", topLeft({}, {}));
        state.root.setInteractable(false);
        state.host = scope.createPanel("editor.shell.host", panelui::columnDesc());
        state.host.setParent(state.root);
        state.host.setInteractable(false);
        state.barTint = scope.createPanel("editor.bar.tint", panelui::barDesc({}));
        state.barTint.setLayoutItem(panelui::fillItem());
        state.barTint.setInteractable(false);
        if (state.engine->services().contains<Localization>())
        {
            state.localization = &state.engine->services().get<Localization>();
            state.shownRevision = state.localization->revision();
        }
        state.feedback = &EditorModelAccess::feedback(state.model);
        // a toolbar control is a control high with the scale's text padding at its sides (size.control, space.xs)
        const auto toolbarControl = [&state](const auto widget)
        {
            panelui::bindFitPadding(widget, panelui::SPACE_XS, panelui::SPACE_NONE);
            return state.density.track(widget, panelui::HEIGHT_CONTROL);
        };
        // the toolbar's buttons are built here, as wide as their text needs (never narrower than `width`, the room
        // the default texts take); the shell document's slots hold them
        const auto toolbarButton = [&](const std::string& id, const float width)
        {
            UiRectDesc desc = panelui::fitWidthDesc();
            desc.layoutItem->minSize = Vec2{width, 0.0f};
            return toolbarControl(scope.createButton(InternedString(id), "", desc));
        };
        state.file = toolbarButton("editor.bar.file", 44.0f);
        state.file.setLocalizedText("editor.bar.file");
        state.edit = toolbarButton("editor.bar.edit", 52.0f);
        state.edit.setLocalizedText("editor.bar.edit");
        state.window = toolbarButton("editor.bar.window", 64.0f);
        state.window.setLocalizedText("editor.bar.window");
        state.help = toolbarButton("editor.bar.help", 48.0f);
        state.help.setLocalizedText("editor.bar.help");
        state.paletteField = toolbarButton("editor.bar.palette", 0.0f); // EditorCommandPalette sets its text
        // an icon button of the Forge header (the gizmo tools and the transport): the control's square, named by one
        // message that stays when its look changes; its tooltip is tip.<id>
        const auto toolbarIcon = [&](const std::string& id, const std::string_view icon, const std::string& nameKey)
        {
            Button button = scope.createButton(InternedString(id), "", panelui::barDesc(Vec2{}));
            button.setIcon(icon);
            button.setAccessibleNameKey(InternedString(nameKey));
            state.density.apply([button](const panelui::Density& now) mutable
                                { button.setSize(Vec2{now.control, now.control}); });
            return button;
        };
        for (u32 i = 0; i < state.gizmoModes.size(); ++i)
        {
            const std::string_view mode = GIZMO_MODE_NAMES[i];
            state.gizmoModes[i] = toolbarIcon(std::format("editor.bar.gizmo.{}", mode), GIZMO_ICONS[i],
                                              std::format("editor.gizmo.{}", GIZMO_LABELS[i]));
            state.gizmoModes[i].onClick(
                [&state, mode]
                { state.pending.push_back(std::format(R"({{"command":"gizmoMode","args":{{"mode":"{}"}}}})", mode)); });
        }
        // World, Local and Snap are gizmoMode requests too; the icon of the space button shows the state it holds
        state.gizmoSpace = toolbarIcon("editor.bar.gizmo.space", GIZMO_SPACE_ICONS[0], "editor.gizmo.space");
        state.gizmoSpace.onClick(
            [&state]
            {
                state.pending.push_back(
                    std::format(R"({{"command":"gizmoMode","args":{{"space":"{}"}}}})",
                                EditorModelAccess::gizmo(state.model).space == GizmoSpace::Local ? "world" : "local"));
            });
        state.gizmoSnap = toolbarIcon("editor.bar.gizmo.snap", "magnet", "editor.gizmo.snap");
        state.gizmoSnap.onClick(
            [&state]
            {
                state.pending.push_back(std::format(R"({{"command":"gizmoMode","args":{{"snap":{}}}}})",
                                                    !EditorModelAccess::gizmo(state.model).snap));
            });

        {
            UiNumberDesc step;
            step.rect = panelui::fitWidthDesc();
            step.min = 0.001;
            step.max = 1.0e6;
            step.step = 0.01;
            step.decimals = 3;
            state.gizmoStep = toolbarControl(scope.createNumberField("editor.bar.gizmo.step", step));
            state.gizmoStep.onNumber(
                [&state](const double value)
                {
                    state.pending.push_back(std::format(
                        R"({{"command":"gizmoMode","args":{{"{}Step":{}}}}})",
                        GIZMO_MODE_NAMES[static_cast<u32>(EditorModelAccess::gizmo(state.model).mode)], value));
                });
        }
        state.gizmoUnit = toolbarControl(scope.createLabel("editor.bar.gizmo.unit", "", panelui::fitWidthDesc()));
        state.gizmoUnit.setStyleClass(InternedString("toolbar.caption"));
        state.gizmoUnit.setInteractable(false);

        const auto request = [&state](const std::string_view line)
        { return [&state, line] { state.pending.emplace_back(line); }; };
        state.play = toolbarIcon("editor.bar.play", "play", "editor.bar.play");
        state.pause = toolbarIcon("editor.bar.pause", "pause", "editor.bar.pause"); // syncToolbar names Resume
        state.step = toolbarIcon("editor.bar.step", "skip-forward", "editor.bar.step");
        state.stop = toolbarIcon("editor.bar.stop", "stop", "editor.bar.stop");
        state.play.onClick(request(R"({"command":"play"})"));
        state.step.onClick(request(R"({"command":"step","args":{"world":"play","frames":1}})"));
        state.stop.onClick(request(R"({"command":"stop"})"));
        state.pause.onClick(
            [&state]
            {
                state.pending.emplace_back(state.paused ? R"({"command":"resume","args":{"world":"play"}})"
                                                        : R"({"command":"pause","args":{"world":"play"}})");
            });

        {
            const UiRectDesc counter = panelui::fitWidthDesc();
            const float padding = state.density.density().xs;
            state.playCounter = std::make_unique<EditorPlayCounter>(engine, scope, counter, padding);
            state.timeScale = std::make_unique<EditorTimeScale>(engine, scope, counter, padding);
            toolbarControl(state.playCounter->root());
            toolbarControl(state.timeScale->field());
            state.timeScaleMark = scope.createLabel("editor.bar.timescale.mark", "\xC3\x97", panelui::fitWidthDesc());
            state.timeScaleMark.setStyleClass(InternedString("toolbar.caption"));
            state.timeScaleMark.setAccessibleRole(UiRole::Presentation); // the field's own name says time scale
            state.density.track(state.timeScaleMark, panelui::HEIGHT_CONTROL);
            // the fields keep the width of their widest reading plus this padding: measured again on a new density
            state.density.apply(
                [&state](const panelui::Density& now)
                {
                    state.stepSheet = ~0ull;
                    state.playCounter->setPadding(now.xs);
                    state.timeScale->setPadding(now.xs);
                });
        }

        state.setEditorViews(true);
        if (!state.editorViews)
        {
            SWARNING_LOG("editor shell: the Scene and Game panels stay empty: config.graphics.renderViewCount < 2");
        }
        UiDockDesc dockDesc;
        dockDesc.rect = topLeft();
        dockDesc.rect.layoutItem = panelui::fillItem(); // fills the document's shell.dock slot
        dockDesc.closeButtons = true;
        dockDesc.detachOutside = true; // syncDetached opens the OS windows
        state.dock = scope.createDock("editor.dock", dockDesc);
        state.dock.setLandmark(true); // the tab strips; each panel's host is a landmark of its own
        state.density.apply(
            [&state](const panelui::Density& now)
            {
                state.dock.setTabHeight(now.tab);
                state.dock.setSplitterSize(now.xs); // the gap between panels
                for (const State::Built& built : state.panels)
                {
                    state.setPanelMin(built.desc, now);
                }
            });
        state.previewJobs = std::make_unique<EditorPreviewJobs>(engine);
        state.inspector = std::make_unique<EditorInspector>(engine, state.model);
        // the toasts sit above the dock and below the drag preview and the menus, which are created after them
        state.statusBar =
            std::make_unique<EditorStatusBar>(engine, state.root, state.dock, state.model, *state.feedback);
        state.applied.push_back(
            state.settings->onApplied([bar = state.statusBar.get()](const EditorSettings& applied)
                                      { bar->settle(applied.telemetry == TELEMETRY_FULL, applied.frameBudgetMs); }));
        // the header's frame lamp and sparkline follow the status bar's windows and its over-budget flag, settled in
        // the same reply (registered after the bar's own listener, so the flag is current)
        state.frameHeader = std::make_unique<EditorFrameHeader>(
            engine, state.settings->settings().telemetry == TELEMETRY_FULL, state.settings->settings().frameBudgetMs);
        state.statusBar->onFrameWindow([header = state.frameHeader.get()](const double ms, const bool over)
                                       { header->sample(ms, over); });
        state.applied.push_back(state.settings->onApplied(
            [&state](const EditorSettings& applied)
            {
                state.frameHeader->settle(applied.telemetry == TELEMETRY_FULL, applied.frameBudgetMs,
                                          state.statusBar->frameOver());
            }));
        state.applied.push_back(state.settings->onApplied(
            [inspector = state.inspector.get()](const EditorSettings& applied)
            { inspector->setTelemetry(applied.telemetry == TELEMETRY_FULL, applied.systemBudgetMs); }));
        EditorModelAccess::dialogs(state.model).setPolicy(state.desc.dialogs);
        state.dialogs = std::make_unique<EditorDialogs>(engine, state.model, state.root);
        state.inspector->setDialogs(state.dialogs.get());
        state.dragPreview = scope.createDragPreview("editor.dragPreview");
        state.actions = std::make_unique<EditorActions>(engine, state.model, *state.feedback, state.desc.hotkeysFile);
        state.seams.ui->ui().setInputFilter(
            [&state](UiInputSnapshot& input)
            {
                UiRouter& router = state.seams.ui->ui().router();
                const bool textFocused =
                    router.wantsTextInput() && state.scope.hierarchy().isVisibleOf(router.focused());
                if (state.gameFocused || router.modalActive() || textFocused ||
                    input.modifiers != UiKeyModifier::CTRL || !std::isfinite(input.wheelDelta.y) ||
                    input.wheelDelta.y == 0.0f)
                {
                    return;
                }
                state.zoomWheel += std::clamp(input.wheelDelta.y, -40.0f, 40.0f);
                input.wheelDelta.y = 0.0f;
                while (std::abs(state.zoomWheel) >= 1.0f)
                {
                    const bool up = state.zoomWheel > 0.0f;
                    state.actions->request(up ? EditorActions::Action::ZoomIn : EditorActions::Action::ZoomOut);
                    state.zoomWheel += up ? -1.0f : 1.0f;
                }
            });
        state.fileMenu = std::make_unique<EditorFileMenu>(engine, state.model, *state.actions);
        state.actions->setFileHandler([&state](const EditorActions::Action action)
                                      { state.fileMenu->request(action); });
        state.palette = std::make_unique<EditorCommandPalette>(
            engine, scope, state.paletteField, state.help, *state.dialogs, *state.actions, *state.feedback,
            [&state]
            {
                std::vector<EditorCommandPalette::PanelRow> rows;
                for (const State::Built& built : state.panels)
                {
                    rows.push_back({built.desc.name, state.title(built.desc)});
                }
                return rows;
            });
        state.actions->setPaletteHandler([&state] { state.palette->open(); });
        // the toolbar's tooltips (tip.<id> in the editor table); a gizmo mode shows its hotkey
        for (Widget item :
             {static_cast<Widget>(state.file), static_cast<Widget>(state.edit), static_cast<Widget>(state.window),
              static_cast<Widget>(state.help), static_cast<Widget>(state.paletteField),
              static_cast<Widget>(state.gizmoSpace), static_cast<Widget>(state.gizmoSnap),
              static_cast<Widget>(state.gizmoStep), state.timeScale->field(), static_cast<Widget>(state.play),
              static_cast<Widget>(state.pause), static_cast<Widget>(state.step), static_cast<Widget>(state.stop)})
        {
            panelui::setTip(item);
        }
        state.showModeHints();
        state.gizmo = std::make_unique<EditorGizmo>(engine, state.model);
        state.agent = std::make_unique<EditorAgentLayer>(engine, *state.settings);
        state.agent->onOpen([&state] { state.pending.push_back(dockRequest(state.showOp(InternedString("agent")))); });
        state.inspector->setAgentLayer(state.agent.get());
        state.inspector->setTelemetry(state.settings->settings().telemetry == TELEMETRY_FULL,
                                      state.settings->settings().systemBudgetMs);
        for (EditorPanelDesc& desc : editorPanels())
        {
            state.panels.push_back({std::move(desc), {}});
        }
        for (u32 i = 0; i < state.panels.size(); ++i)
        {
            state.buildSequence.push_back(i);
        }
        const auto buildKey = [&state](const u32 index)
        {
            const EditorPanelDesc& desc = state.panels[index].desc;
            return desc.buildOrder == EditorPanelDesc::AS_ORDER ? desc.order : desc.buildOrder;
        };
        std::ranges::stable_sort(state.buildSequence, {}, buildKey);
        EditorPanelContext context{engine,
                                   scope,
                                   state.dock,
                                   state.model,
                                   *state.inspector,
                                   *state.settings,
                                   *state.feedback,
                                   *state.dialogs,
                                   [&state](const std::string_view name)
                                   {
                                       const auto it = std::ranges::find(state.panels, name, [](const State::Built& b)
                                                                         { return std::string_view(b.desc.name); });
                                       return it == state.panels.end() ? Widget{} : it->panel.content;
                                   },
                                   [&state](const std::string_view path) { state.fileMenu->openRecent(path); },
                                   *state.agent};
        for (const u32 index : state.buildSequence)
        {
            if (state.panels[index].desc.make)
            {
                state.panels[index].panel = state.panels[index].desc.make(context);
            }
        }
        // the hierarchy's subtree costs follow the telemetry setting like the status bar and the inspector do, in the
        // reply of editorSettings
        if (EditorHierarchy* hierarchy = state.panel<EditorHierarchy>("hierarchy"))
        {
            hierarchy->setTelemetry(state.settings->settings().telemetry == TELEMETRY_FULL,
                                    state.settings->settings().systemBudgetMs);
            state.applied.push_back(state.settings->onApplied(
                [&state](const EditorSettings& applied)
                {
                    if (EditorHierarchy* shown = state.panel<EditorHierarchy>("hierarchy"))
                    {
                        shown->setTelemetry(applied.telemetry == TELEMETRY_FULL, applied.systemBudgetMs);
                    }
                }));
        }
        for (State::Built& built : state.panels)
        {
            Widget content = built.panel.content;
            if (built.desc.view && *built.desc.view >= state.views.size())
            {
                SERROR_LOG("EditorShell: panel {} shows view {}, which has no editor view image; left empty",
                           built.desc.name, *built.desc.view);
            }
            else if (built.desc.view && state.editorViews)
            {
                State::ViewImage& entry = state.views[*built.desc.view];
                entry.view = *built.desc.view;
                entry.host = scope.createPanel(InternedString("editor.view." + built.desc.name + ".host"));
                entry.image = scope.createImage(InternedString("editor.view." + built.desc.name),
                                                UiRectDesc{.anchor = {0.5f, 0.5f}, .pivot = {0.5f, 0.5f}});
                entry.image.setParent(entry.host);
                if (entry.view == SCENE_VIEW)
                {
                    state.modeFrame = std::make_unique<EditorModeFrame>(scope, entry.image, built.desc.name);
                }
                // the panel's entry control: a stop the landmark cycle lands on, named like its image
                const InternedString nameKey("editor-" + built.desc.name + ".name");
                scope.world().getComponent<UiStateComponent>(entry.host.entity()).tabStop = true;
                entry.host.setAccessibleRole(UiRole::Application);
                entry.host.setAccessibleNameKey(nameKey);
                entry.image.setAccessibleNameKey(nameKey);
                SASSERTM(entry.view < MAX_UI_VIEW_TEXTURES, "View {} has no UI image slot.", entry.view);
                entry.image.setImageSlot(VIEW_IMAGE_SLOT + entry.view);
                view.registerImage(VIEW_IMAGE_SLOT + entry.view, uiViewTexture(entry.view));
                if (entry.view == GAME_VIEW)
                {
                    // a press on the unfocused game view, or on its letterbox bars, focuses it
                    const auto focus = [&state](const u8 button, Vec2)
                    {
                        if (button == 0)
                        {
                            state.pending.emplace_back(R"({"command":"gameFocus","args":{"on":true}})");
                        }
                    };
                    entry.image.onPress(focus);
                    entry.host.onPress(focus);
                    // Enter or Space on the focused host gives the game the input, as a press does
                    scope.setKeyReaction(
                        entry.host.entity(),
                        [&state, &scope](const UiKeyDownEvent& event)
                        {
                            constexpr u32 RETURN = 40;
                            constexpr u32 SPACE = 44;
                            constexpr u32 KP_ENTER = 88;
                            if ((event.scancode != RETURN && event.scancode != SPACE && event.scancode != KP_ENTER) ||
                                event.modifiers != 0 || !scope.router().navigationEnabled())
                            {
                                return false;
                            }
                            if (!event.repeat)
                            {
                                state.pending.emplace_back(R"({"command":"gameFocus","args":{"on":true}})");
                            }
                            return true;
                        });
                }
                content = entry.host;
                if (entry.view == GAME_VIEW)
                {
                    state.gameToolbar = std::make_unique<EditorGameToolbar>(engine, *state.feedback, GAME_VIEW,
                                                                            entry.host, entry.image);
                    content = state.gameToolbar->content();
                }
            }
            state.dock.addPanel(InternedString(built.desc.name), state.tabTitle(built.desc), content);
            if (built.panel.tag.isValid())
            {
                state.dock.setPanelTag(InternedString(built.desc.name), built.panel.tag);
            }
            state.setPanelMin(built.desc, state.density.density());
        }
        if (state.editorViews)
        {
            state.sceneView = std::make_unique<EditorSceneView>(engine, state.model, *state.gizmo, state.views[0].image,
                                                                state.settings->settings());
            state.sceneHud = std::make_unique<EditorSceneHud>(engine, scope, state.views[0].host, *state.sceneView,
                                                              state.model, *state.agent);
            state.sceneToolbar = std::make_unique<EditorSceneToolbar>(
                engine, scope, state.views[0].host, *state.sceneView, *state.sceneHud, state.model, *state.settings,
                *state.actions, *state.gizmo,
                std::array<Widget, 7>{state.gizmoModes[0], state.gizmoModes[1], state.gizmoModes[2], state.gizmoSpace,
                                      state.gizmoSnap, state.gizmoStep, state.gizmoUnit});
        }
        // with or without the editor views: the grid pass draws in view 0 whatever shows it
        state.sceneGrid = std::make_unique<EditorSceneGrid>(engine, *state.settings, state.model);
        state.buildDefaultLayout();
        state.defaultLayout = state.dock.saveLayout();
        if (!state.desc.layoutFile.empty())
        {
            if (const Expected<std::string> saved = readTextFile(state.desc.layoutFile);
                saved && !state.dock.loadLayout(*saved))
            {
                SWARNING_LOG("editor shell: {} is not a dock layout; using the default",
                             state.desc.layoutFile.string());
            }
        }
        state.dock.onRequest([&state](const UiDockOp& op) { state.pending.push_back(dockRequest(op)); });

        state.openView();

        UiMenuDesc menuDesc;
        menuDesc.bounds = state.root;
        state.menu = scope.createMenu("editor.menu", menuDesc);
        state.menu.onActivate([&state](const InternedString item) { state.activateItem(item); });
        constexpr u32 BAR_MENUS = 4;
        for (u32 index = 0; index < BAR_MENUS; ++index)
        {
            Button button = index == 0 ? state.file : index == 1 ? state.edit : index == 2 ? state.window : state.help;
            button.onPress(
                [&state, index](const u8 mouse, Vec2)
                {
                    if (mouse == 0)
                    {
                        state.openBarMenu(index);
                    }
                });
        }
        state.menu.onCycle(
            [&state](const i32 direction)
            { state.openBarMenu((state.barMenu + BAR_MENUS + static_cast<u32>(direction)) % BAR_MENUS); });

        state.registerCommands();
        state.syncToolbar();
        scope.commit();
    }

    u64 EditorShellTestSeam::profilerViewToken(EditorShell& shell)
    {
        const EditorProfiler* profiler = shell.m_state->panel<EditorProfiler>("profiler");
        return profiler ? profiler->viewToken() : 0;
    }

    u64 EditorShellTestSeam::profilerMatchToken(EditorShell& shell)
    {
        const EditorProfiler* profiler = shell.m_state->panel<EditorProfiler>("profiler");
        return profiler ? profiler->matchToken() : 0;
    }

    void EditorShellTestSeam::injectProfilerMatches(EditorShell& shell, const CaptureId capture, const u64 token,
                                                    const std::string& name, const u32 count)
    {
        if (EditorProfiler* profiler = shell.m_state->panel<EditorProfiler>("profiler"))
        {
            profiler->injectMatches(capture, token, name, count);
        }
    }

    void EditorShellTestSeam::injectProfilerSpans(EditorShell& shell, const CaptureId capture, const u64 token,
                                                  const u32 lane, const std::int64_t fromNs, const std::int64_t toNs,
                                                  const u32 columns, std::vector<CaptureSpan> spans)
    {
        if (EditorProfiler* profiler = shell.m_state->panel<EditorProfiler>("profiler"))
        {
            profiler->injectSpans(capture, token, lane, fromNs, toNs, columns, std::move(spans));
        }
    }

    void EditorShellTestSeam::setProfilerReveal(EditorShell& shell,
                                                std::function<bool(const std::filesystem::path&)> reveal,
                                                std::function<u64()> nowMs)
    {
        EditorModelAccess::captureFiles(shell.m_state->model).setReveal(std::move(reveal), std::move(nowMs));
    }

    void EditorShellTestSeam::setBuildSource(EditorShell& shell, std::filesystem::path statusDirectory,
                                             std::function<std::vector<AvailableModule>()> loaded)
    {
        EditorBuildState& build = EditorModelAccess::build(shell.m_state->model);
        build.setStatusDirectory(std::move(statusDirectory));
        build.setLoadedModules(std::move(loaded));
        build.refreshNow();
    }

    void EditorShellTestSeam::setAgentLayerClock(EditorShell& shell, std::function<std::int64_t()> nowMs)
    {
        shell.m_state->agent->setClock(std::move(nowMs));
    }

    void EditorShellTestSeam::setProjectReveal(EditorShell& shell,
                                               std::function<bool(const std::filesystem::path&)> reveal,
                                               std::function<u64()> nowMs)
    {
        if (EditorProject* project = shell.m_state->panel<EditorProject>("project"))
        {
            project->setReveal(std::move(reveal), std::move(nowMs));
        }
    }

    void EditorShellTestSeam::setCaptureReveal(EditorShell& shell,
                                               std::function<bool(const std::filesystem::path&)> reveal,
                                               std::function<u64()> nowMs)
    {
        if (EditorCapture* capture = shell.m_state->panel<EditorCapture>("capture"))
        {
            capture->setReveal(std::move(reveal), std::move(nowMs));
        }
    }

    EditorShell::~EditorShell()
    {
        m_state->seams.ui->ui().setInputFilter({});
        State& state = *m_state;
        state.dialogs->closeForShutdown();
        state.previewJobs.reset();
        // the model's settings outlive every part these listeners capture
        for (const u32 id : state.applied)
        {
            state.settings->removeApplied(id);
        }
        state.engine->control().unregisterOwner(OWNER);
        state.engine->control().setLocalSource(RequestSource::Host);
        if (state.editorViews)
        {
            (void)state.setEditorViews(false);
        }
        state.engine->setGameInputFocus(true);
        if (const std::string error = state.desc.layoutFile.empty() ? std::string{} : state.saveLayoutFile();
            !error.empty())
        {
            SWARNING_LOG("editor shell: layout not saved: {}", error);
        }
    }

    void EditorShell::update()
    {
        SPITE_ZONE("EditorShell");
        State& state = *m_state;
#if !defined(_WIN32)
        EditorModelAccess::build(state.model).pollStartedBuilds();
#endif
        EditorModelAccess::build(state.model).pollModuleWork();
        state.previewJobs->update();
        {
            SPITE_ZONE("EditorShell.sync");
            state.syncLocale();
            // first, as the engine's beginFrame did: the overlay's work as the previous frame left it, before this
            // frame queues more
            state.syncOverlayPasses();
        }
        if (!state.pending.empty())
        {
            SPITE_ZONE("EditorShell.requests");
            std::vector<std::string> requests;
            requests.swap(state.pending);
            for (const std::string& request : requests)
            {
                if (const std::string reply = state.engine->control().handle(request);
                    reply.find(R"("ok":false)") != std::string::npos)
                {
                    SWARNING_LOG("editor shell: {} -> {}", request, reply);
                    state.gizmoShown = ~0u; // a refused setting: the toolbar shows the gizmo's again
                }
            }
        }
        if (!state.deferred.empty())
        {
            std::vector<std::function<void()>> calls;
            calls.swap(state.deferred);
            for (const std::function<void()>& call : calls)
            {
                call();
            }
        }
        {
            SPITE_ZONE("EditorActions");
            state.actions->update(state.sceneView && state.sceneView->flying(), state.gameFocused);
            if (state.actions->bindingRevision() != state.seenBindings)
            {
                state.showModeHints();
            }
        }
        {
            SPITE_ZONE("EditorInspector");
            state.model.pruneSelection(); // every panel after it reads live entities
            state.inspector->update();
        }
        if (state.sceneView)
        {
            SPITE_ZONE("EditorSceneView");
            state.sceneView->update(); // its picks select before the hierarchy follows the selection
            state.sceneToolbar->update();
        }
        state.sceneGrid->update();
        {
            SPITE_ZONE("EditorGizmo");
            state.gizmo->update(); // after the scene view's gizmo drags and picks ran
        }
        // the scene file actions start (their dialogs show this frame) and follow the open scene's path; the status bar
        // read sceneInfo last frame
        state.model.followScene(state.statusBar->scenePath());
        state.fileMenu->update();
        state.dialogs->update(); // the answers' callbacks run before the panels read their state
        {
            SPITE_ZONE("EditorAgentLayer");
            state.agent->update();
        }
        if (state.sceneHud)
        {
            SPITE_ZONE("EditorSceneHud");
            state.sceneHud->update(); // the callout reads what the agent layer just followed
        }
        for (const u32 index : state.buildSequence)
        {
            const State::Built& built = state.panels[index];
            if (built.panel.update)
            {
                SPITE_ZONE_NAMED(built.desc.zone ? built.desc.zone : "EditorPanel");
                built.panel.update();
            }
        }
        {
            SPITE_ZONE("EditorShell.views");
            if (state.editorViews)
            {
                if (state.gameToolbar)
                {
                    state.gameToolbar->update();
                }
                state.syncViews();
            }
            state.syncDetached();
            if (const Window* window = state.engine->window())
            {
                const WindowExtent extent = window->getFramebufferExtent();
                const float scale = editorUi(*state.engine).ui().hierarchy().layoutScale();
                setSize(Vec2{static_cast<float>(extent.width), static_cast<float>(extent.height)} * (1.0f / scale));
            }
        }
        {
            SPITE_ZONE("EditorToolbar");
            state.syncToolbar();
        }
        state.statusBar->update(); // after the panels reported their jobs
        state.frameHeader->update();
        state.palette->update();
        const UiResizeAxis resizeAxis = editorUi(*state.engine).ui().resizeAxis();
        if (resizeAxis != state.resizeAxis)
        {
            state.resizeAxis = resizeAxis;
            if (state.engine->window())
            {
                state.engine->window()->setCursorShape(
                    resizeAxis == UiResizeAxis::Horizontal     ? CursorShape::ResizeHorizontal
                        : resizeAxis == UiResizeAxis::Vertical ? CursorShape::ResizeVertical
                        : resizeAxis == UiResizeAxis::DiagonalNorthWestSouthEast
                        ? CursorShape::ResizeDiagonalNorthWestSouthEast
                        : resizeAxis == UiResizeAxis::DiagonalNorthEastSouthWest
                        ? CursorShape::ResizeDiagonalNorthEastSouthWest
                        : CursorShape::Default);
            }
        }
        state.syncSceneTab(state.statusBar->sceneDirty());
    }

    bool EditorShell::editorViews() const { return m_state->editorViews; }

    void EditorShell::reserveViews(GraphicsConfig& graphics)
    {
        constexpr u32 EDITOR_VIEWS = 7;
        graphics.renderViewCount = std::max(graphics.renderViewCount, EDITOR_VIEWS);
        if (graphics.probeCaptureView > 0 && graphics.probeCaptureView < EDITOR_VIEWS)
        {
            graphics.probeCaptureView = EDITOR_VIEWS;
            graphics.renderViewCount = std::max(graphics.renderViewCount, EDITOR_VIEWS + graphics.probeCaptureViews);
        }
    }

    void EditorShell::setSize(const Vec2 size)
    {
        State& state = *m_state;
        if (size == state.size)
        {
            return;
        }
        state.size = size;
        state.root.setSize(size);
        state.host.setSize(size);
        state.statusBar->setSize(size);
    }
} // namespace spite
