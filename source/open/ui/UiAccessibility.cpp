#include "ui/UiAccessibility.hpp"

#include <algorithm>
#include <iterator>
#include <span>

#include "ecs/World.hpp"
#include "localization/Localization.hpp"
#include "ui/UiHierarchyManager.hpp"

namespace spite
{
    namespace
    {
        UiRole roleOfKind(const UiWidgetKind kind)
        {
            static_assert(static_cast<u8>(UiWidgetKind::Chart) == 19, "a new widget kind needs its role here");
            switch (kind)
            {
            case UiWidgetKind::Button:
                return UiRole::Button;
            case UiWidgetKind::Label:
                return UiRole::Label;
            case UiWidgetKind::TextInput:
                return UiRole::TextBox;
            case UiWidgetKind::NumberField:
                return UiRole::SpinButton;
            case UiWidgetKind::Checkbox:
                return UiRole::CheckBox;
            case UiWidgetKind::Toggle:
                return UiRole::Switch;
            case UiWidgetKind::Slider:
                return UiRole::Slider;
            case UiWidgetKind::ProgressBar:
                return UiRole::ProgressBar;
            case UiWidgetKind::RadioGroup:
                return UiRole::RadioGroup;
            case UiWidgetKind::Radio:
                return UiRole::Radio;
            case UiWidgetKind::Image:
                return UiRole::Image;
            case UiWidgetKind::ScrollView:
                return UiRole::ScrollArea;
            case UiWidgetKind::Canvas:
                return UiRole::Canvas;
            case UiWidgetKind::Dropdown:
            case UiWidgetKind::Combo:
                return UiRole::ComboBox;
            case UiWidgetKind::TabBar:
                return UiRole::TabList;
            case UiWidgetKind::Tab:
                return UiRole::Tab;
            case UiWidgetKind::Chart:
                return UiRole::Chart;
            case UiWidgetKind::Panel:
            case UiWidgetKind::ScrollThumb:
                return UiRole::None;
            }
        }

        // A table's focusable rows list stands for the table (a grid): it carries the role and takes the name set on
        // the table (a table without rows has no list shown: the table itself is the grid and the stop).
        bool isTableList(const UiHierarchyManager& hierarchy, const Entity element)
        {
            const World& world = hierarchy.world();
            const Entity parent = hierarchy.parentOf(element);
            const auto* identity = world.tryGetConstComponent<UiIdentityComponent>(element);
            return identity && identity->kind == UiWidgetKind::ScrollView && parent != Entity::undefined() &&
                world.hasComponent<UiTableComponent>(parent);
        }

        UiRole roleOf(const UiHierarchyManager& hierarchy, const Entity element)
        {
            const World& world = hierarchy.world();
            if (const auto* accessible = world.tryGetConstComponent<UiAccessibleComponent>(element);
                accessible && accessible->role != UiRole::None)
            {
                return accessible->role;
            }
            if (const auto* item = world.tryGetConstComponent<UiMenuItemComponent>(element))
            {
                return item->separator ? UiRole::None : item->checkable ? UiRole::MenuItemCheckBox : UiRole::MenuItem;
            }
            if (world.hasComponent<UiMenuComponent>(element))
            {
                return UiRole::Menu;
            }
            if (world.hasComponent<UiTreeRowComponent>(element))
            {
                return UiRole::TreeItem;
            }
            if (world.hasComponent<UiTreeComponent>(element))
            {
                return UiRole::Tree;
            }
            if (world.hasComponent<UiTableRowComponent>(element))
            {
                return UiRole::Row;
            }
            if (isTableList(hierarchy, element))
            {
                return UiRole::Grid;
            }
            if (const auto* state = world.tryGetConstComponent<UiStateComponent>(element);
                state && state->tabStop && world.hasComponent<UiTableComponent>(element))
            {
                return UiRole::Grid; // a table without rows is its own focusable grid: its list is hidden
            }
            const auto* identity = world.tryGetConstComponent<UiIdentityComponent>(element);
            return identity ? roleOfKind(identity->kind) : UiRole::None;
        }

        constexpr u32 LABEL_SEARCH_DEPTH = 3;

        const std::string* textOf(const World& world, const Entity element)
        {
            const auto* label = world.tryGetConstComponent<UiLabelComponent>(element);
            return label && !label->text.empty() ? &label->text : nullptr;
        }

        // the text of the first label under the element, in child order, `depth` levels down; decoration (a tree
        // row's expander glyph) is no name source
        const std::string* labelBelow(const UiHierarchyManager& hierarchy, const Entity element, const u32 depth)
        {
            for (const Entity child : hierarchy.childrenOf(element))
            {
                const auto* own = hierarchy.world().tryGetConstComponent<UiAccessibleComponent>(child);
                if (own && own->role == UiRole::Presentation)
                {
                    continue;
                }
                if (const std::string* text = textOf(hierarchy.world(), child))
                {
                    return text;
                }
                if (depth > 1)
                {
                    if (const std::string* text = labelBelow(hierarchy, child, depth - 1))
                    {
                        return text;
                    }
                }
            }
            return nullptr;
        }

        // the text of the first value (a table cell's) under the element, `depth` levels down
        const std::string* valueBelow(const UiHierarchyManager& hierarchy, const Entity element, const u32 depth)
        {
            for (const Entity child : hierarchy.childrenOf(element))
            {
                const auto* value = hierarchy.world().tryGetConstComponent<UiValueComponent>(child);
                if (value && !value->text.empty())
                {
                    return &value->text;
                }
                if (depth > 1)
                {
                    if (const std::string* text = valueBelow(hierarchy, child, depth - 1))
                    {
                        return text;
                    }
                }
            }
            return nullptr;
        }

        // the text of the nearest label among the siblings before the element
        const std::string* labelBefore(const UiHierarchyManager& hierarchy, const Entity element)
        {
            const Entity parent = hierarchy.parentOf(element);
            if (parent == Entity::undefined())
            {
                return nullptr;
            }
            const std::span<const Entity> siblings = hierarchy.childrenOf(parent);
            const auto self = std::ranges::find(siblings, element);
            for (auto it = std::make_reverse_iterator(self); it != siblings.rend(); ++it)
            {
                const auto* identity = hierarchy.world().tryGetConstComponent<UiIdentityComponent>(*it);
                if (identity && identity->kind == UiWidgetKind::Label)
                {
                    if (const std::string* text = textOf(hierarchy.world(), *it))
                    {
                        return text;
                    }
                }
            }
            return nullptr;
        }
    } // namespace

    std::string_view uiRoleName(const UiRole role)
    {
        switch (role)
        {
        case UiRole::Button:
            return "button";
        case UiRole::TextBox:
            return "textbox";
        case UiRole::SpinButton:
            return "spinbutton";
        case UiRole::CheckBox:
            return "checkbox";
        case UiRole::Switch:
            return "switch";
        case UiRole::Slider:
            return "slider";
        case UiRole::ProgressBar:
            return "progressbar";
        case UiRole::RadioGroup:
            return "radiogroup";
        case UiRole::Radio:
            return "radio";
        case UiRole::Image:
            return "image";
        case UiRole::ScrollArea:
            return "scrollarea";
        case UiRole::Menu:
            return "menu";
        case UiRole::MenuItem:
            return "menuitem";
        case UiRole::MenuItemCheckBox:
            return "menuitemcheckbox";
        case UiRole::Tree:
            return "tree";
        case UiRole::TreeItem:
            return "treeitem";
        case UiRole::Table:
            return "table";
        case UiRole::Row:
            return "row";
        case UiRole::Label:
            return "label";
        case UiRole::Canvas:
            return "canvas";
        case UiRole::ComboBox:
            return "combobox";
        case UiRole::TabList:
            return "tablist";
        case UiRole::Tab:
            return "tab";
        case UiRole::Chart:
            return "chart";
        case UiRole::Option:
            return "option";
        case UiRole::Presentation:
            return "presentation";
        case UiRole::ListBox:
            return "listbox";
        case UiRole::Grid:
            return "grid";
        case UiRole::ColumnHeader:
            return "columnheader";
        case UiRole::GridCell:
            return "gridcell";
        case UiRole::Alert:
            return "alert";
        case UiRole::Status:
            return "status";
        case UiRole::Dialog:
            return "dialog";
        case UiRole::AlertDialog:
            return "alertdialog";
        case UiRole::Separator:
            return "separator";
        case UiRole::Application:
            return "application";
        case UiRole::None:
        default:
            return {};
        }
    }

    std::string_view uiSortStateName(const UiSortState state)
    {
        switch (state)
        {
        case UiSortState::None:
            return "none";
        case UiSortState::Ascending:
            return "ascending";
        case UiSortState::Descending:
            return "descending";
        case UiSortState::NotSortable:
        default:
            return {};
        }
    }

    bool uiRoleIsInteractive(const UiRole role)
    {
        switch (role)
        {
        case UiRole::Button:
        case UiRole::TextBox:
        case UiRole::SpinButton:
        case UiRole::CheckBox:
        case UiRole::Switch:
        case UiRole::Slider:
        case UiRole::Radio:
        case UiRole::MenuItem:
        case UiRole::MenuItemCheckBox:
        case UiRole::TreeItem:
        case UiRole::Tree:
        case UiRole::Table:
        case UiRole::Grid:
        case UiRole::ComboBox:
        case UiRole::Tab:
        case UiRole::Option:
        case UiRole::Separator:
        case UiRole::Application:
            return true;
        default:
            return false;
        }
    }

    UiAccessible uiAccessibleOf(const UiHierarchyManager& hierarchy, const Entity element)
    {
        const World& world = hierarchy.world();
        UiAccessible out;
        out.role = roleOf(hierarchy, element);
        if (out.role == UiRole::Presentation)
        {
            return out;
        }
        if (const auto* own = world.tryGetConstComponent<UiAccessibleComponent>(element))
        {
            out.hasSelected = own->hasSelected;
            out.selected = own->selected;
            out.expand = own->expand;
        }
        if (out.role == UiRole::ColumnHeader)
        {
            const auto* column = world.tryGetConstComponent<UiTableColumnComponent>(element);
            const Entity tableRoot = hierarchy.parentOf(hierarchy.parentOf(element)); // cell, header, root
            const auto* state =
                tableRoot != Entity::undefined() ? world.tryGetConstComponent<UiTableComponent>(tableRoot) : nullptr;
            if (column && state && column->sortable)
            {
                out.sort = state->sortColumn != column->field ? UiSortState::None
                    : state->descending                       ? UiSortState::Descending
                                                              : UiSortState::Ascending;
            }
        }
        const Entity named = out.role == UiRole::Grid && isTableList(hierarchy, element) &&
                !world.hasComponent<UiAccessibleComponent>(element)
            ? hierarchy.parentOf(element)
            : element;
        if (const auto* accessible = world.tryGetConstComponent<UiAccessibleComponent>(named))
        {
            const Localization* localization = hierarchy.localization();
            if (accessible->nameKey.isValid() && localization && localization->contains(accessible->nameKey))
            {
                std::string scratch;
                out.name = std::string(localization->resolve(accessible->nameKey, scratch));
                out.nameKey = accessible->nameKey;
                return out;
            }
            if (!accessible->name.empty())
            {
                out.name = accessible->name;
                return out;
            }
        }
        if (const std::string* own = textOf(world, element))
        {
            out.name = *own;
            return out;
        }
        switch (out.role)
        {
        case UiRole::Button:
        case UiRole::CheckBox:
        case UiRole::Switch:
        case UiRole::Radio:
        case UiRole::MenuItem:
        case UiRole::MenuItemCheckBox:
        case UiRole::TreeItem:
        case UiRole::Row:
        case UiRole::Tab:
            if (const std::string* text = labelBelow(hierarchy, element, LABEL_SEARCH_DEPTH))
            {
                out.name = *text;
            }
            else if (out.role == UiRole::Row && world.hasComponent<UiTableRowComponent>(element))
            {
                // a table row is named by its first cell with text
                for (const Entity cell : hierarchy.childrenOf(element))
                {
                    if (const std::string* value = valueBelow(hierarchy, cell, 1); value)
                    {
                        out.name = *value;
                        break;
                    }
                }
            }
            break;
        case UiRole::GridCell:
            if (const std::string* text = valueBelow(hierarchy, element, LABEL_SEARCH_DEPTH))
            {
                out.name = *text;
            }
            break;
        case UiRole::TextBox:
        case UiRole::SpinButton:
        case UiRole::Slider:
        case UiRole::ComboBox:
            // a field is named by the label placed before it in its parent
            if (const std::string* text = labelBefore(hierarchy, element))
            {
                out.name = *text;
            }
            break;
        default:
            break;
        }
        return out;
    }
} // namespace spite
