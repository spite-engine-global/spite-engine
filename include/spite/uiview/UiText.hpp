#pragma once

#include <cstddef>
#include <span>

#include "base/Platform.hpp"

namespace spite
{
    // Index of a font registered with UiViewContext::registerFont.
    using FontId = u32;
    inline constexpr FontId NO_FONT = ~0u;

    enum class UiTextAlign : u8
    {
        Left,
        Center,
        Right,
    };

    // Multi-line text of a label. None: v1's single line, newlines skipped; Lines: a line per
    // explicit newline; Word: also lines break at spaces, after a hyphen between letters, between CJK ideographs (no
    // break before closing or after opening punctuation), and inside a word wider than the line by glyph.
    enum class UiTextWrap : u8
    {
        None,
        Lines,
        Word,
    };

    // What a line (None) or the last line of `maxLines` (Word) does with text it has no room for: Clip draws it
    // as laid out, Ellipsis cuts at the last glyph that leaves room for "…" and appends it, MiddleEllipsis keeps the
    // start and the end of the cut line and puts the "…" between them (names that differ in their suffix:;
    // a line with right-to-left text cuts at its end instead).
    enum class UiTextOverflow : u8
    {
        Clip,
        Ellipsis,
        MiddleEllipsis,
    };

    // The case a style draws its text in: the stored text, the accessible name and the queries keep their
    // case; measure, wrap, paint, caret and hit test lay out the converted text. Locale rules: ß uppercases to "SS";
    // Turkish and Azeri (tr, az) map i/İ and ı/I as dotted and dotless pairs.
    enum class UiTextTransform : u8
    {
        None,
        Uppercase,
        Lowercase,
    };

    // Where the block of lines sits in the element's rect; Center is the single-line placement of v1.
    enum class UiTextVAlign : u8
    {
        Top,
        Center,
        Bottom,
    };

    // A font's MSDF atlas for upload: RGBA8 rows top to bottom, distance
    // values to be sampled as linear data, never sRGB.
    struct UiFontAtlasView
    {
        u32 width = 0;
        u32 height = 0;
        std::span<const std::byte> rgba8;
    };
} // namespace spite
