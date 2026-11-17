#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "ui/UiComponents.hpp"
#include "ui/UiEvents.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class UiScope;

    // The runtime half of a dropdown, combo box or popup list, owned by its scope and keyed by
    // the root. The UiDropdownComponent on the root holds the selection (the truth); the list is a separate root
    // surface built at the first open (a popup list's root is that surface), holding a bounded pool of rows that show a
    // window of the (filtered) items, so cost scales with the rows shown, never the item count. While it is open the
    // scope's open-dropdown count is above zero and the context re-places the list as its anchor moves; a UI with none
    // open pays nothing.
    class UiDropdownState
    {
    public:
        static constexpr u32 NONE = static_cast<u32>(-1);
        static constexpr u32 MAX_ROWS = 64;

        UiDropdownState(UiScope& owner, const UiWidgetKind widgetKind, const Entity rootElement,
                        const UiDropdownDesc& dropdownDesc, const bool popupOnly) :
            scope(&owner), kind(widgetKind), root(rootElement), desc(dropdownDesc), standalone(popupOnly)
        {
            // a degenerate row height or count would divide by zero or show nothing
            desc.rowHeight = std::isfinite(desc.rowHeight) ? std::clamp(desc.rowHeight, 4.0f, 400.0f) : 22.0f;
            desc.maxRows = std::clamp(desc.maxRows, 1u, MAX_ROWS);
            desc.searchable = desc.searchable && widgetKind != UiWidgetKind::Combo; // a combo's face is its search
        }

        void build();
        // The face parts take the root's class family (partClass); after the root's class changed.
        void restyle();
        // A popup list: the root is the list itself.
        void buildStandalone();

        // -- model --------------------------------------------------------------------------------------------------
        void setItems(std::span<const UiChoiceItem> choices);
        // Writes the selection and shows it; fires nothing, leaves the list as it is.
        void assign(i32 index);
        void setPlaceholder(InternedString key)
        {
            placeholder = key;
            applySelected();
        }
        // The input paths: assign, close the list, then fire onSelect when the selection changed (a popup list: every
        // choice, selection untouched).
        void choose(u32 item);
        // Applies a raw write of the component to what the widget shows; fires nothing.
        void sync();
        // uiActivate setValue: why the text is not a value of this widget, or empty. An item index, -1 (clears), an
        // item id or its label.
        [[nodiscard]] std::string setFromText(std::string_view text);

        // -- list ---------------------------------------------------------------------------------------------------
        void open();
        // A popup list beside the anchor: opening the open list at the same anchor closes it.
        void openAt(Entity anchorElement);
        // revert: a combo shows the selected item's text again (focus lost, chosen); false keeps what was typed.
        void close(bool revert = true);
        void toggle();
        // After a layout resolve: follows the anchor, closes the list when the anchor went away.
        void refit();

        [[nodiscard]] bool insidePopup(Entity element) const;
        [[nodiscard]] bool insideRoot(Entity element) const;
        [[nodiscard]] Entity focusTarget() const;
        // What the list is placed beside: the root, or a popup list's anchor.
        [[nodiscard]] Entity anchorEntity() const { return standalone ? openedFrom : root; }

        UiScope* scope = nullptr;
        UiWidgetKind kind = UiWidgetKind::Dropdown;
        Entity root = Entity::undefined();
        Entity label = Entity::undefined(); // a dropdown's selected text
        Entity input = Entity::undefined(); // a combo's text input
        Entity arrow = Entity::undefined();
        Entity searchInput = Entity::undefined(); // a searchable list's box
        UiDropdownDesc desc;
        bool standalone = false; // a popup list: no face, the root is the popup

        // The list: built at the first open.
        Panel popup;
        Panel highlightBar;
        Label markBar; // selected item's checkmark, under the rows (dropdown.mark)
        Panel thumb;
        Label emptyRow;
        std::vector<Label> rows;
        std::vector<Label> rowIcons;

        std::vector<UiChoiceItem> items;
        std::vector<std::string> searchTerms, exactSearch;
        std::vector<u32> view; // indices into items the list shows, in item order
        bool filtering = false; // the list is filtered by `filter` (a combo's text, a search box)
        std::string filter;
        u32 revision = 0; // bumped by setItems; a row binds again when it was bound at another one
        i32 shownSelected = -1;
        InternedString placeholder{}; // the face's message while nothing is selected (Dropdown::setPlaceholderKey)
        bool listOpen = false;
        u32 first = 0; // position in view of the first row
        i32 highlighted = -1; // position in view, -1 none
        u32 shownRows = 0; // rows the placement fits, at most desc.maxRows
        Entity openedFrom = Entity::undefined(); // a popup list's anchor
        Entity dismissedBy = Entity::undefined(); // the element whose press took the focus and so closed the list
        Entity returnFocus = Entity::undefined(); // where the focus goes back to on a choice or Escape

        std::function<void(u32)> onSelect;

        // A key its opener forwards (UiDropdownDesc::keepFocus); false while the list is closed.
        bool forwardKey(const u32 scancode) { return listOpen && key(UiKeyDownEvent{.scancode = scancode}); }

    private:
        struct RowStamp
        {
            u32 item = NONE;
            u32 revision = 0;
            bool selected = false;
            bool operator==(const RowStamp&) const = default;
        };

        void ensurePopup();
        void ensureRows(u32 count);
        void place();
        void bindRows();
        void showHighlight();
        // The current value's checkmark at the popup's left edge on its row, while the row shows.
        void showMark();
        [[nodiscard]] i32 positionAt(Vec2 pointer) const;
        void showThumb();
        void showEmpty();
        void publish();
        void applySelected();
        void rebuildView();
        void filterChanged(std::string_view text);
        void submit(std::string_view text);
        void closeIfOutside();
        void restoreFocus();
        bool key(const UiKeyDownEvent& event);
        void comboKey(u32 scancode, u8 modifiers);
        bool moveHighlight(u32 scancode);
        void typeAhead(std::string_view text);
        void scrollBy(float rows);
        void dragContent(float pixels);
        void scrollThumb(float pixels);
        void clearSearch();
        [[nodiscard]] bool navigable(u32 item) const { return items[item].enabled && !items[item].header; }
        [[nodiscard]] i32 nextEnabled(i32 from, i32 direction) const;
        void setHighlight(i32 position, bool scrollTo);
        void scrollTo(i32 position);
        [[nodiscard]] std::string_view textOf(const UiChoiceItem& item, std::string& scratch) const;
        [[nodiscard]] float topInset() const; // unscaled height above the rows: the search box

        std::vector<RowStamp> m_stamps;
        UiRect m_anchor;
        UiRect m_bounds;
        float m_scale = 0.0f;
        float m_width = 0.0f; // unscaled popup width the rows are sized for
        i32 m_scrolledTo = -2; // the highlight the window last scrolled to; place() scrolls only when it moved
        i32 m_shownHighlight = -2; // what showHighlight last drew (a position, -1 none); -2 never
        i32 m_shownMark = -2; // the same for showMark
        float m_dragRows = 0.0f; // the rows a drag has moved that the pool has not shown yet
        std::string m_typed; // the prefix typed since the last navigation key
        bool m_built = false;
    };
} // namespace spite
