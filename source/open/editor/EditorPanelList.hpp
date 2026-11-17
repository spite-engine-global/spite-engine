#pragma once

#include "EditorPanelUi.hpp"

// The panel list pattern: a sortable, filterable, keyboard-navigable table with its filter bar, built on
// TableView (SpiteUi) and split into the two slots of a panel's document view. The panel owns a model
// (its rows as a struct with a schema) and one FilteredTable; the table owns what the pattern adds:
//   - a sticky header (titles from loc keys, units go in the title: "Time (ms)"), a sort indicator and the sort state
//     in uiTree (role columnheader, `sort`); a header click sorts, again reverses (stable; numbers compare as
//     numbers, text in natural order ignoring ASCII case: "pass2" before "Pass10")
//   - a filter bar: search box with a placeholder, clear button, match count ("12 / 40"), and an active-filter chip
//     whose click clears
//   - an empty state ("No matches" under a filter, else "Nothing to show")
//   - one Tab stop (the rows list, role grid), Up/Down/Home/End/PageUp/PageDown, Enter (a held key's repeats are
//     ignored) or a click on an `activates` column's cell of the already selected row calls `activate`; a gamepad
//     reaches the same keys through the navigation intents. `headerHints` give a header cell a tooltip (a loc key)
//   - the selection and the cursor follow the filter: a row it hides is neither
//   - the control commands <id>Filter and <id>Table (sort, resize) that typing and header gestures queue, and
//     writeView for the panel's query (filter, sort, cursor, selection, the pooled rows and their texts)
// Adopting it in a panel (Project, Log, Stats, Modules, Systems, ECS, ...):
//   1. Model: `struct Row { std::string name; u32 count; SPITE_OBSERVED_SCHEMA(Row, name, count) };` and a
//      std::vector of the panel's items; display strings are localized by the panel when it fills the row.
//   2. Strings: the column titles and the table's accessible name as keys in assets/loc/en/editor/<panel>.json
//      ("column.name", "table"; the namespace is editor-<panel>); the filter bar's strings are engine.ui.list.*.
//   3. Document: assets/ui/editor/<panel>.json gets two slots, `<id>.filter` (a row container, height 22, grows in the
//      toolbar) and `<id>.rows` (a column container that grows); the fallback builds the same containers
//      (filterSlotPanel, fillSlotPanel). A `childClass` on a slot swaps the families list.filter.* / list.row.*
//      (rules in `classes` of assets/ui/engine.json and all three editor themes; a new family ships all four).
//   4. Code: construct FilteredTable(scope, control, desc) with the schema, columns (titleKey, width, activates), the
//      item accessors, `matches` and `activate`; contract.slot(<id>.filter, fillFilter) and contract.slot(<id>.rows,
//      fillRows); call setItems(count) whenever the model changed and update() every frame (it is idle-clean).
//   5. Control: writeView(out) inside the panel's view query; unregisterOwner(owner) in the panel's destructor.
//   6. Tests: sort, filter, clear, count, empty state and the keyboard (see SpiteEditorPanelsTests, the Passes list).
// A model that streams (the Log panel: thousands of rows, new ones every frame) filters itself: set
// FilteredTableDesc::refilter (the filter text goes there) and call setShown(shown, total) instead of setItems; the
// table then holds no copy of the model and costs the visible rows (SpiteEditorLogTests).
namespace spite::panelui
{
    constexpr std::string_view LIST_FILTER_CLASS = "list.filter"; // the filter bar's class family
    constexpr std::string_view LIST_ROW_CLASS = "list.row"; // the table's (rows, header, empty state)

    inline bool containsIgnoringCase(const std::string_view text, const std::string_view part)
    {
        const auto lower = [](const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        return part.empty() || std::ranges::search(text, part, {}, lower, lower).begin() != text.end();
    }

    // A control of a filter bar: the control height, centred in the slot that holds it, so a density that lowers the
    // control keeps it level with the bar's other widgets.
    inline UiRectDesc fieldDesc(const Vec2 size)
    {
        UiRectDesc desc = barDesc(size);
        desc.layoutItem->alignSelf = UiLayoutAlign::Center;
        return desc;
    }

    // A filter bar control whose width fits its text plus `padding` at each side.
    inline UiRectDesc fittedText(const float height, const float padding)
    {
        UiRectDesc desc = fieldDesc(Vec2{0.0f, height});
        desc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = false, .padding = Vec2{padding, 0.0f}};
        return desc;
    }

    struct FilteredTableDesc
    {
        std::string id; // `<id>.table`, `<id>.search`, ...; the commands <id>Filter and <id>Table
        std::string owner; // the control owner of those commands (the panel unregisters it)
        InternedString nameKey{}; // the table's accessible name
        const StructSchema* schema = nullptr; // of a row the table shows
        std::vector<UiTableColumn> columns; // field, titleKey, width, activates
        std::function<const void*(u32 item)> row; // the item's row, valid until the next setItems
        std::function<Entity(u32 item)> key; // the item's stable key (selection survives a filter and a sort)
        std::function<UiExpandState(u32 item)> expand; // set: a section header item reports expanded or collapsed
        std::function<bool(u32 item, std::string_view needle)> matches; // whether a non-empty filter shows the item
        std::function<void(Entity key, InternedString column)> activate; // Enter (no column) or a click on a column
        std::vector<std::pair<InternedString, InternedString>> headerHints; // column field -> loc key of its tooltip
        // A streaming model the panel filters itself (a log of thousands of rows that grows every frame): the items
        // are the view rows in model order (no sort), the panel calls setShown instead of setItems, and a changed
        // filter text goes to `refilter`, which re-filters the panel's model and answers with setShown. `matches` is
        // not used; `shows` says whether a selected or cursor key is still in the model.
        std::function<void(std::string_view text)> refilter;
        std::function<bool(Entity key)> shows;
    };

    class FilteredTable
    {
    public:
        FilteredTable(UiScope& scope, ControlServer& control, FilteredTableDesc desc);
        ~FilteredTable();
        FilteredTable(const FilteredTable&) = delete;
        FilteredTable& operator=(const FilteredTable&) = delete;

        // The model changed: `count` items now; re-filters, re-sorts and rebinds the shown rows.
        void setItems(const u32 count)
        {
            m_total = count;
            apply();
        }

        // A streaming model changed (FilteredTableDesc::refilter): `shown` rows of `total` now; rebinds the shown rows
        // and drops selected keys the model no longer shows.
        void setShown(u32 shown, u32 total);

        // Every frame: runs the requests typing and header gestures queued (a filter or sort command applies at once).
        void update();

        // The slot fills: the filter bar's widgets and the table with its empty note go in, the slot's child class
        // and scroll apply.
        void fillFilter(const Widget& slot);
        void fillRows(const Widget& slot);

        // The view query's fields: filter {text, shown, total}, sort {column, descending}, empty, cursor, selected
        // (view rows), then the table's firstRow, slots, binds, writes and visible cells.
        void writeView(IFormatWriter& out) const;

        // A panel that shows one of several lists: the table, its empty note and the filter bar follow.
        void setVisible(bool visible);

        [[nodiscard]] TableView table() const { return m_table; }
        // The rows' and the header's height now (the density's size.row and size.control).
        [[nodiscard]] float rowHeight() const { return m_table.rowHeight(); }
        [[nodiscard]] float headerHeight() const { return m_table.headerHeight(); }
        [[nodiscard]] u32 shownCount() const { return m_shownCount; }
        [[nodiscard]] const std::vector<u32>& shown() const { return m_shown; } // item indices, filter order
        [[nodiscard]] const std::string& filterText() const { return m_text; }

    private:
        void apply();
        void prune(const std::function<bool(Entity key)>& keeps);
        void showState();
        [[nodiscard]] u32 itemOf(const u32 viewRow) const { return m_desc.refilter ? viewRow : m_shown[viewRow]; }
        std::string setFilter(const ControlArgs& args);
        std::string tableOp(const ControlArgs& args);
        void registerCommands();
        void follow();

        UiScope* m_scope;
        ControlServer* m_control;
        FilteredTableDesc m_desc;
        Density m_density;
        u32 m_observer = 0;
        TableView m_table;
        TextInput m_search;
        Button m_clear;
        Label m_count;
        Button m_chip;
        Label m_empty;
        Tooltip m_hint; // one tooltip for the header cells with a hint
        std::vector<u32> m_shown; // empty while the panel filters (FilteredTableDesc::refilter)
        u32 m_shownCount = 0;
        std::vector<std::string> m_pending;
        std::vector<std::string> m_docs; // the registered commands' texts (views)
        std::vector<std::string_view> m_columnNames;
        std::string m_text;
        u32 m_total = 0;
        bool m_filtered = false; // the count shows "shown / total"
        u32 m_countedShown = ~0u; // the numbers the count label holds: it is written when they move
        u32 m_countedTotal = ~0u;
        std::string m_chipText; // the filter text the chip holds
        bool m_visible = true;
        u8 m_emptyKind = 0; // 0 none, 1 nothing, 2 no matches
    };

    // A table with no filter bar over a small read-only model: the panel hands in the whole row set, and an equal set
    // writes nothing. Cells compares with == (a defaulted operator). Rows are keyed by their index.
    template <class Cells>
    class RowTable
    {
    public:
        RowTable(UiScope& scope, const std::string_view id, const InternedString nameKey,
                 std::vector<UiTableColumn> columns) : m_scope(&scope), m_density(Density::read(scope))
        {
            UiTableDesc desc;
            desc.rect = topLeft();
            desc.rect.layoutItem = fillItem();
            desc.rowHeight = m_density.row;
            desc.headerHeight = m_density.control;
            desc.padding = m_density.xs;
            desc.schema = &Cells::spiteSchema();
            desc.columns = std::move(columns);
            desc.rowClass = InternedString(LIST_ROW_CLASS);
            desc.row = [this](const u32 row) -> const void* { return &m_rows[row]; };
            desc.key = [](const u32 row) { return Entity(row + 1, 1u); };
            m_table = scope.createTable(InternedString(id), std::move(desc));
            m_table.setAccessibleNameKey(nameKey);
            // a density or theme change: the rows, the header and the cell insets take the new sizes
            m_observer = scope.observeLayoutTokens(
                [this]
                {
                    const Density now = Density::read(*m_scope);
                    if (now != m_density)
                    {
                        m_density = now;
                        m_table.setMetrics(now.row, now.control, now.xs);
                        if (m_headerOnly)
                        {
                            m_table.setLayoutItem(basisItem(now.control));
                        }
                    }
                });
        }

        ~RowTable() { m_scope->unobserveLayoutTokens(m_observer); }
        RowTable(const RowTable&) = delete;
        RowTable& operator=(const RowTable&) = delete;

        // Whether the rows differ from the shown ones.
        bool set(std::vector<Cells> rows)
        {
            if (rows == m_rows)
            {
                return false;
            }
            m_rows = std::move(rows);
            m_table.refresh(static_cast<u32>(m_rows.size()));
            return true;
        }

        // The table is its header while the panel's empty note (emptyNote) takes the rest of the slot; writes only on
        // a change.
        void setHeaderOnly(const bool on)
        {
            if (on != m_headerOnly)
            {
                m_headerOnly = on;
                m_table.setLayoutItem(on ? basisItem(m_density.control) : fillItem());
            }
        }

        // The slot's fill: the table goes in, the slot's scroll and child class apply.
        void fill(UiScope& scope, const Widget& slot)
        {
            putIn(scope, slot, m_table);
            const UiSlotComponent& fields = slotFields(scope, slot);
            m_table.setScrollEnabled(fields.scroll);
            m_table.setRowClass(fields.childClass.isValid() ? fields.childClass : InternedString(LIST_ROW_CLASS));
        }

        [[nodiscard]] TableView table() const { return m_table; }
        [[nodiscard]] const std::vector<Cells>& rows() const { return m_rows; }

    private:
        UiScope* m_scope;
        Density m_density;
        u32 m_observer = 0;
        TableView m_table;
        std::vector<Cells> m_rows;
        bool m_headerOnly = false;
    };

    // The filter slot's fallback panel: a row of the bar's height that grows in its toolbar.
    inline Panel filterSlotPanel(UiScope& scope, const std::string_view id, const Widget& parent)
    {
        const Density density = Density::read(scope);
        UiRectDesc desc = barDesc(Vec2{0.0f, density.control}, stack(UiLayoutDirection::Row, density.xs));
        desc.layoutItem = fillItem();
        Panel bar = scope.createPanel(InternedString(id), desc);
        bindGap(bar, SPACE_XS);
        bar.setParent(parent);
        return bar;
    }
} // namespace spite::panelui
