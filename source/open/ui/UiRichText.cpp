#include "ui/UiRichText.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>

namespace spite
{
    namespace
    {
        enum class Tag : u8
        {
            Color,
            Size,
            Bold,
            Weight,
        };

        constexpr std::array<std::string_view, 4> TAG_NAMES = {"color", "size", "b", "weight"};

        struct Style
        {
            u32 color = 0;
            bool hasColor = false;
            float sizePx = 0.0f;
            float sizeScale = 1.0f;
            u16 weight = 0;

            bool operator==(const Style&) const = default;
        };

        struct Open
        {
            Tag tag;
            Style outer; // the style to return to
        };

        int hexDigit(const char c)
        {
            return c >= '0' && c <= '9' ? c - '0'
                : c >= 'a' && c <= 'f'  ? c - 'a' + 10
                : c >= 'A' && c <= 'F'  ? c - 'A' + 10
                                        : -1;
        }

        bool parseColor(std::string_view value, u32& out)
        {
            if (value.empty() || value[0] != '#')
            {
                return false;
            }
            value.remove_prefix(1);
            if (value.size() != 3 && value.size() != 6 && value.size() != 8)
            {
                return false;
            }
            u32 channels[4] = {0, 0, 0, 255};
            const bool shortForm = value.size() == 3;
            const sizet count = shortForm ? 3 : value.size() / 2;
            for (sizet i = 0; i < count; ++i)
            {
                const int high = hexDigit(value[shortForm ? i : i * 2]);
                const int low = hexDigit(value[shortForm ? i : i * 2 + 1]);
                if (high < 0 || low < 0)
                {
                    return false;
                }
                channels[i] = static_cast<u32>(high * 16 + low);
            }
            out = channels[0] | (channels[1] << 8) | (channels[2] << 16) | (channels[3] << 24);
            return true;
        }

        bool parseNumber(const std::string_view text, float& out)
        {
            const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
            return result.ec == std::errc{} && result.ptr == text.data() + text.size() && std::isfinite(out);
        }

        void literal(const std::string_view markup, UiRichText& out)
        {
            out.plain.assign(markup);
            out.runs.clear();
            out.literal = true;
            if (!markup.empty())
            {
                out.runs.push_back({.begin = 0, .end = static_cast<u32>(markup.size())});
            }
        }
    } // namespace

    bool parseUiRichText(const std::string_view markup, UiRichText& out, std::string& error)
    {
        out.plain.clear();
        out.runs.clear();
        out.literal = false;
        const auto fail = [&](std::string reason)
        {
            error = std::move(reason);
            literal(markup, out);
            return false;
        };
        if (markup.size() > UiRichLimits::MAX_MARKUP_BYTES)
        {
            return fail(
                std::format("markup is {} bytes, the limit is {}", markup.size(), UiRichLimits::MAX_MARKUP_BYTES));
        }

        std::array<Open, UiRichLimits::MAX_DEPTH> stack;
        sizet depth = 0;
        Style style;
        sizet sprites = 0;
        u32 runStart = 0; // where the current text run starts in plain

        // closes the text since runStart as a run in the current style; adjacent runs of one style merge
        const auto flush = [&]
        {
            const auto end = static_cast<u32>(out.plain.size());
            if (end == runStart)
            {
                return true;
            }
            if (!out.runs.empty() && out.runs.back().sprite.empty() && out.runs.back().end == runStart &&
                out.runs.back().hasColor == style.hasColor &&
                (!style.hasColor || out.runs.back().color == style.color) && out.runs.back().sizePx == style.sizePx &&
                out.runs.back().sizeScale == style.sizeScale && out.runs.back().weight == style.weight)
            {
                out.runs.back().end = end;
            }
            else
            {
                if (out.runs.size() >= UiRichLimits::MAX_RUNS)
                {
                    return false;
                }
                out.runs.push_back({.begin = runStart,
                                    .end = end,
                                    .color = style.color,
                                    .hasColor = style.hasColor,
                                    .sizePx = style.sizePx,
                                    .sizeScale = style.sizeScale,
                                    .weight = style.weight});
            }
            runStart = end;
            return true;
        };
        const auto tooMany = [&] { return fail(std::format("more than {} runs", UiRichLimits::MAX_RUNS)); };

        for (sizet i = 0; i < markup.size();)
        {
            const char c = markup[i];
            if (c != '[')
            {
                out.plain.push_back(c);
                ++i;
                continue;
            }
            if (i + 1 < markup.size() && markup[i + 1] == '[')
            {
                out.plain.push_back('[');
                i += 2;
                continue;
            }
            const sizet close = markup.find(']', i + 1);
            if (close == std::string_view::npos)
            {
                return fail(std::format("unterminated tag at byte {}", i));
            }
            std::string_view body = markup.substr(i + 1, close - i - 1);
            const sizet tagAt = i;
            i = close + 1;
            if (!flush())
            {
                return tooMany();
            }

            if (!body.empty() && body[0] == '/')
            {
                body.remove_prefix(1);
                if (depth == 0 || TAG_NAMES[static_cast<sizet>(stack[depth - 1].tag)] != body)
                {
                    return fail(std::format("[/{}] at byte {} closes no open [{}]", body, tagAt,
                                            depth == 0 ? body : TAG_NAMES[static_cast<sizet>(stack[depth - 1].tag)]));
                }
                style = stack[--depth].outer;
                continue;
            }

            const sizet eq = body.find('=');
            const std::string_view name = body.substr(0, eq);
            const bool hasValue = eq != std::string_view::npos;
            const std::string_view value = hasValue ? body.substr(eq + 1) : std::string_view{};
            if (name == "sprite")
            {
                if (value.empty() || value.size() > UiRichLimits::MAX_SPRITE_NAME ||
                    value.find('[') != std::string_view::npos)
                {
                    return fail(std::format("[sprite] at byte {} needs a name of 1..{} characters", tagAt,
                                            UiRichLimits::MAX_SPRITE_NAME));
                }
                if (++sprites > UiRichLimits::MAX_SPRITES)
                {
                    return fail(std::format("more than {} sprites", UiRichLimits::MAX_SPRITES));
                }
                if (out.runs.size() >= UiRichLimits::MAX_RUNS)
                {
                    return tooMany();
                }
                out.plain.append(UI_RICH_SPRITE_PLACEHOLDER);
                out.runs.push_back({.begin = runStart,
                                    .end = static_cast<u32>(out.plain.size()),
                                    .color = style.color,
                                    .hasColor = style.hasColor,
                                    .sizePx = style.sizePx,
                                    .sizeScale = style.sizeScale,
                                    .weight = style.weight,
                                    .sprite = std::string(value)});
                runStart = static_cast<u32>(out.plain.size());
                continue;
            }

            Tag tag;
            Style inner = style;
            if (name == "color")
            {
                tag = Tag::Color;
                if (!parseColor(value, inner.color))
                {
                    return fail(std::format("[color] at byte {} needs #rgb, #rrggbb or #rrggbbaa", tagAt));
                }
                inner.hasColor = true;
            }
            else if (name == "size")
            {
                tag = Tag::Size;
                const bool percent = !value.empty() && value.back() == '%';
                float number = 0.0f;
                if (!parseNumber(percent ? value.substr(0, value.size() - 1) : value, number) || !(number > 0.0f) ||
                    number > (percent ? 1000.0f : 512.0f))
                {
                    return fail(std::format("[size] at byte {} needs 1..512 pixels or 1..1000%", tagAt));
                }
                if (percent)
                {
                    // nesting compounds percentages: the effective size stays capped
                    if (inner.sizePx > 0.0f)
                    {
                        inner.sizePx = std::min(inner.sizePx * number * 0.01f, UiRichLimits::MAX_SIZE_PX);
                    }
                    else
                    {
                        inner.sizeScale = std::min(inner.sizeScale * number * 0.01f, UiRichLimits::MAX_SIZE_SCALE);
                    }
                }
                else
                {
                    inner.sizePx = number;
                    inner.sizeScale = 1.0f;
                }
            }
            else if (name == "b" && !hasValue)
            {
                tag = Tag::Bold;
                inner.weight = UiRichLimits::BOLD_WEIGHT;
            }
            else if (name == "weight")
            {
                tag = Tag::Weight;
                float number = 0.0f;
                if (!parseNumber(value, number) || number < 1.0f || number > 1000.0f)
                {
                    return fail(std::format("[weight] at byte {} needs 1..1000", tagAt));
                }
                inner.weight = static_cast<u16>(number);
            }
            else
            {
                return fail(std::format("unknown tag [{}] at byte {}", body, tagAt));
            }
            if (depth == stack.size())
            {
                return fail(std::format("tags nested deeper than {}", UiRichLimits::MAX_DEPTH));
            }
            stack[depth++] = {tag, style};
            style = inner;
        }
        if (!flush())
        {
            return tooMany();
        }
        if (depth != 0)
        {
            return fail(std::format("[{}] is never closed", TAG_NAMES[static_cast<sizet>(stack[depth - 1].tag)]));
        }
        return true;
    }

} // namespace spite
