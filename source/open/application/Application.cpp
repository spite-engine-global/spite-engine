#include "base/WritableLayout.hpp"
#include "application/Application.hpp"
#include "base/ContentRoot.hpp"

#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include "ApplicationConfigFields.hpp"
#include "application/SchemaCodecs.hpp"
#include "serialization/JsonFormat.hpp"
#include "application/ConfigStore.hpp"
#include "application/Window.hpp"
#include "EventDispatcher.hpp"
#include "application/FrameClock.hpp"
#include "application/FrameScheduler.hpp"

#include "base/Logging.hpp"
#include "base/io/FileSystem.hpp"

#include "profiling/Zone.hpp"


// Remove external reference to global event bus since we're now using the local one

namespace
{
    bool validateWindowConfig(const spite::WindowConfig& config) { return config.width > 0 && config.height > 0; }

    bool validateGraphicsConfig(const spite::GraphicsConfig& config)
    {
        // what ApplicationConfig::validate also holds: a file the store accepts is a config the engine accepts
        return config.maxFramesInFlight > 0 && config.maxBindlessTextures > 0 && config.maxBindlessInstances > 0 &&
            config.readbackStagingMB > 0 && config.gpuHangTimeoutMs > 0 && config.shadows.valid() &&
            config.upscaler.valid() && config.bloom.valid() && spite::validTonemap(config.tonemap) &&
            config.exposureValid();
    }

    bool validateInputConfig(const spite::InputConfig& config)
    {
        return config.mappingsFile.toView()[0] != '\0' && config.controllerDeadzone >= 0.0f &&
            config.controllerDeadzone < 1.0f && config.controllerSensitivity >= 0.0f;
    }

    bool validateTimingConfig(const spite::TimingConfig& config)
    {
        return (!config.limitFPS || config.targetFPS > 0) && config.fixedDeltaTime > 0.0f && config.maxFixedSteps > 0;
    }

    constexpr float MAX_TIME_SCALE = 1000.0f; // the timeScale field's range (ApplicationConfigFields)

    // The setters change what runs now; the same change goes to the staged value, or a later commit of another timing
    // field (setConfig) would restore the old one at the next frame boundary.
    template <typename Change>
    void stageTiming(spite::ConfigStore& store, const Change& change)
    {
        if (auto edit = store.beginEdit<spite::TimingConfig>(spite::InternedString("application.timing")))
        {
            change(edit->get());
            (void)edit->commit();
        }
    }

    const char* windowStateName(const spite::WindowState state)
    {
        switch (state)
        {
        case spite::WindowState::Minimized:
            return "minimized";
        case spite::WindowState::Maximized:
            return "maximized";
        case spite::WindowState::Hidden:
            return "hidden";
        case spite::WindowState::Normal:
            break;
        }
        return "normal";
    }

    // The schema writes enums as integers; the window file names its state.
    spite::ConfigCodecs windowConfigCodecs()
    {
        return {[](const void* value)
                {
                    const auto& config = *static_cast<const spite::WindowConfig*>(value);
                    spite::JsonFragmentWriter writer;
                    writer.writeU32("width", config.width);
                    writer.writeU32("height", config.height);
                    writer.writeString("title", config.title.toView());
                    writer.writeBool("resizable", config.resizable);
                    writer.writeBool("fullscreen", config.fullscreen);
                    writer.writeBool("borderless", config.borderless);
                    writer.writeString("state", windowStateName(config.state));
                    writer.writeBool("highDpi", config.highDpi);
                    std::string text;
                    writer.dump(text, spite::CONFIG_FILE_INDENT);
                    return text;
                },
                [](const void* base, const std::string_view text) -> std::shared_ptr<void>
                {
                    auto config = std::make_shared<spite::WindowConfig>(*static_cast<const spite::WindowConfig*>(base));
                    if (text.empty())
                    {
                        return config;
                    }
                    spite::JsonFragmentReader reader(text);
                    std::string title;
                    std::string state;
                    const spite::FieldRead results[] = {reader.readU32("width", config->width),
                                                        reader.readU32("height", config->height),
                                                        reader.readString("title", title),
                                                        reader.readBool("resizable", config->resizable),
                                                        reader.readBool("fullscreen", config->fullscreen),
                                                        reader.readBool("borderless", config->borderless),
                                                        reader.readString("state", state),
                                                        reader.readBool("highDpi", config->highDpi)};
                    for (const spite::FieldRead result : results)
                    {
                        if (result == spite::FieldRead::Error)
                        {
                            return nullptr;
                        }
                    }
                    if (results[2] == spite::FieldRead::Ok)
                    {
                        config->title = spite::InternedString(title);
                    }
                    if (results[6] == spite::FieldRead::Ok)
                    {
                        bool known = false;
                        for (const spite::WindowState candidate :
                             {spite::WindowState::Normal, spite::WindowState::Minimized, spite::WindowState::Maximized,
                              spite::WindowState::Hidden})
                        {
                            if (state == windowStateName(candidate))
                            {
                                config->state = candidate;
                                known = true;
                            }
                        }
                        if (!known)
                        {
                            return nullptr;
                        }
                    }
                    return config;
                }};
    }

    // each module has its own CRT environment copy: ask the OS
    bool boundedRun()
    {
#if defined(_WIN32)
        // The Win32 process environment is shared by modules with separate static CRTs.
        return GetEnvironmentVariableA("SPITE_NO_USER_STATE", nullptr, 0) != 0;
#else
        return std::getenv("SPITE_NO_USER_STATE") != nullptr;
#endif
    }

    using ModuleList = std::vector<std::string>;

    // {"modules": ["Name", ...]}: a string list, which the schema lattice lacks
    spite::ConfigCodecs moduleListCodecs()
    {
        return {[](const void* value)
                {
                    const auto& modules = *static_cast<const ModuleList*>(value);
                    spite::JsonFragmentWriter writer;
                    writer.beginArray("modules", static_cast<spite::u32>(modules.size()));
                    for (const std::string& module : modules)
                    {
                        writer.writeString({}, module);
                    }
                    writer.endArray();
                    std::string text;
                    writer.dump(text, spite::CONFIG_FILE_INDENT);
                    return text;
                },
                [](const void* base, const std::string_view text) -> std::shared_ptr<void>
                {
                    auto modules = std::make_shared<ModuleList>(*static_cast<const ModuleList*>(base));
                    if (text.empty())
                    {
                        return modules;
                    }
                    spite::JsonFragmentReader reader(text);
                    spite::u32 count = 0;
                    const spite::FieldRead array = reader.beginArray("modules", count);
                    if (array == spite::FieldRead::Error)
                    {
                        return nullptr;
                    }
                    if (array == spite::FieldRead::Absent)
                    {
                        return modules;
                    }
                    modules->assign(count, {});
                    for (std::string& module : *modules)
                    {
                        if (reader.readString({}, module) != spite::FieldRead::Ok)
                        {
                            return nullptr;
                        }
                    }
                    reader.endArray();
                    return modules;
                }};
    }
} // namespace

namespace spite
{
    Application::Application(const ApplicationConfig& config) : m_config(config)
    {
        const std::filesystem::path configRoot = resolveContentPath(
            m_config.configPath.empty() ? std::filesystem::path("config") : std::filesystem::path(m_config.configPath));
        const bool defaultConfig = m_config.configPath.empty() ||
            std::filesystem::path(m_config.configPath).lexically_normal() == std::filesystem::path("config/");
        m_configStore = std::make_unique<ConfigStore>(
            m_config.projectConfigPath.empty() ? configRoot / "project"
                                               : resolveContentPath(pathFromUtf8(m_config.projectConfigPath)),
            defaultConfig ? writableDirectory(WritableFolder::UserConfig, {}, !boundedRun()) : configRoot / "user",
            m_config.projectConfigPath.empty() ? std::filesystem::path{} : configRoot / "project");
        m_configStore->setChangePublisher([this](const ConfigSectionId sectionId)
                                          { m_eventBus.enqueue(ConfigChangedEvent{sectionId}); });
        registerConfigSections();
        m_configStore->loadAll();

        // Validate configuration
        if (!m_config.validate())
        {
            SERROR_LOG("Invalid application configuration provided");
            // Could throw an exception or set an error state, but for now we'll continue with defaults
        }

        SINFO_LOG("Creating Application instance with config: width={}, height={}, title={}", m_config.window.width,
                  m_config.window.height, m_config.window.title.toView());
        // Initialize the event dispatcher with the local event bus
        m_eventDispatcher = std::make_unique<EventDispatcher>(m_eventBus);
        SDEBUG_LOG("Application instance created successfully");
    }

    void Application::registerConfigSections()
    {
        ConfigSection<WindowConfig> windowSection{};
        windowSection.sectionId = InternedString("application.window");
        windowSection.relativePath = "application/window.json";
        windowSection.defaultValue = m_config.window;
        windowSection.validate = validateWindowConfig;
        windowSection.doc = "The primary window: size, state and flags; saved per user.";
        windowSection.fields = windowConfigFields();
        windowSection.applyMode = ConfigApplyMode::LiveAtFrameBoundary;
        // runtime size, state and fullscreen save per user; the project file keeps the defaults
        // (a bounded run, SPITE_NO_USER_STATE, neither reads nor writes it)
        windowSection.persistenceLayer = boundedRun() ? ConfigPersistenceLayer::Memory : ConfigPersistenceLayer::User;
        windowSection.codecs = windowConfigCodecs();
        windowSection.apply = [this](const WindowConfig& windowConfig)
        {
            m_config.window = windowConfig;
            if (m_windowService)
                m_windowService->applyPrimaryConfig(windowConfig);
        };
        m_windowConfigHandle = m_configStore->registerSection(windowSection);

        ConfigSection<GraphicsConfig> graphicsSection{};
        graphicsSection.sectionId = InternedString("application.graphics");
        graphicsSection.relativePath = "application/graphics.json";
        graphicsSection.defaultValue = m_config.graphics;
#ifdef SPITE_SHOWCASE_BUNDLE
        graphicsSection.persistenceLayer = boundedRun() ? ConfigPersistenceLayer::Memory : ConfigPersistenceLayer::User;
        graphicsSection.applyMode = ConfigApplyMode::LiveAtFrameBoundary;
#endif
        graphicsSection.validate = validateGraphicsConfig;
        graphicsSection.doc = "Renderer limits, buffer sizes, culling switches and sun shadows. Most fields size "
                              "resources at startup; the shadow fields, upscaler, bloom, tonemap, the exposure fields "
                              "and objectCapacityCeiling are live.";
        graphicsSection.fields = graphicsConfigFields();
        graphicsSection.codecs = schemaCodecs<GraphicsConfig>();
        graphicsSection.apply = [this](const GraphicsConfig& graphicsConfig) { m_config.graphics = graphicsConfig; };
        m_graphicsConfigHandle = m_configStore->registerSection(graphicsSection);

        ConfigSection<InputConfig> inputSection{};
        inputSection.sectionId = InternedString("application.input");
        inputSection.relativePath = "application/input.json";
        inputSection.defaultValue = m_config.input;
        inputSection.validate = validateInputConfig;
        inputSection.doc = "Input devices: the mapping file, mouse look, gamepad and touch.";
        inputSection.fields = inputConfigFields();
        inputSection.codecs = schemaCodecs<InputConfig>();
        inputSection.applyMode = ConfigApplyMode::LiveAtFrameBoundary;
        inputSection.apply = [this](const InputConfig& inputConfig)
        {
            m_config.input = inputConfig;
            if (m_input)
            {
                m_input->applyConfig(inputConfig);
            }
        };
        m_inputConfigHandle = m_configStore->registerSection(inputSection);

        ConfigSection<TimingConfig> timingSection{};
        timingSection.sectionId = InternedString("application.timing");
        timingSection.relativePath = "application/timing.json";
        timingSection.defaultValue = m_config.timing;
        timingSection.validate = validateTimingConfig;
        timingSection.doc = "Frame pacing and the simulation clock: frame limit, vsync, time scale, fixed step.";
        timingSection.fields = timingConfigFields();
        timingSection.codecs = schemaCodecs<TimingConfig>();
        timingSection.applyMode = ConfigApplyMode::LiveAtFrameBoundary;
#ifdef SPITE_SHOWCASE_BUNDLE
        timingSection.persistenceLayer = boundedRun() ? ConfigPersistenceLayer::Memory : ConfigPersistenceLayer::User;
#endif
        // a bounded run (RunEngine, `spite run`, the panel scenarios) counts frames and waits on them: presenting with
        // FIFO would pace each at the display refresh. setConfig still turns it on; a person's session keeps the file.
        if (boundedRun())
        {
            timingSection.overrideLoaded = [](TimingConfig& config) { config.vsyncEnabled = false; };
        }
        timingSection.apply = [this](const TimingConfig& config)
        {
            m_config.timing = config;
            if (m_frameClock)
            {
                m_frameClock->setTimeScale(config.timeScale);
                m_frameClock->setMaximumDeltaTime(config.maxFrameTime > 0
                                                      ? static_cast<float>(config.maxFrameTime) / 1000.0f
                                                      : std::numeric_limits<float>::max());
            }
            if (m_frameScheduler)
            {
                m_frameScheduler->setFixedDeltaTime(config.fixedDeltaTime);
                m_frameScheduler->setMaximumFixedSteps(config.maxFixedSteps);
                m_frameScheduler->setTargetFps(config.targetFPS);
                m_frameScheduler->setLimitFps(config.limitFPS);
            }
        };
        m_timingConfigHandle = m_configStore->registerSection(timingSection);

        ConfigSection<ModuleList> modulesSection{};
        modulesSection.sectionId = InternedString("application.modules");
        modulesSection.relativePath = "application/modules.json";
        modulesSection.defaultValue = m_config.modules;
        // runtime edits (loadModule persist) save over the checked-in list in the user layer
        modulesSection.persistenceLayer = ConfigPersistenceLayer::User;
        modulesSection.doc = "Runtime module DLLs the engine loads at startup, in order; loadModule and "
                             "unloadModule with persist=true and unpersistModule edit it.";
        modulesSection.writeValues = [](const ModuleList& modules, IFormatWriter& out)
        {
            out.beginArray("modules", static_cast<u32>(modules.size()));
            for (const std::string& module : modules)
            {
                out.writeString({}, module);
            }
            out.endArray();
        };
        modulesSection.codecs = moduleListCodecs();
        modulesSection.apply = [this](const ModuleList& modules) { m_config.modules = modules; };
        m_modulesConfigHandle = m_configStore->registerSection(modulesSection);
    }

    void Application::setupInternalEventHandlers()
    {
        m_internalEventSubscriptions.push_back(m_eventBus.subscribe<WindowCloseEvent>(
            [this](const WindowCloseEvent& event)
            {
                if (m_windowService && event.windowId == m_windowService->getPrimaryWindowId())
                    requestShutdown();
            }));

        m_internalEventSubscriptions.push_back(m_eventBus.subscribe<WindowResizeEvent>(
            [this](const WindowResizeEvent& e)
            {
                if (!m_windowService || e.windowId != m_windowService->getPrimaryWindowId())
                    return;

                bool isMinimized = (e.width == 0 || e.height == 0);
                if (isMinimized && m_state == ApplicationState::Running)
                {
                    m_pendingPauseState = true;
                }
                else if (!isMinimized && m_state == ApplicationState::Paused)
                {
                    m_pendingPauseState = false;
                }
            }));

        m_internalEventSubscriptions.push_back(m_eventBus.subscribe<WindowFocusEvent>(
            [this](const WindowFocusEvent& e) { SDEBUG_LOG("Window focus: {}", e.hasFocus); }));
    }

    Application::~Application()
    {
        SINFO_LOG("Destroying Application instance");
        if (m_state != ApplicationState::Stopped && m_state != ApplicationState::Uninitialized)
        {
            shutdown();
        }
    }

    bool Application::initialize(const ApplicationMode mode)
    {
        if (m_state != ApplicationState::Uninitialized)
        {
            SERROR_LOG("Application already initialized or in invalid state (current state: {})",
                       static_cast<int>(m_state));
            return false;
        }

        SINFO_LOG("Starting application initialization");
        m_mode = mode;
        updateState(ApplicationState::Initializing);

        // Initialize platform abstraction
        if (!initializePlatform())
        {
            SERROR_LOG("Failed to initialize platform abstraction");
            updateState(ApplicationState::Stopped);
            return false;
        }
        SDEBUG_LOG("Platform abstraction initialized");

        // Initialize components
        if (!initializeWindow())
        {
            SERROR_LOG("Failed to initialize window");
            updateState(ApplicationState::Stopped);
            return false;
        }
        SDEBUG_LOG("Window initialized");

        if (!initializeInput())
        {
            SERROR_LOG("Failed to initialize input system");
            updateState(ApplicationState::Stopped);
            return false;
        }
        SDEBUG_LOG("Input system initialized");

        if (!initializeFrameTiming())
        {
            SERROR_LOG("Failed to initialize time system");
            updateState(ApplicationState::Stopped);
            return false;
        }
        SDEBUG_LOG("Time system initialized");

        setupInternalEventHandlers();

        m_eventBus.enqueue(AppInitializeEvent{this});
        m_eventBus.drain();

        updateState(ApplicationState::Running);
        if (m_mode == ApplicationMode::Headless)
        {
            SINFO_LOG("Application initialized headless: no window");
        }
        else
        {
            SINFO_LOG("Application initialized successfully with {}x{} window", m_config.window.width,
                      m_config.window.height);
        }
        return true;
    }

    void Application::run()
    {
        if (m_state != ApplicationState::Running)
        {
            SERROR_LOG("Application not in running state");
            return;
        }

        SINFO_LOG("Starting application main loop");
        prof::setThreadName("main");

        while (shouldContinue())
        {
            SPITE_FRAME();
            const double frameStartSeconds = m_frameClock->nowSeconds();
            const FrameTiming& timing = m_frameClock->tick();
            // before the poll: the OS reports what last frame's commands did
            // (a minimize, a resize) before this frame renders
            m_windowService->drainCommands();
            processEvents();
            updateAndRender(timing);

            const bool headless = m_mode == ApplicationMode::Headless;
            const double nowSeconds = m_frameClock->nowSeconds();
            float sleepDuration = m_frameScheduler->getSleepDuration(frameStartSeconds, nowSeconds,
                                                                     m_config.timing.vsyncEnabled && !headless);
            // no vsync waits headless: without a frame rate limit the loop runs at the fixed-step rate, not flat out
            if (headless && !m_frameScheduler->shouldLimitFps())
            {
                sleepDuration = std::max(
                    0.0f, m_frameScheduler->getFixedDeltaTime() - static_cast<float>(nowSeconds - frameStartSeconds));
            }
            if (sleepDuration > 0.0f)
                m_platform->sleep(sleepDuration);
            m_input->reset();
            if (m_state == ApplicationState::Paused)
                m_eventDispatcher->pollEvents(16);
        }

        SINFO_LOG("Application main loop ended");
    }

    void Application::updateAndRender(const FrameTiming& timing)
    {
        m_input->update(timing.unscaledDeltaTime);
        m_configStore->applyPending();

        if (m_state == ApplicationState::Running)
        {
            m_eventBus.enqueue(AppPreUpdateEvent{timing.deltaTime, timing.unscaledDeltaTime, this});
            m_eventBus.drain();
            const FixedStepResult fixedSteps = m_frameScheduler->advance(timing.deltaTime);
            for (u32 step = 0; step < fixedSteps.stepCount; ++step)
            {
                m_eventBus.enqueue(AppFixedUpdateEvent{m_frameScheduler->getFixedDeltaTime(), this});
                m_eventBus.drain();
            }

            m_eventBus.enqueue(
                AppUpdateEvent{timing.deltaTime, timing.unscaledDeltaTime, fixedSteps.interpolationAlpha, this});
            m_eventBus.drain();
            m_eventBus.enqueue(AppRenderEvent{this});
            m_eventBus.drain();
        }
        m_eventBus.enqueue(AppUiRenderEvent{this});
        m_eventBus.drain();
    }

    // A live-resize frame runs inside the poll of the frame the drag interrupted: no poll, no window commands (they
    // would fight the drag) and no sleep (the swapchain paces it).
    void Application::liveResizeFrame()
    {
        if (m_state != ApplicationState::Running && m_state != ApplicationState::Paused)
        {
            return;
        }
        SPITE_FRAME();
        updateAndRender(m_frameClock->tick());
        m_input->reset();
    }

    void Application::requestShutdown()
    {
        m_shouldShutdown.store(true);
        SINFO_LOG("Shutdown requested");
    }

    void Application::shutdown()
    {
        if (m_state == ApplicationState::Stopping || m_state == ApplicationState::Stopped)
        {
            return;
        }

        updateState(ApplicationState::Stopping);

        // Publish shutdown event - subscribers can cleanup
        m_eventBus.enqueue(AppShutdownEvent{this});
        m_eventBus.drain();
        // user state only: a project file a command edited (setConfig) is rewritten by an explicit saveConfig
        (void)m_configStore->saveDirty(ConfigPersistenceLayer::User);

        cleanup();
        updateState(ApplicationState::Stopped);
        SINFO_LOG("Application shutdown completed");
    }

    void Application::pause()
    {
        if (m_state != ApplicationState::Running)
            return;

        updateState(ApplicationState::Paused);
        if (m_frameClock)
            m_frameClock->setPaused(true);
        m_eventBus.enqueue(AppPauseEvent{this});
        m_eventBus.drain();
        SINFO_LOG("Application paused");
    }

    void Application::resume()
    {
        if (m_state != ApplicationState::Paused)
            return;

        updateState(ApplicationState::Running);
        if (m_frameClock)
            m_frameClock->setPaused(false);
        m_eventBus.enqueue(AppResumeEvent{this});
        m_eventBus.drain();
        SINFO_LOG("Application resumed");
    }

    void Application::updateState(ApplicationState newState) { m_state = newState; }

    // The primary window's observed state, windowed size and fullscreen flag
    // become the config it saves and reopens with. Applying that config back
    // finds nothing to change (Window::applyConfig pushes only differences).
    void Application::recordWindowConfig(const Window& window)
    {
        const WindowConfig& observed = window.getConfig();
        const WindowConfig& current = m_config.window;
        if (observed.width == current.width && observed.height == current.height && observed.state == current.state &&
            observed.fullscreen == current.fullscreen)
        {
            return;
        }
        if (auto edit = m_configStore->beginEdit<WindowConfig>(InternedString("application.window")))
        {
            edit->get().width = observed.width;
            edit->get().height = observed.height;
            edit->get().state = observed.state;
            edit->get().fullscreen = observed.fullscreen;
            (void)edit->commit();
        }
    }

    void Application::processEvents()
    {
        m_eventDispatcher->pollEvents();
        m_eventBus.drain();
        if (const Window* window = getWindow())
        {
            recordWindowConfig(*window);
        }
        if (m_pendingPauseState.has_value())
        {
            const bool shouldPause = *m_pendingPauseState;
            m_pendingPauseState.reset();
            if (shouldPause)
            {
                pause();
            }
            else
            {
                resume();
            }
        }
    }

    bool Application::initializePlatform()
    {
        m_platform = std::make_unique<PlatformServices>(m_mode == ApplicationMode::Headless);
        return m_platform != nullptr;
    }

    bool Application::initializeWindow()
    {
        m_windowService = std::make_unique<WindowService>();
        m_eventDispatcher->setWindowService(m_windowService.get());
        if (m_mode == ApplicationMode::Headless)
        {
            return true;
        }
        if (!m_windowService->createPrimaryWindow(m_config.window))
        {
            SERROR_LOG("Failed to create window");
            return false;
        }
        return true;
    }

    bool Application::initializeInput()
    {
        m_input = m_platform->createInputSystem(m_config.input, getWindow());
        if (!m_input)
        {
            SERROR_LOG("Failed to create input system");
            return false;
        }

        m_input->setEventBus(m_eventBus);
        m_eventDispatcher->setInputSystem(m_input.get());
        m_eventDispatcher->setInputInjector(&m_injector);
        if (m_mode != ApplicationMode::Headless)
        {
            m_eventDispatcher->setLiveResizeFrame([this] { liveResizeFrame(); });
        }

        return true;
    }

    bool Application::initializeFrameTiming()
    {
        const float maximumDeltaTime = m_config.timing.maxFrameTime > 0
            ? static_cast<float>(m_config.timing.maxFrameTime) / 1000.0f
            : std::numeric_limits<float>::max();
        m_frameClock = std::make_unique<FrameClock>(std::make_unique<SdlMonotonicClock>(), maximumDeltaTime);
        m_frameScheduler =
            std::make_unique<FrameScheduler>(FrameSchedulerConfig{.fixedDeltaTime = m_config.timing.fixedDeltaTime,
                                                                  .maximumFixedSteps = m_config.timing.maxFixedSteps,
                                                                  .targetFps = m_config.timing.targetFPS,
                                                                  .limitFps = m_config.timing.limitFPS});
        m_frameClock->setTimeScale(m_config.timing.timeScale);
        m_frameClock->initialize();
        return true;
    }

    void Application::cleanup()
    {
        m_input.reset();
        m_windowService.reset();
        m_frameScheduler.reset();
        m_frameClock.reset();
        SINFO_LOG("Application cleaned up");
    }

    FrameTiming Application::getFrameTiming() const
    {
        static constexpr FrameTiming EMPTY_TIMING{};
        return m_frameClock ? m_frameClock->getTiming() : EMPTY_TIMING;
    }

    float Application::getDeltaTime() const { return getFrameTiming().deltaTime; }

    float Application::getUnscaledDeltaTime() const { return getFrameTiming().unscaledDeltaTime; }

    float Application::getTime() const { return getFrameTiming().elapsedTime; }

    float Application::getFrameRate() const
    {
        const float averageDeltaTime = getFrameTiming().averageDeltaTime;
        return averageDeltaTime > 0.0f ? 1.0f / averageDeltaTime : 0.0f;
    }

    void Application::setTimeScale(const float timeScale)
    {
        if (!std::isfinite(timeScale))
        {
            return;
        }
        m_config.timing.timeScale = std::clamp(timeScale, 0.0f, MAX_TIME_SCALE);
        stageTiming(*m_configStore, [&](TimingConfig& staged) { staged.timeScale = m_config.timing.timeScale; });
        if (m_frameClock)
            m_frameClock->setTimeScale(m_config.timing.timeScale);
    }

    float Application::getTimeScale() const { return m_frameClock ? m_frameClock->getTimeScale() : 0.0f; }

    void Application::setTargetFps(const u32 targetFps)
    {
        m_config.timing.targetFPS = targetFps;
        if (targetFps == 0)
            m_config.timing.limitFPS = false;
        stageTiming(*m_configStore,
                    [&](TimingConfig& staged)
                    {
                        staged.targetFPS = m_config.timing.targetFPS;
                        staged.limitFPS = m_config.timing.limitFPS;
                    });
        if (m_frameScheduler)
            m_frameScheduler->setTargetFps(targetFps);
        if (m_frameScheduler && targetFps == 0)
            m_frameScheduler->setLimitFps(false);
    }

    u32 Application::getTargetFps() const { return m_frameScheduler ? m_frameScheduler->getTargetFps() : 0; }

    void Application::setFixedTimestep(const float timestep)
    {
        m_config.timing.fixedDeltaTime = std::max(0.001f, timestep);
        stageTiming(*m_configStore,
                    [&](TimingConfig& staged) { staged.fixedDeltaTime = m_config.timing.fixedDeltaTime; });
        if (m_frameScheduler)
            m_frameScheduler->setFixedDeltaTime(m_config.timing.fixedDeltaTime);
    }

    float Application::getFixedTimestep() const
    {
        return m_frameScheduler ? m_frameScheduler->getFixedDeltaTime() : 0.0f;
    }

    bool Application::isTimePaused() const { return m_frameClock && m_frameClock->isPaused(); }

    float Application::getAverageDeltaTime(const u32 frameCount) const
    {
        return m_frameClock ? m_frameClock->getAverageDeltaTime(frameCount) : 0.0f;
    }

    float Application::getMinDeltaTime(const u32 frameCount) const
    {
        return m_frameClock ? m_frameClock->getMinimumDeltaTime(frameCount) : 0.0f;
    }

    float Application::getMaxDeltaTime(const u32 frameCount) const
    {
        return m_frameClock ? m_frameClock->getMaximumDeltaTime(frameCount) : 0.0f;
    }

    void Application::resetTimeStatistics()
    {
        if (m_frameClock)
            m_frameClock->resetStatistics();
    }

    u64 Application::getFrameCount() const { return getFrameTiming().frameIndex; }

    float Application::getTimeSinceStatsReset() const
    {
        return m_frameClock ? m_frameClock->getTimeSinceStatisticsReset() : 0.0f;
    }
} // namespace spite
