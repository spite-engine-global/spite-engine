#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "base/MathTypes.hpp"
#include "ui/UiExport.hpp"

namespace spite
{
    // Rich text markup of a label that opted in (UiLabelComponent::rich). Tags in square brackets:
    //   [color=#rgb] [color=#rrggbb] [color=#rrggbbaa] ... [/color]
    //   [size=18] (UI pixels) or [size=150%] (of the enclosing size) ... [/size]
    //   [b] ... [/b]  (weight 700)   [weight=600] ... [/weight]
    //   [sprite=atlas/name]  (an inline atlas sprite, no closing tag)
    // `[[` is a literal `[`; a lone `]` is literal. Tags nest and close innermost first. Anything else (an unknown or
    // unclosed tag, a bad value, a limit passed) makes the whole text malformed: it shows as written.
    namespace UiRichLimits
    {
        inline constexpr sizet MAX_MARKUP_BYTES = 4096;
        inline constexpr sizet MAX_RUNS = 64;
        inline constexpr sizet MAX_DEPTH = 8;
        inline constexpr sizet MAX_SPRITES = 16;
        inline constexpr sizet MAX_SPRITE_NAME = 128;
        inline constexpr u16 BOLD_WEIGHT = 700;
        inline constexpr float MAX_SIZE_PX = 512.0f; // [size] caps, also after percentages compound
        inline constexpr float MAX_SIZE_SCALE = 32.0f; // relative to the label's own size
    } // namespace UiRichLimits

    // The plain text byte [begin, end) with one style; a sprite run covers the placeholder U+FFFC (3 bytes) it
    // stands for in the plain text.
    struct UiRichRun
    {
        u32 begin = 0;
        u32 end = 0;
        u32 color = 0; // rgba8, red in the low byte; meaningful when hasColor
        bool hasColor = false;
        float sizePx = 0.0f; // > 0: an absolute size (UI pixels)
        float sizeScale = 1.0f; // of the label's size (or of sizePx)
        u16 weight = 0; // 0: the label's
        std::string sprite; // set: an inline sprite "atlas/name"
    };

    struct UiRichText
    {
        std::string plain; // the text without tags
        std::vector<UiRichRun> runs; // in order, covering plain exactly
        bool literal = false; // malformed: plain is the markup as written, one run
    };

    inline constexpr std::string_view UI_RICH_SPRITE_PLACEHOLDER = "\xEF\xBF\xBC";

    // Parses `markup` into `out`. On failure returns false with the reason in `error`, and `out` is the literal
    // fallback. Bounded by UiRichLimits; no recursion.
    SPITE_UI_API bool parseUiRichText(std::string_view markup, UiRichText& out, std::string& error);

    // Appends `text` with every `[` doubled, so it shows as written inside markup. Values substituted into a
    // localized message of a rich label go through it, so a value cannot inject tags.
    inline void escapeUiMarkup(std::string_view text, std::string& out)
    {
        for (const char c : text)
        {
            out.push_back(c);
            if (c == '[')
            {
                out.push_back('[');
            }
        }
    }
} // namespace spite
