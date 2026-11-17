#pragma once

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

    // The runtime half of a checkbox, toggle, slider, progress bar or radio group, owned by its scope
    // and keyed by the root. The state component on the root is the truth; the `shown*` fields are what the parts
    // currently display, so applying a raw write twice, or after the widget's own write, changes nothing.
    class UiValueState
    {
    public:
        static constexpr u32 NONE = static_cast<u32>(-1);

        // A radio item's parts; its root is the element the keys and the tab stop belong to.
        struct Item
        {
            Entity root = Entity::undefined();
            Entity box = Entity::undefined();
            Entity mark = Entity::undefined();
            Entity label = Entity::undefined();
            bool disabled = false; // set on the item itself; its group being disabled and enabled leaves it
            Entity content = Entity::undefined(); // a tab's content widget, shown while the tab is active
        };

        UiValueState(UiScope& owner, const UiWidgetKind widgetKind, const Entity rootElement) :
            scope(&owner), kind(widgetKind), root(rootElement)
        {
        }

        // Checkbox/toggle: box (toggle: track), mark (toggle: thumb) and label. Slider: bar, fill, thumb, rest.
        // Progress bar: fill, rest.
        void build(std::string_view text, bool checked);
        void buildSlider(const UiSliderDesc& desc);
        void buildProgress(const UiProgressDesc& desc);
        void buildGroup();
        void buildTabBar();
        Radio addItem(InternedString id, std::string_view label);
        Tab addTab(InternedString id, std::string_view label);
        // The tab bar's active tab, or the radio group's selection: the component the kind keeps it in.
        [[nodiscard]] i32 selectionOf() const;
        [[nodiscard]] bool isGroup() const { return kind == UiWidgetKind::RadioGroup || kind == UiWidgetKind::TabBar; }

        // Applies the state component to the parts when it differs from what they show; corrects a component
        // value that is not normalized. Never fires a callback.
        void sync();

        // The input paths: write the component, apply, and fire the callback when the value changed.
        bool setChecked(bool checked, bool notify);
        bool setSlider(double value, bool notify);
        bool select(i32 index, bool notify);
        // Programmatic range changes (no callback): re-normalizes the value.
        void setSliderRange(double min, double max, double step);
        void setProgress(double value, double min, double max);

        // uiActivate setValue: why the text is not a value of this widget, or empty. A group takes an item index
        // or id; -1 clears the selection and fires nothing (onSelect reports an item, never none). `target` is the
        // element the command named (a radio item selects itself).
        [[nodiscard]] std::string setFromText(Entity target, std::string_view text);

        void applySelection();
        // A tab bar's tabs and labels take the root's class family (partClass); after the root's class changed.
        void restyle();
        void refreshTabStops();
        [[nodiscard]] u32 itemIndexOf(Entity element) const;

        UiScope* scope = nullptr;
        UiWidgetKind kind = UiWidgetKind::Checkbox;
        Entity root = Entity::undefined();
        Entity box = Entity::undefined();
        Entity mark = Entity::undefined();
        Entity label = Entity::undefined();
        Entity bar = Entity::undefined();
        Entity fill = Entity::undefined();
        Entity thumb = Entity::undefined();
        Entity rest = Entity::undefined();
        std::vector<Item> items;

        bool shownChecked = false;
        double shownValue = 0.0;
        double shownMin = 0.0;
        double shownMax = 1.0;
        i32 shownSelected = -1;
        float grabOffset = 0.0f; // pointer minus thumb centre when a slider drag began, layout pixels

        std::function<void(bool)> onToggle;
        std::function<void(double)> onValue;
        std::function<void(u32)> onSelect;
        std::function<void(u32)> onReselect; // a tab bar: input on the tab that is already active

    private:
        Entity makePart(InternedString id, UiWidgetKind partKind, const UiRectDesc& desc, Entity parent,
                        const char* styleClass);
        Entity makeLabel(InternedString id, std::string_view text, Entity parent, const char* styleClass);
        [[nodiscard]] InternedString familyOf(std::string_view own) const;
        void writeSelection(i32 index);
        void applyChecked();
        void applyFill();
        void bindPointer(Entity part, u32 itemIndex);
        void bindKeys(Entity element, u32 itemIndex);
        bool key(const UiKeyDownEvent& event, u32 itemIndex);
        void setFromPointer(Vec2 pointer);
        void focusRoot(u32 itemIndex) const;
    };
} // namespace spite
