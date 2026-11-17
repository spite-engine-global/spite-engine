#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include "localization/Localization.hpp"

namespace spite
{
    namespace
    {
        constexpr std::array<char32_t, 26> LOWER = {
            U'\u00e1', U'\u0180', U'\u0107', U'\u010f', U'\u00e9', U'\u0192', U'\u011d', U'\u0125', U'\u00ed',
            U'\u0135', U'\u0137', U'\u013a', U'\u1e3f', U'\u0144', U'\u00f6', U'\u00fe', U'\u01eb', U'\u0155',
            U'\u015b', U'\u0165', U'\u00fa', U'\u1e7d', U'\u0175', U'\u1e8b', U'\u00fd', U'\u017a'};
        constexpr std::array<char32_t, 26> UPPER = {
            U'\u00c5', U'\u0181', U'\u00c7', U'\u010e', U'\u00c9', U'\u0191', U'\u011c', U'\u0124', U'\u00cd',
            U'\u0134', U'\u0136', U'\u0139', U'\u1e40', U'\u00d1', U'\u00d6', U'\u00de', U'\u01ea', U'\u0154',
            U'\u0160', U'\u0164', U'\u00da', U'\u1e7c', U'\u0174', U'\u1e8a', U'\u00dd', U'\u017d'};

        void appendUtf8(std::string& out, const char32_t c)
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

        // Walks the message grammar: text is accented, argument and plural syntax is copied.
        struct Pseudo
        {
            std::string_view src;
            std::size_t pos = 0;
            std::string out;
            bool ok = true;

            void copyUntil(const std::string_view stops)
            {
                while (pos < src.size() && stops.find(src[pos]) == std::string_view::npos)
                {
                    out.push_back(src[pos++]);
                }
                if (pos >= src.size())
                {
                    ok = false;
                }
            }

            // Text up to the end, or in a branch to its closing brace (left unread).
            void text(const bool inBranch)
            {
                while (ok && pos < src.size())
                {
                    const char c = src[pos];
                    if (c == '{')
                    {
                        argument();
                    }
                    else if (c == '}')
                    {
                        if (!inBranch)
                        {
                            ok = false;
                        }
                        break;
                    }
                    else if (c == '\'' && pos + 1 < src.size() && src[pos + 1] == '\'')
                    {
                        out += "''";
                        pos += 2;
                    }
                    else if (c == '\'' && pos + 1 < src.size() && (src[pos + 1] == '{' || src[pos + 1] == '}'))
                    {
                        out.push_back(src[pos++]); // quoted literal: verbatim to the closing apostrophe
                        while (pos < src.size() && (src[pos] != '\'' || (pos + 1 < src.size() && src[pos + 1] == '\'')))
                        {
                            const bool pair = src[pos] == '\'';
                            out.append(src.substr(pos, pair ? 2 : 1));
                            pos += pair ? 2 : 1;
                        }
                        if (pos < src.size())
                        {
                            out.push_back(src[pos++]);
                        }
                    }
                    else
                    {
                        const bool lower = c >= 'a' && c <= 'z';
                        if (lower || (c >= 'A' && c <= 'Z'))
                        {
                            appendUtf8(out, lower ? LOWER[c - 'a'] : UPPER[c - 'A']);
                        }
                        else
                        {
                            out.push_back(c);
                        }
                        ++pos;
                    }
                }
            }

            void argument()
            {
                out.push_back(src[pos++]); // {
                copyUntil(",}");
                if (!ok)
                {
                    return;
                }
                if (src[pos] == '}')
                {
                    out.push_back(src[pos++]);
                    return;
                }
                out.push_back(src[pos++]);
                const std::size_t typeStart = out.size();
                copyUntil(",}");
                if (!ok)
                {
                    return;
                }
                const bool plural = std::string_view(out).substr(typeStart).find("plural") != std::string_view::npos;
                out.push_back(src[pos++]); // the closing brace of {n, number}, or the comma before the branches
                if (!plural)
                {
                    return;
                }
                while (ok)
                {
                    copyUntil("{}");
                    if (!ok)
                    {
                        return;
                    }
                    out.push_back(src[pos++]);
                    if (src[pos - 1] == '}')
                    {
                        return;
                    }
                    text(true);
                    if (!ok || pos >= src.size())
                    {
                        ok = false;
                        return;
                    }
                    out.push_back(src[pos++]); // the branch's closing brace
                }
            }
        };
    } // namespace

    std::string pseudoLocalize(const std::string_view message)
    {
        Pseudo pseudo{.src = message};
        pseudo.out.push_back('[');
        pseudo.text(false);
        const auto codePoints = [](const std::string_view text)
        {
            return std::ranges::count_if(text,
                                         [](const char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; });
        };
        // at least 30 percent longer than the source, brackets included
        const auto wanted = (codePoints(message) * 13 + 9) / 10;
        pseudo.out.append(static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, wanted - codePoints(pseudo.out) - 1)),
                          '~');
        pseudo.out.push_back(']');
        return pseudo.ok && pseudo.out.size() <= MAX_MESSAGE_BYTES ? std::move(pseudo.out) : std::string(message);
    }
} // namespace spite
