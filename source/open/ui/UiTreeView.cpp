#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>

#include "base/Assert.hpp"
#include "ecs/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiEvents.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiScope.hpp"
#include "ui/UiTextMeasurer.hpp"
#include "UiTreeState.hpp"

namespace spite
{
    namespace
    {
        constexpr float EDGE_ZONE = 0.25f; // of a row height: drops before / after; between them, into
        constexpr float DROP_LINE = 2.0f;
        constexpr float CHIP_SIDE = 8.0f;
        constexpr double SLOW_CLICK_MIN = 0.4; // a second click sooner is a double click
        constexpr double SLOW_CLICK_MAX = 2.0;

        // HID scancodes (the UiKeyTransition domain).
        namespace Key
        {
            constexpr u32 ESCAPE = 41;
            constexpr u32 F2 = 59;
            constexpr u32 HOME = 74;
            constexpr u32 END = 77;
            constexpr u32 RIGHT = 79;
            constexpr u32 LEFT = 80;
            constexpr u32 DOWN = 81;
            constexpr u32 UP = 82;
        } // namespace Key

        InternedString childId(const InternedString id, const std::string_view suffix)
        {
            return InternedString(std::string(id.toView()) + std::string(suffix));
        }

        UiRectDesc topLeft(const Vec2 offset, const Vec2 size)
        {
            UiRectDesc desc;
            desc.offset = offset;
            desc.size = size;
            desc.pivot = Vec2{0.0f, 0.0f};
            return desc;
        }

        bool contains(const std::vector<Entity>& sorted, const Entity entity)
        {
            return std::ranges::binary_search(sorted, entity);
        }

        bool insertSorted(std::vector<Entity>& sorted, const Entity entity)
        {
            const auto it = std::ranges::lower_bound(sorted, entity);
            if (it != sorted.end() && *it == entity)
            {
                return false;
            }
            sorted.insert(it, entity);
            return true;
        }

        bool eraseSorted(std::vector<Entity>& sorted, const Entity entity)
        {
            const auto it = std::ranges::lower_bound(sorted, entity);
            if (it == sorted.end() || *it != entity)
            {
                return false;
            }
            sorted.erase(it);
            return true;
        }

        UiTreeSelect selectMode(const u8 modifiers)
        {
            const bool ctrl = (modifiers & UiKeyModifier::CTRL) != 0;
            if ((modifiers & UiKeyModifier::SHIFT) != 0)
            {
                return ctrl ? UiTreeSelect::AddRange : UiTreeSelect::Range;
            }
            return ctrl ? UiTreeSelect::Toggle : UiTreeSelect::Replace;
        }
    } // namespace

    // -- UiTreeState ---------------------------------------------------------------

    // reads go through the const access: the mutable one marks the component written on every call
    const UiTreeComponent& UiTreeState::tree() const
    {
        return scope->world().getConstComponent<UiTreeComponent>(list.entity());
    }

    UiTreeComponent& UiTreeState::writableTree() const
    {
        return scope->world().getComponent<UiTreeComponent>(list.entity());
    }

    bool UiTreeState::isExpanded(const Entity node) const { return contains(tree().expanded, node); }

    u32 UiTreeState::rowOf(const Entity node) const
    {
        const auto it = std::ranges::find(rows, node, &Row::node);
        return it == rows.end() ? NO_ROW : static_cast<u32>(it - rows.begin());
    }

    Widget UiTreeState::createRow(const InternedString id)
    {
        UiScope& owner = *scope;
        const float height = desc.rowHeight;
        Slot slot;
        slot.root = owner.createPanel(childId(id, ".row"));
        owner.world().addComponent<UiTreeRowComponent>(slot.root.entity(), UiTreeRowComponent{});
        if (desc.mark)
        {
            slot.wash = owner.createPanel(childId(id, ".row.wash"), topLeft(Vec2{}, Vec2{0.0f, height}));
            slot.mark = owner.createLabel(childId(id, ".row.mark"), "", topLeft(Vec2{}, Vec2{0.0f, height}));
        }
        slot.highlight = owner.createPanel(childId(id, ".row.selected"), topLeft(Vec2{}, Vec2{0.0f, height}));
        slot.toggle = owner.createLabel(childId(id, ".row.toggle"), "", topLeft(Vec2{}, Vec2{desc.indent, height}));
        slot.toggle.setAccessibleRole(UiRole::Presentation);
        slot.text = owner.createLabel(childId(id, ".row.text"), "", topLeft(Vec2{desc.indent, 0.0f}, Vec2{}));
        if (desc.detail)
        {
            slot.detail = owner.createLabel(childId(id, ".row.detail"), "", topLeft(Vec2{}, Vec2{0.0f, height}));
        }
        if (desc.icon)
        {
            slot.icon = owner.createLabel(childId(id, ".row.icon"), "", topLeft(Vec2{}, Vec2{desc.indent, height}));
            slot.icon.setAccessibleRole(UiRole::Presentation);
        }
        if (desc.barSize.x > 0.0f && desc.detail)
        {
            slot.bar = owner.createPanel(childId(id, ".row.bar"), topLeft(Vec2{}, desc.barSize));
            slot.barFill = owner.createPanel(childId(id, ".row.bar.fill"), topLeft(Vec2{}, Vec2{0.0f, desc.barSize.y}));
        }
        if (desc.chip)
        {
            const float side = CHIP_SIDE;
            slot.chip = owner.createPanel(childId(id, ".row.chip"), topLeft(Vec2{}, Vec2{side, side}));
            owner.world().addComponent<UiVisualComponent>(slot.chip.entity(), UiVisualComponent{});
        }
        styleRow(slot);
        // the row root takes every hit: parts never shadow it
        for (Widget part :
             {Widget(slot.wash), Widget(slot.highlight), Widget(slot.toggle), Widget(slot.chip), Widget(slot.icon),
              Widget(slot.text), Widget(slot.bar), Widget(slot.barFill), Widget(slot.detail), Widget(slot.mark)})
        {
            if (part.isValid())
            {
                part.setParent(slot.root);
                part.setInteractable(false);
            }
        }
        slot.highlight.setVisible(false);
        if (slot.wash.isValid())
        {
            slot.wash.setVisible(false);
            slot.mark.setVisible(false);
        }
        if (slot.detail.isValid())
        {
            slot.detail.setVisible(false);
        }
        if (slot.bar.isValid())
        {
            slot.bar.setVisible(false);
            slot.barFill.setVisible(false);
        }

        const u32 index = static_cast<u32>(slots.size());
        const Entity root = slot.root.entity();
        owner.setReaction(root, UiEventKind::PointerDown,
                          [this, index](const void* payload)
                          {
                              const auto* event = static_cast<const UiPointerDownEvent*>(payload);
                              if (event->button == 0)
                              {
                                  pressed = slots[index].node;
                              }
                              else if (event->button == 1)
                              {
                                  contextMenu(slots[index].node, event->position);
                              }
                          });
        owner.setReaction(root, UiEventKind::Click,
                          [this, index](const void* payload)
                          {
                              const auto* event = static_cast<const UiClickEvent*>(payload);
                              if (event->button == 0)
                              {
                                  click(index, event->position, event->modifiers);
                              }
                          });
        owner.setReaction(root, UiEventKind::DragStart,
                          [this](const void* payload)
                          {
                              beginDrag();
                              updateDrag(static_cast<const UiDragStartEvent*>(payload)->position);
                          });
        owner.setReaction(root, UiEventKind::DragMove, [this](const void* payload)
                          { updateDrag(static_cast<const UiDragMoveEvent*>(payload)->position); });
        owner.setReaction(root, UiEventKind::DragEnd,
                          [this](const void* payload)
                          {
                              // a modal that took the press ends the drag without a drop
                              if (scope->router().blockedByModal(list.entity()))
                              {
                                  cancelDrag();
                                  return;
                              }
                              endDrag(static_cast<const UiDragEndEvent*>(payload)->position);
                          });
        slots.push_back(slot);
        return slot.root;
    }

    void UiTreeState::styleRow(Slot& slot) const
    {
        const auto partClass = [this](const std::string_view part)
        { return desc.rowClass.isValid() ? childId(desc.rowClass, part) : InternedString{}; };
        slot.root.setStyleClass(desc.rowClass);
        slot.highlight.setStyleClass(partClass(".selected"));
        slot.toggle.setStyleClass(partClass(".toggle"));
        if (slot.chip.isValid())
        {
            slot.chip.setStyleClass(partClass(".chip"));
        }
        slot.text.setStyleClass(partClass(slot.dim ? (slot.dimSelected ? ".text.inactive.selected" : ".text.inactive")
                                              : slot.selected && slot.icon.isValid() ? ".text.selected"
                                                                                     : ".text"));
        if (slot.icon.isValid())
        {
            slot.icon.setStyleClass(partClass(slot.selected  ? ".icon.selected"
                                                  : slot.dim ? ".icon.inactive"
                                                             : ".icon"));
        }
        if (slot.detail.isValid())
        {
            slot.detail.setStyleClass(partClass(slot.detailWarn ? ".detail.warn" : ".detail"));
        }
        if (slot.bar.isValid())
        {
            slot.bar.setStyleClass(partClass(".bar"));
            slot.barFill.setStyleClass(partClass(slot.detailWarn ? ".bar.fill.warn" : ".bar.fill"));
        }
        if (slot.wash.isValid())
        {
            slot.wash.setStyleClass(partClass(".wash"));
            slot.mark.setStyleClass(partClass(".mark"));
        }
    }

    void UiTreeState::bindRow(const u32 slotIndex, const u32 row)
    {
        Slot& slot = slots[slotIndex];
        const Row shown = rows[row];
        slot.node = shown.node;
        // written on a change only: the mutable access marks the row modified on every rebind
        const auto& bound = scope->world().getConstComponent<UiTreeRowComponent>(slot.root.entity());
        if (bound.node != shown.node || bound.depth != shown.depth)
        {
            auto& rowComponent = scope->world().getComponent<UiTreeRowComponent>(slot.root.entity());
            rowComponent.node = shown.node;
            rowComponent.depth = shown.depth;
        }

        const UiTreeComponent& state = tree();
        const float x = static_cast<float>(shown.depth) * desc.indent;
        const Vec2 size = slot.root.size();
        const bool parent = hasChildren(shown.node);
        const bool open = parent && contains(state.expanded, shown.node);
        if (slot.icon.isValid())
        {
            const std::string_view caret = parent ? (open ? "caret-down" : "caret-right") : "";
            if (slot.toggle.icon() != caret)
            {
                slot.toggle.setIcon(caret);
            }
        }
        else
        {
            slot.toggle.setText(parent ? (open ? "-" : "+") : "");
        }
        slot.root.setAccessibleExpanded(!parent    ? UiExpandState::None
                                            : open ? UiExpandState::Expanded
                                                   : UiExpandState::Collapsed);
        slot.toggle.setOffset(Vec2{x, 0.0f});
        const float labelX = x + desc.indent + (slot.chip.isValid() ? desc.chipSpace : 0.0f) +
            (slot.icon.isValid() ? desc.indent : 0.0f);
        const bool selected = contains(state.selected, shown.node);
        bool restyle = slot.selected != selected;
        slot.selected = selected;
        if (slot.icon.isValid())
        {
            const std::string_view icon = desc.icon(shown.node);
            if (slot.icon.icon() != icon)
            {
                slot.icon.setIcon(icon);
            }
            slot.icon.setOffset(Vec2{x + desc.indent, 0.0f});
        }
        if (slot.chip.isValid())
        {
            const Vec4 colour = desc.chip(shown.node);
            slot.chip.setOffset(
                Vec2{x + desc.indent + (desc.chipSpace - CHIP_SIDE) * 0.5f, (desc.rowHeight - CHIP_SIDE) * 0.5f});
            slot.chip.setVisible(colour.w > 0.0f);
            // the tint is the one per-element field; written on a change only, never per frame
            if (colour.w > 0.0f &&
                scope->world().getConstComponent<UiVisualComponent>(slot.chip.entity()).tint != colour)
            {
                scope->world().getComponent<UiVisualComponent>(slot.chip.entity()).tint = colour;
                scope->hierarchy().markVisualDirty(slot.chip.entity());
            }
        }
        // the label's own width: a tag or readout takes room from the name only when the name keeps what it needs
        const std::string_view label = desc.label(shown.node);
        const UiTextMeasurer* measurer = scope->hierarchy().textMeasurer();
        float need = -1.0f;
        const auto labelNeed = [&]
        {
            if (need < 0.0f)
            {
                need = measurer ? std::ceil(measurer->measureText(slot.text.entity(), label).x) : 0.0f;
            }
            return need;
        };
        float markWidth = 0.0f;
        if (slot.wash.isValid())
        {
            const UiTreeMark mark = desc.mark(shown.node);
            const bool tagged = !mark.text.empty();
            std::string_view markText = mark.text;
            std::string_view markName; // the full word, read aloud in place of the short form
            if (tagged)
            {
                const float room = std::max(0.0f, size.x - labelX - desc.markInset);
                const bool full =
                    mark.shortText.empty() || room - desc.markSpace >= std::max(desc.markMinLabel, labelNeed());
                markText = full ? mark.text : mark.shortText;
                markName = full ? std::string_view{} : mark.text;
                markWidth = std::min(full ? desc.markSpace : desc.markShortSpace, room);
            }
            slot.mark.setVisible(tagged);
            if (tagged)
            {
                slot.mark.setOffset(Vec2{size.x - desc.markInset - markWidth, 0.0f});
                slot.mark.setSize(Vec2{markWidth, desc.rowHeight});
                slot.mark.setText(markText);
            }
            if (slot.markName != markName)
            {
                slot.markName = std::string(markName);
                slot.mark.setAccessibleName(markName);
            }
            slot.wash.setSize(size);
            slot.wash.setVisible(mark.wash);
        }
        // the readout ends where the tag begins (the inset's edge without one) and takes its space only when the label
        // keeps detailMinLabel beside it
        float textEdge = size.x - (markWidth > 0.0f ? markWidth + desc.markInset : 0.0f);
        if (slot.detail.isValid())
        {
            const UiTreeDetail detail = desc.detail(shown.node);
            const float detailRight = size.x - desc.markInset - markWidth;
            const bool fits = !detail.text.empty() &&
                detailRight - labelX >= desc.detailSpace + std::max(desc.detailMinLabel, labelNeed());
            slot.detail.setVisible(fits);
            if (fits)
            {
                if (slot.detailWarn != detail.warn)
                {
                    slot.detailWarn = detail.warn;
                    restyle = true;
                }
                textEdge = detailRight - desc.detailSpace;
                slot.detail.setOffset(Vec2{textEdge, 0.0f});
                slot.detail.setSize(Vec2{desc.detailSpace, desc.rowHeight});
                slot.detail.setText(detail.text);
            }
            if (slot.bar.isValid())
            {
                const float room = desc.barSize.x + desc.barGap;
                const bool barred = fits && detail.bar >= 0.0f &&
                    textEdge - labelX >= room + std::max(desc.detailMinLabel, labelNeed());
                slot.bar.setVisible(barred);
                slot.barFill.setVisible(barred && detail.bar > 0.0f);
                if (barred)
                {
                    const float left = textEdge - room;
                    const float top = (desc.rowHeight - desc.barSize.y) * 0.5f;
                    slot.bar.setOffset(Vec2{left, top});
                    slot.barFill.setOffset(Vec2{left, top});
                    slot.barFill.setSize(Vec2{desc.barSize.x * std::min(1.0f, detail.bar), desc.barSize.y});
                    textEdge = left;
                }
            }
        }
        // a label the row clips shows whole in the tooltip, beside the look's own message (or beside "Name")
        const bool clipped = measurer && labelNeed() > std::max(0.0f, textEdge - labelX) + 0.5f;
        InternedString tooltip{};
        if (desc.look)
        {
            const UiTreeLook look = desc.look(shown.node);
            const bool dimSelected = look.dim && contains(state.selected, shown.node);
            if (slot.dim != look.dim || slot.dimSelected != dimSelected)
            {
                slot.dim = look.dim;
                slot.dimSelected = dimSelected;
                restyle = true;
            }
            tooltip = look.tooltip;
        }
        if (clipped && !tooltip.isValid())
        {
            tooltip = InternedString("engine.ui.tree.name");
        }
        if (slot.tooltip != tooltip || slot.tooltipName != (clipped ? label : std::string_view{}))
        {
            slot.tooltip = tooltip;
            slot.tooltipName = clipped ? std::string(label) : std::string{};
            slot.root.setTooltip(tooltip, slot.tooltipName);
        }
        if (restyle)
        {
            styleRow(slot);
        }
        slot.text.setOffset(Vec2{labelX, 0.0f});
        slot.text.setSize(Vec2{std::max(0.0f, textEdge - labelX), desc.rowHeight});
        slot.text.setText(label);
        slot.highlight.setSize(size);
        slot.highlight.setVisible(selected);
    }

    void UiTreeState::appendVisible(const Entity node, const u32 depth, std::vector<Row>& out) const
    {
        struct Frame
        {
            std::span<const Entity> children;
            sizet next = 0;
            u32 depth = 0;
        };
        const std::vector<Entity>& expanded = tree().expanded;
        std::vector<Frame> stack{{desc.children(node), 0, depth}};
        while (!stack.empty())
        {
            Frame& top = stack.back();
            if (top.next == top.children.size())
            {
                stack.pop_back();
                continue;
            }
            const auto sibling = static_cast<u32>(top.next++);
            const Entity child = top.children[sibling];
            const u32 childDepth = top.depth;
            out.push_back({child, childDepth, sibling});
            if (contains(expanded, child))
            {
                stack.push_back({desc.children(child), 0, childDepth + 1});
            }
        }
    }

    void UiTreeState::refresh()
    {
        rows.clear();
        appendVisible(Entity::undefined(), 0, rows);
        // The model may have deleted nodes, which the tree cannot tell from
        // hidden ones: forget the single nodes that no longer show, and keep a
        // drag only while all its nodes show (a drag survives a refresh per frame).
        const UiTreeComponent& state = tree();
        bool cursorShown = false;
        bool anchorShown = false;
        bool pressedShown = false;
        sizet draggedShown = 0;
        for (const Row& row : rows)
        {
            cursorShown |= row.node == state.cursor;
            anchorShown |= row.node == state.anchor;
            pressedShown |= row.node == pressed;
            draggedShown += contains(dragSet, row.node) ? 1 : 0;
        }
        const bool lostCursor = !cursorShown && state.cursor != Entity::undefined();
        const bool lostAnchor = !anchorShown && state.anchor != Entity::undefined();
        if (lostCursor || lostAnchor)
        {
            UiTreeComponent& forgotten = writableTree();
            forgotten.cursor = lostCursor ? Entity::undefined() : forgotten.cursor;
            forgotten.anchor = lostAnchor ? Entity::undefined() : forgotten.anchor;
        }
        pressed = pressedShown ? pressed : Entity::undefined();
        if (draggedShown != dragSet.size())
        {
            cancelDrag();
        }
        else if (!dragSet.empty())
        {
            placeDragged(); // siblings may have moved
        }
        list.setRowCount(static_cast<u32>(rows.size()));
    }

    void UiTreeState::setExpanded(const Entity node, const bool expanded)
    {
        UiTreeComponent& state = writableTree();
        if (!(expanded ? insertSorted(state.expanded, node) : eraseSorted(state.expanded, node)))
        {
            return;
        }
        const u32 row = rowOf(node);
        if (row == NO_ROW)
        {
            return; // under a collapsed ancestor: shows when that expands
        }
        const u32 depth = rows[row].depth;
        if (expanded)
        {
            std::vector<Row> revealed;
            appendVisible(node, depth + 1, revealed);
            rows.insert(rows.begin() + row + 1, revealed.begin(), revealed.end());
        }
        else
        {
            auto end = rows.begin() + row + 1;
            while (end != rows.end() && end->depth > depth)
            {
                ++end;
            }
            // keyboard navigation continues from a visible row
            if (std::ranges::find(rows.begin() + row + 1, end, state.cursor, &Row::node) != end)
            {
                state.cursor = node;
            }
            rows.erase(rows.begin() + row + 1, end);
        }
        list.setRowCount(static_cast<u32>(rows.size()));
    }

    void UiTreeState::select(const Entity node, const UiTreeSelect mode)
    {
        UiTreeComponent& state = writableTree();
        const bool ranged = mode == UiTreeSelect::Range || mode == UiTreeSelect::AddRange;
        const u32 from = ranged ? rowOf(state.anchor) : NO_ROW;
        const u32 to = ranged ? rowOf(node) : NO_ROW;
        if (from != NO_ROW && to != NO_ROW)
        {
            std::vector<Entity> range;
            for (u32 row = std::min(from, to); row <= std::max(from, to); ++row)
            {
                range.push_back(rows[row].node);
            }
            std::ranges::sort(range);
            if (mode == UiTreeSelect::AddRange)
            {
                std::vector<Entity> merged;
                std::ranges::set_union(state.selected, range, std::back_inserter(merged));
                range = std::move(merged);
            }
            state.selected = std::move(range);
        }
        else if (mode == UiTreeSelect::Toggle || mode == UiTreeSelect::AddRange)
        {
            // AddRange without a visible anchor adds just the node
            if (!insertSorted(state.selected, node) && mode == UiTreeSelect::Toggle)
            {
                eraseSorted(state.selected, node);
            }
            state.anchor = node;
        }
        else
        {
            state.selected = {node};
            state.anchor = node;
        }
        state.cursor = node;
        const std::vector<Entity> selection = state.selected; // the callback may set it
        list.refreshRows();
        if (onSelect)
        {
            onSelect(selection);
        }
    }

    void UiTreeState::setSelection(const std::span<const Entity> nodes)
    {
        UiTreeComponent& state = writableTree();
        state.selected.assign(nodes.begin(), nodes.end());
        std::ranges::sort(state.selected);
        state.selected.erase(std::ranges::unique(state.selected).begin(), state.selected.end());
        state.cursor = nodes.empty() ? Entity::undefined() : nodes.back();
        state.anchor = state.cursor;
        list.refreshRows();
    }

    void UiTreeState::apply(const UiTreeCommand command, const bool extend)
    {
        if (rows.empty())
        {
            return;
        }
        const u32 last = static_cast<u32>(rows.size()) - 1;
        const u32 row = rowOf(tree().cursor);
        u32 target = command == UiTreeCommand::End ? last : 0;
        if (row != NO_ROW)
        {
            const Entity node = rows[row].node;
            switch (command)
            {
            case UiTreeCommand::Up:
                target = row > 0 ? row - 1 : 0;
                break;
            case UiTreeCommand::Down:
                target = std::min(row + 1, last);
                break;
            case UiTreeCommand::Left:
                if (isExpanded(node) && hasChildren(node))
                {
                    setExpanded(node, false);
                    return;
                }
                target = rowOf(desc.parent(node));
                if (target == NO_ROW)
                {
                    return;
                }
                break;
            case UiTreeCommand::Right:
                if (!hasChildren(node))
                {
                    return;
                }
                if (!isExpanded(node))
                {
                    setExpanded(node, true);
                    return;
                }
                target = row + 1;
                break;
            case UiTreeCommand::Home:
            case UiTreeCommand::End:
                break;
            }
        }
        const Entity node = rows[target].node;
        select(node, extend ? UiTreeSelect::Range : UiTreeSelect::Replace);
        scrollTo(node);
    }

    void UiTreeState::scrollTo(const Entity node)
    {
        const u32 row = rowOf(node);
        if (row == NO_ROW)
        {
            return;
        }
        const float top = static_cast<float>(row) * desc.rowHeight;
        const float offset = list.scrollOffset().y;
        const float viewHeight = list.size().y;
        if (top < offset)
        {
            list.setScrollOffset(Vec2{list.scrollOffset().x, top});
        }
        else if (top + desc.rowHeight > offset + viewHeight)
        {
            list.setScrollOffset(Vec2{list.scrollOffset().x, top + desc.rowHeight - viewHeight});
        }
    }

    void UiTreeState::click(const u32 slotIndex, const Vec2 position, const u8 modifiers)
    {
        const Slot& slot = slots[slotIndex];
        const Entity node = slot.node;
        const UiHierarchyManager& hierarchy = scope->hierarchy();
        const float x =
            (position.x - hierarchy.absoluteRectOf(slot.root.entity()).position.x) / hierarchy.layoutScale();
        const float expander =
            static_cast<float>(scope->world().getConstComponent<UiTreeRowComponent>(slot.root.entity()).depth) *
            desc.indent;
        if (x >= expander && x < expander + desc.indent && hasChildren(node))
        {
            lastClicked = Entity::undefined();
            setExpanded(node, !isExpanded(node));
            return;
        }
        const bool plain = (modifiers & (UiKeyModifier::SHIFT | UiKeyModifier::CTRL | UiKeyModifier::ALT)) == 0;
        const std::vector<Entity>& selected = tree().selected;
        const double since = time - lastClickAt;
        const bool slow = plain && onRename && node == lastClicked && selected.size() == 1 &&
            selected.front() == node && since > SLOW_CLICK_MIN && since <= SLOW_CLICK_MAX;
        lastClicked = plain && !slow ? node : Entity::undefined();
        lastClickAt = time;
        if (slow)
        {
            onRename(node);
            return;
        }
        select(node, selectMode(modifiers));
    }

    void UiTreeState::contextMenu(const Entity node, const Vec2 position)
    {
        if (node == Entity::undefined())
        {
            return;
        }
        if (!contains(tree().selected, node))
        {
            select(node, UiTreeSelect::Replace);
        }
        if (onContextMenu)
        {
            onContextMenu(node, position);
        }
    }

    void UiTreeState::beginDrag()
    {
        UiTreeComponent& state = writableTree();
        if (pressed == Entity::undefined())
        {
            return;
        }
        dragSet.clear();
        if (contains(state.selected, pressed))
        {
            for (const Entity node : state.selected)
            {
                bool nested = false;
                for (Entity parent = desc.parent(node); parent != Entity::undefined() && !nested;
                     parent = desc.parent(parent))
                {
                    nested = contains(state.selected, parent);
                }
                if (!nested)
                {
                    dragSet.push_back(node);
                }
            }
        }
        else
        {
            dragSet.push_back(pressed);
        }
        // reported in row order; nodes under collapsed parents follow
        std::vector<bool> placed(dragSet.size(), false);
        state.dragged.clear();
        for (const Row& row : rows)
        {
            const auto it = std::ranges::lower_bound(dragSet, row.node);
            if (it != dragSet.end() && *it == row.node)
            {
                placed[static_cast<sizet>(it - dragSet.begin())] = true;
                state.dragged.push_back(row.node);
            }
        }
        for (sizet i = 0; i < dragSet.size(); ++i)
        {
            if (!placed[i])
            {
                state.dragged.push_back(dragSet[i]);
            }
        }
        placeDragged();
    }

    void UiTreeState::placeDragged()
    {
        dragPlaces.clear();
        for (const Entity node : dragSet)
        {
            const Entity parent = desc.parent(node);
            const std::span<const Entity> siblings = desc.children(parent);
            dragPlaces.push_back({parent, static_cast<u32>(std::ranges::find(siblings, node) - siblings.begin())});
        }
    }

    UiTreeState::DropTarget UiTreeState::targetAt(const Vec2 position) const
    {
        const UiHierarchyManager& hierarchy = scope->hierarchy();
        const float height = desc.rowHeight;
        const float y =
            std::max(0.0f,
                     (position.y - hierarchy.absoluteRectOf(list.entity()).position.y) / hierarchy.layoutScale() +
                         list.scrollOffset().y);
        const u32 row = static_cast<u32>(y / height);
        if (row >= rows.size())
        {
            const sizet roots = desc.children(Entity::undefined()).size();
            return {Entity::undefined(), static_cast<u32>(roots), false,
                    Vec2{0.0f, static_cast<float>(rows.size()) * height}};
        }
        const float fraction = y / height - static_cast<float>(row);
        const Row& shown = rows[row];
        const float x = static_cast<float>(shown.depth) * desc.indent;
        const float top = static_cast<float>(row) * height;
        if (fraction >= EDGE_ZONE && fraction <= 1.0f - EDGE_ZONE)
        {
            return {shown.node, static_cast<u32>(desc.children(shown.node).size()), true, Vec2{0.0f, top}};
        }
        const bool after = fraction > 0.5f;
        if (after && isExpanded(shown.node) && hasChildren(shown.node))
        {
            return {shown.node, 0, false, Vec2{x + desc.indent, top + height}}; // before its first child
        }
        return {desc.parent(shown.node), shown.sibling + (after ? 1u : 0u), false, Vec2{x, after ? top + height : top}};
    }

    void UiTreeState::updateDrag(const Vec2 position)
    {
        if (tree().dragged.empty())
        {
            return;
        }
        dragPosition = position;
        const DropTarget target = targetAt(position);
        bool valid = true;
        for (Entity parent = target.parent; parent != Entity::undefined() && valid; parent = desc.parent(parent))
        {
            valid = !contains(dragSet, parent);
        }
        dropRefused = !valid;
        externalDrop = false;
        // the index among the parent's children once the moved nodes left
        u32 index = target.index;
        bool moves = false;
        for (const Place& place : dragPlaces)
        {
            index -= place.parent == target.parent && place.sibling < target.index ? 1u : 0u;
            moves |= place.parent != target.parent;
        }
        valid = valid && (desc.ordered || moves);
        UiTreeComponent& state = writableTree();
        state.dropParent = target.parent;
        state.dropIndex = index;
        state.dropValid = valid;

        const float width = list.size().x;
        indicator.setVisible(valid);
        if (target.into)
        {
            indicator.setOffset(target.marker);
            indicator.setSize(Vec2{width, desc.rowHeight});
        }
        else
        {
            indicator.setOffset(Vec2{target.marker.x, target.marker.y - DROP_LINE * 0.5f});
            indicator.setSize(Vec2{std::max(0.0f, width - target.marker.x), DROP_LINE});
        }
    }

    void UiTreeState::showDropTarget(const Entity node)
    {
        const bool roots = node == Entity::undefined();
        const u32 row = roots ? NO_ROW : rowOf(node);
        if (!roots && row == NO_ROW)
        {
            hideDropTarget();
            return;
        }
        const float width = list.size().x;
        const float height = desc.rowHeight;
        const Vec2 offset = roots ? Vec2{0.0f, static_cast<float>(rows.size()) * height - DROP_LINE * 0.5f}
                                  : Vec2{0.0f, static_cast<float>(row) * height};
        const Vec2 size = roots ? Vec2{width, DROP_LINE} : Vec2{width, height};
        if (externalDrop && offset == externalOffset && size == externalSize)
        {
            return;
        }
        externalDrop = true;
        externalOffset = offset;
        externalSize = size;
        indicator.setOffset(offset);
        indicator.setSize(size);
        indicator.setVisible(true);
    }

    void UiTreeState::hideDropTarget()
    {
        if (externalDrop)
        {
            externalDrop = false;
            indicator.setVisible(false);
        }
    }

    void UiTreeState::endDrag(const Vec2 position)
    {
        updateDrag(position);
        UiTreeComponent& state = writableTree();
        const std::vector<Entity> nodes = std::move(state.dragged);
        const Entity parent = state.dropParent;
        const u32 index = state.dropIndex;
        const bool valid = state.dropValid;
        const bool refused = dropRefused;
        cancelDrag(valid ? parent : Entity::undefined());
        if (valid && !nodes.empty())
        {
            drop(nodes, parent, index);
        }
        else if (refused && !nodes.empty() && onDropRefused)
        {
            onDropRefused();
        }
    }

    void UiTreeState::cancelDrag(const Entity keepOpen)
    {
        UiTreeComponent& state = writableTree();
        state.dragged.clear();
        state.dropParent = Entity::undefined();
        state.dropIndex = 0;
        state.dropValid = false;
        dragSet.clear();
        dragPlaces.clear();
        pressed = Entity::undefined();
        dropRefused = false;
        dragHover = Entity::undefined();
        externalDrop = false;
        indicator.setVisible(false);
        // deepest first, so a node closes with the rows of its opened descendants already gone
        const std::vector<Entity> opened = std::move(dragOpened);
        dragOpened.clear();
        for (auto it = opened.rbegin(); it != opened.rend(); ++it)
        {
            bool keep = false;
            for (Entity at = keepOpen; at != Entity::undefined() && !keep; at = desc.parent(at))
            {
                keep = at == *it;
            }
            if (!keep)
            {
                setExpanded(*it, false);
            }
        }
    }

    void UiTreeState::advance(const double seconds)
    {
        time += seconds;
        if (dragSet.empty())
        {
            return;
        }
        const UiTreeComponent& state = tree();
        const Entity target = state.dropValid && state.dropParent != Entity::undefined() &&
                !isExpanded(state.dropParent) && hasChildren(state.dropParent)
            ? state.dropParent
            : Entity::undefined();
        if (target != dragHover)
        {
            dragHover = target;
            dragHoverSince = time;
        }
        else if (target != Entity::undefined() && time - dragHoverSince >= static_cast<double>(desc.dragOpenSeconds))
        {
            dragHover = Entity::undefined();
            dragOpened.push_back(target);
            setExpanded(target, true);
            updateDrag(dragPosition); // the rows moved under the pointer
        }
    }

    bool UiTreeState::drop(const std::span<const Entity> nodes, const Entity parent, const u32 index)
    {
        std::vector<Entity> moved(nodes.begin(), nodes.end());
        std::ranges::sort(moved);
        for (Entity ancestor = parent; ancestor != Entity::undefined(); ancestor = desc.parent(ancestor))
        {
            if (contains(moved, ancestor))
            {
                return false;
            }
        }
        if (nodes.empty())
        {
            return false;
        }
        UiTreeDropEvent event;
        event.element = list.entity();
        event.nodes.assign(nodes.begin(), nodes.end());
        event.parent = parent;
        event.index = index;
        scope->world().fireEvent(event);
        if (onDrop)
        {
            onDrop(nodes, parent, index);
        }
        return true;
    }

    // -- UiScope ------------------------------------------------------------------

    TreeView UiScope::createTree(const InternedString id, UiTreeDesc desc)
    {
        SASSERTM(desc.children && desc.parent && desc.label && desc.rowHeight > 0.0f,
                 "createTree: needs children, parent and label and a positive row height.");
        auto owned = std::make_unique<UiTreeState>();
        UiTreeState& state = *owned;
        state.scope = this;
        state.desc = std::move(desc);

        UiVirtualListDesc listDesc;
        listDesc.rect = state.desc.rect;
        listDesc.rowHeight = state.desc.rowHeight;
        listDesc.createRow = [&state, id](u32 /*slot*/) { return state.createRow(id); };
        listDesc.bindRow = [&state](const u32 slot, const u32 row) { state.bindRow(slot, row); };
        state.list = createVirtualList(id, std::move(listDesc));
        const Entity viewport = state.list.entity();
        m_world->addComponent<UiTreeComponent>(viewport, UiTreeComponent{});
        m_world->getComponent<UiStateComponent>(viewport).tabStop = true;

        state.indicator = createPanel(childId(id, ".drop"), topLeft(Vec2{}, Vec2{}));
        state.indicator.setParent(state.list.content());
        state.indicator.setInteractable(false);
        state.indicator.setVisible(false);
        state.indicator.setZOffset(1); // over the rows

        // keys bubble from the focused row (a press focuses it) to the viewport;
        // the tree handles only its navigation keys
        setKeyReaction(viewport,
                       [&state](const UiKeyDownEvent& event)
                       {
                           const bool extend = (event.modifiers & UiKeyModifier::SHIFT) != 0;
                           switch (event.scancode)
                           {
                           case Key::UP:
                               state.apply(UiTreeCommand::Up, extend);
                               return true;
                           case Key::DOWN:
                               state.apply(UiTreeCommand::Down, extend);
                               return true;
                           case Key::LEFT:
                               state.apply(UiTreeCommand::Left, extend);
                               return true;
                           case Key::RIGHT:
                               state.apply(UiTreeCommand::Right, extend);
                               return true;
                           case Key::HOME:
                               state.apply(UiTreeCommand::Home, extend);
                               return true;
                           case Key::END:
                               state.apply(UiTreeCommand::End, extend);
                               return true;
                           case Key::ESCAPE:
                               if (state.dragSet.empty())
                               {
                                   return false;
                               }
                               state.cancelDrag();
                               return true;
                           case Key::F2:
                               {
                                   constexpr u8 CHORDS =
                                       UiKeyModifier::CTRL | UiKeyModifier::ALT | UiKeyModifier::SUPER;
                                   const Entity cursor = state.tree().cursor;
                                   if (!state.onRename || !state.dragSet.empty() || (event.modifiers & CHORDS) != 0 ||
                                       state.rowOf(cursor) == UiTreeState::NO_ROW)
                                   {
                                       return false;
                                   }
                                   state.onRename(cursor);
                                   return true;
                               }
                           default:
                               return false;
                           }
                       });
        m_trees.emplace(viewport.id(), std::move(owned));
        state.refresh();
        return TreeView(this, viewport, m_generation);
    }

    UiTreeState* UiScope::treeState(const Entity viewport) const
    {
        const auto it = m_trees.find(viewport.id());
        return it == m_trees.end() ? nullptr : it->second.get();
    }

    // -- TreeView -----------------------------------------------------------------

    UiTreeState* TreeView::runtime() const { return isValid() ? scope()->treeState(entity()) : nullptr; }

    void TreeView::refresh()
    {
        if (UiTreeState* state = runtime())
        {
            state->refresh();
        }
    }

    void TreeView::refreshRows()
    {
        if (UiTreeState* state = runtime())
        {
            state->list.refreshRows();
        }
    }

    void TreeView::setRowClass(const InternedString rowClass)
    {
        UiTreeState* state = runtime();
        if (!state || state->desc.rowClass == rowClass)
        {
            return;
        }
        state->desc.rowClass = rowClass;
        for (UiTreeState::Slot& slot : state->slots)
        {
            state->styleRow(slot);
        }
    }

    InternedString TreeView::rowClass() const
    {
        const UiTreeState* state = runtime();
        return state ? state->desc.rowClass : InternedString{};
    }

    void TreeView::setMetrics(const float rowHeight, const float indent)
    {
        UiTreeState* state = runtime();
        if (!state || !(rowHeight > 0.0f) || !(indent > 0.0f) ||
            (state->desc.rowHeight == rowHeight && state->desc.indent == indent))
        {
            return;
        }
        const bool taller = state->desc.rowHeight != rowHeight;
        state->desc.rowHeight = rowHeight;
        state->desc.indent = indent;
        for (const UiTreeState::Slot& slot : state->slots)
        {
            Label toggle = slot.toggle;
            toggle.setSize(Vec2{indent, rowHeight});
            if (slot.icon.isValid())
            {
                Label icon = slot.icon;
                icon.setSize(Vec2{indent, rowHeight});
            }
        }
        // a new height re-pools and rebinds through the list; the same height with a new indent only rebinds
        state->list.setRowHeight(rowHeight);
        if (!taller)
        {
            state->list.refreshRows();
        }
    }

    void TreeView::setBar(const Vec2 size, const float gap)
    {
        UiTreeState* state = runtime();
        if (!state || !(size.x > 0.0f) || (state->desc.barSize == size && state->desc.barGap == gap))
        {
            return;
        }
        state->desc.barSize = size;
        state->desc.barGap = gap;
        for (const UiTreeState::Slot& slot : state->slots)
        {
            if (slot.bar.isValid())
            {
                Panel bar = slot.bar;
                bar.setSize(size);
            }
        }
        state->list.refreshRows();
    }

    float TreeView::rowHeight() const
    {
        const UiTreeState* state = runtime();
        return state ? state->desc.rowHeight : 0.0f;
    }

    float TreeView::indent() const
    {
        const UiTreeState* state = runtime();
        return state ? state->desc.indent : 0.0f;
    }

    void TreeView::setExpanded(const Entity node, const bool expanded)
    {
        if (UiTreeState* state = runtime())
        {
            state->setExpanded(node, expanded);
        }
    }

    void TreeView::collapseAll()
    {
        if (UiTreeState* state = runtime())
        {
            state->writableTree().expanded.clear();
            state->refresh();
        }
    }

    bool TreeView::isExpanded(const Entity node) const
    {
        const UiTreeState* state = runtime();
        return state && state->isExpanded(node);
    }

    void TreeView::select(const Entity node, const UiTreeSelect mode)
    {
        if (UiTreeState* state = runtime())
        {
            state->select(node, mode);
        }
    }

    void TreeView::apply(const UiTreeCommand command, const bool extend)
    {
        if (UiTreeState* state = runtime())
        {
            state->apply(command, extend);
        }
    }

    void TreeView::setSelection(const std::span<const Entity> nodes)
    {
        if (UiTreeState* state = runtime())
        {
            state->setSelection(nodes);
        }
    }

    std::span<const Entity> TreeView::selection() const
    {
        const UiTreeState* state = runtime();
        return state ? std::span<const Entity>(state->tree().selected) : std::span<const Entity>{};
    }

    Entity TreeView::cursor() const
    {
        const UiTreeState* state = runtime();
        return state ? state->tree().cursor : Entity::undefined();
    }

    bool TreeView::drop(const std::span<const Entity> nodes, const Entity parent, const u32 index)
    {
        UiTreeState* state = runtime();
        return state && state->drop(nodes, parent, index);
    }

    void TreeView::scrollTo(const Entity node)
    {
        if (UiTreeState* state = runtime())
        {
            state->scrollTo(node);
        }
    }

    void TreeView::showDropTarget(const Entity node)
    {
        if (UiTreeState* state = runtime())
        {
            state->showDropTarget(node);
        }
    }

    void TreeView::hideDropTarget()
    {
        if (UiTreeState* state = runtime())
        {
            state->hideDropTarget();
        }
    }

    u32 TreeView::rowCount() const
    {
        const UiTreeState* state = runtime();
        return state ? static_cast<u32>(state->rows.size()) : 0;
    }

    void TreeView::advance(const float seconds)
    {
        if (UiTreeState* state = runtime())
        {
            state->advance(static_cast<double>(seconds));
        }
    }

    bool TreeView::dragging() const
    {
        const UiTreeState* state = runtime();
        return state && !state->dragSet.empty();
    }

    void TreeView::onDropRefused(std::function<void()> fn)
    {
        if (UiTreeState* state = runtime())
        {
            state->onDropRefused = std::move(fn);
        }
    }

    void TreeView::onRename(std::function<void(Entity)> fn)
    {
        if (UiTreeState* state = runtime())
        {
            state->onRename = std::move(fn);
        }
    }

    void TreeView::onSelect(std::function<void(std::span<const Entity>)> fn)
    {
        if (UiTreeState* state = runtime())
        {
            state->onSelect = std::move(fn);
        }
    }

    void TreeView::onDrop(std::function<void(std::span<const Entity>, Entity, u32)> fn)
    {
        if (UiTreeState* state = runtime())
        {
            state->onDrop = std::move(fn);
        }
    }

    void TreeView::onContextMenu(std::function<void(Entity, Vec2)> fn)
    {
        if (UiTreeState* state = runtime())
        {
            state->onContextMenu = std::move(fn);
        }
    }
} // namespace spite
