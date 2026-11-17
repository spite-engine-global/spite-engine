#pragma once

#include "EditorPanelUi.hpp"

// A panel's page that scrolls instead of clipping when the panel is shorter than the page needs: a 1024x600
// window at text scale 1.5 leaves a docked group about 210 px of content, less than the stacked bars and lists of the
// Input, Localization, State and UiDocuments panels hold at their least.
namespace spite::panelui
{
    // The rows (and the header over them) a list slot of the page keeps; the page scrolls rather than squeeze it
    inline constexpr float PAGE_LIST_ROWS = 5.0f;

    class PageScroll
    {
    public:
        PageScroll() = default;
        // `parent` holds the view, which fills it and is named `nameKey` (the panel's title); the document's host is
        // page()
        PageScroll(UiScope& scope, const InternedString id, const InternedString nameKey, const Widget& parent) :
            m_scope(&scope)
        {
            UiRectDesc desc = topLeft();
            desc.layoutItem = fillItem();
            m_view = scope.createScrollView(id, desc);
            m_view.setAccessibleNameKey(nameKey);
            m_view.setParent(parent);
            m_page = m_view.content();
            m_page.setLayout(stack(UiLayoutDirection::Column, 0.0f, {}, UiLayoutAlign::Stretch));
        }

        [[nodiscard]] Widget page() const { return m_page; }

        // What a growing item of the page needs at least: a value above 0 replaces the item's own minimum, 0 keeps it
        void setLeast(std::function<float(Entity, const Density&)> least) { m_least = std::move(least); }

        // The page is as tall as the view, or as the least its items need when that is taller, and a thumb's room
        // narrower while it scrolls; written only when one of them moved
        void fit(const Density& density)
        {
            const Vec2 view = resolvedSize(*m_scope, m_view);
            if (view.x <= 0.0f || view.y <= 0.0f)
            {
                return;
            }
            const float need = neededHeight(density);
            const bool scrolls = need > view.y + 0.5f;
            const Vec2 want{view.x, std::max(view.y, need)};
            if (m_view.contentSize() != want)
            {
                m_view.setContentSize(want);
            }
            const float thumb = scrolls ? THUMB : 0.0f;
            UiLayoutComponent layout = m_scope->world().getConstComponent<UiLayoutComponent>(m_page.entity());
            if (layout.padding.z != thumb)
            {
                layout.padding.z = thumb;
                m_page.setLayout(layout);
            }
        }

    private:
        // What the document's root needs top to bottom: a shown item its height, a growing item (a list or table) its
        // minimum, or PAGE_LIST_ROWS rows when it has none
        [[nodiscard]] float neededHeight(const Density& density) const
        {
            const UiHierarchyManager& hierarchy = m_scope->hierarchy();
            const World& world = m_scope->world();
            const std::span<const Entity> roots = hierarchy.childrenOf(m_page.entity());
            if (roots.empty())
            {
                return 0.0f;
            }
            const Entity root = roots.front();
            const auto* layout = world.tryGetConstComponent<UiLayoutComponent>(root);
            float fixed = layout ? layout->padding.y + layout->padding.w : 0.0f;
            u32 shown = 0;
            for (const Entity child : hierarchy.childrenOf(root))
            {
                if (!hierarchy.isVisibleOf(child))
                {
                    continue;
                }
                ++shown;
                const auto* item = world.tryGetConstComponent<UiLayoutItemComponent>(child);
                if (item && item->grow > 0.0f)
                {
                    float least = item->minSize.y;
                    if (const float own = m_least ? m_least(child, density) : 0.0f; own > 0.0f)
                    {
                        least = own;
                    }
                    fixed += least > 0.0f ? least : PAGE_LIST_ROWS * density.row;
                }
                else
                {
                    fixed += hierarchy.absoluteRectOf(child).size.y / hierarchy.layoutScale();
                }
            }
            return fixed + (layout && shown > 1 ? layout->gap * static_cast<float>(shown - 1) : 0.0f);
        }

        UiScope* m_scope = nullptr;
        std::function<float(Entity, const Density&)> m_least;
        ScrollView m_view;
        Panel m_page;
    };
} // namespace spite::panelui
