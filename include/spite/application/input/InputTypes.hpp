#pragma once

#include <string>

#include "base/InternedString.hpp"
#include "base/Platform.hpp"
namespace spite
{
    enum class KeyCode
    {
        UNKNOWN = 0,
        A = 4,
        B,
        C,
        D,
        E,
        F,
        G,
        H,
        I,
        J,
        K,
        L,
        M,
        N,
        O,
        P,
        Q,
        R,
        S,
        T,
        U,
        V,
        W,
        X,
        Y,
        Z,
        NUM_1 = 30,
        NUM_2,
        NUM_3,
        NUM_4,
        NUM_5,
        NUM_6,
        NUM_7,
        NUM_8,
        NUM_9,
        NUM_0,
        RETURN,
        ESCAPE,
        BACKSPACE,
        TAB,
        SPACE,
        MINUS,
        EQUALS,
        LEFT_BRACKET,
        RIGHT_BRACKET,
        BACKSLASH,
        NONUSHASH,
        SEMICOLON,
        APOSTROPHE,
        GRAVE,
        COMMA,
        PERIOD,
        SLASH,
        CAPSLOCK,
        F1,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12,
        PRINTSCREEN,
        SCROLLLOCK,
        PAUSE,
        INSERT,
        HOME,
        PAGEUP,
        DEL,
        END,
        PAGEDOWN,
        RIGHT,
        LEFT,
        DOWN,
        UP,
        NUMLOCKCLEAR,
        KP_DIVIDE,
        KP_MULTIPLY,
        KP_MINUS,
        KP_PLUS,
        KP_ENTER,
        KP_1,
        KP_2,
        KP_3,
        KP_4,
        KP_5,
        KP_6,
        KP_7,
        KP_8,
        KP_9,
        KP_0,
        KP_PERIOD,
        KP_EQUALS = 103, // SDL scancodes: 100-102 are keys KeyCode does not name
        F13,
        F14,
        F15,
        F16,
        F17,
        F18,
        F19,
        F20,
        F21,
        F22,
        F23,
        F24,
        LCTRL = 224,
        LSHIFT,
        LALT,
        LGUI,
        RCTRL,
        RSHIFT,
        RALT,
        RGUI,
        MODE = 257, // SDL3 scancodes from here on
        SLEEP,
        WAKE,
        CHANNEL_INCREMENT,
        CHANNEL_DECREMENT,
        MEDIA_PLAY,
        MEDIA_PAUSE,
        MEDIA_RECORD,
        MEDIA_FAST_FORWARD,
        MEDIA_REWIND,
        MEDIA_NEXT_TRACK,
        MEDIA_PREVIOUS_TRACK,
        MEDIA_STOP,
        MEDIA_EJECT,
        MEDIA_PLAY_PAUSE,
        MEDIA_SELECT,
        AC_NEW,
        AC_OPEN,
        AC_CLOSE,
        AC_EXIT,
        AC_SAVE,
        AC_PRINT,
        AC_PROPERTIES,
        AC_SEARCH,
        AC_HOME,
        AC_BACK,
        AC_FORWARD,
        AC_STOP,
        AC_REFRESH,
        AC_BOOKMARKS,
        SOFTLEFT,
        SOFTRIGHT,
        CALL,
        ENDCALL
    };
    // the last SDL3 scancode KeyCode names: appending a key means a row in KeyCodeTest's table
    static_assert(static_cast<int>(KeyCode::ENDCALL) == 290);

    enum class MouseButton
    {
        Left = 0,
        Right,
        Middle,
        X1,
        X2
    };

    enum class InputDeviceType : u8
    {
        Keyboard,
        Mouse,
        Gamepad,
        Touch
    };

    enum class InputActionType : u8
    {
        None,
        Press,
        Release,
        Hold,
        Axis
    };

    struct InputState
    {
        bool isPressed = false;
        bool wasPressed = false;
        float holdTime = 0.0f;
        float value = 0.0f;
        bool isChanged = false;
    };

    // The text an input method is composing (not yet committed), as the platform reports it: `start` and `length`
    // select the clause being converted, in codepoints of `text`; -1 when the platform sets none.
    struct TextComposition
    {
        std::string text;
        i32 start = -1;
        i32 length = -1;
    };

    // Where text is being entered, in window units: the input method places its candidate window beside it.
    // `cursor` is the x offset of the insertion point from x.
    struct TextInputArea
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float cursor = 0.0f;
        bool operator==(const TextInputArea&) const = default;
    };

    struct MouseState
    {
        float x = 0.0f;
        float y = 0.0f;
        float deltaX = 0.0f;
        float deltaY = 0.0f;
        InputState buttons[5];
        float scrollDeltaX = 0.0f;
        float scrollDeltaY = 0.0f;
        u64 window = 0; // the WindowId x and y are relative to: the window that last reported the pointer
        u64 hoverWindow = 0; // the native window physically under the pointer
    };

    enum class InputControlDevice : u8
    {
        Keyboard,
        MouseButton,
        GamepadButton,
        GamepadAxis
    };

    struct InputControl
    {
        InputControlDevice device = InputControlDevice::Keyboard;
        u16 code = 0;
        i32 deviceId = -1;

        friend bool operator==(const InputControl&, const InputControl&) = default;
    };

    using InputContextId = u32;
    constexpr InputContextId DEFAULT_INPUT_CONTEXT_ID = 1;
    constexpr InputContextId INVALID_INPUT_CONTEXT_ID = 0;

    struct InputContextDesc
    {
        InternedString name{};
        i32 priority = 0;
        bool enabled = true;
        bool consumesControls = false;
    };

    struct InputBindingDesc
    {
        InternedString action{};
        InputControl control{};
        InputActionType trigger = InputActionType::None;
        float scale = 1.0f;
        // KeyModifier bits (ApplicationEvents.hpp); nonzero: the binding
        // resolves only while exactly these modifiers are held (a chord)
        u16 modifiers = 0;
    };
} // namespace spite
