#pragma once

#include <functional>

#include "application/ApplicationEvents.hpp"

union SDL_Event;

namespace spite
{
    class EventDispatcher
    {
    public:
        EventDispatcher(const EventDispatcher& other) = delete;
        EventDispatcher(EventDispatcher&& other) = delete;
        EventDispatcher& operator=(const EventDispatcher& other) = delete;
        EventDispatcher& operator=(EventDispatcher&& other) = delete;
        ~EventDispatcher();

        explicit EventDispatcher(ApplicationEventBus& eventBus);

        void pollEvents(u32 waitTimeoutMilliseconds = 0);
        void setWindowService(class WindowService* windowService);
        // Input SDL events feed the core's ingestion directly (never the bus).
        void setInputSystem(class InputSystem* inputSystem);
        // Injected input is ingested at the top of each poll, as if the device had reported it.
        void setInputInjector(class InputInjector* injector);
        // A drag on the window's border holds the main thread in the OS's sizing loop inside the poll, so no frame
        // runs until it ends and the OS stretches the last image. The OS asks for a redraw instead (a live-resize
        // expose, on the main thread): `frame` runs once per ask, after the window's sizes were refreshed, so the
        // swapchain follows the drag. Never re-entered; empty removes it.
        void setLiveResizeFrame(std::function<void()> frame);

    private:
        static bool liveResizeWatch(void* userdata, SDL_Event* event);
        void publishFramebuffer(const class Window& window);
        void refreshFramebuffer(WindowId windowId, class Window& window, bool always);
        ApplicationEventBus& m_eventBus;
        class WindowService* m_windowService = nullptr;
        class InputSystem* m_inputSystem = nullptr;
        class InputInjector* m_injector = nullptr;
        std::function<void()> m_liveResizeFrame;
        bool m_inLiveResizeFrame = false;
    };
} // namespace spite
