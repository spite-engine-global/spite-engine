#pragma once
#include <functional>
#include <memory>
#include <atomic>
#include <optional>

#include "application/ApplicationExport.hpp"
#include "base/ApplicationConfig.hpp"
#include "application/ConfigStore.hpp"

#include "application/EventBus.hpp"
#include "application/ApplicationTypes.hpp"
#include "application/PlatformServices.hpp"
#include "application/WindowService.hpp"
#include "application/input/InputInjection.hpp"
#include "application/input/InputSystem.hpp"

namespace spite
{
    class EventDispatcher;
    class FrameClock;
    class FrameScheduler;

    /**
     * @brief Enum representing different application states.
     */
    enum class ApplicationState
    {
        Uninitialized, // Application created but not initialized
        Initializing, // Currently initializing components
        Running, // Application is running normally
        Paused, // Application is paused
        Stopping, // Application is shutting down
        Stopped // Application has stopped
    };

    // Headless: SDL events only (no video or audio subsystem), no window; the
    // run loop paces itself, since no presentation waits on vsync.
    enum class ApplicationMode : u8
    {
        Windowed,
        Headless
    };

    /**
     * @brief Main application class that manages the entire application lifecycle.
     *
     * This class provides a unified interface for managing window, input, timing,
     * and the main application loop, while abstracting all platform-specific details.
     */
    class SPITE_APPLICATION_API Application
    {
    private:
        ApplicationConfig m_config;
        ApplicationState m_state = ApplicationState::Uninitialized;
        ApplicationMode m_mode = ApplicationMode::Windowed;
        std::atomic<bool> m_shouldShutdown = false;
        std::unique_ptr<ConfigStore> m_configStore;
        ConfigStore::SectionHandle m_windowConfigHandle;
        ConfigStore::SectionHandle m_graphicsConfigHandle;
        ConfigStore::SectionHandle m_inputConfigHandle;
        ConfigStore::SectionHandle m_timingConfigHandle;
        ConfigStore::SectionHandle m_modulesConfigHandle;

        std::unique_ptr<PlatformServices> m_platform;
        // Event system using optimized implementation
        ApplicationEventBus m_eventBus;
        std::unique_ptr<EventDispatcher> m_eventDispatcher;
        std::vector<ApplicationEventBus::Subscription> m_internalEventSubscriptions;
        std::optional<bool> m_pendingPauseState;

        // Core components (owned by Application when using unique_ptr from platform)
        std::unique_ptr<WindowService> m_windowService;
        std::unique_ptr<InputSystem> m_input;
        InputInjector m_injector;
        std::unique_ptr<FrameClock> m_frameClock;
        std::unique_ptr<FrameScheduler> m_frameScheduler;

        // Internal state management
        void updateState(ApplicationState newState);

        // Main loop components
        void processEvents();
        // Input, config, the update and render events: a frame's work after its poll.
        void updateAndRender(const FrameTiming& timing);
        // The frame a live resize asks for (EventDispatcher::setLiveResizeFrame).
        void liveResizeFrame();

        // Platform initialization
        bool initializePlatform();
        bool initializeWindow();
        bool initializeInput();
        bool initializeFrameTiming();
        void registerConfigSections();

        void setupInternalEventHandlers();
        // Cleanup
        void cleanup();

    public:
        /**
         * @brief Construct a new Application instance.
         * @param config Configuration to use for the application.
         */
        explicit Application(const ApplicationConfig& config = {});

        ~Application();

        // Non-copyable, non-movable to ensure single instance
        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;
        Application(Application&&) = delete;
        Application& operator=(Application&&) = delete;

        /**
         * @brief Initialize the application with its configured components.
         * @return true if initialization was successful, false otherwise.
         */
        bool initialize(ApplicationMode mode = ApplicationMode::Windowed);

        /**
         * @brief Run the main application loop.
         * @note This function blocks until the application should exit.
         */
        void run();

        /**
         * @brief Request the application to shut down gracefully.
         */
        void requestShutdown();

        /**
         * @brief Shut down the application.
         */
        void shutdown();

        ApplicationState getState() const { return m_state; }

        bool isRunning() const { return m_state == ApplicationState::Running; }

        bool shouldContinue() const { return !m_shouldShutdown; }

        void pause();
        void resume();

        /**
         * @brief Get raw pointers to platform components (non-owning access).
         * @note These are only valid after initialization and while Application exists.
         */
        Window* getWindow() const { return m_windowService ? m_windowService->getPrimaryWindow() : nullptr; }

        WindowService* getWindowService() const { return m_windowService.get(); }

        [[nodiscard]] InputSystem* getInput() const { return m_input.get(); }
        // Queues input the next event poll ingests as the device's own (control commands; see InputInjector).
        [[nodiscard]] InputInjector& getInputInjector() { return m_injector; }

        [[nodiscard]] FrameTiming getFrameTiming() const;
        [[nodiscard]] float getDeltaTime() const;
        [[nodiscard]] float getUnscaledDeltaTime() const;
        [[nodiscard]] float getTime() const;
        [[nodiscard]] float getFrameRate() const;
        void setTimeScale(float timeScale);
        [[nodiscard]] float getTimeScale() const;
        void setTargetFps(u32 targetFps);
        [[nodiscard]] u32 getTargetFps() const;
        void setFixedTimestep(float timestep);
        [[nodiscard]] float getFixedTimestep() const;
        [[nodiscard]] bool isTimePaused() const;
        [[nodiscard]] float getAverageDeltaTime(u32 frameCount = 10) const;
        [[nodiscard]] float getMinDeltaTime(u32 frameCount = 10) const;
        [[nodiscard]] float getMaxDeltaTime(u32 frameCount = 10) const;
        void resetTimeStatistics();
        [[nodiscard]] u64 getFrameCount() const;
        [[nodiscard]] float getTimeSinceStatsReset() const;

        /**
         * @brief Get the event bus instance.
         * @return Reference to the application's event bus.
         */
        ApplicationEventBus& getEventBus() { return m_eventBus; }
        const ApplicationEventBus& getEventBus() const { return m_eventBus; }

        /**
         * @brief Get the application configuration.
         * @return Const reference to the application configuration.
         */
        const ApplicationConfig& getConfig() const { return m_config; }
        // The one live instance: the Engine edits it in place and serves it as the config table
        ApplicationConfig& getMutableConfig() { return m_config; }
        ConfigStore& getConfigStore() const { return *m_configStore; }
        // Commits the window's reopen config (observed state, fullscreen flag,
        // windowed size) to the application.window section when it changed;
        // runs after each event poll for the primary window.
        void recordWindowConfig(const Window& window);
    };
} // namespace spite
