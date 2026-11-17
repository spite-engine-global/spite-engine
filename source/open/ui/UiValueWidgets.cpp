#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <string>

#include "base/Assert.hpp"
#include "ecs/World.hpp"
#include "ui/UiScope.hpp"
#include "UiDropdownState.hpp"
#include "UiPartClass.hpp"
#include "UiValueState.hpp"

// Checkbox, toggle, slider, progress bar and radio group. Each is a root element holding a state
// component plus a few plain parts; see UiValueState.hpp.

namespace spite
{
    namespace
    {
        // HID scancodes (the UiKeyTransition domain).
        namespace Key
        {
            constexpr u32 RETURN = 40;
            constexpr u32 SPACE = 44;
            constexpr u32 HOME = 74;
            constexpr u32 PAGE_UP = 75;
            constexpr u32 END = 77;
            constexpr u32 PAGE_DOWN = 78;
            constexpr u32 RIGHT = 79;
            constexpr u32 LEFT = 80;
            constexpr u32 DOWN = 81;
            constexpr u32 UP = 82;
            constexpr u32 KP_ENTER = 88;
        } // namespace Key

        constexpr float INDICATOR = 16.0f; // checkbox box, radio circle, slider thumb and toggle thumb
        constexpr float MARK = 8.0f;
        constexpr float CHOICE_GAP = 6.0f;
        constexpr Vec2 TOGGLE_TRACK{36.0f, 20.0f};
        constexpr float TOGGLE_PADDING = 2.0f;
        constexpr Vec2 SLIDER_SIZE{160.0f, 20.0f};
        constexpr float SLIDER_BAR_HEIGHT = 4.0f;
        constexpr Vec2 PROGRESS_SIZE{160.0f, 8.0f};
        constexpr double PAGE_STEPS = 10.0;
        constexpr double POINTER_EPSILON = 1e-5; // fraction of the track
        constexpr float TAB_PADDING_X = 12.0f;
        constexpr float TAB_PADDING_Y = 5.0f;
        constexpr double DEFAULT_STEPS = 100.0; // a continuous slider's arrow key moves 1 percent

        InternedString childId(const InternedString id, const std::string_view suffix)
        {
            return InternedString(std::string(id.toView()) + std::string(suffix));
        }

        struct Range
        {
            double min;
            double max;
        };

        // A non-finite bound reads as its default; min above max swap.
        Range normalRange(double min, double max)
        {
            min = std::isfinite(min) ? min : 0.0;
            max = std::isfinite(max) ? max : 1.0;
            return min > max ? Range{max, min} : Range{min, max};
        }

        double normalStep(const double step) { return std::isfinite(step) && step > 0.0 ? step : 0.0; }

        // A non-finite value keeps `fallback`; the value clamps to the range; a positive step snaps it to the nearest
        // min + n * step, or to max when that is nearer (so the maximum is reachable off the grid).
        double normalValue(double value, const double fallback, const Range range, const double step)
        {
            value = std::clamp(std::isfinite(value) ? value : fallback, range.min, range.max);
            if (step > 0.0)
            {
                const double snapped = std::min(range.min + std::round((value - range.min) / step) * step, range.max);
                value = std::abs(value - range.max) < std::abs(value - snapped) ? range.max : snapped;
            }
            return value;
        }

        UiSliderComponent normalized(const UiSliderComponent& component, const double fallback)
        {
            const Range range = normalRange(component.min, component.max);
            const double step = normalStep(component.step);
            return {.value = normalValue(component.value, fallback, range, step),
                    .min = range.min,
                    .max = range.max,
                    .step = step};
        }

        bool same(const UiSliderComponent& a, const UiSliderComponent& b)
        {
            return a.value == b.value && a.min == b.min && a.max == b.max && a.step == b.step;
        }

        // 1 percent of the range; max - min overflows for bounds like +-1e308, the halves do not.
        double keyUnit(const double min, const double max) { return (max * 0.5 - min * 0.5) / (DEFAULT_STEPS * 0.5); }

        float fractionOf(const double value, const double min, const double max)
        {
            if (!(max > min))
            {
                return 0.0f;
            }
            const double span = max - min;
            const double fraction =
                std::isfinite(span) ? (value - min) / span : (value * 0.5 - min * 0.5) / (max * 0.5 - min * 0.5);
            return std::isfinite(fraction) ? static_cast<float>(std::clamp(fraction, 0.0, 1.0)) : 0.0f;
        }

        double lerpRange(const Range range, const double fraction)
        {
            const double span = range.max - range.min;
            return std::isfinite(span) ? range.min + fraction * span
                                       : range.min * (1.0 - fraction) + range.max * fraction;
        }

        bool parseNumber(const std::string_view text, double& out)
        {
            const char* end = text.data() + text.size();
            const auto [stop, error] = std::from_chars(text.data(), end, out);
            return error == std::errc{} && stop == end && std::isfinite(out);
        }

        bool parseBool(const std::string_view text, bool& out)
        {
            std::string lower(text);
            std::ranges::transform(lower, lower.begin(), [](const unsigned char c) { return std::tolower(c); });
            if (lower == "true" || lower == "on" || lower == "1" || lower == "checked" || lower == "yes")
            {
                out = true;
                return true;
            }
            if (lower == "false" || lower == "off" || lower == "0" || lower == "unchecked" || lower == "no")
            {
                out = false;
                return true;
            }
            return false;
        }

        UiLayoutComponent rowLayout(const float gap = 0.0f, const UiLayoutAlign align = UiLayoutAlign::Center)
        {
            UiLayoutComponent layout;
            layout.direction = UiLayoutDirection::Row;
            layout.gap = gap;
            layout.align = align;
            return layout;
        }

        UiRectDesc sized(const Vec2 size)
        {
            UiRectDesc desc;
            desc.size = size;
            return desc;
        }

        UiRectDesc item(const UiLayoutItemComponent& layoutItem, const Vec2 size = {})
        {
            UiRectDesc desc = sized(size);
            desc.layoutItem = layoutItem;
            return desc;
        }

        UiLayoutItemComponent fixedItem()
        {
            UiLayoutItemComponent layoutItem;
            layoutItem.shrink = 0.0f;
            return layoutItem;
        }

        UiLayoutItemComponent growItem(const float grow)
        {
            UiLayoutItemComponent layoutItem;
            layoutItem.grow = grow;
            layoutItem.shrink = 0.0f;
            layoutItem.basis = 0.0f;
            layoutItem.alignSelf = UiLayoutAlign::Stretch;
            return layoutItem;
        }

        bool interactableNow(const World& world, const Entity element)
        {
            const auto* state = world.tryGetConstComponent<UiStateComponent>(element);
            return !state || state->interactable;
        }

        // The callback is copied first: it may destroy the widget it belongs to.
        template <typename Fn, typename... Args>
        void fire(const Fn& callback, Args&&... args)
        {
            if (callback)
            {
                const Fn local = callback;
                local(std::forward<Args>(args)...);
            }
        }
    } // namespace

    // -- UiValueState: building ---------------------------------------------------------------------------------

    Entity UiValueState::makePart(const InternedString id, const UiWidgetKind partKind, const UiRectDesc& desc,
                                  const Entity parent, const char* styleClass)
    {
        const Entity part = scope->createElement(id, partKind, desc);
        scope->world().getComponent<UiIdentityComponent>(part).styleClass = InternedString(styleClass);
        scope->hierarchy().attach(part, parent);
        scope->m_valueParts[part.id()] = root.id();
        return part;
    }

    Entity UiValueState::makeLabel(const InternedString id, const std::string_view text, const Entity parent,
                                   const char* styleClass)
    {
        UiRectDesc desc;
        desc.contentSize = UiContentSizeComponent{};
        const Entity part = scope->createLabel(id, text, desc).entity();
        scope->world().getComponent<UiIdentityComponent>(part).styleClass = InternedString(styleClass);
        scope->hierarchy().attach(part, parent);
        scope->m_valueParts[part.id()] = root.id();
        return part;
    }

    void UiValueState::build(const std::string_view text, const bool checked)
    {
        const InternedString id = scope->world().getConstComponent<UiIdentityComponent>(root).id;
        const bool toggle = kind == UiWidgetKind::Toggle;
        if (toggle)
        {
            UiRectDesc track = item(fixedItem(), TOGGLE_TRACK);
            UiLayoutComponent layout = rowLayout();
            layout.padding = Vec4{TOGGLE_PADDING, TOGGLE_PADDING, TOGGLE_PADDING, TOGGLE_PADDING};
            track.layout = layout;
            box = makePart(childId(id, ".track"), UiWidgetKind::Panel, track, root, "toggle.track");
            mark = makePart(childId(id, ".thumb"), UiWidgetKind::Panel, item(fixedItem(), Vec2{INDICATOR, INDICATOR}),
                            box, "toggle.thumb");
        }
        else
        {
            box = makePart(childId(id, ".box"), UiWidgetKind::Panel, item(fixedItem(), Vec2{INDICATOR, INDICATOR}),
                           root, "checkbox.box");
            UiRectDesc markDesc = sized(Vec2{MARK, MARK});
            markDesc.anchor = Vec2{0.5f, 0.5f};
            mark = makePart(childId(id, ".mark"), UiWidgetKind::Panel, markDesc, box, "checkbox.mark");
        }
        label = makeLabel(childId(id, ".label"), text, root, toggle ? "toggle.label" : "checkbox.label");
        shownChecked = checked;
        applyChecked();
        for (const Entity part : {root, box, mark, label})
        {
            bindPointer(part, NONE);
        }
        bindKeys(root, NONE);
    }

    void UiValueState::buildSlider(const UiSliderDesc& desc)
    {
        const InternedString id = scope->world().getConstComponent<UiIdentityComponent>(root).id;
        const UiSliderComponent value =
            normalized({.value = desc.value, .min = desc.min, .max = desc.max, .step = desc.step}, 0.0);
        scope->world().addComponent<UiSliderComponent>(root, value);
        shownValue = value.value;
        shownMin = value.min;
        shownMax = value.max;
        const float fraction = fractionOf(shownValue, shownMin, shownMax);

        UiRectDesc barDesc = sized(Vec2{0.0f, SLIDER_BAR_HEIGHT});
        UiLayoutItemComponent barItem;
        barItem.grow = 1.0f;
        barItem.basis = 0.0f;
        barDesc.layoutItem = barItem;
        barDesc.layout = rowLayout();
        bar = makePart(childId(id, ".bar"), UiWidgetKind::Panel, barDesc, root, "slider.bar");
        fill = makePart(childId(id, ".fill"), UiWidgetKind::Panel, item(growItem(fraction)), bar, "slider.fill");
        thumb = makePart(childId(id, ".thumb"), UiWidgetKind::Panel, item(fixedItem(), Vec2{INDICATOR, INDICATOR}), bar,
                         "slider.thumb");
        rest = makePart(childId(id, ".rest"), UiWidgetKind::Panel, item(growItem(1.0f - fraction)), bar, "slider.rest");
        for (const Entity part : {root, bar, fill, thumb, rest})
        {
            bindPointer(part, NONE);
        }
        bindKeys(root, NONE);
    }

    void UiValueState::buildProgress(const UiProgressDesc& desc)
    {
        const InternedString id = scope->world().getConstComponent<UiIdentityComponent>(root).id;
        const Range range = normalRange(desc.min, desc.max);
        UiProgressComponent value;
        value.min = range.min;
        value.max = range.max;
        value.value = normalValue(desc.value, 0.0, range, 0.0);
        scope->world().addComponent<UiProgressComponent>(root, value);
        shownValue = value.value;
        shownMin = value.min;
        shownMax = value.max;
        const float fraction = fractionOf(shownValue, shownMin, shownMax);
        fill = makePart(childId(id, ".fill"), UiWidgetKind::Panel, item(growItem(fraction)), root, "progress.fill");
        rest =
            makePart(childId(id, ".rest"), UiWidgetKind::Panel, item(growItem(1.0f - fraction)), root, "progress.rest");
    }

    void UiValueState::buildGroup()
    {
        scope->world().addComponent<UiRadioGroupComponent>(root, UiRadioGroupComponent{});
    }

    Radio UiValueState::addItem(const InternedString id, const std::string_view text)
    {
        const auto index = static_cast<u32>(items.size());
        UiRectDesc rect;
        UiLayoutComponent layout = rowLayout(CHOICE_GAP);
        rect.layout = layout;
        rect.contentSize = UiContentSizeComponent{};
        Item added;
        added.root = scope->createElement(id, UiWidgetKind::Radio, rect);
        scope->hierarchy().attach(added.root, root);
        scope->m_valueParts[added.root.id()] = root.id();
        scope->world().addComponent<UiRadioItemComponent>(added.root, UiRadioItemComponent{.index = index});
        added.box = makePart(childId(id, ".box"), UiWidgetKind::Panel, item(fixedItem(), Vec2{INDICATOR, INDICATOR}),
                             added.root, "radio.box");
        UiRectDesc markDesc = sized(Vec2{MARK, MARK});
        markDesc.anchor = Vec2{0.5f, 0.5f};
        added.mark = makePart(childId(id, ".mark"), UiWidgetKind::Panel, markDesc, added.box, "radio.mark");
        scope->world().getComponent<UiStateComponent>(added.mark).visible = false;
        added.label = makeLabel(childId(id, ".label"), text, added.root, "radio.label");
        for (const Entity part : {added.root, added.box, added.mark, added.label})
        {
            bindPointer(part, index);
        }
        bindKeys(added.root, index);
        items.push_back(added);
        if (!interactableNow(scope->world(), root))
        {
            scope->propagateInteractable(root, false);
        }
        applySelection();
        return Radio(scope, added.root, scope->generation());
    }

    void UiValueState::buildTabBar() { scope->world().addComponent<UiTabBarComponent>(root, UiTabBarComponent{}); }

    Tab UiValueState::addTab(const InternedString id, const std::string_view text)
    {
        const auto index = static_cast<u32>(items.size());
        UiRectDesc rect;
        UiLayoutComponent layout = rowLayout();
        layout.padding = Vec4{TAB_PADDING_X, TAB_PADDING_Y, TAB_PADDING_X, TAB_PADDING_Y};
        rect.layout = layout;
        rect.contentSize = UiContentSizeComponent{};
        Item added;
        added.root = scope->createElement(id, UiWidgetKind::Tab, rect);
        scope->world().getComponent<UiIdentityComponent>(added.root).styleClass = familyOf("tabbar.tab");
        scope->hierarchy().attach(added.root, root);
        scope->m_valueParts[added.root.id()] = root.id();
        scope->world().addComponent<UiTabComponent>(added.root, UiTabComponent{.index = index});
        added.label = makeLabel(childId(id, ".label"), text, added.root, "tabbar.label");
        scope->world().getComponent<UiIdentityComponent>(added.label).styleClass = familyOf("tabbar.label");
        for (const Entity part : {added.root, added.label})
        {
            bindPointer(part, index);
        }
        bindKeys(added.root, index);
        items.push_back(added);
        if (!interactableNow(scope->world(), root))
        {
            scope->propagateInteractable(root, false);
        }
        applySelection();
        return Tab(scope, added.root, scope->generation());
    }

    InternedString UiValueState::familyOf(const std::string_view own) const
    {
        return partClass(scope->world().getConstComponent<UiIdentityComponent>(root).styleClass, own);
    }

    void UiValueState::restyle()
    {
        if (kind != UiWidgetKind::TabBar)
        {
            return;
        }
        World& world = scope->world();
        const InternedString labelClass = familyOf("tabbar.label");
        for (const Item& entry : items)
        {
            const bool active = world.getConstComponent<UiTabComponent>(entry.root).active;
            const InternedString tabClass = familyOf(active ? "tabbar.tab.active" : "tabbar.tab");
            for (const auto& [part, wanted] : {std::pair{entry.root, tabClass}, std::pair{entry.label, labelClass}})
            {
                auto& identity = world.getComponent<UiIdentityComponent>(part);
                if (identity.styleClass != wanted)
                {
                    identity.styleClass = wanted;
                    scope->markLayoutDirty(part);
                }
            }
        }
    }

    i32 UiValueState::selectionOf() const
    {
        const World& world = scope->world();
        return kind == UiWidgetKind::TabBar ? world.getConstComponent<UiTabBarComponent>(root).active
                                            : world.getConstComponent<UiRadioGroupComponent>(root).selected;
    }

    void UiValueState::writeSelection(const i32 index)
    {
        World& world = scope->world();
        if (kind == UiWidgetKind::TabBar)
        {
            world.getComponent<UiTabBarComponent>(root).active = index;
        }
        else
        {
            world.getComponent<UiRadioGroupComponent>(root).selected = index;
        }
    }

    // -- UiValueState: applying state ---------------------------------------------------------------------------

    void UiValueState::applyChecked()
    {
        World& world = scope->world();
        if (kind == UiWidgetKind::Checkbox)
        {
            if (world.getConstComponent<UiStateComponent>(mark).visible != shownChecked)
            {
                world.getComponent<UiStateComponent>(mark).visible = shownChecked;
                scope->markLayoutDirty(mark);
            }
            return;
        }
        const UiLayoutJustify justify = shownChecked ? UiLayoutJustify::End : UiLayoutJustify::Start;
        if (world.getConstComponent<UiLayoutComponent>(box).justify != justify)
        {
            world.getComponent<UiLayoutComponent>(box).justify = justify;
        }
        static const InternedString off("toggle.track");
        static const InternedString on("toggle.track.on");
        world.getComponent<UiIdentityComponent>(box).styleClass = shownChecked ? on : off;
        scope->markLayoutDirty(box);
    }

    void UiValueState::applyFill()
    {
        const float fraction = fractionOf(shownValue, shownMin, shownMax);
        World& world = scope->world();
        for (const auto& [part, grow] : {std::pair{fill, fraction}, std::pair{rest, 1.0f - fraction}})
        {
            if (world.getConstComponent<UiLayoutItemComponent>(part).grow != grow)
            {
                world.getComponent<UiLayoutItemComponent>(part).grow = grow;
                scope->markLayoutDirty(part);
            }
        }
    }

    void UiValueState::applySelection()
    {
        World& world = scope->world();
        const bool tabs = kind == UiWidgetKind::TabBar;
        for (u32 i = 0; i < items.size(); ++i)
        {
            const bool checked = static_cast<i32>(i) == shownSelected;
            const Item& entry = items[i];
            if (tabs)
            {
                if (world.getConstComponent<UiTabComponent>(entry.root).active != checked)
                {
                    world.getComponent<UiTabComponent>(entry.root).active = checked;
                    world.getComponent<UiIdentityComponent>(entry.root).styleClass =
                        familyOf(checked ? "tabbar.tab.active" : "tabbar.tab");
                    scope->markLayoutDirty(entry.root);
                }
                if (scope->hierarchy().isElement(entry.content) &&
                    world.getConstComponent<UiStateComponent>(entry.content).visible != checked)
                {
                    world.getComponent<UiStateComponent>(entry.content).visible = checked;
                    scope->markLayoutDirty(entry.content);
                }
                continue;
            }
            if (world.getConstComponent<UiRadioItemComponent>(entry.root).checked != checked)
            {
                world.getComponent<UiRadioItemComponent>(entry.root).checked = checked;
            }
            if (world.getConstComponent<UiStateComponent>(entry.mark).visible != checked)
            {
                world.getComponent<UiStateComponent>(entry.mark).visible = checked;
                scope->markLayoutDirty(entry.mark);
            }
        }
        refreshTabStops();
    }

    void UiValueState::refreshTabStops()
    {
        World& world = scope->world();
        u32 target = NONE;
        if (shownSelected >= 0 && shownSelected < static_cast<i32>(items.size()) &&
            interactableNow(world, items[static_cast<u32>(shownSelected)].root))
        {
            target = static_cast<u32>(shownSelected);
        }
        else
        {
            for (u32 i = 0; i < items.size() && target == NONE; ++i)
            {
                target = interactableNow(world, items[i].root) ? i : NONE;
            }
        }
        for (u32 i = 0; i < items.size(); ++i)
        {
            const bool stop = i == target;
            if (world.getConstComponent<UiStateComponent>(items[i].root).tabStop != stop)
            {
                world.getComponent<UiStateComponent>(items[i].root).tabStop = stop;
            }
        }
    }

    void UiValueState::sync()
    {
        World& world = scope->world();
        switch (kind)
        {
        case UiWidgetKind::Checkbox:
        case UiWidgetKind::Toggle:
            {
                const bool checked = world.getConstComponent<UiToggleComponent>(root).checked;
                if (checked != shownChecked)
                {
                    shownChecked = checked;
                    applyChecked();
                }
                break;
            }
        case UiWidgetKind::Slider:
            {
                const UiSliderComponent value =
                    normalized(world.getConstComponent<UiSliderComponent>(root), shownValue);
                if (!same(value, world.getConstComponent<UiSliderComponent>(root)))
                {
                    world.getComponent<UiSliderComponent>(root) = value;
                }
                if (value.value != shownValue || value.min != shownMin || value.max != shownMax)
                {
                    shownValue = value.value;
                    shownMin = value.min;
                    shownMax = value.max;
                    applyFill();
                }
                break;
            }
        case UiWidgetKind::ProgressBar:
            {
                const auto& current = world.getConstComponent<UiProgressComponent>(root);
                const Range range = normalRange(current.min, current.max);
                const double value = normalValue(current.value, shownValue, range, 0.0);
                if (value != current.value || range.min != current.min || range.max != current.max)
                {
                    auto& written = world.getComponent<UiProgressComponent>(root);
                    written.value = value;
                    written.min = range.min;
                    written.max = range.max;
                }
                if (value != shownValue || range.min != shownMin || range.max != shownMax)
                {
                    shownValue = value;
                    shownMin = range.min;
                    shownMax = range.max;
                    applyFill();
                }
                break;
            }
        case UiWidgetKind::RadioGroup:
        case UiWidgetKind::TabBar:
            {
                const i32 selected = selectionOf();
                if (selected < -1 || selected >= static_cast<i32>(items.size()))
                {
                    writeSelection(shownSelected);
                }
                else if (selected != shownSelected)
                {
                    shownSelected = selected;
                    applySelection();
                }
                break;
            }
        default:
            break;
        }
    }

    // -- UiValueState: changes ------------------------------------------------------------------------------------

    bool UiValueState::setChecked(const bool checked, const bool notify)
    {
        auto& component = scope->world().getComponent<UiToggleComponent>(root);
        const bool changed = component.checked != checked;
        component.checked = checked;
        shownChecked = checked;
        applyChecked();
        if (changed && notify)
        {
            fire(onToggle, checked);
        }
        return changed;
    }

    bool UiValueState::setSlider(const double value, const bool notify)
    {
        World& world = scope->world();
        const UiSliderComponent before = world.getConstComponent<UiSliderComponent>(root);
        UiSliderComponent after = before;
        after.value = value;
        after = normalized(after, before.value);
        if (!same(after, before))
        {
            world.getComponent<UiSliderComponent>(root) = after;
        }
        shownValue = after.value;
        shownMin = after.min;
        shownMax = after.max;
        applyFill();
        const bool changed = after.value != before.value;
        if (changed && notify)
        {
            fire(onValue, after.value);
        }
        return changed;
    }

    void UiValueState::setSliderRange(const double min, const double max, const double step)
    {
        const UiSliderComponent before = scope->world().getConstComponent<UiSliderComponent>(root);
        UiSliderComponent after = before;
        after.min = min;
        after.max = max;
        after.step = step;
        after = normalized(after, before.value);
        if (!same(after, before))
        {
            scope->world().getComponent<UiSliderComponent>(root) = after;
        }
        shownValue = after.value;
        shownMin = after.min;
        shownMax = after.max;
        applyFill();
    }

    void UiValueState::setProgress(const double value, const double min, const double max)
    {
        const Range range = normalRange(min, max);
        const auto& current = scope->world().getConstComponent<UiProgressComponent>(root);
        const double clamped = normalValue(value, current.value, range, 0.0);
        if (current.value != clamped || current.min != range.min || current.max != range.max)
        {
            auto& written = scope->world().getComponent<UiProgressComponent>(root);
            written.value = clamped;
            written.min = range.min;
            written.max = range.max;
        }
        shownValue = clamped;
        shownMin = range.min;
        shownMax = range.max;
        applyFill();
    }

    bool UiValueState::select(const i32 index, const bool notify)
    {
        if (index < -1 || index >= static_cast<i32>(items.size()))
        {
            return false;
        }
        const bool changed = selectionOf() != index;
        writeSelection(index);
        shownSelected = index;
        applySelection();
        if (changed && notify && index >= 0)
        {
            fire(onSelect, static_cast<u32>(index));
        }
        else if (!changed && notify && index >= 0)
        {
            fire(onReselect, static_cast<u32>(index));
        }
        return changed;
    }

    u32 UiValueState::itemIndexOf(const Entity element) const
    {
        for (u32 i = 0; i < items.size(); ++i)
        {
            if (items[i].root == element)
            {
                return i;
            }
        }
        return NONE;
    }

    std::string UiValueState::setFromText(const Entity target, const std::string_view text)
    {
        World& world = scope->world();
        switch (kind)
        {
        case UiWidgetKind::Checkbox:
        case UiWidgetKind::Toggle:
            {
                bool checked = false;
                if (!parseBool(text, checked))
                {
                    return "value is true or false";
                }
                setChecked(checked, true);
                return {};
            }
        case UiWidgetKind::Slider:
        case UiWidgetKind::ProgressBar:
            {
                double value = 0.0;
                if (!parseNumber(text, value))
                {
                    return "value is a finite number";
                }
                if (kind == UiWidgetKind::Slider)
                {
                    setSlider(value, true);
                }
                else
                {
                    const auto& progress = world.getConstComponent<UiProgressComponent>(root);
                    setProgress(value, progress.min, progress.max);
                }
                return {};
            }
        default:
            break;
        }
        // a group: an item index or item id; an item: true selects it
        u32 index = itemIndexOf(target);
        if (index != NONE)
        {
            bool selected = false;
            if (!parseBool(text, selected) || !selected)
            {
                return "a radio item is set to true; select another item to clear it";
            }
        }
        else
        {
            double number = 0.0;
            if (parseNumber(text, number))
            {
                if (number != std::floor(number) || number < -1.0 || number >= static_cast<double>(items.size()))
                {
                    return "index is -1 or the index of an item";
                }
                if (number < 0.0)
                {
                    select(-1, true);
                    return {};
                }
                index = static_cast<u32>(number);
            }
            else
            {
                for (u32 i = 0; i < items.size() && index == NONE; ++i)
                {
                    const auto& identity = world.getConstComponent<UiIdentityComponent>(items[i].root);
                    index = identity.id.toView() == text ? i : NONE;
                }
                if (index == NONE)
                {
                    return "value is an item index or id";
                }
            }
        }
        if (!interactableNow(world, items[index].root))
        {
            return "the item is not interactable";
        }
        select(static_cast<i32>(index), true);
        return {};
    }

    // -- UiValueState: input ----------------------------------------------------------------------------------------

    void UiValueState::focusRoot(const u32 itemIndex) const
    {
        scope->router().requestFocus(itemIndex == NONE ? root : items[itemIndex].root);
    }

    void UiValueState::setFromPointer(const Vec2 pointer)
    {
        const UiHierarchyManager& hierarchy = scope->hierarchy();
        const UiRect barRect = hierarchy.absoluteRectOf(bar);
        const float thumbWidth = hierarchy.absoluteRectOf(thumb).size.x;
        const float usable = barRect.size.x - thumbWidth;
        const auto& slider = scope->world().getConstComponent<UiSliderComponent>(root);
        const Range range = normalRange(slider.min, slider.max);
        const double fraction = usable > 0.0f
            ? std::clamp(static_cast<double>(pointer.x - grabOffset - barRect.position.x - thumbWidth * 0.5f) /
                             static_cast<double>(usable),
                         0.0, 1.0)
            : 0.0;
        // float pointer arithmetic differs from the stored value by rounding: a press on the thumb must not fire
        if (std::abs(fraction - fractionOf(slider.value, range.min, range.max)) < POINTER_EPSILON)
        {
            return;
        }
        setSlider(lerpRange(range, fraction), true);
    }

    void UiValueState::bindPointer(const Entity part, const u32 itemIndex)
    {
        scope->setReaction(part, UiEventKind::PointerDown,
                           [this, part, itemIndex](const void* payload)
                           {
                               const auto* event = static_cast<const UiPointerDownEvent*>(payload);
                               if (event->button != 0)
                               {
                                   return;
                               }
                               focusRoot(itemIndex);
                               if (kind == UiWidgetKind::Slider)
                               {
                                   const UiRect thumbRect = scope->hierarchy().absoluteRectOf(thumb);
                                   grabOffset = part == thumb
                                       ? event->position.x - (thumbRect.position.x + thumbRect.size.x * 0.5f)
                                       : 0.0f;
                                   setFromPointer(event->position);
                               }
                           });
        if (kind == UiWidgetKind::Slider)
        {
            // the router keeps sending a captured drag to a slider that was disabled mid-drag: it ignores it
            scope->setReaction(part, UiEventKind::DragStart,
                               [this](const void* payload)
                               {
                                   if (interactableNow(scope->world(), root))
                                   {
                                       setFromPointer(static_cast<const UiDragStartEvent*>(payload)->position);
                                   }
                               });
            scope->setReaction(part, UiEventKind::DragMove,
                               [this](const void* payload)
                               {
                                   if (interactableNow(scope->world(), root))
                                   {
                                       setFromPointer(static_cast<const UiDragMoveEvent*>(payload)->position);
                                   }
                               });
            return;
        }
        scope->setReaction(part, UiEventKind::Click,
                           [this, itemIndex](const void* payload)
                           {
                               if (static_cast<const UiClickEvent*>(payload)->button != 0)
                               {
                                   return;
                               }
                               if (itemIndex == NONE)
                               {
                                   setChecked(!scope->world().getConstComponent<UiToggleComponent>(root).checked, true);
                                   return;
                               }
                               select(static_cast<i32>(itemIndex), true);
                           });
    }

    void UiValueState::bindKeys(const Entity element, const u32 itemIndex)
    {
        scope->setKeyReaction(element,
                              [this, itemIndex](const UiKeyDownEvent& event) { return key(event, itemIndex); });
    }

    bool UiValueState::key(const UiKeyDownEvent& event, const u32 itemIndex)
    {
        constexpr u8 CHORDS = UiKeyModifier::CTRL | UiKeyModifier::ALT | UiKeyModifier::SUPER;
        if ((event.modifiers & CHORDS) != 0)
        {
            return false;
        }
        World& world = scope->world();
        if (!interactableNow(world, itemIndex == NONE ? root : items[itemIndex].root))
        {
            return false;
        }
        const u32 code = event.scancode;
        const bool activate = code == Key::SPACE || code == Key::RETURN || code == Key::KP_ENTER;
        switch (kind)
        {
        case UiWidgetKind::Checkbox:
        case UiWidgetKind::Toggle:
            if (!activate)
            {
                return false;
            }
            if (!event.repeat) // a held key toggles once
            {
                setChecked(!world.getConstComponent<UiToggleComponent>(root).checked, true);
            }
            return true;
        case UiWidgetKind::Slider:
            {
                const auto& slider = world.getConstComponent<UiSliderComponent>(root);
                const Range range = normalRange(slider.min, slider.max);
                const double step = normalStep(slider.step);
                const double unit = step > 0.0 ? step : keyUnit(range.min, range.max);
                double target = slider.value;
                switch (code)
                {
                case Key::LEFT:
                case Key::DOWN:
                    target -= unit;
                    break;
                case Key::RIGHT:
                case Key::UP:
                    target += unit;
                    break;
                case Key::PAGE_DOWN:
                    target -= unit * PAGE_STEPS;
                    break;
                case Key::PAGE_UP:
                    target += unit * PAGE_STEPS;
                    break;
                case Key::HOME:
                    target = range.min;
                    break;
                case Key::END:
                    target = range.max;
                    break;
                default:
                    return false;
                }
                if (!std::isfinite(target)) // the step overflowed: the end of the range in that direction
                {
                    target = target > 0.0 ? range.max : range.min;
                }
                setSlider(target, true);
                return true;
            }
        case UiWidgetKind::RadioGroup:
        case UiWidgetKind::TabBar:
            {
                if (activate)
                {
                    if (!event.repeat)
                    {
                        select(static_cast<i32>(itemIndex), true);
                    }
                    return true;
                }
                const auto count = static_cast<i32>(items.size());
                if (kind == UiWidgetKind::TabBar && (code == Key::HOME || code == Key::END))
                {
                    // the first or last enabled tab
                    for (i32 step = 0; step < count; ++step)
                    {
                        const i32 at = code == Key::HOME ? step : count - 1 - step;
                        if (interactableNow(world, items[static_cast<u32>(at)].root))
                        {
                            scope->router().requestFocus(items[static_cast<u32>(at)].root);
                            select(at, true);
                            break;
                        }
                    }
                    return true;
                }
                const bool back = code == Key::LEFT || code == Key::UP;
                if (!back && code != Key::RIGHT && code != Key::DOWN)
                {
                    return false;
                }
                for (i32 step = 1; step < count; ++step)
                {
                    const i32 next = ((static_cast<i32>(itemIndex) + (back ? -step : step)) % count + count) % count;
                    if (interactableNow(world, items[static_cast<u32>(next)].root))
                    {
                        // focus first: the callback may destroy the group, and nothing of this state is used after it
                        scope->router().requestFocus(items[static_cast<u32>(next)].root);
                        select(next, true);
                        break;
                    }
                }
                return true;
            }
        default:
            return false;
        }
    }

    // -- UiScope ----------------------------------------------------------------------------------------------------

    UiValueState* UiScope::valueState(const Entity element) const
    {
        if (m_valueWidgets.empty())
        {
            return nullptr;
        }
        u64 root = element.id();
        if (const auto part = m_valueParts.find(root); part != m_valueParts.end())
        {
            root = part->second;
        }
        const auto it = m_valueWidgets.find(root);
        return it == m_valueWidgets.end() ? nullptr : it->second.get();
    }

    void UiScope::syncValueWidget(const Entity root)
    {
        if (const auto it = m_valueWidgets.find(root.id()); it != m_valueWidgets.end())
        {
            it->second->sync();
        }
        else if (const auto choice = m_dropdowns.find(root.id()); choice != m_dropdowns.end())
        {
            choice->second->sync();
        }
    }

    void UiScope::propagateInteractable(const Entity element, bool interactable)
    {
        if (UiValueState* owner = valueState(element))
        {
            if (const u32 index = owner->itemIndexOf(element); index != UiValueState::NONE)
            {
                owner->items[index].disabled = !interactable; // survives its group being disabled and enabled
                interactable = interactable && interactableNow(*m_world, owner->root);
                if (m_world->getConstComponent<UiStateComponent>(element).interactable != interactable)
                {
                    m_world->getComponent<UiStateComponent>(element).interactable = interactable;
                }
            }
        }
        applyInteractable(element, interactable);
    }

    void UiScope::applyInteractable(const Entity element, const bool interactable)
    {
        const std::span<const Entity> children = hierarchy().childrenOf(element);
        for (const Entity child : std::vector<Entity>(children.begin(), children.end()))
        {
            if (!m_valueParts.contains(child.id()))
            {
                continue;
            }
            bool enabled = interactable;
            const auto choice = m_dropdowns.find(m_valueParts.at(child.id()));
            if (choice != m_dropdowns.end() && choice->second->kind == UiWidgetKind::Dropdown)
                enabled = false; // the label and caret remain decorative when the field is re-enabled
            if (const UiValueState* owner = valueState(child))
            {
                const u32 index = owner->itemIndexOf(child);
                enabled = interactable && (index == UiValueState::NONE || !owner->items[index].disabled);
            }
            if (m_world->getConstComponent<UiStateComponent>(child).interactable != enabled)
            {
                m_world->getComponent<UiStateComponent>(child).interactable = enabled;
                markLayoutDirty(child);
            }
            applyInteractable(child, enabled);
        }
        if (UiValueState* state = valueState(element); state && state->isGroup())
        {
            state->refreshTabStops();
        }
    }

    Entity UiScope::createCheckable(const InternedString id, const std::string_view label, const UiRectDesc& desc,
                                    const bool checked, const UiWidgetKind kind)
    {
        UiRectDesc rect = desc;
        rect.layout = rowLayout(CHOICE_GAP);
        if (!rect.contentSize)
        {
            rect.contentSize = UiContentSizeComponent{};
        }
        const Entity entity = createElement(id, kind, rect);
        m_world->getComponent<UiStateComponent>(entity).tabStop = true;
        m_world->addComponent<UiToggleComponent>(entity, UiToggleComponent{.checked = checked});
        auto& state = m_valueWidgets[entity.id()] = std::make_unique<UiValueState>(*this, kind, entity);
        state->build(label, checked);
        return entity;
    }

    Checkbox UiScope::createCheckbox(const InternedString id, const std::string_view label, const UiRectDesc& desc,
                                     const bool checked)
    {
        return Checkbox(this, createCheckable(id, label, desc, checked, UiWidgetKind::Checkbox), m_generation);
    }

    Toggle UiScope::createToggle(const InternedString id, const std::string_view label, const UiRectDesc& desc,
                                 const bool checked)
    {
        return Toggle(this, createCheckable(id, label, desc, checked, UiWidgetKind::Toggle), m_generation);
    }

    Slider UiScope::createSlider(const InternedString id, const UiSliderDesc& desc)
    {
        UiRectDesc rect = desc.rect;
        if (rect.size == Vec2{})
        {
            rect.size = SLIDER_SIZE;
        }
        rect.layout = rowLayout();
        const Entity entity = createElement(id, UiWidgetKind::Slider, rect);
        m_world->getComponent<UiStateComponent>(entity).tabStop = true;
        auto& state = m_valueWidgets[entity.id()] = std::make_unique<UiValueState>(*this, UiWidgetKind::Slider, entity);
        state->buildSlider(desc);
        return Slider(this, entity, m_generation);
    }

    ProgressBar UiScope::createProgressBar(const InternedString id, const UiProgressDesc& desc)
    {
        UiRectDesc rect = desc.rect;
        if (rect.size == Vec2{})
        {
            rect.size = PROGRESS_SIZE;
        }
        rect.layout = rowLayout(0.0f, UiLayoutAlign::Stretch);
        const Entity entity = createElement(id, UiWidgetKind::ProgressBar, rect);
        auto& state = m_valueWidgets[entity.id()] =
            std::make_unique<UiValueState>(*this, UiWidgetKind::ProgressBar, entity);
        state->buildProgress(desc);
        return ProgressBar(this, entity, m_generation);
    }

    RadioGroup UiScope::createRadioGroup(const InternedString id, const UiRadioGroupDesc& desc)
    {
        UiRectDesc rect = desc.rect;
        if (!rect.contentSize)
        {
            rect.contentSize = UiContentSizeComponent{};
        }
        UiLayoutComponent layout;
        layout.direction = desc.horizontal ? UiLayoutDirection::Row : UiLayoutDirection::Column;
        layout.gap = desc.gap;
        layout.align = UiLayoutAlign::Start;
        rect.layout = layout;
        const Entity entity = createElement(id, UiWidgetKind::RadioGroup, rect);
        auto& state = m_valueWidgets[entity.id()] =
            std::make_unique<UiValueState>(*this, UiWidgetKind::RadioGroup, entity);
        state->buildGroup();
        return RadioGroup(this, entity, m_generation);
    }

    TabBar UiScope::createTabBar(const InternedString id, const UiTabBarDesc& desc)
    {
        UiRectDesc rect = desc.rect;
        if (!rect.contentSize)
        {
            rect.contentSize = UiContentSizeComponent{};
        }
        UiLayoutComponent layout = rowLayout(desc.gap, UiLayoutAlign::Stretch);
        rect.layout = layout;
        const Entity entity = createElement(id, UiWidgetKind::TabBar, rect);
        auto& state = m_valueWidgets[entity.id()] = std::make_unique<UiValueState>(*this, UiWidgetKind::TabBar, entity);
        state->buildTabBar();
        return TabBar(this, entity, m_generation);
    }

    // -- Handles ----------------------------------------------------------------------------------------------------

    namespace
    {
        // Stale-handle guard as the other widgets': assert in dev/Test, no-op + log in release.
        UiValueState* stateOf(const Widget& widget)
        {
            if (!widget.isValid())
            {
                SASSERTM(false, "Stale UI widget handle: the owning scope died or the element was destroyed.");
                return nullptr;
            }
            return widget.scope()->valueState(widget.entity());
        }
    } // namespace

    void Checkable::setChecked(const bool checked)
    {
        if (UiValueState* state = stateOf(*this))
        {
            state->setChecked(checked, false);
        }
    }

    bool Checkable::checked() const
    {
        return stateOf(*this) && m_scope->world().getConstComponent<UiToggleComponent>(m_entity).checked;
    }

    void Checkable::onChange(std::function<void(bool)> fn)
    {
        if (UiValueState* state = stateOf(*this))
        {
            state->onToggle = std::move(fn);
        }
    }

    Label Checkable::label() const
    {
        const UiValueState* state = stateOf(*this);
        return state ? Label(m_scope, state->label, m_generation) : Label{};
    }

    void Slider::setValue(const double value)
    {
        if (UiValueState* state = stateOf(*this))
        {
            state->setSlider(value, false);
        }
    }

    void Slider::setRange(const double min, const double max, const double step)
    {
        if (UiValueState* state = stateOf(*this))
        {
            state->setSliderRange(min, max, step);
        }
    }

    double Slider::value() const
    {
        return stateOf(*this) ? m_scope->world().getConstComponent<UiSliderComponent>(m_entity).value : 0.0;
    }

    double Slider::min() const
    {
        return stateOf(*this) ? m_scope->world().getConstComponent<UiSliderComponent>(m_entity).min : 0.0;
    }

    double Slider::max() const
    {
        return stateOf(*this) ? m_scope->world().getConstComponent<UiSliderComponent>(m_entity).max : 0.0;
    }

    double Slider::step() const
    {
        return stateOf(*this) ? m_scope->world().getConstComponent<UiSliderComponent>(m_entity).step : 0.0;
    }

    void Slider::onChange(std::function<void(double)> fn)
    {
        if (UiValueState* state = stateOf(*this))
        {
            state->onValue = std::move(fn);
        }
    }

    void ProgressBar::setValue(const double value)
    {
        if (UiValueState* state = stateOf(*this))
        {
            const auto& progress = m_scope->world().getConstComponent<UiProgressComponent>(m_entity);
            state->setProgress(value, progress.min, progress.max);
        }
    }

    void ProgressBar::setRange(const double min, const double max)
    {
        if (UiValueState* state = stateOf(*this))
        {
            state->setProgress(m_scope->world().getConstComponent<UiProgressComponent>(m_entity).value, min, max);
        }
    }

    double ProgressBar::value() const
    {
        return stateOf(*this) ? m_scope->world().getConstComponent<UiProgressComponent>(m_entity).value : 0.0;
    }

    double ProgressBar::min() const
    {
        return stateOf(*this) ? m_scope->world().getConstComponent<UiProgressComponent>(m_entity).min : 0.0;
    }

    double ProgressBar::max() const
    {
        return stateOf(*this) ? m_scope->world().getConstComponent<UiProgressComponent>(m_entity).max : 0.0;
    }

    u32 Radio::index() const
    {
        return stateOf(*this) ? m_scope->world().getConstComponent<UiRadioItemComponent>(m_entity).index : 0;
    }

    bool Radio::isSelected() const
    {
        return stateOf(*this) && m_scope->world().getConstComponent<UiRadioItemComponent>(m_entity).checked;
    }

    Label Radio::label() const
    {
        const UiValueState* state = stateOf(*this);
        if (!state)
        {
            return {};
        }
        const u32 index = state->itemIndexOf(m_entity);
        return index == UiValueState::NONE ? Label{} : Label(m_scope, state->items[index].label, m_generation);
    }

    Radio RadioGroup::addItem(const InternedString id, const std::string_view label)
    {
        UiValueState* state = stateOf(*this);
        return state ? state->addItem(id, label) : Radio{};
    }

    u32 RadioGroup::itemCount() const
    {
        const UiValueState* state = stateOf(*this);
        return state ? static_cast<u32>(state->items.size()) : 0;
    }

    Radio RadioGroup::item(const u32 index) const
    {
        const UiValueState* state = stateOf(*this);
        return state && index < state->items.size() ? Radio(m_scope, state->items[index].root, m_generation) : Radio{};
    }

    void RadioGroup::setSelected(const i32 index)
    {
        if (UiValueState* state = stateOf(*this))
        {
            state->select(index, false);
        }
    }

    i32 RadioGroup::selected() const
    {
        return stateOf(*this) ? m_scope->world().getConstComponent<UiRadioGroupComponent>(m_entity).selected : -1;
    }

    void RadioGroup::onChange(std::function<void(u32)> fn)
    {
        if (UiValueState* state = stateOf(*this))
        {
            state->onSelect = std::move(fn);
        }
    }

    u32 Tab::index() const
    {
        return stateOf(*this) ? m_scope->world().getConstComponent<UiTabComponent>(m_entity).index : 0;
    }

    bool Tab::isActive() const
    {
        return stateOf(*this) && m_scope->world().getConstComponent<UiTabComponent>(m_entity).active;
    }

    Label Tab::label() const
    {
        const UiValueState* state = stateOf(*this);
        if (!state)
        {
            return {};
        }
        const u32 index = state->itemIndexOf(m_entity);
        return index == UiValueState::NONE ? Label{} : Label(m_scope, state->items[index].label, m_generation);
    }

    void Tab::setContent(const Widget& content)
    {
        UiValueState* state = stateOf(*this);
        const u32 index = state ? state->itemIndexOf(m_entity) : UiValueState::NONE;
        if (index == UiValueState::NONE)
        {
            return;
        }
        state->items[index].content = content.isValid() ? content.entity() : Entity::undefined();
        state->applySelection();
    }

    Tab TabBar::addTab(const InternedString id, const std::string_view label)
    {
        UiValueState* state = stateOf(*this);
        return state ? state->addTab(id, label) : Tab{};
    }

    u32 TabBar::tabCount() const
    {
        const UiValueState* state = stateOf(*this);
        return state ? static_cast<u32>(state->items.size()) : 0;
    }

    Tab TabBar::tab(const u32 index) const
    {
        const UiValueState* state = stateOf(*this);
        return state && index < state->items.size() ? Tab(m_scope, state->items[index].root, m_generation) : Tab{};
    }

    void TabBar::setActive(const i32 index)
    {
        if (UiValueState* state = stateOf(*this))
        {
            state->select(index, false);
        }
    }

    i32 TabBar::active() const
    {
        return stateOf(*this) ? m_scope->world().getConstComponent<UiTabBarComponent>(m_entity).active : -1;
    }

    void TabBar::onChange(std::function<void(u32)> fn)
    {
        if (UiValueState* state = stateOf(*this))
        {
            state->onSelect = std::move(fn);
        }
    }

    void TabBar::onReselect(std::function<void(u32)> fn)
    {
        if (UiValueState* state = stateOf(*this))
        {
            state->onReselect = std::move(fn);
        }
    }
} // namespace spite
