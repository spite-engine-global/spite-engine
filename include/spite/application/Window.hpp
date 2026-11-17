#pragma once

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "application/ApplicationExport.hpp"
#include "base/ApplicationConfig.hpp"
#include "application/WindowTypes.hpp"
#include "application/vulkan/IVulkanWindowSurface.hpp"
#include "base/Assert.hpp"
#include "base/InternedString.hpp"
#include "base/Platform.hpp"

namespace spite
{
    // Window commands: the core records what the OS window should do; the
    // platform adapter drains and applies them (see WindowService::drainCommands).
    struct WindowShowCommand
    {
    };
    struct WindowHideCommand
    {
    };
    struct WindowTitleCommand
    {
        std::string title;
    };
    struct WindowPositionCommand
    {
        i32 x = 0;
        i32 y = 0;
    };
    struct WindowExtentCommand
    {
        u32 width = 0;
        u32 height = 0;
    };
    struct WindowFlagsCommand
    {
        bool resizable = false;
        bool borderless = false;
    };
    struct WindowFullscreenCommand
    {
        bool fullscreen = false;
    };
    struct WindowStateCommand
    {
        WindowState state = WindowState::Normal;
    };
    struct WindowCursorCommand
    {
        CursorShape shape = CursorShape::Default;
    };

    using WindowCommand = std::variant<WindowShowCommand, WindowHideCommand, WindowTitleCommand, WindowPositionCommand,
                                       WindowExtentCommand, WindowFlagsCommand, WindowFullscreenCommand,
                                       WindowStateCommand, WindowCursorCommand>;

    /**
     * @brief A single application window: the platform-free core state plus
     *        typed access to its Vulkan surface integration.
     *
     * State changes arrive through ingestion (onResized, onFocusChanged, ...)
     * from the platform adapter; user-driven mutations (title, size, state,
     * cursor) are recorded as commands for the adapter to apply. Queries
     * reflect what the OS reported, not what was requested.
     *
     * getConfig() is the window as it should reopen: ingestion keeps its
     * state (Normal or Maximized), fullscreen flag and windowed (restore) size
     * in step with the OS window, so persisting it and applying it back
     * (applyConfig pushes only the fields that differ) never fights the OS.
     */
    class SPITE_APPLICATION_API Window
    {
    public:
        Window(WindowId id, const WindowConfig& config, IVulkanWindowSurface& surface);

        [[nodiscard]] WindowId getId() const { return m_id; }
        // Platform event-loop id (SDL window id); 0 until the adapter reports it.
        [[nodiscard]] u32 getNativeId() const { return m_nativeId; }
        [[nodiscard]] const WindowConfig& getConfig() const { return m_config; }
        [[nodiscard]] IVulkanWindowSurface& surface() const
        {
            SASSERTM(m_surface, "Window has no Vulkan surface integration.");
            return *m_surface;
        }
        // Platform-native window handle (SDL_Window* on SDL builds).
        [[nodiscard]] void* nativeWindowHandle() const { return m_nativeHandle; }

        // -- state queries --
        [[nodiscard]] WindowExtent getLogicalExtent() const { return m_logicalExtent; }
        [[nodiscard]] WindowExtent getFramebufferExtent() const { return m_framebufferExtent; }
        // the injected scale while one is set (injectDisplayScale), else the display's
        [[nodiscard]] float getDisplayScale() const
        {
            return m_injectedScale > 0.0f ? m_injectedScale : m_displayScale;
        }
        [[nodiscard]] float getHostDisplayScale() const { return m_displayScale; }
        [[nodiscard]] bool isDisplayScaleInjected() const { return m_injectedScale > 0.0f; }
        static constexpr float MIN_DISPLAY_SCALE = 0.25f;
        static constexpr float MAX_DISPLAY_SCALE = 8.0f;
        [[nodiscard]] WindowPosition getPosition() const { return m_position; }
        [[nodiscard]] bool isCloseRequested() const { return m_closeRequested; }
        [[nodiscard]] bool isMinimized() const { return m_state == WindowState::Minimized; }
        [[nodiscard]] bool isMaximized() const { return m_state == WindowState::Maximized; }
        [[nodiscard]] bool isFullscreen() const { return m_fullscreen; }
        [[nodiscard]] WindowState getState() const { return m_state; }
        [[nodiscard]] bool isVisible() const { return m_state != WindowState::Hidden; }
        [[nodiscard]] bool hasFocus() const { return m_hasFocus; }

        // -- ingestion (adapter -> core) --
        void onNativeWindowOpened(u32 nativeId, void* nativeHandle);
        void onResized(u32 width, u32 height);
        // The display's scale is untrusted: a non-finite or non-positive one is ignored (the last stays), any
        // other clamps to [MIN_DISPLAY_SCALE, MAX_DISPLAY_SCALE]. True when the extent or the effective scale moved.
        bool onFramebufferResized(u32 width, u32 height, float displayScale);
        // Stands in for the display's scale (a window moved to another monitor without one): 0 returns to the
        // display's; a non-finite or negative value is refused (false).
        bool injectDisplayScale(float scale);
        void onStateChanged(WindowState state);
        void onFullscreenChanged(bool fullscreen);
        void onFocusChanged(bool hasFocus);
        void onCloseRequested();
        void onMoved(i32 x, i32 y);

        // -- commands (core -> adapter) --
        void show();
        void raise();
        void hide();
        void requestClose();
        void setTitle(InternedString title);
        void setPosition(WindowPosition position);
        void setCursorShape(CursorShape shape);
        void setState(WindowState state);
        void setFullscreen(bool fullscreen);
        void setExtent(u32 width, u32 height) { pushCommand(WindowExtentCommand{.width = width, .height = height}); }
        void applyConfig(const WindowConfig& config);
        [[nodiscard]] const std::vector<WindowCommand>& pendingCommands() const { return m_commands; }
        [[nodiscard]] std::vector<WindowCommand> drainCommands();

    private:
        void pushCommand(const WindowCommand& command);

        WindowId m_id;
        WindowConfig m_config;
        WindowState m_state = WindowState::Normal;
        WindowExtent m_logicalExtent{};
        WindowExtent m_framebufferExtent{};
        float m_displayScale = 1.0f;
        float m_injectedScale = 0.0f; // 0: none
        WindowPosition m_position{};
        bool m_fullscreen = false;
        bool m_closeRequested = false;
        bool m_hasFocus = false;

        IVulkanWindowSurface* m_surface = nullptr;
        u32 m_nativeId = 0;
        void* m_nativeHandle = nullptr;
        std::vector<WindowCommand> m_commands;
    };
} // namespace spite
