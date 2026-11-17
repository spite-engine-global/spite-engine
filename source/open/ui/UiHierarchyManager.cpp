#include "ui/UiHierarchyManager.hpp"

namespace spite
{
    UiRect UiLayoutView::absoluteRect(const Entity entity) const { return m_manager->absoluteRectOf(entity); }

    UiRect UiLayoutView::clipRect(const Entity entity) const { return m_manager->clipRectOf(entity); }

    Entity UiLayoutView::clipOwner(const Entity entity) const { return m_manager->clipOwnerOf(entity); }

    bool UiLayoutView::isClipped(const Entity entity) const { return m_manager->isClippedOf(entity); }

    bool UiLayoutView::isVisible(const Entity entity) const { return m_manager->isVisibleOf(entity); }

    u32 UiLayoutView::paintIndex(const Entity entity) const { return m_manager->paintIndexOf(entity); }

    std::span<const Entity> UiLayoutView::paintOrder() const { return m_manager->paintOrderOf(); }
} // namespace spite
