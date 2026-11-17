#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "base/InternedString.hpp"
#include "base/Platform.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiExport.hpp"

namespace spite
{
    class World;

    // The layout scalars a token may write, in UiLayoutTokensComponent field order.
    enum class UiLayoutField : u8
    {
        Gap,
        PaddingLeft,
        PaddingTop,
        PaddingRight,
        PaddingBottom,
        Basis,
        MinWidth,
        MinHeight,
        MaxWidth,
        MaxHeight,
        FitPaddingX,
        FitPaddingY,
        Count,
    };

    inline constexpr std::array<std::string_view, static_cast<sizet>(UiLayoutField::Count)> UI_LAYOUT_FIELD_NAMES = {
        "layout.gap",    "layout.padding.left", "layout.padding.top", "layout.padding.right", "layout.padding.bottom",
        "item.basis",    "item.min.x",          "item.min.y",         "item.max.x",           "item.max.y",
        "fit.padding.x", "fit.padding.y"};

    // The token each layout field of the element takes from the context's layout token table (an invalid name: the
    // field is a literal). Written by UI documents ("$name") and Widget::bindLayoutToken; the binding is the field's
    // writer, the stylesheet never is.
    struct UiLayoutTokensComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiLayoutTokens";
        InternedString gap{};
        InternedString paddingLeft{};
        InternedString paddingTop{};
        InternedString paddingRight{};
        InternedString paddingBottom{};
        InternedString basis{};
        InternedString minWidth{};
        InternedString minHeight{};
        InternedString maxWidth{};
        InternedString maxHeight{};
        InternedString fitPaddingX{};
        InternedString fitPaddingY{};
        SPITE_SCHEMA(UiLayoutTokensComponent, gap, paddingLeft, paddingTop, paddingRight, paddingBottom, basis,
                     minWidth, minHeight, maxWidth, maxHeight, fitPaddingX, fitPaddingY)

        [[nodiscard]] InternedString& operator[](UiLayoutField field);
        [[nodiscard]] InternedString operator[](UiLayoutField field) const;
        [[nodiscard]] bool empty() const;
    };

    inline constexpr std::array<InternedString UiLayoutTokensComponent::*, static_cast<sizet>(UiLayoutField::Count)>
        UI_LAYOUT_TOKEN_SLOTS = {&UiLayoutTokensComponent::gap,           &UiLayoutTokensComponent::paddingLeft,
                                 &UiLayoutTokensComponent::paddingTop,    &UiLayoutTokensComponent::paddingRight,
                                 &UiLayoutTokensComponent::paddingBottom, &UiLayoutTokensComponent::basis,
                                 &UiLayoutTokensComponent::minWidth,      &UiLayoutTokensComponent::minHeight,
                                 &UiLayoutTokensComponent::maxWidth,      &UiLayoutTokensComponent::maxHeight,
                                 &UiLayoutTokensComponent::fitPaddingX,   &UiLayoutTokensComponent::fitPaddingY};

    inline InternedString& UiLayoutTokensComponent::operator[](const UiLayoutField field)
    {
        return this->*UI_LAYOUT_TOKEN_SLOTS[static_cast<u8>(field)];
    }
    inline InternedString UiLayoutTokensComponent::operator[](const UiLayoutField field) const
    {
        return this->*UI_LAYOUT_TOKEN_SLOTS[static_cast<u8>(field)];
    }
    inline bool UiLayoutTokensComponent::empty() const
    {
        for (const auto slot : UI_LAYOUT_TOKEN_SLOTS)
        {
            if ((this->*slot).isValid())
            {
                return false;
            }
        }
        return true;
    }

    // The field a slot names on the element (in UiLayout, UiLayoutItem or UiContentSize), read without marking its
    // component written; null when the element has no such component.
    [[nodiscard]] SPITE_UI_API const float* uiLayoutFieldValue(const World& world, Entity element, UiLayoutField field);
    // Writes the field when the element has it and the value differs (only then is its component marked written);
    // whether it wrote.
    SPITE_UI_API bool writeUiLayoutField(const World& world, Entity element, UiLayoutField field, float value);

    // Name to number; the generation moves only when the content changes, so the bound fields are rewritten once per
    // real change. A UI with a view (SpiteUiView) has its content replaced by the active theme's numeric tokens on
    // every stylesheet generation: define tokens in a theme or the base sheet's "vars", not by code. Code sets
    // entries only for a UI without a view (a headless or test UI).
    class SPITE_UI_API UiLayoutTokenTable
    {
    public:
        // A non-finite value is refused. True when the content changed.
        bool set(InternedString name, float value);
        bool erase(InternedString name);
        // Replaces the whole content.
        bool assign(std::unordered_map<InternedString, float> values);
        [[nodiscard]] std::optional<float> find(InternedString name) const;
        [[nodiscard]] u64 generation() const { return m_generation; }
        [[nodiscard]] sizet size() const { return m_values.size(); }

    private:
        std::unordered_map<InternedString, float> m_values;
        u64 m_generation = 0;
    };

    // The colours of the active theme's "vars" by name (sRGB rgba8, red in the low byte: a uiCanvasColor), for the code
    // that paints a canvas. A UI with a view has its content replaced on every stylesheet generation; the layout
    // token observers also run when it changes (UiScope::observeLayoutTokens).
    class SPITE_UI_API UiColorTokenTable
    {
    public:
        // True when the content changed.
        bool assign(std::unordered_map<InternedString, u32> values);
        [[nodiscard]] std::optional<u32> find(InternedString name) const;
        [[nodiscard]] u64 generation() const { return m_generation; }

    private:
        std::unordered_map<InternedString, u32> m_values;
        u64 m_generation = 0;
    };
} // namespace spite
