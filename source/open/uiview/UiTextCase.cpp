#include "UiTextCase.hpp"

#include "UiFont.hpp"

namespace spite
{
    namespace
    {
        constexpr u32 DOTTED_CAPITAL_I = 0x130;
        constexpr u32 DOTLESS_SMALL_I = 0x131;

        constexpr bool between(const u32 c, const u32 first, const u32 last) { return c >= first && c <= last; }

        // Latin Extended-A and the Additional block pair letters by parity: a pair is (even upper, odd lower) in some
        // runs and (odd upper, even lower) in others.
        constexpr bool upperIsEven(const u32 c)
        {
            return between(c, 0x100, 0x137) || between(c, 0x14A, 0x177) || between(c, 0x1DE, 0x1EF) ||
                (between(c, 0x1E00, 0x1EFF) && !between(c, 0x1E96, 0x1E9F));
        }

        constexpr bool upperIsOdd(const u32 c) { return between(c, 0x139, 0x148) || between(c, 0x179, 0x17E); }

        // The simple lowercase letter of `c`, or `c`
        constexpr u32 lowerOf(const u32 c)
        {
            if (c < 0x80)
            {
                return between(c, 'A', 'Z') ? c + 0x20 : c;
            }
            if (c == DOTTED_CAPITAL_I)
            {
                return c;
            }
            if (between(c, 0xC0, 0xDE))
            {
                return c == 0xD7 ? c : c + 0x20;
            }
            if (c == 0x178)
            {
                return 0xFF;
            }
            if (c == 0x1E9E)
            {
                return 0xDF;
            }
            if (c == 0x243)
            {
                return 0x180;
            }
            if (c == 0x181)
            {
                return 0x253;
            }
            if (c == 0x191)
            {
                return 0x192;
            }
            if (between(c, 0x391, 0x3A9))
            {
                return c == 0x3A2 ? c : c + 0x20;
            }
            if (between(c, 0x410, 0x42F))
            {
                return c + 0x20;
            }
            if (between(c, 0x400, 0x40F))
            {
                return c + 0x50;
            }
            if (upperIsEven(c))
            {
                return (c & 1) == 0 ? c + 1 : c;
            }
            return upperIsOdd(c) && (c & 1) != 0 ? c + 1 : c;
        }

        // The simple uppercase letter of `c`, or `c`; ß is the caller's
        constexpr u32 upperOf(const u32 c)
        {
            if (c < 0x80)
            {
                return between(c, 'a', 'z') ? c - 0x20 : c;
            }
            if (between(c, 0xE0, 0xFE))
            {
                return c == 0xF7 ? c : c - 0x20;
            }
            if (c == 0xFF)
            {
                return 0x178;
            }
            if (c == DOTLESS_SMALL_I)
            {
                return 'I';
            }
            if (c == 0x17F)
            {
                return 'S';
            }
            if (c == 0x180)
            {
                return 0x243;
            }
            if (c == 0x253)
            {
                return 0x181;
            }
            if (c == 0x192)
            {
                return 0x191;
            }
            if (between(c, 0x3B1, 0x3C9))
            {
                return c == 0x3C2 ? 0x3A3 : c - 0x20;
            }
            if (between(c, 0x430, 0x44F))
            {
                return c - 0x20;
            }
            if (between(c, 0x450, 0x45F))
            {
                return c - 0x50;
            }
            if (upperIsEven(c))
            {
                return (c & 1) != 0 ? c - 1 : c;
            }
            return upperIsOdd(c) && (c & 1) == 0 ? c - 1 : c;
        }

        void appendUtf8(std::string& out, const u32 c)
        {
            if (c < 0x80)
            {
                out.push_back(static_cast<char>(c));
            }
            else if (c < 0x800)
            {
                out.push_back(static_cast<char>(0xC0 | (c >> 6)));
                out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
            else
            {
                out.push_back(static_cast<char>(0xE0 | (c >> 12)));
                out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
        }

        // Appends the converted codepoint; false (nothing appended) when it maps to itself
        bool convert(const u32 c, const bool upper, const bool turkic, std::string& out)
        {
            if (upper)
            {
                if (c == 0xDF)
                {
                    out += "SS";
                    return true;
                }
                const u32 mapped = turkic && c == 'i' ? DOTTED_CAPITAL_I : upperOf(c);
                if (mapped != c)
                {
                    appendUtf8(out, mapped);
                    return true;
                }
                return false;
            }
            if (c == DOTTED_CAPITAL_I)
            {
                out += turkic ? "i" : "i\xCC\x87";
                return true;
            }
            const u32 mapped = turkic && c == 'I' ? DOTLESS_SMALL_I : lowerOf(c);
            if (mapped != c)
            {
                appendUtf8(out, mapped);
                return true;
            }
            return false;
        }
    } // namespace

    UiCaseLocale uiCaseLocale(const std::string_view localeId)
    {
        const bool turkic = localeId.size() >= 2 && (localeId.substr(0, 2) == "tr" || localeId.substr(0, 2) == "az") &&
            (localeId.size() == 2 || localeId[2] == '-' || localeId[2] == '_');
        return turkic ? UiCaseLocale::Turkic : UiCaseLocale::Default;
    }

    std::string_view applyUiTextTransform(const UiTextTransform transform, const UiCaseLocale locale,
                                          const std::string_view text, std::string& scratch)
    {
        if (transform == UiTextTransform::None)
        {
            return text;
        }
        const bool upper = transform == UiTextTransform::Uppercase;
        const bool turkic = locale == UiCaseLocale::Turkic;
        std::string piece; // at most 3 bytes: small-string storage
        bool changed = false;
        sizet cursor = 0;
        while (cursor < text.size())
        {
            const sizet start = cursor;
            const u32 codepoint = nextCodepoint(text, cursor);
            piece.clear();
            const bool converted = convert(codepoint, upper, turkic, piece);
            if (!changed)
            {
                if (!converted)
                {
                    continue;
                }
                scratch.assign(text, 0, start); // the first change: the unchanged prefix, then the conversions
                changed = true;
            }
            if (converted)
            {
                scratch += piece;
            }
            else
            {
                scratch.append(text, start, cursor - start); // verbatim, malformed bytes too
            }
        }
        return changed ? std::string_view(scratch) : text;
    }
} // namespace spite
