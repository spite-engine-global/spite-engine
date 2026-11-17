#include "EventDispatcher.hpp"
#include "base/Logging.hpp"

#include <SDL3/SDL_events.h>

#include "WindowBackend.hpp"
#include "application/ApplicationEvents.hpp"
#include "application/Window.hpp"
#include "application/WindowService.hpp"
#include "application/input/InputInjection.hpp"
#include "application/input/InputSystem.hpp"

namespace spite
{
    EventDispatcher::EventDispatcher(ApplicationEventBus& eventBus) : m_eventBus(eventBus)
    {
        SDEBUG_LOG("EventDispatcher initialized");
    }

    EventDispatcher::~EventDispatcher()
    {
        if (m_liveResizeFrame)
        {
            SDL_RemoveEventWatch(liveResizeWatch, this);
        }
    }

    void EventDispatcher::setWindowService(WindowService* windowService) { m_windowService = windowService; }

    void EventDispatcher::setLiveResizeFrame(std::function<void()> frame)
    {
        if (!m_liveResizeFrame && frame)
        {
            SDL_AddEventWatch(liveResizeWatch, this);
        }
        else if (m_liveResizeFrame && !frame)
        {
            SDL_RemoveEventWatch(liveResizeWatch, this);
        }
        m_liveResizeFrame = std::move(frame);
    }

    bool EventDispatcher::liveResizeWatch(void* userdata, SDL_Event* event)
    {
        auto& self = *static_cast<EventDispatcher*>(userdata);
        if (event->type != SDL_EVENT_WINDOW_EXPOSED || event->window.data1 != 1 || self.m_inLiveResizeFrame ||
            !self.m_windowService)
        {
            return true;
        }
        Window* window = self.m_windowService->findWindowByNativeId(event->window.windowID);
        const WindowId windowId = self.m_windowService->findWindowIdByNativeId(event->window.windowID);
        IWindowBackend* backend = window ? self.m_windowService->getBackend(windowId) : nullptr;
        if (!backend)
        {
            return true;
        }
        // the queued resize events reach the poll only after the drag: the sizes are read from the window now
        if (const WindowExtent logical = backend->logicalExtent();
            logical.width != window->getLogicalExtent().width || logical.height != window->getLogicalExtent().height)
        {
            window->onResized(logical.width, logical.height);
            self.m_eventBus.enqueue(
                WindowResizeEvent{.width = logical.width, .height = logical.height, .windowId = windowId});
        }
        self.refreshFramebuffer(windowId, *window, false);
        self.m_inLiveResizeFrame = true;
        self.m_liveResizeFrame();
        self.m_inLiveResizeFrame = false;
        return true;
    }

    void EventDispatcher::setInputSystem(InputSystem* inputSystem) { m_inputSystem = inputSystem; }

    void EventDispatcher::setInputInjector(InputInjector* injector) { m_injector = injector; }

    void EventDispatcher::publishFramebuffer(const Window& window)
    {
        const WindowExtent framebuffer = window.getFramebufferExtent();
        m_eventBus.enqueue(WindowFramebufferResizeEvent{.width = framebuffer.width,
                                                        .height = framebuffer.height,
                                                        .displayScale = window.getDisplayScale(),
                                                        .windowId = window.getId()});
    }

    void EventDispatcher::refreshFramebuffer(const WindowId windowId, Window& window, const bool always)
    {
        IWindowBackend* backend = m_windowService->getBackend(windowId);
        if (!backend)
        {
            return;
        }
        const WindowExtent framebuffer = backend->framebufferExtent();
        const bool changed =
            window.onFramebufferResized(framebuffer.width, framebuffer.height, backend->displayScale());
        if (changed || always)
        {
            publishFramebuffer(window);
        }
    }

    void EventDispatcher::pollEvents(const u32 waitTimeoutMilliseconds)
    {
        // injected input enters here, ahead of the device's, through the same ingestion
        if (m_injector && m_inputSystem)
        {
            m_injector->deliver(*m_inputSystem, m_eventBus,
                                m_windowService ? m_windowService->getPrimaryWindowId() : INVALID_WINDOW_ID);
        }
        SDL_Event event;
        int eventCount = 0;
        bool focusLost = false;
        bool hasEvent = waitTimeoutMilliseconds > 0
            ? SDL_WaitEventTimeout(&event, static_cast<Sint32>(waitTimeoutMilliseconds))
            : SDL_PollEvent(&event);
        while (hasEvent)
        {
            eventCount++;

            const auto resolveWindowId = [this](const unsigned int nativeWindowId)
            { return m_windowService ? m_windowService->findWindowIdByNativeId(nativeWindowId) : INVALID_WINDOW_ID; };
            const auto resolveWindow = [this](const unsigned int nativeWindowId) -> Window*
            { return m_windowService ? m_windowService->findWindowByNativeId(nativeWindowId) : nullptr; };

            switch (event.type)
            {
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                {
                    MouseButton btn;
                    switch (event.button.button)
                    {
                    case SDL_BUTTON_RIGHT:
                        btn = MouseButton::Right;
                        break;
                    case SDL_BUTTON_MIDDLE:
                        btn = MouseButton::Middle;
                        break;
                    case SDL_BUTTON_X1:
                        btn = MouseButton::X1;
                        break;
                    case SDL_BUTTON_X2:
                        btn = MouseButton::X2;
                        break;
                    case SDL_BUTTON_LEFT:
                    default:
                        btn = MouseButton::Left;
                        break;
                    }

                    ingestInput(MouseButtonObservation{btn, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN}, m_inputSystem,
                                m_eventBus, resolveWindowId(event.button.windowID));
                    if (m_inputSystem && m_windowService)
                        m_inputSystem->onPointerHoverWindow(m_windowService->windowUnderPointer());
                    break;
                }

            case SDL_EVENT_DROP_FILE:
                {
                    if (event.drop.data)
                    {
                        m_eventBus.enqueue(FileDropEvent{.filePath = InternedString(event.drop.data),
                                                         .windowId = resolveWindowId(event.drop.windowID),
                                                         .x = event.drop.x,
                                                         .y = event.drop.y});
                    }
                    break;
                }

            case SDL_EVENT_MOUSE_WHEEL:
                {
                    ingestInput(WheelObservation{event.wheel.x, event.wheel.y}, m_inputSystem, m_eventBus,
                                resolveWindowId(event.wheel.windowID));
                    break;
                }

            case SDL_EVENT_MOUSE_MOTION:
                {
                    ingestInput(
                        MouseMoveObservation{event.motion.x, event.motion.y, event.motion.xrel, event.motion.yrel},
                        m_inputSystem, m_eventBus, resolveWindowId(event.motion.windowID));
                    if (m_inputSystem && m_windowService)
                        m_inputSystem->onPointerHoverWindow(m_windowService->windowUnderPointer());
                    break;
                }

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                {
                    const SDL_Keymod mod = event.key.mod;
                    const auto modifiers = static_cast<u16>(((mod & SDL_KMOD_SHIFT) ? KeyModifier::SHIFT : 0) |
                                                            ((mod & SDL_KMOD_CTRL) ? KeyModifier::CTRL : 0) |
                                                            ((mod & SDL_KMOD_ALT) ? KeyModifier::ALT : 0) |
                                                            ((mod & SDL_KMOD_GUI) ? KeyModifier::SUPER : 0));
                    ingestInput(KeyObservation{.scancode = static_cast<u32>(event.key.scancode),
                                               .pressed = event.type == SDL_EVENT_KEY_DOWN,
                                               .repeat = event.key.repeat,
                                               .modifiers = modifiers},
                                m_inputSystem, m_eventBus, resolveWindowId(event.key.windowID));
                    break;
                }

            case SDL_EVENT_QUIT:
                {
                    m_eventBus.enqueue(
                        WindowCloseEvent{m_windowService ? m_windowService->getPrimaryWindowId() : INVALID_WINDOW_ID});
                    SDEBUG_LOG("Window close event received");
                    break;
                }
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                {
                    if (Window* window = resolveWindow(event.window.windowID))
                    {
                        window->onCloseRequested();
                    }
                    m_eventBus.enqueue(WindowCloseEvent{resolveWindowId(event.window.windowID)});
                    SDEBUG_LOG("Window close event received");
                    break;
                }

            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                {
                    if (Window* window = resolveWindow(event.window.windowID))
                    {
                        window->onFocusChanged(true);
                    }
                    if (m_inputSystem)
                    {
                        m_inputSystem->onFocusChanged(true);
                    }
                    m_eventBus.enqueue(WindowFocusEvent{true, resolveWindowId(event.window.windowID)});
                    SDEBUG_LOG("Window focus gained");
                    break;
                }

            case SDL_EVENT_WINDOW_FOCUS_LOST:
                {
                    if (Window* window = resolveWindow(event.window.windowID))
                    {
                        window->onFocusChanged(false);
                    }
                    // a switch between the application's own windows reports the loss before the gain: the held
                    // input is dropped only once the poll ends with no window of ours focused
                    focusLost = true;
                    m_eventBus.enqueue(WindowFocusEvent{false, resolveWindowId(event.window.windowID)});
                    SDEBUG_LOG("Window focus lost");
                    break;
                }

            case SDL_EVENT_WINDOW_RESTORED:
                {
                    if (Window* window = resolveWindow(event.window.windowID))
                    {
                        window->onStateChanged(WindowState::Normal);
                    }
                    m_eventBus.enqueue(WindowRestoreEvent{resolveWindowId(event.window.windowID)});
                    SDEBUG_LOG("Window restored");
                    break;
                }

            case SDL_EVENT_WINDOW_MAXIMIZED:
                {
                    if (Window* window = resolveWindow(event.window.windowID))
                    {
                        window->onStateChanged(WindowState::Maximized);
                    }
                    SDEBUG_LOG("Window maximized");
                    break;
                }

            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
                {
                    const bool fullscreen = event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
                    if (Window* window = resolveWindow(event.window.windowID))
                    {
                        window->onFullscreenChanged(fullscreen);
                    }
                    SDEBUG_LOG("Window fullscreen: {}", fullscreen);
                    break;
                }

            case SDL_EVENT_WINDOW_MINIMIZED:
                {
                    if (Window* window = resolveWindow(event.window.windowID))
                    {
                        window->onStateChanged(WindowState::Minimized);
                    }
                    m_eventBus.enqueue(WindowMinimizeEvent{resolveWindowId(event.window.windowID)});
                    SDEBUG_LOG("Window minimized");
                    break;
                }

            case SDL_EVENT_WINDOW_MOVED:
                {
                    if (Window* window = resolveWindow(event.window.windowID))
                    {
                        window->onMoved(event.window.data1, event.window.data2);
                    }
                    break;
                }

            case SDL_EVENT_WINDOW_RESIZED:
                {
                    const WindowId windowId = resolveWindowId(event.window.windowID);
                    if (Window* window = resolveWindow(event.window.windowID))
                    {
                        window->onResized(static_cast<u32>(event.window.data1), static_cast<u32>(event.window.data2));
                        refreshFramebuffer(windowId, *window, true);
                        m_eventBus.enqueue(WindowResizeEvent{.width = static_cast<u32>(event.window.data1),
                                                             .height = static_cast<u32>(event.window.data2),
                                                             .windowId = windowId});
                    }
                    SDEBUG_LOG("Window resized to: {}x{}", event.window.data1, event.window.data2);
                    break;
                }

            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
                {
                    // a window moved to a monitor of another density keeps its logical size: only these report it
                    const WindowId windowId = resolveWindowId(event.window.windowID);
                    if (Window* window = resolveWindow(event.window.windowID))
                    {
                        refreshFramebuffer(windowId, *window, false);
                    }
                    break;
                }

            case SDL_EVENT_TEXT_INPUT:
                {
                    ingestInput(TextObservation{event.text.text}, m_inputSystem, m_eventBus,
                                resolveWindowId(event.text.windowID));
                    break;
                }

            case SDL_EVENT_TEXT_EDITING:
                {
                    // the application draws the composition (PlatformServices sets the IME UI hint); the candidate
                    // list stays the platform's, so SDL_EVENT_TEXT_EDITING_CANDIDATES is not needed
                    ingestInput(TextObservation{.text = event.edit.text ? event.edit.text : "",
                                                .composition = true,
                                                .start = event.edit.start,
                                                .length = event.edit.length},
                                m_inputSystem, m_eventBus, resolveWindowId(event.edit.windowID));
                    break;
                }

            default:
                break;
            }

            hasEvent = SDL_PollEvent(&event);
        }
        if (focusLost && m_inputSystem && !(m_windowService && m_windowService->anyWindowFocused()))
        {
            m_inputSystem->onFocusChanged(false);
        }
    }
} // namespace spite
