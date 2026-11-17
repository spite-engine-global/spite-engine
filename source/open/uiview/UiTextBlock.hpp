#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "UiFont.hpp"
#include "uiview/UiFrame.hpp"
#include "uiview/UiText.hpp"

namespace spite
{
    inline constexpr u32 NO_SPAN = ~0u;

    struct UiTextBlockParams
    {
        float width = -1.0f; // px a line may take; <= 0 unbounded
        UiTextWrap wrap = UiTextWrap::None;
        UiTextOverflow overflow = UiTextOverflow::Clip;
        u32 maxLines = 0; // 0: no limit
        float letterSpacing = 0.0f; // px after each glyph's advance, in the line width, the wrap and the ellipsis
    };

    // A line's glyphs, a range of UiTextBlock::glyphs, and its visible width (trailing spaces excluded).
    struct UiTextLine
    {
        u32 first = 0;
        u32 count = 0;
        float width = 0.0f;
        float ascent = 0.0f; // rich blocks: the tallest item above the baseline and the deepest below it, px
        float descent = 0.0f;
    };

    struct UiTextBlock
    {
        std::vector<UiPlacedGlyph> glyphs; // penX counts from the start of the glyph's line
        // rich blocks: the span of each glyph (a sprite's glyph pointer is null), NO_SPAN for the ellipsis
        std::vector<u32> spanOf;
        std::vector<UiTextLine> lines; // at least one, empty text included
        float width = 0.0f; // the widest line
    };

    // Lays out UTF-8 text in lines: a line ends at an explicit newline (not with wrap None:
    // newlines are skipped) or, with Word wrapping and a `width` above 0, where the next glyph would pass it, at
    // the last break opportunity of the line (after a space
    // run, after a hyphen between letters, between ideographs, never before closing or after opening punctuation)
    // or, with none, by glyph. Spaces that end a line hang: they take no width. With maxLines the last line takes
    // the rest of the text; Ellipsis cuts a line wider than `width`, or the last one when text remains, at the
    // last glyph that leaves room for "…" (U+2026, else three periods) and appends it. Glyph faces and kerning as
    // layoutUiText. Runs of codepoints that need shaping (uiNeedsShaping, with the spaces between them and the
    // character a leading mark attaches to) are shaped per face and direction; lines break and cut only
    // between their clusters, and a right-to-left run shows reversed within each line (no bidi reordering across
    // runs: the paragraph is left to right).
    void layoutUiTextBlock(UiFonts fonts, FontId primary, std::string_view text, float sizePx,
                           const UiTextBlockParams& params, UiTextBlock& out);

    // layoutUiTextBlock for rich text: `plain` is covered by `spans`, each with its own face and size (a
    // sprite span is a box as wide as its aspect says and as high as the span's line; its glyph is null). Runs of
    // consecutive spans with one face and size lay out and shape together, so a color change never splits a
    // shaping run; kerning stops at a face or size change. Lines break, cut and align across spans as in
    // layoutUiTextBlock; each line records the tallest ascent and deepest descent of its items (the label's own face
    // and size, `primary` and `sizePx`, for an empty line). The ellipsis takes the label's face and size.
    void layoutUiRichTextBlock(UiFonts fonts, FontId primary, std::string_view plain, float sizePx,
                               std::span<const UiPaintSpan> spans, const UiTextBlockParams& params, UiTextBlock& out);
} // namespace spite
