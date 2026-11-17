#pragma once

#include <span>
#include <string_view>

#include "base/MathTypes.hpp"
#include "ui/UiExport.hpp"

namespace spite
{
    // Plain-data per-frame input record the router consumes. Filled by the
    // application-side adapter from InputSystem queries; the ui module never
    // includes application headers. Scancodes use the HID value domain — the
    // same u32 domain InputSystem::onKey ingests.
    // Modifier bits held at a key transition (UiKeyTransition::modifiers).
    namespace UiKeyModifier
    {
        inline constexpr u8 SHIFT = 1;
        inline constexpr u8 CTRL = 2;
        inline constexpr u8 ALT = 4;
        inline constexpr u8 SUPER = 8;
    } // namespace UiKeyModifier

    // A press, a release, or an OS key repeat of a held key (pressed and
    // repeat both set).
    struct UiKeyTransition
    {
        u32 scancode = 0;
        bool pressed = false;
        bool repeat = false;
        u8 modifiers = 0;
    };

    // What a menu is driven by besides the pointer: directions, accept, cancel, tab switching and landmark cycling,
    // resolved by the application from input actions (so remapping applies) and from any device. Navigation turns one
    // into focus movement, a widget's own key handling, a click or a close. The tab intents go to the focus chain
    // first (a tab bar) and cycle the landmarks when it leaves them.
    enum class UiNavIntent : u8
    {
        Up,
        Down,
        Left,
        Right,
        Accept,
        Cancel,
        NextTab,
        PreviousTab,
        NextLandmark,
        PreviousLandmark,
        Count
    };
    inline constexpr u32 UI_NAV_INTENT_COUNT = static_cast<u32>(UiNavIntent::Count);

    // An intent that went down this frame, or repeated while held (a direction only). `sourceKey` is the keyboard
    // scancode that holds the intent down, 0 when none does (a gamepad, a stick): the router saw that key as a key
    // transition already, so a key a widget handled is not navigation.
    struct UiNavTransition
    {
        UiNavIntent intent = UiNavIntent::Up;
        bool repeat = false;
        u32 sourceKey = 0;
    };

    struct UiPointerButton
    {
        bool down = false;
        bool pressedThisFrame = false;
        bool releasedThisFrame = false;
    };

    struct SPITE_UI_API UiInputSnapshot
    {
        Vec2 pointerPos{0.0f, 0.0f}; // active-surface space (== layout space in v1)
        u64 pointerWindow = 0; // the OS window the pointer is over; 0: the main window
        UiPointerButton buttons[5];
        Vec2 wheelDelta{0.0f, 0.0f};
        u8 modifiers = 0; // UiKeyModifier bits held this frame; presses and clicks carry them
        std::span<const UiKeyTransition> keyTransitions{};
        std::span<const UiNavTransition> navigation{}; // intents that went down or repeated this frame
        u32 navHeld = 0; // bit per UiNavIntent held now; a claimed intent no longer held is released
        std::string_view text{}; // accumulated this frame
        // The text an input method is composing, UTF-8; unlike `text` it persists across frames until the
        // method changes it, and is empty when nothing is composed. The selected clause (the part being converted)
        // is `compositionSelected` bytes from `compositionCursor`; without one the cursor is the end.
        std::string_view composition{};
        u32 compositionCursor = 0;
        u32 compositionSelected = 0;
    };
} // namespace spite
