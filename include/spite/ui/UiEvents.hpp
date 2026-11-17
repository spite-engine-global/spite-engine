#pragma once

#include <string>
#include <vector>

#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include "ui/UiExport.hpp"

namespace spite
{
    // Typed ECS event payloads the router dispatches. Each is an event
    // component (fired through World::fireEventById), so game systems consume
    // UI events like any other ECS event; the presenter layer's reaction
    // tables are fed from the same dispatch pass (one mechanism, two views).
    // `element` is the resolved target: the hit/captured element for pointer
    // events, the bubbling winner for wheel/key/text, the focused element for
    // focus and text input.

    enum class UiEventKind : u8
    {
        HoverEnter,
        HoverMove,
        HoverLeave,
        PointerDown,
        PointerUp,
        Click,
        DragStart,
        DragMove,
        DragEnd,
        Wheel,
        KeyDown,
        TextInput,
        TextComposition,
        FocusGained,
        FocusLost,
        Count
    };

    struct UiHoverEnterEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiHoverEnterEvent";
        Entity element = Entity::undefined();
        Vec2 position{0.0f, 0.0f};
        SPITE_OBSERVED_SCHEMA(UiHoverEnterEvent, element, position)
    };

    struct UiHoverMoveEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiHoverMoveEvent";
        Entity element = Entity::undefined();
        Vec2 position{0.0f, 0.0f};
        SPITE_OBSERVED_SCHEMA(UiHoverMoveEvent, element, position)
    };

    struct UiHoverLeaveEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiHoverLeaveEvent";
        Entity element = Entity::undefined();
        Vec2 position{0.0f, 0.0f};
        SPITE_OBSERVED_SCHEMA(UiHoverLeaveEvent, element, position)
    };

    struct UiPointerDownEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiPointerDownEvent";
        Entity element = Entity::undefined();
        Vec2 position{0.0f, 0.0f};
        u8 button = 0;
        u8 modifiers = 0; // UiKeyModifier bits
        SPITE_OBSERVED_SCHEMA(UiPointerDownEvent, element, position, button, modifiers)
    };

    struct UiPointerUpEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiPointerUpEvent";
        Entity element = Entity::undefined();
        Vec2 position{0.0f, 0.0f};
        u8 button = 0;
        SPITE_OBSERVED_SCHEMA(UiPointerUpEvent, element, position, button)
    };

    // Press + release over the same element while captured.
    struct UiClickEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiClickEvent";
        Entity element = Entity::undefined();
        Vec2 position{0.0f, 0.0f};
        u8 button = 0;
        u8 modifiers = 0; // UiKeyModifier bits held at the release
        SPITE_OBSERVED_SCHEMA(UiClickEvent, element, position, button, modifiers)
    };

    struct UiDragStartEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiDragStartEvent";
        Entity element = Entity::undefined();
        Vec2 position{0.0f, 0.0f};
        Vec2 delta{0.0f, 0.0f};
        u64 pointerWindow = 0;
        SPITE_OBSERVED_SCHEMA(UiDragStartEvent, element, position, delta, pointerWindow)
    };

    struct UiDragMoveEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiDragMoveEvent";
        Entity element = Entity::undefined();
        Vec2 position{0.0f, 0.0f};
        Vec2 delta{0.0f, 0.0f};
        u64 pointerWindow = 0;
        SPITE_OBSERVED_SCHEMA(UiDragMoveEvent, element, position, delta, pointerWindow)
    };

    struct UiDragEndEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiDragEndEvent";
        Entity element = Entity::undefined();
        Vec2 position{0.0f, 0.0f};
        Vec2 delta{0.0f, 0.0f};
        u64 pointerWindow = 0;
        SPITE_OBSERVED_SCHEMA(UiDragEndEvent, element, position, delta, pointerWindow)
    };

    struct UiWheelEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiWheelEvent";
        Entity element = Entity::undefined();
        Vec2 wheelDelta{0.0f, 0.0f};
        u8 modifiers = 0; // UiKeyModifier bits held when the wheel turned
        SPITE_OBSERVED_SCHEMA(UiWheelEvent, element, wheelDelta, modifiers)
    };

    struct UiKeyDownEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiKeyDownEvent";
        Entity element = Entity::undefined();
        u32 scancode = 0;
        u8 modifiers = 0; // UiKeyModifier bits
        bool repeat = false;
        SPITE_OBSERVED_SCHEMA(UiKeyDownEvent, element, scancode, modifiers, repeat)
    };

    struct UiTextInputEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiTextInputEvent";
        Entity element = Entity::undefined();
        std::string text;
        SPITE_OBSERVED_SCHEMA(UiTextInputEvent, element, text)
    };

    // The input method's composition in a focused text input changed: `text` is empty when it ended
    // (committed, cancelled, or the input lost focus). `cursor` and `selected` are byte offsets within `text`.
    struct UiTextCompositionEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiTextCompositionEvent";
        Entity element = Entity::undefined();
        std::string text;
        u32 cursor = 0;
        u32 selected = 0;
        SPITE_OBSERVED_SCHEMA(UiTextCompositionEvent, element, text, cursor, selected)
    };

    struct UiFocusGainedEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiFocusGainedEvent";
        Entity element = Entity::undefined();
        SPITE_OBSERVED_SCHEMA(UiFocusGainedEvent, element)
    };

    struct UiFocusLostEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiFocusLostEvent";
        Entity element = Entity::undefined();
        SPITE_OBSERVED_SCHEMA(UiFocusLostEvent, element)
    };

    // A tree view's drop (pointer or TreeView::drop): move `nodes` under
    // `parent` (undefined: the roots) so the first lands at `index` among the
    // parent's children without the moved nodes. Fired on the tree's viewport.
    struct UiTreeDropEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiTreeDropEvent";
        Entity element = Entity::undefined();
        std::vector<Entity> nodes;
        Entity parent = Entity::undefined();
        u32 index = 0;
        SPITE_OBSERVED_SCHEMA(UiTreeDropEvent, element, nodes, parent, index)
    };

    // A chart's drag selection ended; fired on the chart. The range in x units and as the absolute sample
    // numbers (a histogram: bins) it covers, first..last inclusive.
    struct UiChartSelectEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiChartSelectEvent";
        Entity element = Entity::undefined();
        double xMin = 0.0;
        double xMax = 0.0;
        u64 first = 0;
        u64 last = 0;
        SPITE_OBSERVED_SCHEMA(UiChartSelectEvent, element, xMin, xMax, first, last)
    };

    // What a drag carries: `type` names the kind ("asset.model", "item"), which targets accept by;
    // the rest is the data: an entity, a number, and `text` (newline-separated names or ids when it carries several);
    // `label` is what the drag preview shows.
    struct UiDragPayload
    {
        InternedString type{};
        Entity entity = Entity::undefined();
        u64 data = 0;
        std::string text;
        std::string label;
        SPITE_OBSERVED_SCHEMA(UiDragPayload, type, entity, data, text, label)
    };

    // A drag released over an accepting drop target; fired on the target (`element`), once per drag. `hit` is the
    // deepest element under the pointer (a target spanning pooled rows finds the row from it), `position` the pointer.
    struct UiDropEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiDropEvent";
        Entity element = Entity::undefined();
        Entity source = Entity::undefined();
        UiDragPayload payload;
        Entity hit = Entity::undefined();
        Vec2 position{0.0f, 0.0f};
        SPITE_OBSERVED_SCHEMA(UiDropEvent, element, source, payload, hit, position)
    };

    // A tween ran its last play (or reduced motion snapped it) and left the table; not fired for a cancelled
    // one. `tween` is the UiTweenId start returned.
    struct UiTweenFinishedEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiTweenFinishedEvent";
        Entity element = Entity::undefined();
        u32 tween = 0;
        SPITE_OBSERVED_SCHEMA(UiTweenFinishedEvent, element, tween)
    };

    // A menu item's activation (pointer, keys or Menu::activate), fired on
    // the menu's root after the menu closed.
    struct UiMenuActivateEvent : IUiEventComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiMenuActivateEvent";
        Entity element = Entity::undefined();
        InternedString item{};
        SPITE_OBSERVED_SCHEMA(UiMenuActivateEvent, element, item)
    };
} // namespace spite
