#pragma once

#include <cmath>
#include <string_view>

#include "base/MathTypes.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    struct UiTextMeasure
    {
        Vec2 size;
        bool widthDependent = false;
    };

    // Supplied by the view module (it owns fonts and styles) so layout resolve
    // can size content-fitted elements and text inputs can place the caret
    // without SpiteUi knowing about text.
    class UiTextMeasurer
    {
    public:
        virtual ~UiTextMeasurer() = default;

        // The element's text as it would paint: run width and line height in
        // UI pixels before layout scale.
        [[nodiscard]] virtual Vec2 measureText(Entity element, std::string_view text) const = 0;

        // A label's text as lines laid out within `width` (UI pixels before layout scale; < 0: unbounded): the
        // widest line and the height of all lines. `widthDependent`: the lines depend on the width, so the height
        // asked at another width may differ (a wrapping style); layout then sizes the label at the width its
        // container gives it. The default is the single line of measureText.
        [[nodiscard]] virtual UiTextMeasure measureTextBlock(const Entity element, const std::string_view text,
                                                             [[maybe_unused]] const float width) const
        {
            return {measureText(element, text), false};
        }

        // The caret offset (bytes, on a UTF-8 boundary) nearest `x`: UI pixels
        // from the left edge of the element, `width` wide, before layout
        // scale. The default places the text at the left edge; a view that
        // aligns it otherwise overrides.
        [[nodiscard]] virtual u32 caretAt(const Entity element, const std::string_view text, const float x,
                                          [[maybe_unused]] const float width) const
        {
            u32 best = 0;
            float bestDistance = std::abs(x);
            for (u32 end = 0; end < text.size();)
            {
                ++end;
                while (end < text.size() && (static_cast<u8>(text[end]) & 0xC0u) == 0x80u)
                {
                    ++end;
                }
                const float distance = std::abs(x - measureText(element, text.substr(0, end)).x);
                if (distance < bestDistance)
                {
                    best = end;
                    bestDistance = distance;
                }
            }
            return best;
        }

        // measureTextBlock for a label whose text is rich text markup (UiLabelComponent::rich): `text`
        // is the markup, measured as it paints, spans and inline sprites included. The default measures the markup
        // as plain text.
        [[nodiscard]] virtual UiTextMeasure measureRichTextBlock(const Entity element, const std::string_view text,
                                                                 const float width) const
        {
            return measureTextBlock(element, text, width);
        }

        // New virtuals go last: a module built against the earlier table keeps its slots.
        // measureText at an explicit text size (UI pixels, the text scale not applied: what a canvas text primitive
        // of that size paints), in the element's font and letter spacing. The default is measureText.
        [[nodiscard]] virtual Vec2 measureTextSized(const Entity element, const std::string_view text,
                                                    [[maybe_unused]] const float textSize) const
        {
            return measureText(element, text);
        }
    };
} // namespace spite
