#include "application/Window.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace spite
{
    Window::Window(const WindowId id, const WindowConfig& config, IVulkanWindowSurface& surface) :
        m_id(id), m_config(config), m_state(config.state), m_logicalExtent{config.width, config.height},
        m_framebufferExtent{config.width, config.height}, m_fullscreen(config.fullscreen), m_surface(&surface)
    {
    }

    void Window::onNativeWindowOpened(const u32 nativeId, void* nativeHandle)
    {
        m_nativeId = nativeId;
        m_nativeHandle = nativeHandle;
    }

    void Window::onResized(const u32 width, const u32 height)
    {
        m_logicalExtent = {width, height};
        // only the windowed size is the one to restore to
        if (m_state == WindowState::Normal && !m_fullscreen && width > 0 && height > 0)
        {
            m_config.width = width;
            m_config.height = height;
        }
    }

    bool Window::onFramebufferResized(const u32 width, const u32 height, const float displayScale)
    {
        const float before = getDisplayScale();
        const WindowExtent extentBefore = m_framebufferExtent;
        m_framebufferExtent = {width, height};
        if (std::isfinite(displayScale) && displayScale > 0.0f)
        {
            m_displayScale = std::clamp(displayScale, MIN_DISPLAY_SCALE, MAX_DISPLAY_SCALE);
        }
        return before != getDisplayScale() || extentBefore.width != width || extentBefore.height != height;
    }

    bool Window::injectDisplayScale(const float scale)
    {
        if (!std::isfinite(scale) || scale < 0.0f)
        {
            return false;
        }
        m_injectedScale = scale == 0.0f ? 0.0f : std::clamp(scale, MIN_DISPLAY_SCALE, MAX_DISPLAY_SCALE);
        return true;
    }

    void Window::onStateChanged(const WindowState state)
    {
        m_state = state;
        // entering fullscreen restores a maximized window; leaving maximizes it again
        if (!m_fullscreen && (state == WindowState::Normal || state == WindowState::Maximized))
        {
            m_config.state = state;
        }
    }

    void Window::onFullscreenChanged(const bool fullscreen)
    {
        m_fullscreen = fullscreen;
        m_config.fullscreen = fullscreen;
    }

    void Window::onFocusChanged(const bool hasFocus) { m_hasFocus = hasFocus; }

    void Window::onCloseRequested() { m_closeRequested = true; }

    void Window::onMoved(const i32 x, const i32 y) { m_position = {x, y}; }

    void Window::show()
    {
        if (m_state == WindowState::Hidden)
        {
            m_state = WindowState::Normal;
        }
        pushCommand(WindowShowCommand{});
    }

    void Window::raise() { pushCommand(WindowShowCommand{}); }

    void Window::hide()
    {
        m_state = WindowState::Hidden;
        pushCommand(WindowHideCommand{});
    }

    void Window::requestClose() { m_closeRequested = true; }

    void Window::setTitle(const InternedString title)
    {
        m_config.title = title;
        pushCommand(WindowTitleCommand{.title = std::string(title.toView())});
    }

    void Window::setPosition(const WindowPosition position)
    {
        m_position = position;
        pushCommand(WindowPositionCommand{.x = position.x, .y = position.y});
    }

    void Window::setCursorShape(const CursorShape shape) { pushCommand(WindowCursorCommand{.shape = shape}); }

    void Window::setState(const WindowState state) { pushCommand(WindowStateCommand{.state = state}); }

    void Window::setFullscreen(const bool fullscreen)
    {
        pushCommand(WindowFullscreenCommand{.fullscreen = fullscreen});
    }

    void Window::applyConfig(const WindowConfig& config)
    {
        const WindowConfig previous = std::exchange(m_config, config);
        if (config.title != previous.title)
        {
            pushCommand(WindowTitleCommand{.title = std::string(config.title.toView())});
        }
        if (config.width != previous.width || config.height != previous.height)
        {
            pushCommand(WindowExtentCommand{.width = config.width, .height = config.height});
        }
        if (config.resizable != previous.resizable || config.borderless != previous.borderless)
        {
            pushCommand(WindowFlagsCommand{.resizable = config.resizable, .borderless = config.borderless});
        }
        if (config.fullscreen != previous.fullscreen)
        {
            pushCommand(WindowFullscreenCommand{.fullscreen = config.fullscreen});
        }
        if (config.state != previous.state)
        {
            pushCommand(WindowStateCommand{.state = config.state});
        }
    }

    std::vector<WindowCommand> Window::drainCommands()
    {
        auto commands = std::exchange(m_commands, {});
        return commands;
    }

    void Window::pushCommand(const WindowCommand& command) { m_commands.push_back(command); }
} // namespace spite
