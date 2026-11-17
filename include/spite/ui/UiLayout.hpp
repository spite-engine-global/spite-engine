#pragma once

#include <span>

#include "base/MathTypes.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include "ui/UiExport.hpp"

namespace spite
{
    class UiHierarchyManager;

    // Axis-aligned rect in UI-pixel space; position is the top-left corner.
    struct SPITE_UI_API UiRect
    {
        Vec2 position{0.0f, 0.0f};
        Vec2 size{0.0f, 0.0f};

        [[nodiscard]] constexpr bool contains(Vec2 point) const
        {
            return point.x >= position.x && point.y >= position.y && point.x < position.x + size.x &&
                point.y < position.y + size.y;
        }

        // The overlap area; degenerate (empty) when the rects are disjoint.
        [[nodiscard]] constexpr UiRect intersect(const UiRect& other) const
        {
            const Vec2 minPoint{position.x > other.position.x ? position.x : other.position.x,
                                position.y > other.position.y ? position.y : other.position.y};
            const Vec2 maxPoint{
                (position.x + size.x) < (other.position.x + other.size.x) ? (position.x + size.x)
                                                                          : (other.position.x + other.size.x),
                (position.y + size.y) < (other.position.y + other.size.y) ? (position.y + size.y)
                                                                          : (other.position.y + other.size.y)};
            return UiRect{minPoint, Vec2{maxPoint.x - minPoint.x, maxPoint.y - minPoint.y}};
        }

        constexpr bool operator==(const UiRect& other) const = default;

        SPITE_OBSERVED_SCHEMA(UiRect, position, size)
    };

    // Read-only view over the last resolved layout. Hit testing and rect
    // queries always see the previous resolve — presenters that mutate rects
    // this frame read stale values by contract until the next resolve.
    class SPITE_UI_API UiLayoutView
    {
    public:
        explicit UiLayoutView(const UiHierarchyManager& manager) : m_manager(&manager) {}

        [[nodiscard]] UiRect absoluteRect(Entity entity) const;
        // The clip rect that applies to the element (ancestor chain). Without
        // clipping ancestors the rect is meaningless; check isClipped.
        [[nodiscard]] UiRect clipRect(Entity entity) const;
        // The innermost clipping ancestor, whose rect bounds the clip rect;
        // Entity::undefined() when unclipped.
        [[nodiscard]] Entity clipOwner(Entity entity) const;
        [[nodiscard]] bool isClipped(Entity entity) const;
        // False when the element or any ancestor is hidden, or the element was
        // not part of the last resolve (created since: not drawn, not hit).
        [[nodiscard]] bool isVisible(Entity entity) const;
        // Position in paint order; drawing order == hit priority.
        // UiHierarchyManager::NO_INDEX when not part of the last resolve.
        [[nodiscard]] u32 paintIndex(Entity entity) const;
        [[nodiscard]] std::span<const Entity> paintOrder() const;

    private:
        const UiHierarchyManager* m_manager;
    };
} // namespace spite
