#pragma once

#include <span>
#include <string_view>

#include "base/Platform.hpp"
#include "ui/UiExport.hpp"

namespace spite
{
    // Icons are glyphs of the Phosphor icon font (MIT), named, not numbered: "eye",
    // "eye-slash", "caret-down". The baked table (source/ui/UiIconTable.inc, generated from the release's mapping by
    // scripts/MakeIconTable.py) maps a name to the codepoint all five weights share; swapping the set later changes
    // that table and the fonts only.
    struct UiIconEntry
    {
        std::string_view name;
        u32 codepoint = 0;
    };

    // The font family an icon draws in: the name the host registers the Regular face under. Its weights are the
    // stylesheet's "fontWeights" of this family (100 Thin, 300 Light, 400 Regular, 700 Bold, 900 Fill).
    inline constexpr std::string_view UI_ICON_FONT = "icons";

    // The codepoint of an icon name; 0 when the table has no such name.
    [[nodiscard]] SPITE_UI_API u32 uiIconCodepoint(std::string_view name);
    // The table's name nearest to `name` by edit distance (ties: the first by name), for an error message.
    [[nodiscard]] SPITE_UI_API std::string_view uiIconClosestName(std::string_view name);
    // Every icon, sorted by name.
    [[nodiscard]] SPITE_UI_API std::span<const UiIconEntry> uiIcons();

    // The UTF-8 of an icon codepoint (three bytes: the table lives in the private use area); the bytes written.
    [[nodiscard]] constexpr u32 uiIconUtf8(const u32 codepoint, char (&out)[4])
    {
        out[0] = static_cast<char>(0xE0u | (codepoint >> 12));
        out[1] = static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[2] = static_cast<char>(0x80u | (codepoint & 0x3Fu));
        return 3;
    }
} // namespace spite
