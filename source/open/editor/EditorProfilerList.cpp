#include "EditorProfilerList.hpp"

#include <algorithm>

#include "ui/UiEvents.hpp"

namespace spite
{
    using namespace panelui;

    namespace
    {
        constexpr u32 RETURN = 40, SPACE = 44, DELETE_KEY = 76, HOME = 74, PAGE_UP = 75, END = 77, PAGE_DOWN = 78,
                      DOWN = 81, UP = 82, KEYPAD_ENTER = 88;
        constexpr std::string_view ROW_CLASS = "list.row";
    } // namespace

    ProfilerCaptureList::ProfilerCaptureList(Host host) : m_host(std::move(host))
    {
        UiScope& scope = *m_host.scope;
        UiVirtualListDesc desc;
        desc.rect = topLeft();
        desc.rowHeight = rowHeight(m_host.density->density());
        desc.createRow = [this](const u32 slot) -> Widget { return createSlot(slot); };
        desc.bindRow = [this](const u32 slot, const u32 row)
        {
            m_slots[slot].row = row;
            bind(slot);
        };
        list = scope.createVirtualList(InternedString("profiler.captures"), std::move(desc));
        scope.world().getComponent<UiStateComponent>(list.entity()).tabStop = true;
        scope.setKeyReaction(list.entity(), [this](const UiKeyDownEvent& event)
                             { return key(event.scancode, event.modifiers, event.repeat); });
        m_host.density->apply(
            [this](const Density& density)
            {
                for (Slot& slot : m_slots)
                {
                    slot.width = -1.0f;
                }
                list.setRowHeight(rowHeight(density));
            });
    }

    Widget ProfilerCaptureList::createSlot(const u32 index)
    {
        UiScope& scope = *m_host.scope;
        Slot& slot = m_slots.emplace_back();
        slot.root = scope.createPanel(InternedString("profiler.captures.row"), topLeft());
        slot.root.setStyleClass(InternedString(ROW_CLASS));
        slot.root.setAccessibleRole(UiRole::Row);
        slot.root.setAccessibleSelected(false);
        slot.highlight = scope.createPanel(InternedString("profiler.captures.row.selected"), topLeft());
        slot.highlight.setStyleClass(InternedString("list.row.selected"));
        slot.highlight.setParent(slot.root);
        slot.highlight.setInteractable(false);
        slot.highlight.setVisible(false);
        for (u32 line = 0; line < 2; ++line)
        {
            slot.clips[line] = scope.createPanel(InternedString("profiler.captures.row.cell"), topLeft());
            slot.clips[line].setParent(slot.root);
            slot.clips[line].setInteractable(false);
            scope.world().getComponent<UiStateComponent>(slot.clips[line].entity()).clipsChildren = true;
            slot.lines[line] = scope.createPanel(InternedString("profiler.captures.row.text"), topLeft());
            scope.world().addComponent<UiValueComponent>(slot.lines[line].entity(), UiValueComponent{});
            slot.lines[line].setInteractable(false);
            slot.lines[line].setParent(slot.clips[line]);
            slot.lines[line].setStyleClass(InternedString(line == 0 ? "list.row.text" : "list.row.text.host"));
        }
        scope.setReaction(slot.root.entity(), UiEventKind::Click,
                          [this, index](const void* payload)
                          {
                              const auto* event = static_cast<const UiClickEvent*>(payload);
                              const u32 row = m_slots[index].row;
                              if (event->button == 0 && row < list.rowCount())
                              {
                                  setCursor(static_cast<i32>(row), false);
                                  m_host.activate(row);
                              }
                          });
        return slot.root;
    }

    void ProfilerCaptureList::layout(Slot& slot)
    {
        const Density& density = m_host.density->density();
        const float width = std::max(0.0f, list.size().x - THUMB);
        if (slot.width == width)
        {
            return;
        }
        slot.width = width;
        Widget highlight = slot.highlight;
        place(highlight, Vec2{0.0f, 0.0f}, Vec2{width, rowHeight(density)});
        const float inner = std::max(0.0f, width - 2.0f * density.xs);
        for (u32 line = 0; line < 2; ++line)
        {
            Widget clip = slot.clips[line];
            Widget text = slot.lines[line];
            place(clip, Vec2{density.xs, static_cast<float>(line) * density.row}, Vec2{inner, density.row});
            place(text, Vec2{0.0f, 0.0f}, Vec2{inner, density.row});
        }
    }

    void ProfilerCaptureList::bind(const u32 index)
    {
        Slot& slot = m_slots[index];
        if (slot.row == NO_ROW || slot.row >= list.rowCount())
        {
            return;
        }
        ++binds;
        layout(slot);
        m_scratch = {};
        m_host.read(slot.row, m_scratch);
        bool changed = false;
        const std::string* now[2] = {&m_scratch.title, &m_scratch.detail};
        for (u32 line = 0; line < 2; ++line)
        {
            if (*now[line] != slot.shown[line])
            {
                changed = true;
                slot.shown[line].assign(*now[line]);
                m_host.scope->world().getComponent<UiValueComponent>(slot.lines[line].entity()).text = slot.shown[line];
                ++writes;
            }
        }
        if (changed)
        {
            slot.root.setAccessibleName(slot.shown[0] + ", " + slot.shown[1]);
        }
        if (m_scratch.warn != slot.warn)
        {
            slot.warn = m_scratch.warn;
            slot.lines[1].setStyleClass(InternedString(slot.warn ? "list.row.text.warn" : "list.row.text.host"));
        }
        if (slot.highlight.isVisible() != (static_cast<i32>(slot.row) == m_cursor))
        {
            slot.highlight.setVisible(static_cast<i32>(slot.row) == m_cursor);
            slot.root.setAccessibleSelected(static_cast<i32>(slot.row) == m_cursor);
        }
    }

    void ProfilerCaptureList::setRowCount(const u32 count)
    {
        if (list.rowCount() != count)
        {
            list.setRowCount(count);
        }
        if (m_cursor >= static_cast<i32>(count))
        {
            setCursor(static_cast<i32>(count) - 1, false);
        }
    }

    void ProfilerCaptureList::rebindVisible()
    {
        for (u32 slot = 0; slot < m_slots.size(); ++slot)
        {
            bind(slot);
        }
    }

    void ProfilerCaptureList::setCursor(i32 row, const bool reveal)
    {
        row = std::clamp(row, -1, static_cast<i32>(list.rowCount()) - 1);
        if (reveal && row >= 0)
        {
            const float height = list.rowHeight();
            const float top = static_cast<float>(row) * height;
            const float offset = list.scrollOffset().y;
            if (top < offset)
            {
                list.setScrollOffset(Vec2{0.0f, top});
            }
            else if (top + height > offset + list.size().y)
            {
                list.setScrollOffset(Vec2{0.0f, top + height - list.size().y});
            }
        }
        if (row == m_cursor)
        {
            return;
        }
        m_cursor = row;
        rebindVisible();
        if (m_host.cursorMoved)
        {
            m_host.cursorMoved();
        }
    }

    std::vector<std::vector<std::string>> ProfilerCaptureList::visibleLines() const
    {
        std::vector<const Slot*> slots;
        for (const Slot& slot : m_slots)
        {
            if (slot.row != NO_ROW && slot.row < list.rowCount())
            {
                slots.push_back(&slot);
            }
        }
        std::ranges::sort(slots, {}, &Slot::row);
        std::vector<std::vector<std::string>> lines;
        for (const Slot* slot : slots)
        {
            lines.push_back({slot->shown[0], slot->shown[1]});
        }
        return lines;
    }

    bool ProfilerCaptureList::key(const u32 scancode, const u8 modifiers, const bool repeat)
    {
        const i32 count = static_cast<i32>(list.rowCount());
        const i32 page = std::max(1, static_cast<i32>(list.size().y / list.rowHeight()));
        const auto go = [&](const i32 to)
        {
            if (count > 0)
            {
                setCursor(std::clamp(to, 0, count - 1));
            }
            return true;
        };
        if (modifiers != 0)
        {
            return false;
        }
        switch (scancode)
        {
        case UP:
            return go(m_cursor < 0 ? count - 1 : m_cursor - 1);
        case DOWN:
            return go(m_cursor + 1);
        case HOME:
            return go(0);
        case END:
            return go(count - 1);
        case PAGE_UP:
            return go(m_cursor < 0 ? 0 : m_cursor - page);
        case PAGE_DOWN:
            return go(m_cursor + page);
        case RETURN:
        case KEYPAD_ENTER:
        case SPACE:
            if (!repeat && m_cursor >= 0)
            {
                m_host.activate(static_cast<u32>(m_cursor));
            }
            return m_cursor >= 0;
        case DELETE_KEY:
            if (!repeat && m_cursor >= 0)
            {
                m_host.remove();
            }
            return m_cursor >= 0;
        default:
            return false;
        }
    }
} // namespace spite
