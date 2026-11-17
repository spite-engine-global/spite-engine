#include "SdlWindowBackend.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <type_traits>
#include <variant>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.hpp>

#include "application/Window.hpp"
#include "base/ContentRoot.hpp"
#include "base/Logging.hpp"

namespace spite
{
    namespace
    {
        bool validateWindowConfig(const WindowConfig& config) { return config.width > 0 && config.height > 0; }

        void setWindowIcon(SDL_Window* window)
        {
            const auto path = resolveContentPath("assets/branding/SpiteShowcases.png").u8string();
            SDL_Surface* icon = SDL_LoadPNG(reinterpret_cast<const char*>(path.c_str()));
            if (!icon)
            {
                if (const char* executableDirectory = SDL_GetBasePath())
                {
                    const auto sdkPath =
                        (std::filesystem::path(executableDirectory) / "../assets/branding/SpiteShowcases.png")
                            .lexically_normal()
                            .u8string();
                    icon = SDL_LoadPNG(reinterpret_cast<const char*>(sdkPath.c_str()));
                }
            }
            if (icon)
            {
                SDL_SetWindowIcon(window, icon);
                SDL_DestroySurface(icon);
            }
        }
    } // namespace

    SdlWindowBackend::SdlWindowBackend(const WindowId id) : m_id(id) {}

    SdlWindowBackend::~SdlWindowBackend()
    {
        if (m_defaultCursor)
            SDL_DestroyCursor(m_defaultCursor);
        if (m_handCursor)
            SDL_DestroyCursor(m_handCursor);
        if (m_iBeamCursor)
            SDL_DestroyCursor(m_iBeamCursor);
        if (m_resizeHorizontalCursor)
            SDL_DestroyCursor(m_resizeHorizontalCursor);
        if (m_resizeVerticalCursor)
            SDL_DestroyCursor(m_resizeVerticalCursor);
        if (m_resizeDiagonalNorthWestSouthEastCursor)
            SDL_DestroyCursor(m_resizeDiagonalNorthWestSouthEastCursor);
        if (m_resizeDiagonalNorthEastSouthWestCursor)
            SDL_DestroyCursor(m_resizeDiagonalNorthEastSouthWestCursor);
        if (m_window)
            SDL_DestroyWindow(m_window);
    }

    bool SdlWindowBackend::open(const WindowConfig& config)
    {
        if (!validateWindowConfig(config))
        {
            SERROR_LOG("Invalid window config: {}x{}", config.width, config.height);
            return false;
        }

        SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
        if (config.highDpi)
            flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (config.resizable)
            flags |= SDL_WINDOW_RESIZABLE;
        if (config.fullscreen)
            flags |= SDL_WINDOW_FULLSCREEN;
        if (config.borderless)
            flags |= SDL_WINDOW_BORDERLESS;
        if (config.state == WindowState::Maximized)
            flags |= SDL_WINDOW_MAXIMIZED;
        if (config.state == WindowState::Minimized)
            flags |= SDL_WINDOW_MINIMIZED;
        if (config.state == WindowState::Hidden)
            flags |= SDL_WINDOW_HIDDEN;

        m_window = SDL_CreateWindow(config.title.toView().data(), static_cast<int>(config.width),
                                    static_cast<int>(config.height), flags);
        if (!m_window)
        {
            SERROR_LOG("Failed to create window: {}", SDL_GetError());
            return false;
        }
        setWindowIcon(m_window);
        m_sdlWindowId = SDL_GetWindowID(m_window);
        return true;
    }

    bool SdlWindowBackend::openSecondary(const SecondaryWindowDesc& desc, void* parent)
    {
        if (desc.extent.width == 0 || desc.extent.height == 0)
        {
            SERROR_LOG("Invalid secondary window extent: {}x{}", desc.extent.width, desc.extent.height);
            return false;
        }
        const SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY |
            (desc.ghost ? SDL_WINDOW_UTILITY | SDL_WINDOW_BORDERLESS | SDL_WINDOW_NOT_FOCUSABLE : SDL_WINDOW_RESIZABLE);
        // a ghost shows without taking the activation from the window that holds the drag
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, desc.ghost ? "0" : "1");
        m_window = SDL_CreateWindow(desc.title.toView().data(), static_cast<int>(desc.extent.width),
                                    static_cast<int>(desc.extent.height), flags);
        SDL_ResetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN);
        if (!m_window)
        {
            SERROR_LOG("Failed to create a secondary window: {}", SDL_GetError());
            return false;
        }
        if (parent && !SDL_SetWindowParent(m_window, static_cast<SDL_Window*>(parent)))
        {
            SWARNING_LOG("A secondary window has no parent: {}", SDL_GetError());
        }
        SDL_SetWindowPosition(m_window, desc.position.x, desc.position.y);
        setWindowIcon(m_window);
        m_sdlWindowId = SDL_GetWindowID(m_window);
        return true;
    }

    void SdlWindowBackend::apply(const std::vector<WindowCommand>& commands)
    {
        if (!m_window)
        {
            return;
        }
        for (const WindowCommand& command : commands)
        {
            std::visit(
                [this](const auto& typed)
                {
                    using T = std::decay_t<decltype(typed)>;
                    if constexpr (std::is_same_v<T, WindowShowCommand>)
                    {
                        SDL_ShowWindow(m_window);
                        SDL_RaiseWindow(m_window);
                    }
                    else if constexpr (std::is_same_v<T, WindowHideCommand>)
                    {
                        SDL_HideWindow(m_window);
                    }
                    else if constexpr (std::is_same_v<T, WindowTitleCommand>)
                    {
                        SDL_SetWindowTitle(m_window, typed.title.c_str());
                    }
                    else if constexpr (std::is_same_v<T, WindowPositionCommand>)
                    {
                        SDL_SetWindowPosition(m_window, typed.x, typed.y);
                    }
                    else if constexpr (std::is_same_v<T, WindowExtentCommand>)
                    {
                        SDL_SetWindowSize(m_window, static_cast<int>(typed.width), static_cast<int>(typed.height));
                    }
                    else if constexpr (std::is_same_v<T, WindowFlagsCommand>)
                    {
                        SDL_SetWindowResizable(m_window, typed.resizable);
                        SDL_SetWindowBordered(m_window, !typed.borderless);
                    }
                    else if constexpr (std::is_same_v<T, WindowFullscreenCommand>)
                    {
                        // no fullscreen mode set: borderless at the desktop resolution
                        SDL_SetWindowFullscreen(m_window, typed.fullscreen);
                    }
                    else if constexpr (std::is_same_v<T, WindowStateCommand>)
                    {
                        switch (typed.state)
                        {
                        case WindowState::Hidden:
                            SDL_HideWindow(m_window);
                            break;
                        case WindowState::Minimized:
                            SDL_MinimizeWindow(m_window);
                            break;
                        case WindowState::Maximized:
                            SDL_MaximizeWindow(m_window);
                            break;
                        case WindowState::Normal:
                        default:
                            SDL_RestoreWindow(m_window);
                            SDL_ShowWindow(m_window);
                            break;
                        }
                    }
                    else if constexpr (std::is_same_v<T, WindowCursorCommand>)
                    {
                        m_currentCursorShape = typed.shape;
                        if (const SDL_Cursor* cursor = cursorForShape(typed.shape))
                        {
                            SDL_SetCursor(const_cast<SDL_Cursor*>(cursor));
                        }
                    }
                },
                command);
        }
    }

    WindowExtent SdlWindowBackend::logicalExtent() const
    {
        int width = 0;
        int height = 0;
        if (m_window)
        {
            SDL_GetWindowSize(m_window, &width, &height);
        }
        return {static_cast<u32>(width), static_cast<u32>(height)};
    }

    WindowExtent SdlWindowBackend::framebufferExtent() const
    {
        WindowExtent extent{};
        if (m_window)
        {
            int width = 0;
            int height = 0;
            SDL_GetWindowSizeInPixels(m_window, &width, &height);
            extent.width = static_cast<u32>(width);
            extent.height = static_cast<u32>(height);
        }
        return extent;
    }

    float SdlWindowBackend::displayScale() const { return m_window ? SDL_GetWindowDisplayScale(m_window) : 1.0f; }

    WindowPosition SdlWindowBackend::position() const
    {
        int x = 0;
        int y = 0;
        if (m_window)
        {
            SDL_GetWindowPosition(m_window, &x, &y);
        }
        return {x, y};
    }

    const char* const* SdlWindowBackend::getRequiredInstanceExtensions(u32& extensionCount) const
    {
        return SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    }

    vk::SurfaceKHR SdlWindowBackend::createSurface(const vk::Instance instance) const
    {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        SASSERTM(m_window && SDL_Vulkan_CreateSurface(m_window, instance, nullptr, &surface),
                 "Failed to create Vulkan surface: {}", SDL_GetError());
        return surface;
    }

    SDL_Cursor* SdlWindowBackend::cursorForShape(const CursorShape shape) const
    {
        SDL_Cursor** cursorPtr = nullptr;
        SDL_SystemCursor cursorId = SDL_SYSTEM_CURSOR_DEFAULT;
        switch (shape)
        {
        case CursorShape::Hand:
            cursorPtr = const_cast<SDL_Cursor**>(&m_handCursor);
            cursorId = SDL_SYSTEM_CURSOR_POINTER;
            break;
        case CursorShape::IBeam:
            cursorPtr = const_cast<SDL_Cursor**>(&m_iBeamCursor);
            cursorId = SDL_SYSTEM_CURSOR_TEXT;
            break;
        case CursorShape::ResizeHorizontal:
            cursorPtr = const_cast<SDL_Cursor**>(&m_resizeHorizontalCursor);
            cursorId = SDL_SYSTEM_CURSOR_EW_RESIZE;
            break;
        case CursorShape::ResizeVertical:
            cursorPtr = const_cast<SDL_Cursor**>(&m_resizeVerticalCursor);
            cursorId = SDL_SYSTEM_CURSOR_NS_RESIZE;
            break;
        case CursorShape::ResizeDiagonalNorthWestSouthEast:
            cursorPtr = const_cast<SDL_Cursor**>(&m_resizeDiagonalNorthWestSouthEastCursor);
            cursorId = SDL_SYSTEM_CURSOR_NWSE_RESIZE;
            break;
        case CursorShape::ResizeDiagonalNorthEastSouthWest:
            cursorPtr = const_cast<SDL_Cursor**>(&m_resizeDiagonalNorthEastSouthWestCursor);
            cursorId = SDL_SYSTEM_CURSOR_NESW_RESIZE;
            break;
        case CursorShape::Default:
        default:
            cursorPtr = const_cast<SDL_Cursor**>(&m_defaultCursor);
            cursorId = SDL_SYSTEM_CURSOR_DEFAULT;
            break;
        }

        if (!*cursorPtr)
        {
            *cursorPtr = SDL_CreateSystemCursor(cursorId);
        }
        return *cursorPtr;
    }
} // namespace spite
