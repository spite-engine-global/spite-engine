#pragma once
#include <cstring>

#include "base/InternedString.hpp"
#include "base/Platform.hpp"

#include "application/EventBus.hpp"
#include "application/WindowTypes.hpp"

#include "application/input/InputTypes.hpp"

namespace spite
{
    struct WindowResizeEvent
    {
        u32 width;
        u32 height;
        WindowId windowId = INVALID_WINDOW_ID;
    };

    struct WindowFramebufferResizeEvent
    {
        u32 width;
        u32 height;
        float displayScale = 1.0f;
        WindowId windowId = INVALID_WINDOW_ID;
    };

    struct WindowMinimizeEvent
    {
        WindowId windowId = INVALID_WINDOW_ID;
    };

    struct WindowRestoreEvent
    {
        WindowId windowId = INVALID_WINDOW_ID;
    };

    struct WindowCloseEvent
    {
        WindowId windowId = INVALID_WINDOW_ID;
    };

    // KeyPressEvent::modifiers bits: either side of the key.
    namespace KeyModifier
    {
        inline constexpr u16 SHIFT = 1;
        inline constexpr u16 CTRL = 2;
        inline constexpr u16 ALT = 4;
        inline constexpr u16 SUPER = 8;
    } // namespace KeyModifier

    struct KeyPressEvent
    {
        u32 scancode;
        bool isPressed;
        bool isRepeat = false;
        u16 modifiers = 0; // KeyModifier bits
        WindowId windowId = INVALID_WINDOW_ID;
    };

    struct MouseMoveEvent
    {
        float x, y;
        float xrel, yrel;
        WindowId windowId = INVALID_WINDOW_ID;
    };

    struct MouseButtonEvent
    {
        MouseButton button;
        bool isPressed;
        float x, y;
        WindowId windowId = INVALID_WINDOW_ID;
    };

    struct MouseWheelEvent
    {
        float deltaX;
        float deltaY;
        WindowId windowId = INVALID_WINDOW_ID;
    };

    struct WindowFocusEvent
    {
        bool hasFocus;
        WindowId windowId = INVALID_WINDOW_ID;
    };

    struct GamepadConnectedEvent
    {
        u32 gamepadIndex = 0;
        i32 instanceId = -1;
        InternedString name{};
    };

    struct GamepadDisconnectedEvent
    {
        u32 gamepadIndex = 0;
        i32 instanceId = -1;
        InternedString name{};
    };

    struct TextInputEvent
    {
        // For scratch allocation compatibility, use fixed buffer or string
        char text[64];

        TextInputEvent() { text[0] = '\0'; }

        explicit TextInputEvent(const char* str)
        {
            strncpy(text, str, sizeof(text) - 1);
            text[sizeof(text) - 1] = '\0';
        }

        WindowId windowId = INVALID_WINDOW_ID;
    };

    class Application;

    struct AppInitializeEvent
    {
        Application* app;
    };

    struct AppShutdownEvent
    {
        Application* app;
    };

    struct AppPauseEvent
    {
        Application* app;
    };

    struct AppResumeEvent
    {
        Application* app;
    };

    struct AppPreUpdateEvent
    {
        float deltaTime;
        float unscaledDeltaTime;
        Application* app;
    };
    struct AppUpdateEvent
    {
        float deltaTime;
        float unscaledDeltaTime;
        // the frame's position between the last two fixed steps, [0, 1]
        float interpolationAlpha;
        Application* app;
    };

    struct AppFixedUpdateEvent
    {
        float fixedDeltaTime;
        Application* app;
    };

    struct AppRenderEvent
    {
        Application* app;
    };

    struct AppUiRenderEvent
    {
        Application* app;
    };

    struct ConfigChangedEvent
    {
        InternedString sectionId{};
    };

    struct FileDropEvent
    {
        InternedString filePath{};
        WindowId windowId = INVALID_WINDOW_ID;
        float x = 0.0f;
        float y = 0.0f;
    };

    struct FileOpenEvent
    {
        InternedString filePath{};

        FileOpenEvent() {}

        explicit FileOpenEvent(const char* path) : filePath(path) {}
    };

    using ApplicationEvent =
        std::variant<WindowResizeEvent, WindowFramebufferResizeEvent, WindowMinimizeEvent, WindowRestoreEvent,
                     WindowCloseEvent, KeyPressEvent, MouseMoveEvent, MouseButtonEvent, MouseWheelEvent,
                     WindowFocusEvent, GamepadConnectedEvent, GamepadDisconnectedEvent, TextInputEvent,
                     AppInitializeEvent, AppShutdownEvent, AppPauseEvent, AppResumeEvent, AppPreUpdateEvent,
                     AppUpdateEvent, AppFixedUpdateEvent, AppRenderEvent, AppUiRenderEvent, FileDropEvent,
                     FileOpenEvent, ConfigChangedEvent>;
    using ApplicationEventBus = EventBus<ApplicationEvent>;
} // namespace spite
