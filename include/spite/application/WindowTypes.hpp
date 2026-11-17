#pragma once

#include "base/InternedString.hpp"
#include "base/Platform.hpp"

namespace spite
{
    using WindowId = u64;
    constexpr WindowId INVALID_WINDOW_ID = 0;

    struct WindowExtent
    {
        u32 width = 0;
        u32 height = 0;
    };

    struct WindowPosition
    {
        i32 x = 0;
        i32 y = 0;
    };

    // A window opened beside the primary one: owned by `parent` (it stays above it and has no taskbar
    // entry), at desktop position `position` (the OS's window coordinates) with a client area `extent` big.
    struct SecondaryWindowDesc
    {
        InternedString title{};
        WindowPosition position{};
        WindowExtent extent{};
        WindowId parent = INVALID_WINDOW_ID;
        // Borderless and never focused or activated: a preview that must not take a drag's pointer or the keyboard.
        bool ghost = false;
    };

    enum class CursorShape : u8
    {
        Default,
        Hand,
        IBeam,
        ResizeHorizontal,
        ResizeVertical,
        ResizeDiagonalNorthWestSouthEast,
        ResizeDiagonalNorthEastSouthWest
    };
} // namespace spite
