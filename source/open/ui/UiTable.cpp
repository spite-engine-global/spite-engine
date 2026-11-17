#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>
#include <string>

#include "base/Assert.hpp"
#include "ecs/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiEvents.hpp"
#include "ui/UiScope.hpp"
#include "UiTableState.hpp"

namespace spite
{
    namespace
    {
        constexpr float GRIP_WIDTH = 6.0f;
        constexpr float MARK_WIDTH = 12.0f; // a sortable column's sort indicator
        constexpr std::string_view HEADER_CURSOR_CLASS = "table.header.cursor"; // the header cell the keys are on

        // HID scancodes (the UiKeyTransition domain).
        namespace Key
        {
            constexpr u32 RETURN = 40;
            constexpr u32 SPACE = 44;
            constexpr u32 RIGHT = 79;
            constexpr u32 LEFT = 80;
            constexpr u32 PAGE_UP = 75;
            constexpr u32 HOME = 74;
            constexpr u32 END = 77;
            constexpr u32 PAGE_DOWN = 78;
            constexpr u32 DOWN = 81;
            constexpr u32 UP = 82;
            constexpr u32 KEYPAD_ENTER = 88;
        } // namespace Key

        InternedString childId(const InternedString id, const std::string_view suffix)
        {
            return InternedString(std::string(id.toView()) + std::string(suffix));
        }

        UiRectDesc topLeft(const Vec2 offset = {}, const Vec2 size = {})
        {
            UiRectDesc desc;
            desc.offset = offset;
            desc.size = size;
            desc.pivot = Vec2{0.0f, 0.0f};
            return desc;
        }

        // Writes only what changed: an unchanged rect marks nothing dirty.
        void place(Widget widget, const Vec2 offset, const Vec2 size)
        {
            if (widget.offset() != offset)
            {
                widget.setOffset(offset);
            }
            if (widget.size() != size)
            {
                widget.setSize(size);
            }
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

        bool contains(const std::vector<Entity>& sorted, const Entity entity)
        {
            return std::ranges::binary_search(sorted, entity);
        }

        template <class T>
        const T& read(const std::byte* p)
        {
            return *reinterpret_cast<const T*>(p);
        }

        u32 floatLanes(const FieldKind kind)
        {
            switch (kind)
            {
            case FieldKind::Vec2:
                return 2;
            case FieldKind::Vec3:
                return 3;
            case FieldKind::Vec4:
            case FieldKind::Quat:
                return 4;
            case FieldKind::Mat4:
                return 16;
            default:
                return 0;
            }
        }

        void appendValue(std::string& out, const FieldKind kind, const StructSchema* nested, const std::byte* p);

        void appendStruct(std::string& out, const StructSchema& schema, const std::byte* base)
        {
            for (const FieldDesc& field : schema.fields)
            {
                out += &field == schema.fields.data() ? "" : " ";
                appendValue(out, field.kind, field.structSchema, base + field.offset);
            }
        }

        // A field value as cell text: numbers in their shortest form, vectors
        // and structs space-separated, entities by id.
        void appendValue(std::string& out, const FieldKind kind, const StructSchema* nested, const std::byte* p)
        {
            const auto put = [&out](const auto value) { std::format_to(std::back_inserter(out), "{}", value); };
            switch (kind)
            {
            case FieldKind::Bool:
                out += read<bool>(p) ? "true" : "false";
                break;
            case FieldKind::U8:
                put(static_cast<u32>(read<u8>(p)));
                break;
            case FieldKind::U16:
                put(read<u16>(p));
                break;
            case FieldKind::U32:
                put(read<u32>(p));
                break;
            case FieldKind::U64:
                put(read<u64>(p));
                break;
            case FieldKind::I32:
                put(read<i32>(p));
                break;
            case FieldKind::F32:
                put(read<f32>(p));
                break;
            case FieldKind::F64:
                put(read<double>(p));
                break;
            case FieldKind::Vec2:
            case FieldKind::Vec3:
            case FieldKind::Vec4:
            case FieldKind::Quat:
            case FieldKind::Mat4:
                for (u32 lane = 0; lane < floatLanes(kind); ++lane)
                {
                    out += lane ? " " : "";
                    put(reinterpret_cast<const f32*>(p)[lane]);
                }
                break;
            case FieldKind::String:
                out += read<std::string>(p);
                break;
            case FieldKind::InternedString:
                out += reinterpret_cast<const InternedString*>(p)->toView();
                break;
            case FieldKind::Entity:
                put(reinterpret_cast<const Entity*>(p)->id());
                break;
            case FieldKind::EntityList:
                {
                    out += "[";
                    const auto& list = *reinterpret_cast<const std::vector<Entity>*>(p);
                    for (sizet i = 0; i < list.size(); ++i)
                    {
                        out += i ? " " : "";
                        put(list[i].id());
                    }
                    out += "]";
                    break;
                }
            case FieldKind::Struct:
                appendStruct(out, *nested, p);
                break;
            }
        }

        void formatField(const FieldDesc& field, const void* row, std::string& text)
        {
            text.clear();
            const auto* base = static_cast<const std::byte*>(row) + field.offset;
            for (u32 i = 0; i < std::max(field.arrayCount, 1u); ++i)
            {
                text += i ? " " : "";
                appendValue(text, field.kind, field.structSchema, base + i * field.elementSize);
            }
        }

        bool isSortable(const FieldDesc& field)
        {
            return field.arrayCount == 0 && field.kind != FieldKind::Struct && field.kind != FieldKind::EntityList &&
                floatLanes(field.kind) == 0;
        }

        bool isText(const FieldKind kind) { return kind == FieldKind::String || kind == FieldKind::InternedString; }

        // A row's sort key, read once per sort: text sorts in natural order (textLess).
        struct SortKey
        {
            double number = 0.0;
            std::string_view text;
            u32 row = 0;
        };

        double numberOf(const FieldKind kind, const std::byte* p)
        {
            switch (kind)
            {
            case FieldKind::Bool:
                return read<bool>(p) ? 1.0 : 0.0;
            case FieldKind::U8:
                return read<u8>(p);
            case FieldKind::U16:
                return read<u16>(p);
            case FieldKind::U32:
                return read<u32>(p);
            case FieldKind::U64:
                return static_cast<double>(read<u64>(p));
            case FieldKind::I32:
                return read<i32>(p);
            case FieldKind::F32:
                return read<f32>(p);
            case FieldKind::F64:
                return read<double>(p);
            case FieldKind::Entity:
                return static_cast<double>(reinterpret_cast<const Entity*>(p)->id());
            default:
                return 0.0;
            }
        }

        bool isDigit(const char c) { return c >= '0' && c <= '9'; }

        // Natural order: ASCII case ignored, a run of digits compares as a number ("2" before "10").
        bool textLess(const std::string_view a, const std::string_view b)
        {
            const auto lower = [](const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
            const auto number = [](const std::string_view text, sizet& at)
            {
                sizet end = at;
                while (end < text.size() && isDigit(text[end]))
                {
                    ++end;
                }
                sizet first = at;
                while (first + 1 < end && text[first] == '0')
                {
                    ++first;
                }
                const std::string_view digits = text.substr(first, end - first);
                at = end;
                return digits;
            };
            sizet i = 0;
            sizet j = 0;
            while (i < a.size() && j < b.size())
            {
                if (isDigit(a[i]) && isDigit(b[j]))
                {
                    const std::string_view x = number(a, i);
                    const std::string_view y = number(b, j);
                    if (x.size() != y.size())
                    {
                        return x.size() < y.size();
                    }
                    if (const int order = x.compare(y); order != 0)
                    {
                        return order < 0;
                    }
                    continue;
                }
                if (lower(a[i]) != lower(b[j]))
                {
                    return lower(a[i]) < lower(b[j]);
                }
                ++i;
                ++j;
            }
            return a.size() - i < b.size() - j;
        }
    } // namespace

    // -- UiTableState --------------------------------------------------------------

    UiTableComponent& UiTableState::table() const
    {
        return scope->world().getComponent<UiTableComponent>(root.entity());
    }

    const UiTableComponent& UiTableState::tableRead() const
    {
        return scope->world().getConstComponent<UiTableComponent>(root.entity());
    }

    float UiTableState::widthOf(const u32 column) const
    {
        if (!columns[column].visible)
            return 0.0f;
        return scope->world().getConstComponent<UiTableColumnComponent>(columns[column].cell.entity()).width;
    }

    float UiTableState::totalWidth() const
    {
        float total = 0.0f;
        for (u32 column = 0; column < columns.size(); ++column)
        {
            total += widthOf(column);
        }
        return total;
    }

    u32 UiTableState::columnOf(const InternedString field) const
    {
        for (u32 column = 0; column < columns.size(); ++column)
        {
            if (InternedString(columns[column].field->name) == field)
            {
                return column;
            }
        }
        return NO_ROW;
    }

    u32 UiTableState::viewRowOf(const Entity key, const u32 hint) const
    {
        if (key == Entity::undefined())
        {
            return NO_ROW;
        }
        if (hint < rowCount && keyAt(hint) == key)
        {
            return hint;
        }
        for (u32 view = 0; view < rowCount; ++view)
        {
            if (keyAt(view) == key)
            {
                return view;
            }
        }
        return NO_ROW;
    }

    Widget UiTableState::createRow(const InternedString id)
    {
        UiScope& owner = *scope;
        World& world = owner.world();
        Slot slot;
        slot.root = owner.createPanel(childId(id, ".row"));
        world.addComponent<UiTableRowComponent>(slot.root.entity(), UiTableRowComponent{});
        slot.highlight = owner.createPanel(childId(id, ".row.selected"), topLeft());
        slot.highlight.setParent(slot.root);
        slot.highlight.setInteractable(false);
        slot.highlight.setVisible(false);
        for (sizet column = 0; column < columns.size(); ++column)
        {
            Panel cell = owner.createPanel(childId(id, ".cell"), topLeft());
            Panel text = owner.createPanel(childId(id, ".cell.text"), topLeft());
            world.getComponent<UiStateComponent>(cell.entity()).clipsChildren = true;
            world.addComponent<UiValueComponent>(text.entity(), UiValueComponent{});
            cell.setAccessibleRole(UiRole::GridCell);
            // the row root takes every hit: parts never shadow it
            cell.setParent(slot.root);
            cell.setInteractable(false);
            text.setParent(cell);
            text.setInteractable(false);
            slot.cells.push_back(cell);
            slot.texts.push_back(text);
        }

        if (desc.decoration && !slot.cells.empty())
        {
            slot.image = owner.createImage(childId(id, ".cell.image"), topLeft());
            slot.icon = owner.createLabel(childId(id, ".cell.icon"), "", topLeft());
            slot.image.setParent(slot.cells[0]);
            slot.icon.setParent(slot.cells[0]);
            slot.image.setInteractable(false);
            slot.icon.setInteractable(false);
        }

        styleRow(slot);
        const u32 index = static_cast<u32>(slots.size());
        owner.setReaction(slot.root.entity(), UiEventKind::PointerDown,
                          [this, index](const void* payload)
                          {
                              const auto* event = static_cast<const UiPointerDownEvent*>(payload);
                              if (event->button == 1 && slots[index].view != NO_ROW && onContextMenu)
                              {
                                  onContextMenu(slots[index].key, event->position);
                              }
                          });
        owner.setReaction(slot.root.entity(), UiEventKind::Click,
                          [this, index](const void* payload)
                          {
                              const auto* event = static_cast<const UiClickEvent*>(payload);
                              if (event->button == 0 && slots[index].view != NO_ROW)
                              {
                                  // the first click selects; a click on an activating cell of the selected row
                                  // activates
                                  const UiTableComponent& state = tableRead();
                                  const Entity key = slots[index].key;
                                  const bool wasChosen = state.cursor == key && contains(state.selected, key);
                                  select(slots[index].view, selectMode(event->modifiers));
                                  if (onDoubleClick)
                                  {
                                      const auto now = std::chrono::steady_clock::now();
                                      const bool twice = event->modifiers == 0 && lastClick == key &&
                                          now - lastClickTime <= std::chrono::milliseconds(400);
                                      lastClick = event->modifiers == 0 && !twice ? key : Entity::undefined();
                                      lastClickTime = now;
                                      if (twice)
                                          onDoubleClick(key);
                                  }
                                  const UiHierarchyManager& hierarchy = scope->hierarchy();
                                  const float x = (event->position.x -
                                                   hierarchy.absoluteRectOf(slots[index].root.entity()).position.x) /
                                      hierarchy.layoutScale();
                                  if (const u32 column = columnAt(x); wasChosen && event->modifiers == 0 &&
                                      column != NO_ROW && columns[column].activates)
                                  {
                                      activate(InternedString(columns[column].field->name));
                                  }
                              }
                          });
        owner.setReaction(slot.root.entity(), UiEventKind::DragStart,
                          [this, index](const void* payload)
                          {
                              const Slot& dragged = slots[index];
                              if (!onDragRows || dragged.view == NO_ROW)
                              {
                                  return;
                              }
                              const std::vector<Entity>& selected = tableRead().selected;
                              dragKeys = contains(selected, dragged.key) ? selected : std::vector<Entity>{dragged.key};
                              onDragRows(dragKeys, static_cast<const UiDragStartEvent*>(payload)->position, false);
                          });
        owner.setReaction(slot.root.entity(), UiEventKind::DragMove,
                          [this](const void* payload)
                          {
                              if (onDragRows && !dragKeys.empty())
                              {
                                  onDragRows(dragKeys, static_cast<const UiDragMoveEvent*>(payload)->position, false);
                              }
                          });
        owner.setReaction(slot.root.entity(), UiEventKind::DragEnd,
                          [this](const void* payload)
                          {
                              const std::vector<Entity> keys = std::move(dragKeys);
                              dragKeys.clear();
                              if (onDragRows && !keys.empty())
                              {
                                  onDragRows(keys, static_cast<const UiDragEndEvent*>(payload)->position, true);
                              }
                          });
        slots.push_back(std::move(slot));
        return slots.back().root;
    }

    void UiTableState::styleRow(Slot& slot) const
    {
        const auto partClass = [this](const std::string_view part)
        { return desc.rowClass.isValid() ? childId(desc.rowClass, part) : InternedString{}; };
        slot.root.setStyleClass(desc.rowClass);
        slot.highlight.setStyleClass(partClass(".selected"));
        if (desc.decoration && !slot.cells.empty())
        {
            slot.image.setStyleClass(partClass(".image"));
            slot.icon.setStyleClass(partClass(".icon"));
        }
        for (sizet column = 0; column < slot.cells.size(); ++column)
        {
            slot.cells[column].setStyleClass(partClass(".cell"));
            slot.texts[column].setStyleClass(partClass(".text"));
        }
    }

    void UiTableState::styleHeader() const
    {
        const auto partClass = [this](const std::string_view part)
        { return desc.rowClass.isValid() ? childId(desc.rowClass, part) : InternedString{}; };
        Widget bar = header;
        bar.setStyleClass(partClass(".header"));
        for (const Column& column : columns)
        {
            Widget cell = column.cell;
            Widget title = column.text;
            Widget mark = column.mark;
            cell.setStyleClass(column.sortable && scope->router().focused() == cell.entity()
                                   ? InternedString(HEADER_CURSOR_CLASS)
                                   : partClass(".header.cell"));
            title.setStyleClass(partClass(".header.text"));
            mark.setStyleClass(partClass(".header.mark"));
        }
    }

    u32 UiTableState::columnAt(const float x) const
    {
        float left = 0.0f;
        for (u32 column = 0; column < columns.size(); ++column)
        {
            left += widthOf(column);
            if (x < left)
            {
                return column;
            }
        }
        return NO_ROW;
    }

    void UiTableState::bindRow(const u32 slotIndex, const u32 view)
    {
        Slot& slot = slots[slotIndex];
        World& world = scope->world();
        ++binds;
        const u32 model = modelRow(view);
        const void* data = desc.row(model);
        slot.view = view;
        slot.key = desc.key(model);
        // const reads first: a mutable access marks the component written, and a refresh rebinds every visible row
        if (const auto& now = world.getConstComponent<UiTableRowComponent>(slot.root.entity());
            now.key != slot.key || now.row != model)
        {
            auto& rowComponent = world.getComponent<UiTableRowComponent>(slot.root.entity());
            rowComponent.key = slot.key;
            rowComponent.row = model;
        }

        const float height = desc.rowHeight;
        float x = 0.0f;
        for (u32 column = 0; column < columns.size(); ++column)
        {
            const float width = widthOf(column);
            slot.cells[column].setVisible(columns[column].visible);
            place(slot.cells[column], Vec2{x, 0.0f}, Vec2{width, height});
            const float decorationSpace = column == 0 && desc.decoration
                ? std::max(0.0f, desc.decorationSize > 0.0f ? desc.decorationSize : height - 2.0f * desc.padding) +
                    desc.padding
                : 0.0f;
            place(slot.texts[column], Vec2{desc.padding + decorationSpace, 0.0f},
                  Vec2{std::max(0.0f, width - 2.0f * desc.padding - decorationSpace), height});
            formatField(*columns[column].field, data, cellText);
            if (world.getConstComponent<UiValueComponent>(slot.texts[column].entity()).text != cellText)
            {
                world.getComponent<UiValueComponent>(slot.texts[column].entity()).text = cellText;
                scope->markLayoutDirty(slot.texts[column].entity());
                ++writes;
            }
            if (columns[column].classField && desc.rowClass.isValid())
            {
                formatField(*columns[column].classField, data, cellText);
                slot.texts[column].setStyleClass(
                    childId(desc.rowClass, cellText.empty() ? ".text" : ".text." + cellText));
            }
            x += width;
        }
        if (desc.decoration && !slot.cells.empty())
        {
            bindDecoration(slot, model);
        }
        if (desc.expand)
        {
            slot.root.setAccessibleExpanded(desc.expand(model));
        }
        place(slot.highlight, Vec2{}, slot.root.size());
        slot.highlight.setVisible(contains(tableRead().selected, slot.key));
    }

    void UiTableState::bindDecoration(Slot& slot, const u32 model)
    {
        const UiTableDecoration decoration = desc.decoration(model);
        const float size =
            std::max(0.0f, desc.decorationSize > 0.0f ? desc.decorationSize : desc.rowHeight - 2.0f * desc.padding);
        const float aspect = std::isfinite(decoration.aspect) && decoration.aspect > 0.0f ? decoration.aspect : 1.0f;
        const Vec2 imageSize{size * std::min(1.0f, aspect), size * std::min(1.0f, 1.0f / aspect)};
        const Vec2 origin{desc.padding, (desc.rowHeight - size) * 0.5f};
        place(slot.image, Vec2{origin.x + (size - imageSize.x) * 0.5f, origin.y + (size - imageSize.y) * 0.5f},
              imageSize);
        place(slot.icon, origin, Vec2{size, size});
        if (slot.image.imageSlot() != decoration.imageSlot)
        {
            slot.image.setImageSlot(decoration.imageSlot);
        }
        if (slot.icon.icon() != decoration.icon)
        {
            slot.icon.setIcon(decoration.icon);
        }
        slot.image.setVisible(decoration.imageSlot != 0);
        slot.icon.setVisible(decoration.imageSlot == 0 && !decoration.icon.empty());
    }

    void UiTableState::layout()
    {
        const Vec2 size = root.size();
        place(list, Vec2{0.0f, desc.headerHeight}, Vec2{size.x, std::max(0.0f, size.y - desc.headerHeight)});
        placeHeaderBand();
    }

    // The header band is as wide as a row: the columns' sum, or the view where the columns do not fill it.
    void UiTableState::placeHeaderBand()
    {
        place(header, header.offset(), Vec2{std::max(totalWidth(), root.size().x), desc.headerHeight});
    }

    void UiTableState::layoutColumns()
    {
        float x = 0.0f;
        const float height = desc.headerHeight;
        for (u32 column = 0; column < columns.size(); ++column)
        {
            const Column& shown = columns[column];
            const float width = widthOf(column);
            Panel(shown.cell).setVisible(shown.visible);
            Panel(shown.grip).setVisible(shown.visible);
            place(shown.cell, Vec2{x, 0.0f}, Vec2{width, height});
            const float markWidth = shown.sortable ? MARK_WIDTH : 0.0f;
            place(shown.text, Vec2{desc.padding, 0.0f},
                  Vec2{std::max(0.0f, width - 2.0f * desc.padding - markWidth), height});
            place(shown.mark, Vec2{std::max(0.0f, width - desc.padding - markWidth), 0.0f}, Vec2{markWidth, height});
            place(shown.grip, Vec2{x + width - GRIP_WIDTH * 0.5f, 0.0f}, Vec2{GRIP_WIDTH, height});
            x += width;
        }
        placeHeaderBand();
        list.setContentWidth(x); // rebinds the visible rows, which place their cells
    }

    void UiTableState::updateHeaderMarks()
    {
        const UiTableComponent& state = tableRead();
        for (const Column& column : columns)
        {
            const bool sorted = state.sortColumn == InternedString(column.field->name);
            const std::string_view icon = sorted ? (state.descending ? "caret-down" : "caret-up") : "";
            if (column.mark.icon() != icon)
            {
                Label(column.mark).setIcon(icon);
            }
        }
    }

    void UiTableState::requestSortBy(const u32 column)
    {
        const InternedString name(columns[column].field->name);
        const UiTableComponent& state = tableRead();
        request(UiTableOp{
            .kind = UiTableOpKind::Sort, .column = name, .descending = state.sortColumn == name && !state.descending});
    }

    // The sortable column beside `from` in a direction (the first/last when `from` is NO_ROW): NO_ROW past the ends.
    u32 UiTableState::sortableBeside(const u32 from, const bool forward) const
    {
        const auto count = static_cast<std::ptrdiff_t>(columns.size());
        std::ptrdiff_t at = from == NO_ROW ? (forward ? -1 : count) : static_cast<std::ptrdiff_t>(from);
        for (at += forward ? 1 : -1; at >= 0 && at < count; at += forward ? 1 : -1)
        {
            if (columns[static_cast<sizet>(at)].sortable && columns[static_cast<sizet>(at)].visible)
            {
                return static_cast<u32>(at);
            }
        }
        return NO_ROW;
    }

    // The header cell holding the focus, NO_ROW when the keys are not on the header.
    u32 UiTableState::focusedColumn() const
    {
        const Entity focus = scope->router().focused();
        for (u32 column = 0; column < columns.size(); ++column)
        {
            if (columns[column].cell.entity() == focus)
            {
                return column;
            }
        }
        return NO_ROW;
    }

    bool UiTableState::focusHeader()
    {
        const u32 column = headerColumn != NO_ROW && columns[headerColumn].sortable && columns[headerColumn].visible
            ? headerColumn
            : sortableBeside(NO_ROW, true);
        if (column == NO_ROW)
        {
            return false;
        }
        headerColumn = column;
        scope->router().requestFocus(columns[column].cell.entity());
        return true;
    }

    // Keys on the header cursor: Left and Right move it over the sortable columns (past an end they are left to
    // directional navigation, which the pad shares), Enter and Space sort, Down returns to the rows.
    bool UiTableState::headerKey(const UiKeyDownEvent& event)
    {
        const u32 column = focusedColumn();
        if (column == NO_ROW || (event.modifiers & (UiKeyModifier::CTRL | UiKeyModifier::ALT)) != 0)
        {
            return false;
        }
        switch (event.scancode)
        {
        case Key::LEFT:
        case Key::RIGHT:
            if (const u32 beside = sortableBeside(column, event.scancode == Key::RIGHT); beside != NO_ROW)
            {
                headerColumn = beside;
                scope->router().requestFocus(columns[beside].cell.entity());
                return true;
            }
            return false;
        case Key::RETURN:
        case Key::KEYPAD_ENTER:
        case Key::SPACE:
            if (!event.repeat && columns[column].sortable)
            {
                requestSortBy(column);
            }
            return true;
        case Key::DOWN:
            scope->router().requestFocus(list.isVisible() ? list.entity() : root.entity());
            return true;
        default:
            return false;
        }
    }

    void UiTableState::sort()
    {
        const UiTableComponent& state = tableRead();
        const u32 column = columnOf(state.sortColumn);
        cursorRow = NO_ROW;
        anchorRow = NO_ROW;
        if (column == NO_ROW)
        {
            order.clear();
            return;
        }
        const FieldDesc& field = *columns[column].field;
        std::vector<SortKey> keys(rowCount);
        for (u32 row = 0; row < rowCount; ++row)
        {
            const auto* p = static_cast<const std::byte*>(desc.row(row)) + field.offset;
            keys[row].row = row;
            if (field.kind == FieldKind::String)
            {
                keys[row].text = read<std::string>(p);
            }
            else if (field.kind == FieldKind::InternedString)
            {
                keys[row].text = reinterpret_cast<const InternedString*>(p)->toView();
            }
            else
            {
                keys[row].number = numberOf(field.kind, p);
            }
        }
        const bool text = isText(field.kind);
        const auto less = [text](const SortKey& a, const SortKey& b)
        { return text ? textLess(a.text, b.text) : a.number < b.number; };
        // stable: ties keep model order in both directions
        if (state.descending)
        {
            std::ranges::stable_sort(keys, [&less](const SortKey& a, const SortKey& b) { return less(b, a); });
        }
        else
        {
            std::ranges::stable_sort(keys, less);
        }
        order.resize(rowCount);
        for (u32 view = 0; view < rowCount; ++view)
        {
            order[view] = keys[view].row;
        }
    }

    void UiTableState::refresh(const u32 count)
    {
        rowCount = count;
        sort();
        list.setRowCount(count); // rebinds the visible rows
        if (const bool hasRows = count != 0; list.isVisible() != hasRows)
        {
            // without rows the table is its header: the list has no height to show, so the table itself is the tab
            // stop (and the grid) that keeps its panel a landmark the F6 cycle enters
            list.setVisible(hasRows);
            scope->world().getComponent<UiStateComponent>(root.entity()).tabStop = !hasRows;
            UiRouter& router = scope->router();
            if (hasRows && router.focused() == root.entity())
            {
                router.requestFocus(list.entity());
            }
        }
    }

    bool UiTableState::apply(const UiTableOp& op)
    {
        const u32 column = columnOf(op.column);
        UiTableComponent& state = table();
        switch (op.kind)
        {
        case UiTableOpKind::Sort:
            if (op.column.isValid() && (column == NO_ROW || !columns[column].sortable))
            {
                return false;
            }
            if (state.sortColumn == op.column && (state.descending == op.descending || !op.column.isValid()))
            {
                return true; // already holds
            }
            state.sortColumn = op.column;
            state.descending = op.column.isValid() && op.descending;
            sort();
            updateHeaderMarks();
            list.refreshRows();
            return true;
        case UiTableOpKind::Resize:
            {
                if (column == NO_ROW || !std::isfinite(op.width))
                {
                    return false;
                }
                const float width = std::max(desc.minColumnWidth, op.width);
                float& current =
                    scope->world().getComponent<UiTableColumnComponent>(columns[column].cell.entity()).width;
                if (current != width)
                {
                    current = width;
                    layoutColumns();
                }
                return true;
            }
        }
        return false;
    }

    void UiTableState::request(const UiTableOp& op)
    {
        if (onRequest)
        {
            onRequest(op);
            return;
        }
        apply(op);
    }

    void UiTableState::select(const u32 view, const UiTreeSelect mode)
    {
        UiTableComponent& state = table();
        const Entity key = keyAt(view);
        const bool ranged = mode == UiTreeSelect::Range || mode == UiTreeSelect::AddRange;
        const u32 from = ranged ? viewRowOf(state.anchor, anchorRow) : NO_ROW;
        if (from != NO_ROW)
        {
            std::vector<Entity> range;
            for (u32 row = std::min(from, view); row <= std::max(from, view); ++row)
            {
                range.push_back(keyAt(row));
            }
            std::ranges::sort(range);
            if (mode == UiTreeSelect::AddRange)
            {
                std::vector<Entity> merged;
                std::ranges::set_union(state.selected, range, std::back_inserter(merged));
                range = std::move(merged);
            }
            state.selected = std::move(range);
            anchorRow = from;
        }
        else if (mode == UiTreeSelect::Replace)
        {
            state.selected = {key};
            state.anchor = key;
            anchorRow = view;
        }
        else
        {
            // Toggle, or AddRange without a visible anchor: adds just the key
            const auto it = std::ranges::lower_bound(state.selected, key);
            if (it == state.selected.end() || *it != key)
            {
                state.selected.insert(it, key);
            }
            else if (mode == UiTreeSelect::Toggle)
            {
                state.selected.erase(it);
            }
            state.anchor = key;
            anchorRow = view;
        }
        state.cursor = key;
        cursorRow = view;
        const std::vector<Entity> selection = state.selected; // the callback may set it
        repaintSelection();
        if (onSelect)
        {
            onSelect(selection);
        }
    }

    void UiTableState::setSelection(const std::span<const Entity> keys)
    {
        UiTableComponent& state = table();
        state.selected.assign(keys.begin(), keys.end());
        std::ranges::sort(state.selected);
        state.selected.erase(std::ranges::unique(state.selected).begin(), state.selected.end());
        state.cursor = keys.empty() ? Entity::undefined() : keys.back();
        state.anchor = state.cursor;
        cursorRow = NO_ROW;
        anchorRow = NO_ROW;
        repaintSelection();
    }

    void UiTableState::repaintSelection()
    {
        const std::vector<Entity>& selected = tableRead().selected;
        for (Slot& slot : slots)
        {
            const bool shown = slot.view != NO_ROW && contains(selected, slot.key);
            if (slot.highlight.isVisible() != shown)
            {
                slot.highlight.setVisible(shown);
            }
        }
    }

    void UiTableState::navigate(const UiTreeCommand command, const bool extend)
    {
        if (rowCount == 0)
        {
            return;
        }
        const u32 last = rowCount - 1;
        const u32 row = viewRowOf(tableRead().cursor, cursorRow);
        u32 target = command == UiTreeCommand::End ? last : 0;
        if (row != NO_ROW)
        {
            switch (command)
            {
            case UiTreeCommand::Up:
                target = row > 0 ? row - 1 : 0;
                break;
            case UiTreeCommand::Down:
                target = std::min(row + 1, last);
                break;
            case UiTreeCommand::Left:
            case UiTreeCommand::Right:
                return;
            case UiTreeCommand::Home:
            case UiTreeCommand::End:
                break;
            }
        }
        moveCursorTo(target, extend);
    }

    void UiTableState::moveCursorTo(const u32 target, const bool extend)
    {
        select(target, extend ? UiTreeSelect::Range : UiTreeSelect::Replace);
        scrollTo(target);
    }

    void UiTableState::page(const bool down, const bool extend)
    {
        if (rowCount == 0)
        {
            return;
        }
        const u32 last = rowCount - 1;
        const u32 visible = static_cast<u32>(std::max(0.0f, list.size().y / desc.rowHeight));
        const u32 rows = visible > 1 ? visible - 1 : 1;
        const u32 row = viewRowOf(tableRead().cursor, cursorRow);
        const u32 target = row == NO_ROW ? 0 : down ? std::min(row + rows, last) : row > rows ? row - rows : 0;
        moveCursorTo(target, extend);
    }

    bool UiTableState::activate(const InternedString column)
    {
        const Entity cursor = tableRead().cursor;
        if (!onActivate || cursor == Entity::undefined())
        {
            return false;
        }
        onActivate(cursor, column);
        return true;
    }

    void UiTableState::scrollTo(const u32 view)
    {
        const float top = static_cast<float>(view) * desc.rowHeight;
        const Vec2 offset = list.scrollOffset();
        const float viewHeight = list.size().y;
        if (top < offset.y)
        {
            list.setScrollOffset(Vec2{offset.x, top});
        }
        else if (top + desc.rowHeight > offset.y + viewHeight)
        {
            list.setScrollOffset(Vec2{offset.x, top + desc.rowHeight - viewHeight});
        }
    }

    // -- UiScope ------------------------------------------------------------------

    TableView UiScope::createTable(const InternedString id, UiTableDesc desc)
    {
        SASSERTM(desc.schema && desc.row && desc.key && desc.rowHeight > 0.0f,
                 "createTable: needs a schema, row and key and a positive row height.");
        if (desc.columns.empty())
        {
            for (const FieldDesc& field : desc.schema->fields)
            {
                desc.columns.push_back({InternedString(field.name)});
            }
        }
        auto owned = std::make_unique<UiTableState>();
        UiTableState& state = *owned;
        state.scope = this;
        state.desc = std::move(desc);
        state.rowCount = state.desc.rowCount;

        state.root = createPanel(id, state.desc.rect);
        const Entity root = state.root.entity();
        m_world->getComponent<UiStateComponent>(root).clipsChildren = true;
        m_world->addComponent<UiTableComponent>(root, UiTableComponent{});

        state.header = createPanel(childId(id, ".header"), topLeft());
        state.header.setParent(state.root);
        state.header.setAccessibleRole(UiRole::Row);
        for (const UiTableColumn& column : state.desc.columns)
        {
            const auto field = std::ranges::find_if(state.desc.schema->fields, [&column](const FieldDesc& candidate)
                                                    { return InternedString(candidate.name) == column.field; });
            SASSERTM(field != state.desc.schema->fields.end(), "createTable: a column names no field of the schema.");
            if (field == state.desc.schema->fields.end())
            {
                continue;
            }
            UiTableState::Column shown;
            shown.field = &*field;
            shown.title = column.title.empty() ? std::string(field->name) : column.title;
            shown.sortable = column.sortable && isSortable(*field);
            shown.activates = column.activates;
            if (column.classField.isValid())
            {
                const auto classField =
                    std::ranges::find_if(state.desc.schema->fields, [&column](const FieldDesc& candidate)
                                         { return InternedString(candidate.name) == column.classField; });
                SASSERTM(classField != state.desc.schema->fields.end() && isText(classField->kind),
                         "createTable: a column's classField names no text field of the schema.");
                if (classField != state.desc.schema->fields.end() && isText(classField->kind))
                {
                    shown.classField = &*classField;
                }
            }
            shown.cell = createPanel(childId(id, ".header.cell"), topLeft());
            m_world->getComponent<UiStateComponent>(shown.cell.entity()).clipsChildren = true;
            UiTableColumnComponent columnState;
            columnState.field = column.field;
            columnState.width = std::max(state.desc.minColumnWidth, column.width);
            columnState.sortable = shown.sortable;
            m_world->addComponent<UiTableColumnComponent>(shown.cell.entity(), columnState);
            shown.text = createLabel(childId(id, ".header.cell.text"), shown.title, topLeft());
            shown.mark = createLabel(childId(id, ".header.cell.mark"), "", topLeft());
            shown.grip = createPanel(childId(id, ".header.grip"), topLeft());
            shown.cell.setParent(state.header);
            shown.text.setParent(shown.cell);
            shown.text.setInteractable(false);
            shown.mark.setParent(shown.cell);
            shown.mark.setInteractable(false);
            shown.cell.setAccessibleRole(UiRole::ColumnHeader);
            if (column.titleKey.isValid())
            {
                shown.text.setLocalizedText(column.titleKey);
                shown.cell.setAccessibleNameKey(column.titleKey);
            }
            else
            {
                shown.cell.setAccessibleName(shown.title);
            }
            shown.grip.setParent(state.header);
            shown.grip.setZOffset(1); // over the cells it straddles

            const u32 index = static_cast<u32>(state.columns.size());
            setReaction(shown.cell.entity(), UiEventKind::Click,
                        [&state, index](const void* payload)
                        {
                            if (static_cast<const UiClickEvent*>(payload)->button == 0 && state.columns[index].sortable)
                            {
                                state.requestSortBy(index);
                            }
                        });
            // the cell the keys are on takes the cursor class, and gives its own back
            const auto cursorClass = [&state, index](const bool on)
            {
                const InternedString own =
                    state.desc.rowClass.isValid() ? childId(state.desc.rowClass, ".header.cell") : InternedString{};
                state.headerColumn = on ? index : state.headerColumn;
                Widget(state.columns[index].cell)
                    .setStyleClass(on && state.columns[index].sortable ? InternedString(HEADER_CURSOR_CLASS) : own);
            };
            setReaction(shown.cell.entity(), UiEventKind::FocusGained,
                        [cursorClass](const void*) { cursorClass(true); });
            setReaction(shown.cell.entity(), UiEventKind::FocusLost,
                        [cursorClass](const void*) { cursorClass(false); });
            // the drag accumulates its own width, so a request applied later never lags it
            const auto resize = [&state, index](const Vec2 delta)
            {
                if (state.dragColumn != index)
                {
                    state.dragColumn = index;
                    state.dragWidth = state.widthOf(index);
                }
                state.dragWidth += delta.x / state.scope->hierarchy().layoutScale();
                state.request(UiTableOp{.kind = UiTableOpKind::Resize,
                                        .column = InternedString(state.columns[index].field->name),
                                        .width = std::max(state.desc.minColumnWidth, state.dragWidth)});
            };
            setReaction(shown.grip.entity(), UiEventKind::DragStart, [resize](const void* payload)
                        { resize(static_cast<const UiDragStartEvent*>(payload)->delta); });
            setReaction(shown.grip.entity(), UiEventKind::DragMove,
                        [resize](const void* payload) { resize(static_cast<const UiDragMoveEvent*>(payload)->delta); });
            setReaction(shown.grip.entity(), UiEventKind::DragEnd,
                        [&state](const void*) { state.dragColumn = UiTableState::NO_ROW; });
            state.columns.push_back(std::move(shown));
        }

        UiVirtualListDesc listDesc;
        listDesc.rect = topLeft();
        listDesc.rowHeight = state.desc.rowHeight;
        listDesc.createRow = [&state, id](u32 /*slot*/) { return state.createRow(id); };
        listDesc.bindRow = [&state](const u32 slot, const u32 view) { state.bindRow(slot, view); };
        state.list = createVirtualList(childId(id, ".list"), std::move(listDesc));
        state.list.setParent(state.root);
        m_world->getComponent<UiStateComponent>(state.list.entity()).tabStop = true;
        // the header follows the rows' horizontal scroll
        onScrolled(state.list.entity(),
                   [&state](const Vec2 offset)
                   {
                       if (state.header.offset().x != -offset.x)
                       {
                           state.header.setOffset(Vec2{-offset.x, 0.0f});
                       }
                   });

        // keys bubble from the focused row (a press focuses it) to the list
        setKeyReaction(state.list.entity(),
                       [&state](const UiKeyDownEvent& event)
                       {
                           const bool extend = (event.modifiers & UiKeyModifier::SHIFT) != 0;
                           switch (event.scancode)
                           {
                           case Key::UP:
                               // Up from the first row goes on to the header, the way to its sort keys
                               if (!extend && !event.repeat && state.rowCount != 0 &&
                                   state.viewRowOf(state.tableRead().cursor, state.cursorRow) == 0 &&
                                   state.focusHeader())
                               {
                                   return true;
                               }
                               state.navigate(UiTreeCommand::Up, extend);
                               return true;
                           case Key::DOWN:
                               state.navigate(UiTreeCommand::Down, extend);
                               return true;
                           case Key::HOME:
                               state.navigate(UiTreeCommand::Home, extend);
                               return true;
                           case Key::END:
                               state.navigate(UiTreeCommand::End, extend);
                               return true;
                           case Key::PAGE_UP:
                           case Key::PAGE_DOWN:
                               state.page(event.scancode == Key::PAGE_DOWN, extend);
                               return true;
                           case Key::RETURN:
                           case Key::KEYPAD_ENTER:
                               return event.repeat || state.activate(InternedString{}); // a held key activates once
                           default:
                               return false;
                           }
                       });
        setKeyReaction(state.header.entity(), [&state](const UiKeyDownEvent& event) { return state.headerKey(event); });
        // without rows the table root holds the focus: Up reaches the header from it
        setKeyReaction(state.root.entity(),
                       [&state](const UiKeyDownEvent& event)
                       {
                           return event.scancode == Key::UP && event.modifiers == 0 &&
                               state.scope->router().focused() == state.root.entity() && state.focusHeader();
                       });
        state.styleHeader();
        m_tables.emplace(root.id(), std::move(owned));
        hierarchy().observeSize(root);
        state.layout();
        state.layoutColumns();
        state.refresh(state.rowCount);
        return TableView(this, root, m_generation);
    }

    UiTableState* UiScope::tableState(const Entity root) const
    {
        const auto it = m_tables.find(root.id());
        return it == m_tables.end() ? nullptr : it->second.get();
    }

    // -- TableView ----------------------------------------------------------------

    UiTableState* TableView::runtime() const { return isValid() ? scope()->tableState(entity()) : nullptr; }

    void TableView::refresh(const u32 rowCount)
    {
        if (UiTableState* state = runtime())
        {
            state->refresh(rowCount);
        }
    }

    void TableView::refreshRowDecorations()
    {
        if (UiTableState* state = runtime(); state && state->desc.decoration && !state->columns.empty())
        {
            for (UiTableState::Slot& slot : state->slots)
            {
                if (slot.view < state->rowCount)
                {
                    state->bindDecoration(slot, state->modelRow(slot.view));
                }
            }
        }
    }

    u32 TableView::rowCount() const
    {
        const UiTableState* state = runtime();
        return state ? state->rowCount : 0;
    }

    void TableView::setRowClass(const InternedString rowClass)
    {
        UiTableState* state = runtime();
        if (!state || state->desc.rowClass == rowClass)
        {
            return;
        }
        state->desc.rowClass = rowClass;
        for (UiTableState::Slot& slot : state->slots)
        {
            state->styleRow(slot);
        }
        state->styleHeader();
        // a cell classed by its row's field takes its class again at the bind
        if (std::ranges::any_of(state->columns, [](const auto& column) { return column.classField != nullptr; }))
        {
            state->list.refreshRows();
        }
    }

    InternedString TableView::rowClass() const
    {
        const UiTableState* state = runtime();
        return state ? state->desc.rowClass : InternedString{};
    }

    void TableView::setScrollEnabled(const bool enabled)
    {
        if (UiTableState* state = runtime())
        {
            state->list.setScrollEnabled(enabled);
        }
    }

    bool TableView::scrollEnabled() const
    {
        const UiTableState* state = runtime();
        return state && state->list.scrollEnabled();
    }

    bool TableView::apply(const UiTableOp& op)
    {
        UiTableState* state = runtime();
        return state && state->apply(op);
    }

    void TableView::onRequest(std::function<void(const UiTableOp&)> fn)
    {
        if (UiTableState* state = runtime())
        {
            state->onRequest = std::move(fn);
        }
    }

    void TableView::setMetrics(const float rowHeight, const float headerHeight, const float padding)
    {
        UiTableState* state = runtime();
        if (!state || !(rowHeight > 0.0f) || !(headerHeight > 0.0f) || padding < 0.0f ||
            (state->desc.rowHeight == rowHeight && state->desc.headerHeight == headerHeight &&
             state->desc.padding == padding))
        {
            return;
        }
        state->desc.rowHeight = rowHeight;
        state->desc.headerHeight = headerHeight;
        state->desc.padding = padding;
        state->list.setRowHeight(rowHeight);
        state->layout();
        state->layoutColumns(); // rebinds the visible rows, which place their cells
    }

    float TableView::rowHeight() const
    {
        const UiTableState* state = runtime();
        return state ? state->desc.rowHeight : 0.0f;
    }

    float TableView::headerHeight() const
    {
        const UiTableState* state = runtime();
        return state ? state->desc.headerHeight : 0.0f;
    }

    float TableView::columnWidth(const InternedString column) const
    {
        const UiTableState* state = runtime();
        const u32 index = state ? state->columnOf(column) : UiTableState::NO_ROW;
        return index == UiTableState::NO_ROW ? 0.0f : state->widthOf(index);
    }

    void TableView::setColumnVisible(const InternedString column, const bool visible)
    {
        UiTableState* state = runtime();
        const u32 index = state ? state->columnOf(column) : UiTableState::NO_ROW;
        if (index == UiTableState::NO_ROW || state->columns[index].visible == visible)
            return;
        state->columns[index].visible = visible;
        state->layoutColumns();
        state->list.refreshRows();
    }

    Widget TableView::headerCell(const InternedString column) const
    {
        const UiTableState* state = runtime();
        const u32 index = state ? state->columnOf(column) : UiTableState::NO_ROW;
        return index == UiTableState::NO_ROW ? Widget{} : Widget(state->columns[index].cell);
    }

    u32 TableView::modelRow(const u32 viewRow) const
    {
        const UiTableState* state = runtime();
        return state && viewRow < state->rowCount ? state->modelRow(viewRow) : UiTableState::NO_ROW;
    }

    void TableView::select(const Entity key, const UiTreeSelect mode)
    {
        UiTableState* state = runtime();
        if (!state)
        {
            return;
        }
        if (const u32 view = state->viewRowOf(key, state->cursorRow); view != UiTableState::NO_ROW)
        {
            state->select(view, mode);
        }
    }

    void TableView::navigate(const UiTreeCommand command, const bool extend)
    {
        if (UiTableState* state = runtime())
        {
            state->navigate(command, extend);
        }
    }

    void TableView::page(const bool down, const bool extend)
    {
        if (UiTableState* state = runtime())
        {
            state->page(down, extend);
        }
    }

    void TableView::onContextMenu(std::function<void(Entity, Vec2)> fn)
    {
        if (UiTableState* state = runtime())
        {
            state->onContextMenu = std::move(fn);
        }
    }

    void TableView::onActivate(std::function<void(Entity, InternedString)> fn)
    {
        if (UiTableState* state = runtime())
        {
            state->onActivate = std::move(fn);
        }
    }

    void TableView::onDoubleClick(std::function<void(Entity)> fn)
    {
        if (UiTableState* state = runtime())
        {
            state->onDoubleClick = std::move(fn);
        }
    }

    void TableView::activate()
    {
        if (UiTableState* state = runtime())
        {
            state->activate(InternedString{});
        }
    }

    TableView::VisibleRows TableView::visibleRows() const
    {
        const UiTableState* state = runtime();
        if (!state)
            return {};
        return {static_cast<u32>(state->list.scrollOffset().y / state->desc.rowHeight),
                static_cast<u32>(state->slots.size()), state->binds, state->writes};
    }

    TableView::Shown TableView::shown() const
    {
        Shown out;
        const UiTableState* state = runtime();
        if (!state)
            return out;
        const auto range = visibleRows();
        out.firstRow = range.firstRow;
        out.slots = range.slots;
        out.binds = range.binds;
        out.writes = range.writes;
        std::vector<const UiTableState::Slot*> bound;
        for (const UiTableState::Slot& slot : state->slots)
        {
            if (slot.view != UiTableState::NO_ROW && slot.view < state->rowCount)
            {
                bound.push_back(&slot);
            }
        }
        std::ranges::sort(bound, {}, &UiTableState::Slot::view);
        const World& world = state->scope->world();
        for (const UiTableState::Slot* slot : bound)
        {
            std::vector<std::string>& cells = out.cells.emplace_back();
            for (const Panel& text : slot->texts)
            {
                cells.push_back(world.getConstComponent<UiValueComponent>(text.entity()).text);
            }
        }
        return out;
    }

    void TableView::setSelection(const std::span<const Entity> keys)
    {
        if (UiTableState* state = runtime())
        {
            state->setSelection(keys);
        }
    }

    std::span<const Entity> TableView::selection() const
    {
        const UiTableState* state = runtime();
        return state ? std::span<const Entity>(state->tableRead().selected) : std::span<const Entity>{};
    }

    Entity TableView::cursor() const
    {
        const UiTableState* state = runtime();
        return state ? state->tableRead().cursor : Entity::undefined();
    }

    float TableView::scrollY() const
    {
        const UiTableState* state = runtime();
        return state ? state->list.scrollOffset().y : 0.0f;
    }

    bool TableView::atEnd() const
    {
        const UiTableState* state = runtime();
        if (!state)
        {
            return true;
        }
        const float last = static_cast<float>(state->rowCount) * state->desc.rowHeight - state->list.size().y;
        return state->list.scrollOffset().y >= last - 1.0f;
    }

    void TableView::scrollToEnd()
    {
        if (UiTableState* state = runtime())
        {
            state->list.setScrollOffset(Vec2{state->list.scrollOffset().x, 1.0e9f});
        }
    }

    void TableView::setScrollY(const float y)
    {
        if (UiTableState* state = runtime())
        {
            state->list.setScrollOffset(Vec2{state->list.scrollOffset().x, y});
        }
    }

    void TableView::scrollTo(const Entity key)
    {
        UiTableState* state = runtime();
        if (!state)
        {
            return;
        }
        if (const u32 view = state->viewRowOf(key, state->cursorRow); view != UiTableState::NO_ROW)
        {
            state->scrollTo(view);
        }
    }

    void TableView::onSelect(std::function<void(std::span<const Entity>)> fn)
    {
        if (UiTableState* state = runtime())
        {
            state->onSelect = std::move(fn);
        }
    }

    void TableView::onDragRows(std::function<void(std::span<const Entity>, Vec2, bool)> fn)
    {
        if (UiTableState* state = runtime())
        {
            state->onDragRows = std::move(fn);
        }
    }
} // namespace spite
