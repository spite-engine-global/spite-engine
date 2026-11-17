#include "ui/UiLayoutTokens.hpp"

#include <cmath>
#include <type_traits>

#include "ecs/World.hpp"

namespace spite
{
    namespace
    {
        // the mutable lookup marks the component written (the layout's change scan reads that): only for a write
        template <bool Write>
        std::conditional_t<Write, float*, const float*> fieldIn(const World& world, const Entity element,
                                                                const UiLayoutField field)
        {
            const auto get = [&]<typename T>()
            {
                if constexpr (Write)
                {
                    return world.tryGetComponent<T>(element);
                }
                else
                {
                    return world.tryGetConstComponent<T>(element);
                }
            };
            switch (field)
            {
            case UiLayoutField::Gap:
            case UiLayoutField::PaddingLeft:
            case UiLayoutField::PaddingTop:
            case UiLayoutField::PaddingRight:
            case UiLayoutField::PaddingBottom:
                {
                    auto* layout = get.template operator()<UiLayoutComponent>();
                    if (!layout)
                    {
                        return nullptr;
                    }
                    auto& padding = layout->padding;
                    return field == UiLayoutField::Gap         ? &layout->gap
                        : field == UiLayoutField::PaddingLeft  ? &padding.x
                        : field == UiLayoutField::PaddingTop   ? &padding.y
                        : field == UiLayoutField::PaddingRight ? &padding.z
                                                               : &padding.w;
                }
            case UiLayoutField::FitPaddingX:
            case UiLayoutField::FitPaddingY:
                {
                    auto* fit = get.template operator()<UiContentSizeComponent>();
                    return !fit ? nullptr : field == UiLayoutField::FitPaddingX ? &fit->padding.x : &fit->padding.y;
                }
            default:
                {
                    auto* item = get.template operator()<UiLayoutItemComponent>();
                    return !item                            ? nullptr
                        : field == UiLayoutField::Basis     ? &item->basis
                        : field == UiLayoutField::MinWidth  ? &item->minSize.x
                        : field == UiLayoutField::MinHeight ? &item->minSize.y
                        : field == UiLayoutField::MaxWidth  ? &item->maxSize.x
                                                            : &item->maxSize.y;
                }
            }
        }
    } // namespace

    const float* uiLayoutFieldValue(const World& world, const Entity element, const UiLayoutField field)
    {
        return fieldIn<false>(world, element, field);
    }

    bool writeUiLayoutField(const World& world, const Entity element, const UiLayoutField field, const float value)
    {
        const float* now = fieldIn<false>(world, element, field);
        if (!now || *now == value)
        {
            return false;
        }
        *fieldIn<true>(world, element, field) = value;
        return true;
    }

    bool UiLayoutTokenTable::set(const InternedString name, const float value)
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        const auto [it, inserted] = m_values.try_emplace(name, value);
        if (!inserted && it->second == value)
        {
            return false;
        }
        it->second = value;
        ++m_generation;
        return true;
    }

    bool UiLayoutTokenTable::erase(const InternedString name)
    {
        if (m_values.erase(name) == 0)
        {
            return false;
        }
        ++m_generation;
        return true;
    }

    bool UiLayoutTokenTable::assign(std::unordered_map<InternedString, float> values)
    {
        std::erase_if(values, [](const auto& entry) { return !std::isfinite(entry.second); });
        if (values == m_values)
        {
            return false;
        }
        m_values = std::move(values);
        ++m_generation;
        return true;
    }

    std::optional<float> UiLayoutTokenTable::find(const InternedString name) const
    {
        const auto it = m_values.find(name);
        return it == m_values.end() ? std::nullopt : std::optional(it->second);
    }

    bool UiColorTokenTable::assign(std::unordered_map<InternedString, u32> values)
    {
        if (values == m_values)
        {
            return false;
        }
        m_values = std::move(values);
        ++m_generation;
        return true;
    }

    std::optional<u32> UiColorTokenTable::find(const InternedString name) const
    {
        const auto it = m_values.find(name);
        return it == m_values.end() ? std::nullopt : std::optional(it->second);
    }
} // namespace spite
