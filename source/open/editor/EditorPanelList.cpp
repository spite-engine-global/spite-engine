#include "EditorPanelList.hpp"

#include <cstdlib>
#include <unordered_set>

#include "ui/UiComponents.hpp"

namespace spite::panelui
{
    namespace
    {
        constexpr std::array<std::string_view, 2> OP_NAMES{"sort", "resize"}; // indexed by UiTableOpKind
        constexpr float SEARCH_MIN_WIDTH = 60.0f;
        constexpr float CHIP_MAX_WIDTH = 160.0f;

    } // namespace

    FilteredTable::FilteredTable(UiScope& scope, ControlServer& control, FilteredTableDesc desc) :
        m_scope(&scope), m_control(&control), m_desc(std::move(desc)), m_density(Density::read(scope))
    {
        const std::string& id = m_desc.id;
        for (const UiTableColumn& column : m_desc.columns)
        {
            m_columnNames.push_back(column.field.toView());
        }

        UiTableDesc table;
        table.rect = topLeft();
        table.rect.layoutItem = fillItem();
        table.rowHeight = m_density.row;
        table.headerHeight = m_density.control;
        table.padding = m_density.xs;
        table.schema = m_desc.schema;
        table.columns = m_desc.columns;
        table.rowClass = InternedString(LIST_ROW_CLASS);
        table.row = [this](const u32 row) { return m_desc.row(itemOf(row)); };
        table.key = [this](const u32 row) { return m_desc.key(itemOf(row)); };
        if (m_desc.expand)
        {
            table.expand = [this](const u32 row) { return m_desc.expand(itemOf(row)); };
        }
        m_table = scope.createTable(InternedString(id + ".table"), std::move(table));
        m_table.setAccessibleNameKey(m_desc.nameKey);
        m_table.onRequest(
            [this](const UiTableOp& op)
            {
                Json args{{"kind", std::string(OP_NAMES[static_cast<u8>(op.kind)])},
                          {"column", op.column.isValid() ? std::string(op.column.toView()) : std::string{}}};
                if (op.kind == UiTableOpKind::Sort)
                {
                    args["descending"] = op.descending;
                }
                else
                {
                    args["width"] = op.width;
                }
                m_pending.push_back(Json{{"command", m_desc.id + "Table"}, {"args", std::move(args)}}.dump());
            });
        m_table.onActivate(
            [this](const Entity key, const InternedString column)
            {
                if (m_desc.activate)
                {
                    m_desc.activate(key, column);
                }
            });

        if (!m_desc.headerHints.empty())
        {
            m_hint = scope.createTooltip(InternedString(id + ".hint"), UiTooltipDesc{});
            for (const auto& [column, key] : m_desc.headerHints)
            {
                m_hint.attach(m_table.headerCell(column));
            }
            m_hint.onShow(
                [this](const Entity target)
                {
                    for (const auto& [column, key] : m_desc.headerHints)
                    {
                        if (m_table.headerCell(column).entity() == target)
                        {
                            m_hint.label().setLocalizedText(key);
                        }
                    }
                });
        }

        UiRectDesc searchDesc = barDesc(Vec2{0.0f, m_density.control});
        searchDesc.layoutItem = fillItem();
        searchDesc.layoutItem->minSize = Vec2{SEARCH_MIN_WIDTH, 0.0f};
        searchDesc.layoutItem->alignSelf = UiLayoutAlign::Center;
        m_search = scope.createTextInput(InternedString(id + ".search"), searchDesc);
        bindInputPadding(m_search);
        m_search.setAccessibleNameKey(InternedString("engine.ui.list.search"));
        m_search.setLocalizedPlaceholder(InternedString("engine.ui.list.placeholder"));
        m_search.onChange(
            [this](const std::string_view typed)
            {
                m_pending.push_back(
                    Json{{"command", m_desc.id + "Filter"}, {"args", {{"text", std::string(typed)}}}}.dump());
            });

        // the button and the chip hide as the filter clears: focus goes to the box (the command empties it)
        const auto clearFilter = [this]
        {
            m_scope->router().requestFocus(m_search.entity());
            m_pending.push_back(Json{{"command", m_desc.id + "Filter"}, {"args", {{"text", ""}}}}.dump());
        };
        m_clear = scope.createButton(InternedString(id + ".clear"), "\xC3\x97",
                                     fieldDesc(Vec2{m_density.control, m_density.control}));
        m_clear.setAccessibleNameKey(InternedString("engine.ui.list.clear"));
        m_clear.setTooltip(InternedString("engine.ui.list.clear")); // an icon control: its name is its tooltip
        m_clear.onClick(clearFilter);
        m_clear.setVisible(false);

        m_count = scope.createLabel(InternedString(id + ".count"), "", fittedText(m_density.control, m_density.xs));
        bindFitPadding(m_count, SPACE_XS, SPACE_NONE);
        m_count.setLocalizedText(InternedString("engine.ui.list.total"));

        // a long filter shortens the chip (ellipsis) instead of pushing the box and the count out of the bar
        UiRectDesc chipDesc = fittedText(m_density.control, m_density.md);
        chipDesc.layoutItem =
            UiLayoutItemComponent{.maxSize = Vec2{CHIP_MAX_WIDTH, 0.0f}, .alignSelf = UiLayoutAlign::Center};
        m_chip = scope.createButton(InternedString(id + ".chip"), "", chipDesc);
        bindFitPadding(m_chip, SPACE_MD, SPACE_NONE);
        m_chip.setLocalizedText(InternedString("engine.ui.list.chip"));
        m_chip.onClick(clearFilter);
        m_chip.setVisible(false);

        m_empty = emptyNote(scope, id + ".empty");

        registerCommands();
        m_observer = scope.observeLayoutTokens([this] { follow(); });
    }

    FilteredTable::~FilteredTable() { m_scope->unobserveLayoutTokens(m_observer); }

    // A density or theme change: the table's rows and header and the filter bar's controls take the new sizes (the
    // bound paddings follow on their own).
    void FilteredTable::follow()
    {
        const Density now = Density::read(*m_scope);
        if (now == m_density)
        {
            return;
        }
        m_density = now;
        m_table.setMetrics(now.row, now.control, now.xs);
        for (Widget field : {Widget(m_search), Widget(m_count), Widget(m_chip)})
        {
            field.setSize(Vec2{field.size().x, now.control});
        }
        Widget(m_clear).setSize(Vec2{now.control, now.control});
        if (m_emptyKind != 0)
        {
            m_table.setLayoutItem(basisItem(now.control));
        }
    }

    void FilteredTable::update()
    {
        if (!m_pending.empty())
        {
            dispatch(*m_control, m_pending, m_desc.id);
        }
    }

    void FilteredTable::apply()
    {
        m_shown.clear();
        for (u32 item = 0; item < m_total; ++item)
        {
            if (m_text.empty() || m_desc.matches(item, m_text))
            {
                m_shown.push_back(item);
            }
        }
        m_shownCount = static_cast<u32>(m_shown.size());
        m_table.refresh(m_shownCount);
        std::unordered_set<Entity> shownKeys;
        shownKeys.reserve(m_shown.size());
        for (const u32 item : m_shown)
        {
            shownKeys.insert(m_desc.key(item));
        }
        prune([&shownKeys](const Entity key) { return shownKeys.contains(key); });
        showState();
    }

    void FilteredTable::setShown(const u32 shown, const u32 total)
    {
        m_shownCount = shown;
        m_total = total;
        m_table.refresh(shown);
        prune(m_desc.shows);
        showState();
    }

    // selection and cursor keep to the shown rows (a hidden row leaves a stale cursor even when nothing selected)
    void FilteredTable::prune(const std::function<bool(Entity key)>& keeps)
    {
        std::vector<Entity> kept;
        for (const Entity key : m_table.selection())
        {
            if (keeps(key))
            {
                kept.push_back(key);
            }
        }
        const Entity cursor = m_table.cursor();
        if (kept.size() != m_table.selection().size() || (cursor != Entity::undefined() && !keeps(cursor)))
        {
            m_table.setSelection(kept);
        }
    }

    // Writes only what changed: the widgets re-resolve their text when an argument or a key moves.
    void FilteredTable::showState()
    {
        const bool filtered = !m_text.empty();
        if (filtered != m_filtered)
        {
            m_filtered = filtered;
            m_count.setLocalizedText(InternedString(filtered ? "engine.ui.list.count" : "engine.ui.list.total"));
            m_countedShown = m_countedTotal = ~0u;
        }
        // a localized argument rewrite allocates: a refresh that moves no number writes none
        if (m_total != m_countedTotal)
        {
            m_countedTotal = m_total;
            m_count.setLocalizedArg("total", m_total);
        }
        if (m_shownCount != m_countedShown)
        {
            m_countedShown = m_shownCount;
            m_count.setLocalizedArg("shown", m_shownCount);
        }
        if (m_clear.isVisible() != (filtered && m_visible))
        {
            m_clear.setVisible(filtered && m_visible);
            m_chip.setVisible(filtered && m_visible);
        }
        if (filtered && m_text != m_chipText)
        {
            m_chipText = m_text;
            m_chip.setLocalizedArgText("text", m_text);
        }
        const u8 emptyKind = m_shownCount != 0 ? 0 : filtered && m_total > 0 ? 2 : 1;
        if (emptyKind != m_emptyKind)
        {
            m_emptyKind = emptyKind;
            if (emptyKind != 0)
            {
                m_empty.setLocalizedText(
                    InternedString(emptyKind == 2 ? "engine.ui.list.noMatches" : "engine.ui.list.nothing"));
            }
            m_empty.setVisible(emptyKind != 0 && m_visible);
            // without rows the table is its header and the note takes the rest of the slot
            m_table.setLayoutItem(emptyKind != 0 ? basisItem(m_density.control) : fillItem());
        }
    }

    void FilteredTable::setVisible(const bool visible)
    {
        if (visible != m_visible)
        {
            m_visible = visible;
            m_table.setVisible(visible);
            m_empty.setVisible(visible && m_emptyKind != 0);
            m_search.setVisible(visible);
            m_count.setVisible(visible);
            m_clear.setVisible(visible && m_filtered);
            m_chip.setVisible(visible && m_filtered);
        }
    }

    void FilteredTable::fillFilter(const Widget& slot)
    {
        const InternedString base = slotFields(*m_scope, slot).childClass;
        const InternedString family = base.isValid() ? base : InternedString(LIST_FILTER_CLASS);
        const auto part = [&family](const std::string_view suffix) { return classPart(family, suffix); };
        putIn(*m_scope, slot, m_search);
        putIn(*m_scope, slot, m_clear);
        putIn(*m_scope, slot, m_count);
        putIn(*m_scope, slot, m_chip);
        m_search.setStyleClass(part(".search"));
        m_clear.setStyleClass(part(".clear"));
        m_count.setStyleClass(part(".count"));
        m_chip.setStyleClass(part(".chip"));
    }

    void FilteredTable::fillRows(const Widget& slot)
    {
        putIn(*m_scope, slot, m_table);
        putIn(*m_scope, slot, m_empty);
        const UiSlotComponent& fields = slotFields(*m_scope, slot);
        const InternedString family = fields.childClass.isValid() ? fields.childClass : InternedString(LIST_ROW_CLASS);
        m_table.setScrollEnabled(fields.scroll);
        m_table.setRowClass(family);
        m_empty.setStyleClass(classPart(family, ".empty"));
    }

    std::string FilteredTable::setFilter(const ControlArgs& args)
    {
        m_text = args.string("text").value_or("");
        // the box shows a filter set from elsewhere or cleared; typing already holds the same text
        if (m_search.value() != m_text)
        {
            m_search.setText(m_text);
        }
        if (m_desc.refilter)
        {
            m_desc.refilter(m_text);
        }
        else
        {
            apply();
        }
        return {};
    }

    std::string FilteredTable::tableOp(const ControlArgs& args)
    {
        const auto kind = std::ranges::find(OP_NAMES, args.string("kind").value_or(""));
        UiTableOp op{.kind = static_cast<UiTableOpKind>(kind - OP_NAMES.begin())};
        if (const std::optional<std::string> column = args.string("column"); column && !column->empty())
        {
            op.column = InternedString(*column);
        }
        op.descending = args.boolean("descending").value_or(false);
        if (const std::optional<std::string> width = args.json("width"))
        {
            op.width = std::strtof(width->c_str(), nullptr);
        }
        return m_table.apply(op) ? std::string{}
                                 : m_desc.id +
                "Table op refused: unknown column, a column that does not sort or a "
                "non-finite width";
    }

    void FilteredTable::registerCommands()
    {
        m_docs.push_back("filters the " + m_desc.id + " list's rows by text; no text clears the filter");
        m_docs.push_back("sorts the " + m_desc.id + " table or resizes one of its columns");
        m_control->registerViewCommand(
            m_desc.id + "Filter", m_desc.owner, m_docs[0],
            {optionalArg("text", ArgKind::String, "the text a row contains", "empty: no filter")},
            [this](const ControlArgs& args, IFormatWriter&) { return setFilter(args); });
        m_control->registerViewCommand(
            m_desc.id + "Table", m_desc.owner, m_docs[1],
            {requiredArg("kind", ArgKind::String, "sort|resize", OP_NAMES),
             optionalArg("column", ArgKind::String, "one of the table's columns", "none (unsorted for sort)",
                         m_columnNames),
             optionalArg("descending", ArgKind::Boolean, "sort descending", "false"),
             optionalArg("width", ArgKind::Number, "the column's new width, for resize", "the minimum")},
            [this](const ControlArgs& args, IFormatWriter&) { return tableOp(args); });
    }

    void FilteredTable::writeView(IFormatWriter& out) const
    {
        out.beginObject("filter");
        out.writeString("text", m_text);
        out.writeU32("shown", m_shownCount);
        out.writeU32("total", m_total);
        out.endObject();
        const UiTableComponent& state = m_scope->world().getConstComponent<UiTableComponent>(m_table.entity());
        out.beginObject("sort");
        out.writeString("column", state.sortColumn.isValid() ? state.sortColumn.toView() : std::string_view{});
        out.writeBool("descending", state.descending);
        out.endObject();
        out.writeString("empty", m_emptyKind == 0 ? "" : m_emptyKind == 2 ? "noMatches" : "nothing");

        // view rows (the sorted order) of the cursor and the selection
        const std::span<const Entity> selection = m_table.selection();
        std::vector<u32> selected;
        i32 cursor = -1;
        for (u32 view = 0; view < m_shownCount; ++view)
        {
            const Entity key = m_desc.key(itemOf(m_table.modelRow(view)));
            cursor = key == m_table.cursor() ? static_cast<i32>(view) : cursor;
            if (std::ranges::contains(selection, key))
            {
                selected.push_back(view);
            }
        }
        out.writeI32("cursor", cursor);
        out.beginArray("selected", static_cast<u32>(selected.size()));
        for (const u32 view : selected)
        {
            out.writeU32("", view);
        }
        out.endArray();

        const TableView::Shown bound = m_table.shown();
        writeBoundCells(out, bound.firstRow, bound.slots, bound.binds, bound.writes, bound.cells);
    }
} // namespace spite::panelui
