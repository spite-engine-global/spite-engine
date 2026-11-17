#include <algorithm>
#include <cmath>
#include <string>

#include "base/Assert.hpp"
#include "ecs/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        constexpr float THUMB_WIDTH = 8.0f;
        constexpr float THUMB_MIN_HEIGHT = 16.0f;
        constexpr u32 NO_ROW = static_cast<u32>(-1);

        InternedString childId(const InternedString id, const std::string_view suffix)
        {
            return InternedString(std::string(id.toView()) + std::string(suffix));
        }

        Vec2 maxOffsetOf(const Vec2 contentSize, const Vec2 viewSize)
        {
            return Vec2{std::max(0.0f, contentSize.x - viewSize.x), std::max(0.0f, contentSize.y - viewSize.y)};
        }

        // Drag deltas arrive in scaled pointer pixels; scroll state is unscaled.
        float layoutScaleOf(const UiScope& scope) { return scope.hierarchy().layoutScale(); }
    } // namespace

    // -- ScrollView / VirtualList ------------------------------------------------

    Panel ScrollView::content() const
    {
        return isValid() ? Panel(m_scope, m_scope->scrollContent(m_entity), m_generation) : Panel{};
    }

    void ScrollView::setContentSize(const Vec2 size)
    {
        if (!isValid() || m_scope->world().hasComponent<UiVirtualListComponent>(m_entity))
        {
            return; // a list's content size is derived from rowCount, rowHeight and contentWidth
        }
        if (m_scope->world().getConstComponent<UiScrollComponent>(m_entity).contentSize != size)
        {
            m_scope->world().getComponent<UiScrollComponent>(m_entity).contentSize = size;
        }
        m_scope->syncScroll(m_entity, scrollOffset(), true);
    }

    Vec2 ScrollView::contentSize() const
    {
        return isValid() ? m_scope->world().getConstComponent<UiScrollComponent>(m_entity).contentSize : Vec2{};
    }

    void ScrollView::setScrollOffset(const Vec2 offset)
    {
        if (isValid())
        {
            m_scope->syncScroll(m_entity, offset, false);
        }
    }

    Vec2 ScrollView::scrollOffset() const
    {
        return isValid() ? m_scope->world().getConstComponent<UiScrollComponent>(m_entity).offset : Vec2{};
    }

    void ScrollView::scrollBy(const Vec2 delta) { setScrollOffset(scrollOffset() + delta); }

    void ScrollView::setScrollEnabled(const bool enabled)
    {
        if (!isValid())
        {
            return;
        }
        if (m_scope->world().getConstComponent<UiScrollComponent>(m_entity).enabled != enabled)
        {
            m_scope->world().getComponent<UiScrollComponent>(m_entity).enabled = enabled;
            m_scope->syncScroll(m_entity, scrollOffset(), true);
        }
    }

    bool ScrollView::scrollEnabled() const
    {
        return isValid() && m_scope->world().getConstComponent<UiScrollComponent>(m_entity).enabled;
    }

    void VirtualList::setRowCount(const u32 count)
    {
        if (!isValid())
        {
            return;
        }
        if (m_scope->world().getConstComponent<UiVirtualListComponent>(m_entity).rowCount != count)
        {
            m_scope->world().getComponent<UiVirtualListComponent>(m_entity).rowCount = count;
        }
        m_scope->syncScroll(m_entity, scrollOffset(), true);
    }

    u32 VirtualList::rowCount() const
    {
        return isValid() ? m_scope->world().getConstComponent<UiVirtualListComponent>(m_entity).rowCount : 0;
    }

    void VirtualList::setContentWidth(const float width)
    {
        if (!isValid())
        {
            return;
        }
        if (m_scope->world().getConstComponent<UiVirtualListComponent>(m_entity).contentWidth != width)
        {
            m_scope->world().getComponent<UiVirtualListComponent>(m_entity).contentWidth = width;
        }
        m_scope->syncScroll(m_entity, scrollOffset(), true);
    }

    void VirtualList::setRowHeight(const float height)
    {
        if (!isValid() || !(height > 0.0f))
        {
            return;
        }
        const float old = m_scope->world().getConstComponent<UiVirtualListComponent>(m_entity).rowHeight;
        if (old == height)
        {
            return;
        }
        // the view keeps its top row, and a view at the end stays at the end (the clamp finds the new end)
        const Vec2 offset = scrollOffset();
        const float viewHeight = m_scope->hierarchy().layoutSizeOf(m_entity).y;
        const float end = static_cast<float>(rowCount()) * old - viewHeight;
        m_scope->world().getComponent<UiVirtualListComponent>(m_entity).rowHeight = height;
        const float y = end > 0.0f && offset.y >= end - 1.0f ? 1.0e9f : offset.y * height / old;
        m_scope->syncScroll(m_entity, Vec2{offset.x, y}, true);
    }

    float VirtualList::rowHeight() const
    {
        return isValid() ? m_scope->world().getConstComponent<UiVirtualListComponent>(m_entity).rowHeight : 0.0f;
    }

    void VirtualList::refreshRows()
    {
        if (isValid())
        {
            m_scope->syncScroll(m_entity, scrollOffset(), true);
        }
    }

    // -- UiScope scroll internals -------------------------------------------------

    Entity UiScope::createScrollElements(const InternedString id, const UiRectDesc& desc, const Vec2 contentSize)
    {
        const Entity viewport = createElement(id, UiWidgetKind::ScrollView, desc);
        m_world->getComponent<UiStateComponent>(viewport).clipsChildren = true;
        UiScrollComponent scroll;
        scroll.contentSize = contentSize;
        m_world->addComponent<UiScrollComponent>(viewport, scroll);

        UiRectDesc contentDesc;
        contentDesc.pivot = Vec2{0.0f, 0.0f};
        const Entity content = createElement(childId(id, ".content"), UiWidgetKind::Panel, contentDesc);
        UiRectDesc thumbDesc;
        thumbDesc.anchor = Vec2{1.0f, 0.0f};
        thumbDesc.pivot = Vec2{1.0f, 0.0f};
        thumbDesc.zOffset = 1; // over the content
        const Entity thumb = createElement(childId(id, ".thumb"), UiWidgetKind::ScrollThumb, thumbDesc);
        hierarchy().attach(content, viewport);
        hierarchy().attach(thumb, viewport);
        hierarchy().observeSize(viewport);

        ScrollData& data = m_scrollViews[viewport.id()];
        data.content = content;
        data.thumb = thumb;
        m_scrollParts[content.id()] = viewport;
        m_scrollParts[thumb.id()] = viewport;

        setReaction(viewport, UiEventKind::Wheel,
                    [this, viewport](const void* payload)
                    {
                        const auto& event = *static_cast<const UiWheelEvent*>(payload);
                        // wheel up (+y) moves the view toward the top; Shift turns the wheel onto the horizontal axis
                        Vec2 step{event.wheelDelta.x, -event.wheelDelta.y};
                        if ((event.modifiers & UiKeyModifier::SHIFT) != 0)
                        {
                            step = Vec2{step.x + step.y, 0.0f};
                        }
                        const Vec2 offset = m_world->getConstComponent<UiScrollComponent>(viewport).offset;
                        syncScroll(viewport, offset + step * UI_WHEEL_STEP, false);
                    });
        // DragStart carries the first movement, so both kinds scroll.
        const auto pan = [this, viewport](const Vec2 delta)
        {
            const Vec2 offset = m_world->getConstComponent<UiScrollComponent>(viewport).offset;
            syncScroll(viewport, offset - delta * (1.0f / layoutScaleOf(*this)), false);
        };
        setReaction(content, UiEventKind::DragStart,
                    [pan](const void* payload) { pan(static_cast<const UiDragStartEvent*>(payload)->delta); });
        setReaction(content, UiEventKind::DragMove,
                    [pan](const void* payload) { pan(static_cast<const UiDragMoveEvent*>(payload)->delta); });
        bindThumbDrag(viewport, thumb, false);
        return viewport;
    }

    ScrollView UiScope::createScrollView(const InternedString id, const UiRectDesc& desc, const Vec2 contentSize)
    {
        const Entity viewport = createScrollElements(id, desc, contentSize);
        syncScroll(viewport, Vec2{}, true);
        return ScrollView(this, viewport, m_generation);
    }

    VirtualList UiScope::createVirtualList(const InternedString id, UiVirtualListDesc desc)
    {
        SASSERTM(desc.createRow && desc.bindRow && desc.rowHeight > 0.0f,
                 "createVirtualList: needs createRow, bindRow and a positive row height.");
        const Entity viewport = createScrollElements(id, desc.rect, Vec2{});
        UiVirtualListComponent list;
        list.rowCount = desc.rowCount;
        list.rowHeight = desc.rowHeight;
        m_world->addComponent<UiVirtualListComponent>(viewport, list);
        ScrollData& data = m_scrollViews[viewport.id()];
        data.createRow = std::move(desc.createRow);
        data.bindRow = std::move(desc.bindRow);
        syncScroll(viewport, Vec2{}, true);
        return VirtualList(this, viewport, m_generation);
    }

    Entity UiScope::scrollContent(const Entity viewport) const
    {
        const auto it = m_scrollViews.find(viewport.id());
        return it == m_scrollViews.end() ? Entity::undefined() : it->second.content;
    }

    void UiScope::syncScroll(const Entity viewport, const Vec2 offset, const bool full)
    {
        const auto it = m_scrollViews.find(viewport.id());
        if (it == m_scrollViews.end())
        {
            return;
        }
        ScrollData& data = it->second;
        const Vec2 viewSize = hierarchy().layoutSizeOf(viewport);
        if (data.hthumb == Entity::undefined())
        {
            const auto* list = m_world->tryGetConstComponent<UiVirtualListComponent>(viewport);
            const float contentWidth =
                list ? list->contentWidth : m_world->getConstComponent<UiScrollComponent>(viewport).contentSize.x;
            if (contentWidth > viewSize.x)
            {
                createHorizontalThumb(viewport, data); // before taking component references
            }
        }
        // A refresh of an unchanged list rewrites nothing: each write below is made only when its value moves, and
        // the relayout is asked for only then (a rebound row marks what its own text changes).
        const auto* list = m_world->tryGetConstComponent<UiVirtualListComponent>(viewport);
        const UiScrollComponent& scrollNow = m_world->getConstComponent<UiScrollComponent>(viewport);
        const Vec2 contentSize = list
            ? Vec2{std::max(viewSize.x, list->contentWidth), static_cast<float>(list->rowCount) * list->rowHeight}
            : scrollNow.contentSize;
        const Vec2 maxOffset = scrollNow.enabled ? maxOffsetOf(contentSize, viewSize) : Vec2{};
        const Vec2 clamped{std::clamp(offset.x, 0.0f, maxOffset.x), std::clamp(offset.y, 0.0f, maxOffset.y)};
        bool moved = scrollNow.contentSize != contentSize;
        const bool offsetMoved = scrollNow.offset != clamped;
        if (moved || offsetMoved)
        {
            auto& scroll = m_world->getComponent<UiScrollComponent>(viewport);
            scroll.contentSize = contentSize;
            scroll.offset = offsetMoved ? clamped : scroll.offset;
        }
        if (!full && !offsetMoved)
        {
            return; // pinned at an edge or no movement: no relayout
        }
        moved = moved || offsetMoved;

        const auto place = [this, &moved](const Entity entity, const Vec2 at, const Vec2 size)
        {
            const UiRectComponent& now = m_world->getConstComponent<UiRectComponent>(entity);
            if (now.offset != at || now.size != size)
            {
                auto& rect = m_world->getComponent<UiRectComponent>(entity);
                rect.offset = at;
                rect.size = size;
                moved = true;
            }
        };
        const auto show = [this, &moved](const Entity entity, const bool visible)
        {
            if (m_world->getConstComponent<UiStateComponent>(entity).visible != visible)
            {
                m_world->getComponent<UiStateComponent>(entity).visible = visible;
                moved = true;
            }
        };
        place(data.content, Vec2{-clamped.x, -clamped.y}, contentSize);
        show(data.thumb, maxOffset.y > 0.0f);
        if (maxOffset.y > 0.0f)
        {
            const float height =
                std::min(viewSize.y, std::max(THUMB_MIN_HEIGHT, viewSize.y * viewSize.y / contentSize.y));
            place(data.thumb, Vec2{0.0f, (viewSize.y - height) * clamped.y / maxOffset.y}, Vec2{THUMB_WIDTH, height});
        }
        if (data.hthumb != Entity::undefined())
        {
            show(data.hthumb, maxOffset.x > 0.0f);
            if (maxOffset.x > 0.0f)
            {
                const float width =
                    std::min(viewSize.x, std::max(THUMB_MIN_HEIGHT, viewSize.x * viewSize.x / contentSize.x));
                place(data.hthumb, Vec2{(viewSize.x - width) * clamped.x / maxOffset.x, 0.0f},
                      Vec2{width, THUMB_WIDTH});
            }
        }

        if (list)
        {
            // by value: creating pool rows may move component storage
            syncRows(viewport, data, UiVirtualListComponent(*list), viewSize, contentSize.x, clamped.y, full);
        }
        if (moved)
        {
            markLayoutDirty(viewport);
        }
        if (data.onScrolled)
        {
            data.onScrolled(clamped);
        }
    }

    void UiScope::onScrolled(const Entity viewport, std::function<void(Vec2)> fn)
    {
        if (const auto it = m_scrollViews.find(viewport.id()); it != m_scrollViews.end())
        {
            it->second.onScrolled = std::move(fn);
        }
    }

    void UiScope::createHorizontalThumb(const Entity viewport, ScrollData& data)
    {
        UiRectDesc desc;
        desc.anchor = Vec2{0.0f, 1.0f};
        desc.pivot = Vec2{0.0f, 1.0f};
        desc.zOffset = 1; // over the content
        const InternedString id = m_world->getConstComponent<UiIdentityComponent>(viewport).id;
        const Entity thumb = createElement(childId(id, ".hthumb"), UiWidgetKind::ScrollThumb, desc);
        hierarchy().attach(thumb, viewport);
        data.hthumb = thumb;
        m_scrollParts[thumb.id()] = viewport;
        bindThumbDrag(viewport, thumb, true);
    }

    // The thumb follows the pointer's total travel since the drag began, so
    // movement the clamp swallowed at an edge is not lost.
    void UiScope::bindThumbDrag(const Entity viewport, const Entity thumb, const bool horizontal)
    {
        const auto drag = [this, viewport, thumb, horizontal](const void* payload, const bool start)
        {
            const Vec2 position = start ? static_cast<const UiDragStartEvent*>(payload)->position
                                        : static_cast<const UiDragMoveEvent*>(payload)->position;
            ScrollData& data = m_scrollViews.at(viewport.id());
            const auto& scroll = m_world->getConstComponent<UiScrollComponent>(viewport);
            if (start)
            {
                data.dragOrigin = position - static_cast<const UiDragStartEvent*>(payload)->delta;
                data.dragOffset = scroll.offset;
            }
            const Vec2 viewSize = hierarchy().layoutSizeOf(viewport);
            const Vec2 thumbSize = m_world->getConstComponent<UiRectComponent>(thumb).size;
            const u32 axis = horizontal ? 0 : 1;
            const float track = viewSize[axis] - thumbSize[axis];
            if (track <= 0.0f)
            {
                return;
            }
            const float travel = (position[axis] - data.dragOrigin[axis]) / layoutScaleOf(*this);
            Vec2 offset = data.dragOffset;
            offset[axis] += travel * (scroll.contentSize[axis] - viewSize[axis]) / track;
            syncScroll(viewport, offset, false);
        };
        setReaction(thumb, UiEventKind::DragStart, [drag](const void* payload) { drag(payload, true); });
        setReaction(thumb, UiEventKind::DragMove, [drag](const void* payload) { drag(payload, false); });
    }

    void UiScope::syncRows(const Entity viewport, ScrollData& data, const UiVirtualListComponent& list,
                           const Vec2 viewSize, const float rowWidth, const float offsetY, const bool full)
    {
        const u32 poolSize = static_cast<u32>(std::ceil(viewSize.y / list.rowHeight)) + 1;
        while (data.rows.size() < poolSize)
        {
            const Widget row = data.createRow(static_cast<u32>(data.rows.size()));
            SASSERTM(row.scope() == this && owns(row.entity()),
                     "createRow: the row must be created in the list's scope.");
            auto& rect = m_world->getComponent<UiRectComponent>(row.entity());
            rect.anchor = Vec2{0.0f, 0.0f};
            rect.pivot = Vec2{0.0f, 0.0f};
            hierarchy().attach(row.entity(), data.content);
            data.rows.push_back(row.entity());
            m_scrollParts[row.entity().id()] = viewport;
            data.boundRows.push_back(NO_ROW);
        }
        // A rebind of unchanged rows writes nothing and asks for no relayout (the rows' own text marks its cell)
        bool moved = data.poolSize != poolSize;
        const auto show = [this, &moved](const Entity entity, const bool visible)
        {
            if (m_world->getConstComponent<UiStateComponent>(entity).visible != visible)
            {
                m_world->getComponent<UiStateComponent>(entity).visible = visible;
                moved = true;
            }
        };
        // A pool resize changes the slot mapping; slots past it stay hidden.
        const bool rebindAll = full || poolSize != data.poolSize;
        for (u32 slot = poolSize; slot < data.poolSize; ++slot)
        {
            show(data.rows[slot], false);
            data.boundRows[slot] = NO_ROW;
        }
        data.poolSize = poolSize;

        const u32 first = static_cast<u32>(offsetY / list.rowHeight);
        for (u32 row = first; row < first + poolSize; ++row)
        {
            const u32 slot = row % poolSize;
            if (row >= list.rowCount)
            {
                show(data.rows[slot], false);
                data.boundRows[slot] = NO_ROW;
                continue;
            }
            if (!rebindAll && data.boundRows[slot] == row)
            {
                continue;
            }
            data.boundRows[slot] = row;
            const Vec2 at{0.0f, static_cast<float>(row) * list.rowHeight};
            const Vec2 size{rowWidth, list.rowHeight};
            const UiRectComponent& now = m_world->getConstComponent<UiRectComponent>(data.rows[slot]);
            if (now.offset != at || now.size != size)
            {
                auto& rect = m_world->getComponent<UiRectComponent>(data.rows[slot]);
                rect.offset = at;
                rect.size = size;
                moved = true;
            }
            show(data.rows[slot], true);
            data.bindRow(slot, row);
        }
        if (moved)
        {
            markLayoutDirty(viewport);
        }
    }
} // namespace spite
