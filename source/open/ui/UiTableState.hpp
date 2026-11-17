#pragma once

#include <chrono>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "ui/UiComponents.hpp"
#include "ui/UiEvents.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class UiScope;

    // A table's runtime half, owned by its scope: the model, the view order
    // and the header and row pool's parts. Sort and selection live in
    // UiTableComponent, column widths in UiTableColumnComponent.
    class UiTableState
    {
    public:
        static constexpr u32 NO_ROW = static_cast<u32>(-1);

        struct Column
        {
            const FieldDesc* field = nullptr;
            std::string title;
            bool sortable = false;
            bool activates = false;
            bool visible = true;
            const FieldDesc* classField = nullptr; // UiTableColumn::classField, when the row schema has it
            Panel cell; // the header cell, holding UiTableColumnComponent
            Label text; // the title
            Label mark; // the sort indicator
            Panel grip;
        };

        struct Slot
        {
            Panel root;
            Panel highlight;
            Image image;
            Label icon;
            std::vector<Panel> cells;
            std::vector<Panel> texts; // UiValue text: changing values are not interned
            u32 view = NO_ROW;
            Entity key = Entity::undefined();
        };

        [[nodiscard]] UiTableComponent& table() const; // for a write: it marks the component modified
        [[nodiscard]] const UiTableComponent& tableRead() const;
        [[nodiscard]] float widthOf(u32 column) const;
        [[nodiscard]] float totalWidth() const;
        [[nodiscard]] u32 columnOf(InternedString field) const;
        [[nodiscard]] u32 modelRow(u32 view) const { return order.empty() ? view : order[view]; }
        [[nodiscard]] Entity keyAt(u32 view) const { return desc.key(modelRow(view)); }
        // The view row showing `key`: `hint` when it still does, else a scan.
        [[nodiscard]] u32 viewRowOf(Entity key, u32 hint) const;

        Widget createRow(InternedString id);
        void styleRow(Slot& slot) const;
        void styleHeader() const;
        void bindRow(u32 slot, u32 view);
        void bindDecoration(Slot& slot, u32 model);
        void layout();
        void layoutColumns();
        void placeHeaderBand();
        void updateHeaderMarks();
        void requestSortBy(u32 column); // the click's request: ascending first, then toggling
        [[nodiscard]] u32 sortableBeside(u32 from, bool forward) const;
        [[nodiscard]] u32 focusedColumn() const;
        bool focusHeader(); // false when no column sorts
        bool headerKey(const UiKeyDownEvent& event);
        void sort();
        void refresh(u32 count);
        bool apply(const UiTableOp& op);
        void request(const UiTableOp& op);
        void select(u32 view, UiTreeSelect mode);
        void setSelection(std::span<const Entity> keys);
        void navigate(UiTreeCommand command, bool extend);
        void moveCursorTo(u32 target, bool extend);
        void page(bool down, bool extend);
        bool activate(InternedString column); // whether the cursor row was activated
        [[nodiscard]] u32 columnAt(float x) const;
        void scrollTo(u32 view);
        void repaintSelection();

        UiScope* scope = nullptr;
        UiTableDesc desc;
        Panel root;
        Panel header;
        VirtualList list;
        std::vector<Column> columns;
        std::vector<Slot> slots;
        std::vector<u32> order; // view row -> model row; empty: model order
        u32 rowCount = 0;
        u32 cursorRow = NO_ROW; // view-row caches of the cursor and anchor keys
        u32 anchorRow = NO_ROW;
        u32 dragColumn = NO_ROW;
        u32 headerColumn = NO_ROW; // the header cell the keys were last on
        float dragWidth = 0.0f;
        std::function<void(std::span<const Entity>)> onSelect;
        std::function<void(const UiTableOp&)> onRequest;
        std::function<void(Entity, InternedString)> onActivate;
        std::function<void(Entity)> onDoubleClick;
        Entity lastClick = Entity::undefined();
        std::chrono::steady_clock::time_point lastClickTime{};
        std::function<void(Entity, Vec2)> onContextMenu;
        u64 binds = 0; // row binds
        u64 writes = 0; // cell texts written
        std::function<void(std::span<const Entity>, Vec2, bool)> onDragRows;
        std::vector<Entity> dragKeys; // the rows a drag in flight carries
        std::string cellText; // bindRow's scratch: a bind that changes nothing allocates nothing
    };
} // namespace spite
