#pragma once

#include <string>
#include <string_view>

#include "ecs/EcsTypes.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiExport.hpp"

namespace spite
{
    class UiHierarchyManager;

    // "button", "textbox", "spinbutton", "checkbox", "switch", "slider", "progressbar", "radiogroup", "radio",
    // "image", "scrollarea", "menu", "menuitem", "tree", "treeitem", "table", "row", "label", "canvas", "combobox",
    // "tablist", "tab", "chart", "option", "presentation", "listbox", "grid", "columnheader", "gridcell", "alert",
    // "status", "dialog", "alertdialog", "menuitemcheckbox", "separator", "application"; empty for None.
    [[nodiscard]] SPITE_UI_API std::string_view uiRoleName(UiRole role);
    // Roles a person acts on (press, type, set, choose, or move through with the keys: a tree or a table); an element
    // with one needs an accessible name.
    [[nodiscard]] SPITE_UI_API bool uiRoleIsInteractive(UiRole role);

    // A column header's sort state (aria-sort): NotSortable reports nothing.
    enum class UiSortState : u8
    {
        NotSortable,
        None,
        Ascending,
        Descending,
    };
    [[nodiscard]] SPITE_UI_API std::string_view uiSortStateName(
        UiSortState state); // "", "none", "ascending", "descending"

    struct UiAccessible
    {
        UiRole role = UiRole::None;
        std::string name; // empty: none could be derived
        InternedString nameKey{}; // the localization key the name came from, when it did
        bool hasSelected = false; // the element is one of a set of choices
        bool selected = false;
        UiSortState sort = UiSortState::NotSortable; // a table view's column header
        UiExpandState expand = UiExpandState::None; // a row that opens and closes content
    };

    // The role and name of an element. Role: the explicit UiAccessibleComponent role, else from the
    // components (menu item, tree row, ...), else from the widget kind. Name: the explicit nameKey (resolved now, so
    // it follows the locale) or name, else the element's own text (a button, label, menu row; localized text
    // already follows the locale), else for a checkbox, toggle or radio the text of its label child. An input, slider
    // or number field has no text of its own: it takes an explicit name.
    [[nodiscard]] SPITE_UI_API UiAccessible uiAccessibleOf(const UiHierarchyManager& hierarchy, Entity element);
} // namespace spite
