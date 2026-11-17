#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "application/ApplicationEvents.hpp"
#include "application/ConfigStore.hpp"
#include "asset/AssetBakerConfig.hpp"
#include "base/ApplicationConfig.hpp"
#include "base/Expected.hpp"
#include "base/ServiceLocator.hpp"
#include "ecs/World.hpp"
#include "engine/EngineExport.hpp"
#include "engine/UiDisplayCommands.hpp"
#include "engine/UiInputAdapter.hpp"
#include "physics/PhysicsSettings.hpp"
#include "engine/UiWorld.hpp"
#include "rendering/Extent2D.hpp"
#include "rendering/GpuBudget.hpp"
#include "rendering/RenderPassSetupService.hpp"

namespace spite
{
    class Application;
    class Window;
    class AnimationWorld;
    class AssetRuntime;
    class ControlServer;
    class LogHistory;
    class Localization;
    class LocalizationContent;
    class EntityTransformManager;
    class RenderRuntime;
    struct RenderRuntimeDesc;
    class UiContext;
    class UiViewContext;
    class EnvironmentResolver;
    class UiTextureResolver;
    class UiInputAdapter;
    class PlayWorld;
    class ModuleHost;
    class Screenshots;
    class GpuTables;
    class PixelInfo;
    class QuiescenceMeter;
    class FrameCapture;
    class ProfileCapture;
    class RenderDocCapture;
    class Engine;
    struct SceneDocument;
    struct SceneRecoveryAccess;

    struct EngineDesc
    {
        // With an application the engine shares the application's live config (one instance behind the config
        // table): only config.graphics and config.modules are taken from here, the host's startup choices (graphics
        // sizes the engine's resources, modules is what loads); window, input and timing stay the application's.
        // Without one, all of it.
        ApplicationConfig config;
        // null = headless: device, transfer and bindless only; no pass graph,
        // no presentation
        Window* window = nullptr;
        // Feeds the UI from its input at pre-update, before any gameplay reads
        // actions; null = no UI input (tools, tests). Its InputSystem is a root
        // service: game systems read their actions through it.
        Application* application = nullptr;
        // Sizes the geometry heaps from the baked manifest's summed footprint;
        // off when the caller sets the heap sizes in config itself.
        bool seedHeapsFromManifest = true;
        // where the asset module's manifest and bakes live (default assets_baked
        // under the content root); tests point it at a private directory
        AssetBakerConfig assetBaker;
        // where importModel reads model files from (paths are relative to it)
        std::string modelImportRoot = "models";
        // where an AnimationControllerComponent's controller asset (JSON) is read from, relative to the content root
        // unless absolute: the component names a file under it
        std::filesystem::path animationRoot = "assets/animation";
        // where saveScene/loadScene resolve relative names; empty = %LOCALAPPDATA%/SPITE/scenes (never the working
        // directory, so a save cannot dirty a checkout)
        std::filesystem::path sceneRoot;
        // where the string tables <locale>/<namespace>.json load from; empty = assets/loc under the content root
        std::filesystem::path localizationRoot;
        // the bindings of the UI navigation intents (ui.up ... ui.previousTab, UiInputAdapter), a mapping file read
        // relative to the content root; tests point it at a private file
        std::filesystem::path navigationBindings = "config/project/ui/navigation.json";
        // Extra pass modules (validation, tooling) registered with the standard
        // set before the pass setup finalizes; called once the world, services
        // and asset runtime exist. Windowed only.
        std::function<void(Engine&, std::vector<RenderPassModuleDefinition>&)> extraPassModules;
        // pass enables and implementation pins for the pass setup
        RenderPassSetupRuntimeConfig passConfig;
        // Loads RenderDoc before the Vulkan instance exists and serves the renderdocCapture command; never for perf
        // runs
        bool renderDoc = false;
        // the tree profileStop writes <root>/<machine>/prof/ under; empty = "artifacts" of the working directory
        std::filesystem::path artifactsRoot;
        // false: no Vulkan instance or device, for CPU-only test suites. The worlds update, control serves, scenes
        // load and save, Play runs; nothing renders, uploads or loads assets, and the GPU-only and windowed verbs
        // (Engine::GPU_VERBS, windowedVerbs) answer unavailable naming the mode. Needs window null, renderDoc false.
        bool gpu = true;
    };

    // The composition root every exe consumes: the scene world (scene
    // content, transforms, asset and render runtimes), the play world while
    // playing and the UI worlds tools add, each with its own world
    // services under one root, the policy wiring them together, and their
    // shutdown order. The host owns the application shell and the run loop and
    // calls update() once per frame.
    class SPITE_ENGINE_API Engine
    {
    public:
        explicit Engine(EngineDesc desc);
        ~Engine();

        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;
        Engine(Engine&&) = delete;
        Engine& operator=(Engine&&) = delete;

        // A frame is beginFrame, fixedUpdate per application fixed step, update.
        // beginFrame executes control requests, so a step lands before the
        // frame's fixed steps, then runs the fixed steps `step fixed=N` queued.
        void beginFrame(float fixedDeltaTime);
        // One fixed step of every world update() runs, in the same order.
        void fixedUpdate(float fixedDeltaTime);
        // The asset module, then the UI worlds, the scene world (whose render
        // stages draw every UI), the play world while playing and the attached
        // worlds in one stage loop (World::updateWorlds): every world simulates
        // and uploads before the scene world renders. interpolationAlpha reaches
        // systems as SystemContext::interpolationAlpha. deltaTime is the scaled time
        // (zero while paused or at time scale 0); unscaledDeltaTime is the real frame
        // time, negative: the same as deltaTime. The UI worlds run on the unscaled time, so tweens, tooltips and
        // dialogs keep time at time scale 0.
        void update(float deltaTime, float interpolationAlpha, float unscaledDeltaTime = -1.0f);
        // The real (unscaled) frame time the last update() ran with: what the editor's status bar shows and times
        // its toasts by.
        [[nodiscard]] float lastFrameSeconds() const { return m_lastFrameSeconds; }

        // Draws a host-owned world through the scene world's GPU scene: the
        // render and asset runtimes bind it and update() runs it with the
        // engine's worlds. Returns its scene id for RenderView::scene. Detach
        // before destroying the world.
        u32 attachWorld(World& world);
        void detachWorld(const World& world);
        // Queues a PNG readback of an offscreen view. Completion runs on the frame thread or during shutdown.
        // Returns false when captures are unavailable or the view is invalid.
        bool captureView(u32 view, std::filesystem::path path, u32 settle, std::function<void(std::string)> completion);
        enum class CaptureCancelResult
        {
            Cancelled,
            Encoding,
            NotFound
        };
        // Zero means unavailable; tickets identify requests, not views.
        u64 captureViewTracked(u32 view, std::filesystem::path path, u32 settle,
                               std::function<void(std::string)> completion);
        CaptureCancelResult cancelCapture(u64 ticket);
        // A semantic panel-target screenshot needs this view rendered through its readback.
        [[nodiscard]] bool screenshotNeedsView(u32 view) const;
        // The world a render scene id names (RenderView::scene): the scene world (0), the play world while it
        // plays, an attached world; null for an id none owns.
        [[nodiscard]] const World* sceneWorld(u32 scene) const;

        // the scene world: the engine-owned handle consumers that keep a World& bind to
        [[nodiscard]] World& world() { return m_world; }
        // the scene world's container; falls back to the root container on lookup
        [[nodiscard]] ServiceLocator& services() { return m_worldServices; }
        [[nodiscard]] AssetRuntime& assets() { return *m_assets; }
        // the Localization root service (every world finds it) and the files that feed it; the locale-changed
        // signal is Localization::revision(), also delivered to LocalizationContent::subscribe listeners at the
        // frame point (beginFrame)
        [[nodiscard]] Localization& localization() { return *m_localization; }
        [[nodiscard]] LocalizationContent& localizationContent() { return *m_locContent; }
        [[nodiscard]] RenderRuntime& render() { return *m_render; }
        [[nodiscard]] EntityTransformManager& transforms() { return *m_transforms; }
        // A UI world (/) a tool builds its UI in, served over control
        // as name (refused, null, when a world of that name exists). With an application the engine
        // feeds it input at pre-update, before the game UI and the UI worlds added before it, and
        // withholds what it claimed from gameplay input contexts; its view paints in the frame and
        // the scene world's render graph draws it onto the swapchain after the game UI, over the
        // UI worlds added before it. Remove it before destroying what its UI references.
        UiWorld* addUiWorld(std::string name);
        void removeUiWorld(UiWorld& world);
        [[nodiscard]] UiWorld* uiWorld(std::string_view name);
        // in the order added: the last draws on top and takes input first
        [[nodiscard]] std::span<const std::unique_ptr<UiWorld>> uiWorlds() const { return m_uiWorlds; }
        // The scene world's UI: game UI built by code, drawn into the game view (gameView()),
        // which a tool shows in its UI or the UI worlds draw over on the swapchain. It gets
        // the input the UI worlds left over, and none while the scene world is paused.
        // While playing, the play world's UI (PlayWorld::ui) takes its place in both.
        [[nodiscard]] UiContext& sceneUi() { return *m_sceneUi; }
        [[nodiscard]] UiViewContext& sceneUiView() { return *m_sceneUiView; }
        // The UI layers that take input this frame, topmost first (UiInputAdapter::route):
        // the UI worlds over the window, then the game UI (the play world's while playing),
        // drawn into the game view and hit-tested in its pixels, unless its world is paused
        // or the game lacks input focus (setGameInputFocus).
        [[nodiscard]] std::span<const UiInputLayer> uiInputLayers() const;
        // The control server: serves the worlds "scene" (the default),
        // "play" while playing and each UI world by name; requests execute at the
        // start of update(). A windowed engine serves screenshot: a PNG of the
        // presented frame, a UI world's dock panel or a render view, written under the
        // `captures` root, <artifactsRoot>/<machine>/captures/ (file args are relative to named roots).
        [[nodiscard]] ControlServer& control() { return *m_control; }
        [[nodiscard]] SceneRecoveryAccess& sceneRecovery() { return *m_sceneRecovery; }
        // Frame thread only: restore a selected complete publication record before any command or frame; retain the
        // journal. GPU bindings load asynchronously: the host must keep the result hidden until they are ready.
        Expected<void> recoverPublication(std::string_view journalId);
        // Select and restore the sole journal record; empty means none, multiple records are refused.
        Expected<std::string> recoverPendingPublication();
        // The runtime modules: config.modules load at construction,
        // before the worlds initialize their systems; all unload at destruction,
        // after Stop. Play attaches the play world to it. Unload through it with
        // frameWorlds(); the reloadModule control command (name=<module>) swaps
        // in a rebuilt module between frames with its component state kept,
        // loadModule and unloadModule load and unload one (persist=true also
        // edits the module list in the user config; unpersistModule edits it
        // alone).
        [[nodiscard]] ModuleHost& modules() { return *m_modules; }
        // every world update() runs: the UI worlds, scene, play while playing, attached
        [[nodiscard]] std::span<const World> frameWorlds() const { return m_frameWorlds; }
        [[nodiscard]] Window* window() const { return m_window; }
        // an OS window owned by the main one, with its own presentation target; INVALID_WINDOW_ID without a
        // window service (headless) or when the platform or the GPU refuses it. Its rect and input are its owner's
        // business; closeWindow (or the engine's end) destroys it.
        [[nodiscard]] WindowId openWindow(SecondaryWindowDesc desc);
        void closeWindow(WindowId id);
        [[nodiscard]] Window* secondaryWindow(WindowId id) const;
        // The UI display settings (UI scale, text scale, reduced motion): a write applies to every UI at the next
        // update(), in one re-layout; the `uiDisplay` control command writes the same. update() makes a non-finite or
        // non-positive field 1 and clamps the others to their ranges.
        [[nodiscard]] UiDisplaySettings& uiDisplay() { return m_uiDisplay; }
        // Applies the display settings now, as update() does first: a command that wrote them calls it so every UI
        // has laid out at them when the command replies. Costs nothing when they did not change.
        void settleUiDisplay();
        [[nodiscard]] const ApplicationConfig& config() const { return m_config; }
        // The application's config store (the `config` query lists its sections): null without an application. A host
        // declares the files its own modules read and write (ConfigStore::declareFile), so the query lists them.
        [[nodiscard]] ConfigStore* configStore() const { return m_configStore; }

        // Device heaps clamped by config.graphics.gpuMemoryBudgetMB: the ceiling
        // residency admission and streaming growth work against.
        [[nodiscard]] GpuBudget gpuBudget() const;
        // Visible-meshlet capacity doublings since startup.
        [[nodiscard]] u32 overflowGrowthEvents() const { return m_overflowGrowthEvents; }

        // The edit scene as a scene document: every scene-world root (an entity
        // without a transform parent) and its subtree. UI elements are runtime
        // state built by code, never scene content, so they stay out.
        [[nodiscard]] Expected<SceneDocument> collectEditScene();

        // Play: clones the edit scene document into the play world,
        // served as "play" over control, and pauses the scene world; Stop
        // discards the play world and restores the scene world's pause state.
        // While playing, the game view (gameView()) draws the play world through
        // the clone of its edit camera, and so do the views that follow the game
        // (setViewFollowsGame); Stop binds them back to the edit scene, which
        // stays resident throughout. The play world's game UI draws into the
        // game view and takes input in place of the scene world's until Stop
        // discards it; the UI worlds neither navigate nor keep their focus
        // meanwhile (the game's keys are its own). Also the control commands
        // play and stop.
        Expected<void> play();
        bool stop(); // false when not playing
        [[nodiscard]] PlayWorld* playWorld() { return m_play.get(); }
#ifdef SPITE_SHIPPING
        [[nodiscard]] bool renderDocEnabled() const { return false; }
#else
        [[nodiscard]] bool renderDocEnabled() const { return m_renderDoc != nullptr; }
#endif
        [[nodiscard]] bool rendererRestartPending() const { return m_restartRendererPending; }
        // Called once Play made the play world (true) and once before Stop discards it (false), while both worlds
        // are served: a tool carries its state across through PlayWorld::cloneOf and sourceOf.
        // One listener at a time (asserted); an empty function clears it.
        void setPlayListener(std::function<void(bool playing)> listener);

        // The world the game view draws: the play world while playing, else the scene world; its control world name
        // and its transforms.
        [[nodiscard]] World& gameWorld();
        [[nodiscard]] EntityTransformManager& gameTransforms();
        [[nodiscard]] std::string_view gameWorldName() const { return m_play ? "play" : "scene"; }

        // The render view the game draws into (RenderRuntime::setGameView): the game camera (the play
        // world's clone of it while playing) and the game UI; view 0 by default. Moving it moves a
        // running Play's binding along; the old view returns to the edit scene through the camera Play
        // started from, unless it follows the game. False when out of range.
        bool setGameView(u32 view);
        [[nodiscard]] u32 gameView() const { return m_gameView; }
        // A view that follows the game draws the game view's world: Play binds it to the play world
        // with the game view's camera and Stop back. Until Play it shares the game view's camera
        // binding: bindCamera on either binds both, and a follower drawn from its own pose
        // (RenderView::pose) with no live camera takes the game view's. A tool's own view of the game
        // world (a camera it flies) follows. Following while playing binds it to the play world now.
        void setViewFollowsGame(u32 view, bool follows);
        [[nodiscard]] bool viewFollowsGame(u32 view) const;
        // the view's output extent as the render runtime resolves it: the window's for a view with none
        [[nodiscard]] Extent2D viewDisplayExtent(u32 view) const;
        // While set, the setSwapchainView command is refused with this reason (a tool that renders
        // every view offscreen into its own UI); empty clears it.
        void setSwapchainViewLock(std::string reason) { m_swapchainViewLock = std::move(reason); }
        // Whether the game takes input: its UI (uiInputLayers) and the input
        // contexts below TOOL_INPUT_PRIORITY, which tools' input contexts sit at
        // or above. A tool grants it while its view of the game has focus; on by
        // default.
        static constexpr i32 TOOL_INPUT_PRIORITY = 100;
        void setGameInputFocus(bool focused);
        [[nodiscard]] bool gameInputFocused() const { return m_gameInputFocused; }

        // The control verbs only a window serves (window commands, screenshots, frame captures, GPU tables); a
        // windowless engine answers them as unavailable.
        static std::span<const std::string_view> windowedVerbs();
        // The control verbs that need the GPU device beyond a window's: render graph passes, view modes, shaders, the
        // render pipeline, asset loads and imports (an import settles in the asset update, which runs only with the
        // device). An engine built with EngineDesc::gpu false answers them, and the windowed verbs, as unavailable.
        static constexpr std::array<std::string_view, 14> GPU_VERBS{
            "setPassEnabled",      "setPassesEnabled", "viewMode",       "setViewMode",
            "reloadShaders",       "shaderStatus",     "renderPipeline", "reloadRenderPipeline",
            "loadAsset",           "instantiateModel", "importModel",    "reimportModel",
            "importExternalModel", "createPrimitive"};

    private:
        bool canRecoverPublication();
        Expected<void> recoverPublicationLocked(std::string_view journalId);
        friend struct VisibleGrowthTestAccess;
        void growVisibleCapacity(u32 visibleCount, u32 capacity);
        void rebuildFrameWorlds();
        // each animated world is seen from the camera of the view that draws it (AnimationWorld::setViewer)
        void feedAnimationViewers();
        // the game UI (the play world's while playing) draws into the game view, the UI worlds onto the swapchain
        void bindUiTargets();
        [[nodiscard]] std::string rendererRestartBlocker();
        void restartRenderer(std::unique_ptr<RenderDocCapture> preparedRenderDoc);

        ApplicationConfig m_ownedConfig; // used only without an application
        ApplicationConfig& m_config;
        ConfigStore* m_configStore = nullptr;
        // the physics.world config section: what the next play world starts from and the running one is set to
        PhysicsSettings m_physicsSettings;
        Window* m_window = nullptr;
        Application* m_application = nullptr;
        std::vector<WindowId> m_secondaryWindows; // detached dock windows, one OS window each
        std::function<void(Engine&, std::vector<RenderPassModuleDefinition>&)> m_extraPassModules;
        ServiceLocator m_rootServices;
        ServiceLocator m_worldServices{&m_rootServices};
        World m_world;
        std::unique_ptr<RenderRuntime> m_render;
        std::unique_ptr<RenderRuntimeDesc> m_renderDesc;
        bool m_restartRendererPending = false;
        bool m_restartRenderDocTarget = false;
        bool m_renderDocPreviousDebugLabels = false;
        std::string m_restartRendererStatus = "idle";
        std::unique_ptr<EntityTransformManager> m_transforms;
        // the edit scene's: skins bind and poses live here, the play world has its own
        std::unique_ptr<AnimationWorld> m_animation;
        std::unique_ptr<AssetRuntime> m_assets;
        std::vector<std::unique_ptr<UiWorld>> m_uiWorlds;
        std::unique_ptr<UiContext> m_sceneUi;
        std::unique_ptr<UiViewContext> m_sceneUiView;
        std::unique_ptr<UiTextureResolver> m_sceneUiTextures;
        std::unique_ptr<EnvironmentResolver> m_environments;
        // the UI worlds topmost first, then the game UI
        std::vector<UiInputLayer> m_uiLayers;
        std::unique_ptr<UiInputAdapter> m_uiInput;
        std::shared_ptr<Localization> m_localization;
        std::unique_ptr<LocalizationContent> m_locContent;
        std::unique_ptr<ControlServer> m_control;
        std::unique_ptr<SceneRecoveryAccess> m_sceneRecovery;
        u64 m_recoveryPumpCount = 0;
        u64 m_recoveryCommandGeneration = 0;
        u64 m_recoveryRegistryGeneration = 0;
        u64 m_recoveryManifestGeneration = 0;
        u64 m_recoveryImportsGeneration = 0;
        u32 m_recoveryLoadsStarted = 0;
        u64 m_recoveryAssetUpdateFrames = 0;
        u64 m_recoveryFixedSteps = 0;
        bool m_recoveryCompleted = false;
        std::unique_ptr<ModuleHost> m_modules;
#ifndef SPITE_SHIPPING
        std::unique_ptr<Screenshots> m_screenshots; // windowed only
        std::unique_ptr<GpuTables> m_gpuTables; // windowed only
        std::unique_ptr<PixelInfo> m_pixelInfo; // windowed only
#endif
        std::unique_ptr<QuiescenceMeter> m_quiescence;
#ifndef SPITE_SHIPPING
        std::unique_ptr<FrameCapture> m_frameCapture; // windowed only
        std::unique_ptr<ProfileCapture> m_profileCapture;
        std::unique_ptr<RenderDocCapture> m_renderDoc; // startup option or an explicitly enabled restart
#endif
        std::unique_ptr<PlayWorld> m_play;
        std::unique_ptr<UiTextureResolver> m_playUiTextures; // while playing
        u32 m_playScene = 0; // the play world's render scene id while playing
        Entity m_editCamera = Entity::undefined(); // the game view's camera before Play
        std::function<void(bool)> m_playListener;
        bool m_scenePausedBeforePlay = false;
        float m_lastFrameSeconds = 0.0f;
        u32 m_gameView = 0;
        UpscalerConfig m_appliedUpscale; // graphics.upscaler as last given to the game view
        std::vector<u32> m_gameFollowers; // setViewFollowsGame
        std::string m_swapchainViewLock;
        bool m_gameInputFocused = true;
        struct AttachedWorld
        {
            const World* world = nullptr;
            u32 scene = 0;
        };
        std::vector<AttachedWorld> m_attached;
        // what update() runs, in order: the UI worlds, scene, play, attached
        std::vector<World> m_frameWorlds;
        std::shared_ptr<LogHistory> m_logHistory;
        ApplicationEventBus::Subscription m_uiInputFrame;
        UiDisplaySettings m_uiDisplay;
        float m_uiScale = 1.0f; // the layout scale the UIs were last laid out at: display scale times uiDisplay.uiScale
        float m_textScale = 1.0f; // likewise the text scale
        bool m_reducedMotion = false;
        void applyUiDisplay(UiViewContext& view, UiContext& ui, bool fresh);
        [[nodiscard]] float displayScale() const;
        u32 m_overflowGrowthEvents = 0;
        u32 m_visibleGrowthBackoff = 0;
        bool m_guardLogged = false;
        // last: the handles unregister the sections (whose callbacks hold this) before anything else is destroyed
        ConfigStore::SectionHandle m_physicsConfig;
        ConfigStore::SectionHandle m_navigationConfig;
    };

    // Empty when engineSize and descSize are what SpiteEngine.dll was built with, else what differs. An exe or
    // module compiled against another Engine.hpp builds an Engine and an EngineDesc of the wrong layout.
    SPITE_ENGINE_API std::string engineAbiMismatch(size_t engineSize, size_t descSize);
    // ends the process with that text on stderr instead of letting the first read of a shifted field crash
    SPITE_ENGINE_API void requireEngineAbi(size_t engineSize, size_t descSize);
    // every binary that includes this header runs the check as it loads, before any Engine exists
    inline const bool ENGINE_ABI_CHECKED = (requireEngineAbi(sizeof(Engine), sizeof(EngineDesc)), true);
} // namespace spite
