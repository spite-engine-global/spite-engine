#include "application/WindowService.hpp"

#include "SdlWindowBackend.hpp"
#include "WindowBackend.hpp"

#include <SDL3/SDL.h>
#if defined(SPITE_PLATFORM_WINDOWS)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include <limits>
#include "application/Window.hpp"

namespace spite
{
    WindowService::WindowService() = default;

    WindowService::~WindowService() = default;

    bool WindowService::createPrimaryWindow(const WindowConfig& config)
    {
        if (m_primaryWindowId != INVALID_WINDOW_ID)
            return false;
        const WindowId id = m_nextWindowId++;
        auto backend = std::make_unique<SdlWindowBackend>(id);
        if (!backend->open(config))
            return false;

        auto window = std::make_unique<Window>(id, config, backend->surface());
        window->onNativeWindowOpened(backend->nativeId(), backend->nativeHandle());
        // a maximized, fullscreen or high-density window opens at another size than the config's
        const WindowExtent logical = backend->logicalExtent();
        window->onResized(logical.width, logical.height);
        const WindowExtent framebuffer = backend->framebufferExtent();
        window->onFramebufferResized(framebuffer.width, framebuffer.height, backend->displayScale());
        // SDL reports a move only after the first one: the window starts where the OS put it
        const WindowPosition position = backend->position();
        window->onMoved(position.x, position.y);

        m_windows.emplace(id, std::move(window));
        m_backends.emplace(id, std::move(backend));
        m_primaryWindowId = id;
        return true;
    }

    WindowId WindowService::createWindow(const SecondaryWindowDesc& desc)
    {
        const Window* parent = getWindow(desc.parent);
        const WindowId id = m_nextWindowId++;
        auto backend = std::make_unique<SdlWindowBackend>(id);
        if (!backend->openSecondary(desc, parent ? parent->nativeWindowHandle() : nullptr))
        {
            return INVALID_WINDOW_ID;
        }
        WindowConfig config;
        config.width = desc.extent.width;
        config.height = desc.extent.height;
        config.title = desc.title;
        auto window = std::make_unique<Window>(id, config, backend->surface());
        window->onNativeWindowOpened(backend->nativeId(), backend->nativeHandle());
        const WindowExtent logical = backend->logicalExtent();
        window->onResized(logical.width, logical.height);
        const WindowExtent framebuffer = backend->framebufferExtent();
        window->onFramebufferResized(framebuffer.width, framebuffer.height, backend->displayScale());
        const WindowPosition position = backend->position();
        window->onMoved(position.x, position.y);
        m_windows.emplace(id, std::move(window));
        m_backends.emplace(id, std::move(backend));
        if (desc.ghost)
            m_ghostWindows.insert(id);
        return id;
    }

    void WindowService::destroyWindow(const WindowId id)
    {
        if (id == m_primaryWindowId)
        {
            return;
        }
        m_ghostWindows.erase(id);
        m_windows.erase(id);
        m_backends.erase(id); // closes the OS window
    }

    WindowId WindowService::windowUnderPointer() const
    {
#if defined(SPITE_PLATFORM_WINDOWS)
        POINT point{};
        if (!GetCursorPos(&point))
            return std::numeric_limits<WindowId>::max();
        HWND hwnd = GetAncestor(WindowFromPoint(point), GA_ROOT);
        while (hwnd)
        {
            WindowId owner = std::numeric_limits<WindowId>::max();
            for (const auto& [id, window] : m_windows)
            {
                auto* native = static_cast<SDL_Window*>(window->nativeWindowHandle());
                if (SDL_GetPointerProperty(SDL_GetWindowProperties(native), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
                                           nullptr) == hwnd)
                {
                    owner = id;
                    break;
                }
            }
            if (!m_ghostWindows.contains(owner))
                return owner;
            do
            {
                hwnd = GetWindow(hwnd, GW_HWNDNEXT);
                RECT rect{};
                if (hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetWindowRect(hwnd, &rect) &&
                    PtInRect(&rect, point))
                    break;
            }
            while (hwnd);
        }
#else
        if (SDL_Window* window = SDL_GetMouseFocus())
        {
            const WindowId id = findWindowIdByNativeId(SDL_GetWindowID(window));
            if (id != INVALID_WINDOW_ID && !m_ghostWindows.contains(id))
                return id;
        }
#endif
        return std::numeric_limits<WindowId>::max();
    }

    bool WindowService::anyWindowFocused() const
    {
        for (const auto& [id, window] : m_windows)
        {
            if (window->hasFocus())
            {
                return true;
            }
        }
        return false;
    }

    Window* WindowService::getWindow(const WindowId id)
    {
        const auto it = m_windows.find(id);
        return it == m_windows.end() ? nullptr : it->second.get();
    }

    const Window* WindowService::getWindow(const WindowId id) const
    {
        const auto it = m_windows.find(id);
        return it == m_windows.end() ? nullptr : it->second.get();
    }

    Window* WindowService::getPrimaryWindow() { return getWindow(m_primaryWindowId); }
    const Window* WindowService::getPrimaryWindow() const { return getWindow(m_primaryWindowId); }
    WindowId WindowService::getPrimaryWindowId() const { return m_primaryWindowId; }

    Window* WindowService::findWindowByNativeId(const u32 nativeId)
    {
        if (nativeId == 0)
        {
            return nullptr;
        }
        for (const auto& [id, window] : m_windows)
        {
            if (window->getNativeId() == nativeId)
            {
                return window.get();
            }
        }
        return nullptr;
    }

    WindowId WindowService::findWindowIdByNativeId(const u32 nativeId) const
    {
        if (nativeId == 0)
        {
            return INVALID_WINDOW_ID;
        }
        for (const auto& [id, window] : m_windows)
        {
            if (window->getNativeId() == nativeId)
            {
                return id;
            }
        }
        return INVALID_WINDOW_ID;
    }

    IWindowBackend* WindowService::getBackend(const WindowId id)
    {
        const auto it = m_backends.find(id);
        return it == m_backends.end() ? nullptr : it->second.get();
    }

    void WindowService::applyPrimaryConfig(const WindowConfig& config)
    {
        if (Window* window = getPrimaryWindow())
        {
            window->applyConfig(config);
        }
    }

    void WindowService::drainCommands()
    {
        for (const auto& [id, window] : m_windows)
        {
            auto commands = window->drainCommands();
            if (commands.empty())
                continue;
            if (IWindowBackend* backend = getBackend(id))
            {
                backend->apply(commands);
            }
        }
    }
} // namespace spite
