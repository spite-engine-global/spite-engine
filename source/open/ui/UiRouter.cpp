#include "ui/UiRouter.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>
#include <unordered_map>
#include <vector>

#include "base/Assert.hpp"
#include "base/Handle.hpp"
#include "ecs/World.hpp"
#include "ui/UiNavigation.hpp"

namespace spite
{
    namespace
    {
        constexpr Vec2 vec2Zero() { return Vec2{0.0f, 0.0f}; }

        // HID scancodes
        namespace Key
        {
            constexpr u32 RETURN = 40;
            constexpr u32 ESCAPE = 41;
            constexpr u32 TAB = 43;
            constexpr u32 SPACE = 44;
            constexpr u32 KP_ENTER = 88;
            constexpr u32 F6 = 63;
            constexpr u32 PAGE_UP = 75;
            constexpr u32 PAGE_DOWN = 78;
            constexpr u32 RIGHT = 79;
            constexpr u32 LEFT = 80;
            constexpr u32 DOWN = 81;
            constexpr u32 UP = 82;
        } // namespace Key

        // The key a navigation intent stands for when a widget handles keys: what the widget would see from a
        // keyboard, and what the router navigates by once nothing handled it
        UiKeyTransition keyOf(const UiNavIntent intent)
        {
            constexpr u8 CTRL = UiKeyModifier::CTRL;
            switch (intent)
            {
            case UiNavIntent::Up:
                return {.scancode = Key::UP, .pressed = true};
            case UiNavIntent::Down:
                return {.scancode = Key::DOWN, .pressed = true};
            case UiNavIntent::Left:
                return {.scancode = Key::LEFT, .pressed = true};
            case UiNavIntent::Right:
                return {.scancode = Key::RIGHT, .pressed = true};
            case UiNavIntent::Accept:
                return {.scancode = Key::RETURN, .pressed = true};
            case UiNavIntent::Cancel:
                return {.scancode = Key::ESCAPE, .pressed = true};
            case UiNavIntent::NextTab:
                return {.scancode = Key::PAGE_DOWN, .pressed = true, .modifiers = CTRL};
            case UiNavIntent::PreviousTab:
                return {.scancode = Key::PAGE_UP, .pressed = true, .modifiers = CTRL};
            case UiNavIntent::NextLandmark:
                return {.scancode = Key::F6, .pressed = true};
            default:
                return {.scancode = Key::F6, .pressed = true, .modifiers = UiKeyModifier::SHIFT};
            }
        }

        constexpr u32 MAX_DEPTH = 64;

        // A landmark is a focus scope the landmark cycle visits
        bool isScope(const UiStateComponent* state) { return state && (state->focusScope || state->landmark); }
    } // namespace

    struct UiRouter::State
    {
        World* world = nullptr;
        UiHierarchyManager* hierarchy = nullptr;

        // A slot's origin is where the code it ends up calling lives: the
        // wrapped function's for a Widget::onX wrapper, else its own.
        template <typename Fn>
        struct Reaction
        {
            Fn fn;
            CodeOrigin origin;
        };

        // Presenter reaction slots: element -> one slot per kind. Fed at
        // dispatch from the same pass that fires the ECS events.
        std::unordered_map<Entity, std::array<Reaction<ReactionFn>, static_cast<u32>(UiEventKind::Count)>> reactions;
        std::unordered_map<Entity, Reaction<KeyReactionFn>> keyReactions;

        // Transient state machines.
        Entity hovered = Entity::undefined();
        Entity pressed = Entity::undefined();
        Entity capture = Entity::undefined();
        Entity dragged = Entity::undefined();
        Entity focused = Entity::undefined();
        // a key or navigation intent moved the focus last (Tab, F6, an arrow), not a press or a call
        bool focusByKeys = false;
        bool keyDriven = false; // inside a queued key or navigation entry
        UiRouter::KeyClaimFn keyClaim;
        Entity activeSurface = Entity::undefined();
        Vec2 lastPointer{0.0f, 0.0f};
        Vec2 pressPointer{0.0f, 0.0f};
        u64 lastPointerWindow = 0;
        bool wantsTextInput = false;
        // the composition last delivered to compositionTarget
        Entity compositionTarget = Entity::undefined();
        std::string composition;
        u32 compositionCursor = 0;
        u32 compositionSelected = 0;
        bool cancelsComposition = false;
        bool consumed = false;
        bool navigationEnabled = true;
        bool navigationKey = false; // a key reaction runs for the key an intent stands for
        std::function<void(const UiPointerObservation&)> pointerObserver;
        // Controls the UI claimed from gameplay: pressed over the UI (or into
        // its focus), held until released.
        u8 claimedButtons = 0;
        std::vector<u32> claimedKeys;
        // scancodes whose press a key reaction handled this frame, even when the release followed in the same frame
        std::vector<u32> handledKeys;
        // keys an open modal swallowed unused: they stay off gameplay and the layers below, but a navigation intent
        // from the same key still acts (the keyboard walks a menu dialog as the pad does)
        std::vector<u32> swallowedKeys;
        // intents navigation acted on and still holds (bit per UiNavIntent): their controls stay off gameplay
        u32 navClaimed = 0;
        // the element directional navigation focused since the host last took it (to scroll it into view)
        Entity navigated = Entity::undefined();

        // The frame event queue: closures fire the ECS event and invoke the
        // reaction from the dispatch point. Payloads are captured by value,
        // so a reaction destroying elements mid-dispatch cannot corrupt later
        // entries; reaction tables are looked up at run time.
        std::vector<std::function<void()>> queue;

        // Marks the focus moves made while a queued key or navigation entry runs as key-driven.
        struct KeyDrive
        {
            explicit KeyDrive(State& owner) : state(owner) { state.keyDriven = true; }
            ~KeyDrive() { state.keyDriven = false; }
            KeyDrive(const KeyDrive&) = delete;
            KeyDrive& operator=(const KeyDrive&) = delete;
            State& state;
        };

        template <typename E>
        void queueEvent(E payload, Entity target, UiEventKind kind)
        {
            queue.push_back(
                [this, payload, target, kind]() mutable
                {
                    world->fireEvent(payload);
                    const auto slot = reactions.find(target);
                    if (slot == reactions.end())
                    {
                        return;
                    }
                    const auto& fn = slot->second[static_cast<u32>(kind)].fn;
                    if (fn)
                    {
                        // Copy before the call: the reaction may destroy the
                        // element, erasing its own slot mid-invocation.
                        std::function<void(const void*)> fnCopy = fn;
                        fnCopy(&payload);
                    }
                });
        }

        // Offers a press to the focus chain from start, ancestors on: whether a reaction handled it (the ECS event
        // fires then). Releases and claims are the caller's.
        bool routeKey(const UiKeyTransition& transition, const Entity start)
        {
            if (keyClaim && keyClaim(transition))
            {
                return true;
            }
            UiKeyDownEvent event{};
            event.scancode = transition.scancode;
            event.modifiers = transition.modifiers;
            event.repeat = transition.repeat;
            u32 depth = 0;
            for (Entity current = start; current != Entity::undefined() && depth < MAX_DEPTH;
                 current = hierarchy->parentOf(current), ++depth)
            {
                const auto slot = keyReactions.find(current);
                if (slot == keyReactions.end())
                {
                    continue;
                }
                event.element = current;
                // Copy before the call: the reaction may destroy the element.
                const KeyReactionFn fn = slot->second.fn;
                if (fn(event))
                {
                    world->fireEvent(event);
                    return true;
                }
            }
            return false;
        }

        // Queued like events so claims and releases keep the frame's order: a
        // press and release in one frame leave no claim behind.
        void queueKey(const UiKeyTransition& transition, Entity start)
        {
            queue.push_back(
                [this, transition, start]()
                {
                    const KeyDrive drive{*this};
                    if (!transition.pressed)
                    {
                        // the release of a key the UI held is the UI's too: its latched edge stays off gameplay
                        std::erase(swallowedKeys, transition.scancode);
                        if (std::erase(claimedKeys, transition.scancode) != 0)
                        {
                            handledKeys.push_back(transition.scancode);
                        }
                        return;
                    }
                    const auto claim = [this, &transition]
                    {
                        consumed = true;
                        handledKeys.push_back(transition.scancode);
                        if (std::ranges::find(claimedKeys, transition.scancode) == claimedKeys.end())
                        {
                            claimedKeys.push_back(transition.scancode);
                        }
                    };
                    if (routeKey(transition, start))
                    {
                        claim();
                        return;
                    }
                    // a modal swallows every key it did not use: nothing reaches gameplay or the layers below
                    if (navigate(transition))
                    {
                        claim();
                    }
                    else if (modalActive())
                    {
                        if (std::ranges::find(swallowedKeys, transition.scancode) == swallowedKeys.end())
                        {
                            swallowedKeys.push_back(transition.scancode);
                        }
                        claim();
                    }
                });
        }

        // What the focus chain left unhandled: Tab moves focus, Escape clears
        // it, Enter and Space click a focused button.
        bool navigate(const UiKeyTransition& transition)
        {
            constexpr u8 CHORDS = UiKeyModifier::CTRL | UiKeyModifier::ALT | UiKeyModifier::SUPER;
            if (!navigationEnabled || !hierarchy->isElement(focused) || (transition.modifiers & CHORDS) != 0)
            {
                return false;
            }
            switch (transition.scancode)
            {
            case Key::TAB:
                return moveFocus((transition.modifiers & UiKeyModifier::SHIFT) != 0);
            case Key::ESCAPE:
                setFocus(Entity::undefined());
                return true;
            case Key::RETURN:
            case Key::KP_ENTER:
            case Key::SPACE:
                {
                    if (!activatable(focused))
                    {
                        return false;
                    }
                    // a held key's repeats stay claimed without clicking again
                    if (!transition.repeat)
                    {
                        queueClick(focused);
                    }
                    return true;
                }
            default:
                return false;
            }
        }

        [[nodiscard]] bool focusable(const Entity element) const
        {
            if (!hierarchy->isElement(element) || !hierarchy->isVisibleOf(element) || blockedByModal(element))
            {
                return false;
            }
            const UiStateComponent* state = stateOf(element);
            return !state || state->interactable;
        }

        // The explicit neighbour (UiNavigationComponent) of the focus or of an ancestor in the direction, if it can
        // take focus now.
        [[nodiscard]] Entity explicitNeighborOf(const UiNavIntent direction) const
        {
            u32 depth = 0;
            for (Entity current = focused; current != Entity::undefined() && depth < MAX_DEPTH;
                 current = hierarchy->parentOf(current), ++depth)
            {
                const auto* navigation = world->tryGetConstComponent<UiNavigationComponent>(current);
                if (!navigation)
                {
                    continue;
                }
                const Entity target = direction == UiNavIntent::Up ? navigation->up
                    : direction == UiNavIntent::Down               ? navigation->down
                    : direction == UiNavIntent::Left               ? navigation->left
                                                                   : navigation->right;
                if (target != Entity::undefined() && target != focused && focusable(target))
                {
                    return target;
                }
            }
            return Entity::undefined();
        }

        // The element a direction press focuses: the explicit neighbour, else the nearest tab stop that way in the
        // focus scope; undefined when none. Focus on no tab stop (a scope's root, a hidden element) enters the scope
        // at its first stop.
        [[nodiscard]] Entity neighborOf(const UiNavIntent direction, const bool useExplicit = true) const
        {
            if (!hierarchy->isElement(focused))
            {
                return Entity::undefined();
            }
            if (const Entity target = useExplicit ? explicitNeighborOf(direction) : Entity::undefined();
                target != Entity::undefined())
            {
                return target;
            }
            std::vector<Entity> stops;
            tabStops(stops);
            if (stops.empty())
            {
                return Entity::undefined();
            }
            Entity origin = Entity::undefined();
            u32 depth = 0;
            for (Entity current = focused;
                 current != Entity::undefined() && origin == Entity::undefined() && depth < MAX_DEPTH;
                 current = hierarchy->parentOf(current), ++depth)
            {
                if (std::ranges::contains(stops, current))
                {
                    origin = current;
                }
            }
            if (origin == Entity::undefined())
            {
                return stops.front();
            }
            std::vector<Entity> candidates;
            std::vector<UiRect> rects;
            for (const Entity stop : stops)
            {
                if (stop != origin)
                {
                    candidates.push_back(stop);
                    rects.push_back(hierarchy->absoluteRectOf(stop));
                }
            }
            const i32 pick = pickNavigationTarget(direction, hierarchy->absoluteRectOf(origin), rects);
            return pick < 0 ? Entity::undefined() : candidates[static_cast<sizet>(pick)];
        }

        // One intent press or repeat: first as the key it stands for to the focus chain (a widget's own keys: a
        // menu's, a slider's, a tree's), unless the keyboard just delivered that very key, then what navigation does
        // with an unhandled one. False when nothing acted.
        bool actNav(const UiNavTransition& transition)
        {
            // the tab and landmark intents also act with nothing focused: they enter the first landmark
            const bool cycles = transition.intent >= UiNavIntent::NextTab;
            if (!navigationEnabled || (!hierarchy->isElement(focused) && !cycles))
            {
                return false;
            }
            // a key the chain handled, or still holds from an earlier press, is not navigation
            if (transition.sourceKey != 0 && !std::ranges::contains(swallowedKeys, transition.sourceKey) &&
                (std::ranges::contains(handledKeys, transition.sourceKey) ||
                 std::ranges::contains(claimedKeys, transition.sourceKey)))
            {
                return false;
            }
            UiKeyTransition key = keyOf(transition.intent);
            key.repeat = transition.repeat;
            bool explicitRefused = false;
            // the chain already had this very key from the keyboard; any other source (a pad, a remapped key) offers it
            if (!(transition.sourceKey == key.scancode && key.modifiers == 0))
            {
                // an explicit neighbour is the author's way out of a widget that takes every arrow (a text input)
                if (transition.intent <= UiNavIntent::Right)
                {
                    if (const Entity target = explicitNeighborOf(transition.intent); target != Entity::undefined())
                    {
                        setFocus(target);
                        if (focused == target)
                        {
                            navigated = target;
                            return true;
                        }
                        explicitRefused = true; // the search below must not pick the same refused target
                    }
                }
                navigationKey = true;
                const bool taken = routeKey(key, focused);
                navigationKey = false;
                if (taken)
                {
                    consumed = true;
                    handledKeys.push_back(key.scancode);
                    return true;
                }
            }
            if (!hierarchy->isElement(focused) && !cycles) // a reaction destroyed it
            {
                return false;
            }
            switch (transition.intent)
            {
            case UiNavIntent::NextTab:
            case UiNavIntent::PreviousTab:
            case UiNavIntent::NextLandmark:
            case UiNavIntent::PreviousLandmark:
                // a tab intent no widget took leaves the tab bars: it cycles as the landmark intents do
                return cycleLandmark(transition.intent == UiNavIntent::PreviousTab ||
                                     transition.intent == UiNavIntent::PreviousLandmark);
            case UiNavIntent::Up:
            case UiNavIntent::Down:
            case UiNavIntent::Left:
            case UiNavIntent::Right:
                {
                    const Entity target = neighborOf(transition.intent, !explicitRefused);
                    if (target == Entity::undefined())
                    {
                        return false;
                    }
                    setFocus(target);
                    if (focused != target)
                    {
                        return false;
                    }
                    navigated = target;
                    return true;
                }
            case UiNavIntent::Accept:
            case UiNavIntent::Cancel:
                return navigate(key);
            default:
                return false;
            }
        }

        void queueNav(const UiNavTransition& transition)
        {
            queue.push_back(
                [this, transition]
                {
                    const KeyDrive drive{*this};
                    if (actNav(transition) || modalActive())
                    {
                        navClaimed |= 1u << static_cast<u32>(transition.intent);
                        consumed = true;
                    }
                });
        }

        // PointerDown, PointerUp and Click at the element's centre, as a primary click
        void queueClick(const Entity element)
        {
            const UiRect rect = hierarchy->absoluteRectOf(element);
            const Vec2 center = rect.position + rect.size * 0.5f;
            UiPointerDownEvent down{};
            down.element = element;
            down.position = center;
            queueEvent(down, element, UiEventKind::PointerDown);
            UiPointerUpEvent up{};
            up.element = element;
            up.position = center;
            queueEvent(up, element, UiEventKind::PointerUp);
            UiClickEvent click{};
            click.element = element;
            click.position = center;
            queueEvent(click, element, UiEventKind::Click);
        }

        [[nodiscard]] const UiStateComponent* stateOf(const Entity element) const
        {
            return world->tryGetConstComponent<UiStateComponent>(element);
        }

        // A Button tab stop with a click or press reaction, laid out by the
        // last resolve (its rect is where the synthesized click lands).
        [[nodiscard]] bool activatable(const Entity element) const
        {
            if (!hierarchy->isElement(element) || hierarchy->paintIndexOf(element) == UiHierarchyManager::NO_INDEX ||
                !isTabStop(element))
            {
                return false;
            }
            const auto* identity = world->tryGetConstComponent<UiIdentityComponent>(element);
            if (!identity || identity->kind != UiWidgetKind::Button)
            {
                return false;
            }
            const auto slot = reactions.find(element);
            return slot != reactions.end() &&
                (slot->second[static_cast<u32>(UiEventKind::Click)].fn ||
                 slot->second[static_cast<u32>(UiEventKind::PointerDown)].fn);
        }

        [[nodiscard]] bool isTabStop(const Entity element) const
        {
            const UiStateComponent* state = stateOf(element);
            return state && state->tabStop && state->interactable && hierarchy->isVisibleOf(element);
        }

        [[nodiscard]] bool isWithin(Entity element, const Entity ancestor) const
        {
            for (u32 depth = 0; element != Entity::undefined() && depth < MAX_DEPTH; ++depth)
            {
                if (element == ancestor)
                {
                    return true;
                }
                element = hierarchy->parentOf(element);
            }
            return false;
        }

        // Undefined, too, while focus is on an element destroyed since.
        [[nodiscard]] Entity focusScope() const
        {
            if (!hierarchy->isElement(focused))
            {
                return Entity::undefined();
            }
            Entity current = focused;
            for (u32 depth = 0; current != Entity::undefined() && depth < MAX_DEPTH; ++depth)
            {
                const UiStateComponent* state = stateOf(current);
                const Entity parent = hierarchy->parentOf(current);
                if (isScope(state) || parent == Entity::undefined())
                {
                    return current;
                }
                current = parent;
            }
            return Entity::undefined();
        }

        void tabStops(std::vector<Entity>& out) const { stopsIn(focusScope(), out); }

        // The tab stops of a scope in paint order. A subtree is one contiguous range of the pre-order paint order.
        // Nested focus scopes keep their stops to themselves.
        void stopsIn(const Entity scope, std::vector<Entity>& out) const
        {
            out.clear();
            const u32 start =
                scope == Entity::undefined() ? UiHierarchyManager::NO_INDEX : hierarchy->paintIndexOf(scope);
            if (start == UiHierarchyManager::NO_INDEX)
            {
                return;
            }
            const std::span<const Entity> paint = hierarchy->paintOrderOf();
            Entity nested = Entity::undefined();
            // Walks to the end, not to the first element outside the scope: the
            // paint order is the last resolve's, and an element destroyed or
            // reparented since would end the range early.
            for (u32 i = start; i < paint.size(); ++i)
            {
                const Entity element = paint[i];
                if (!hierarchy->isElement(element) || !isWithin(element, scope))
                {
                    continue;
                }
                if (nested != Entity::undefined() && isWithin(element, nested))
                {
                    continue;
                }
                const UiStateComponent* state = stateOf(element);
                if (i != start && isScope(state))
                {
                    nested = element;
                    continue;
                }
                if (isTabStop(element))
                {
                    out.push_back(element);
                }
            }
        }

        // The landmarks the cycle visits, in paint order (document order; a zOffset reorders siblings as it paints):
        // laid out, visible, not behind a modal and holding a tab stop to enter by.
        void landmarks(std::vector<Entity>& out) const
        {
            out.clear();
            std::vector<Entity> stops;
            for (const Entity element : hierarchy->paintOrderOf())
            {
                // the paint order is the last resolve's: an element destroyed since is no landmark
                const UiStateComponent* state = hierarchy->isElement(element) ? stateOf(element) : nullptr;
                if (!state || !state->landmark || !hierarchy->isVisibleOf(element) || blockedByModal(element))
                {
                    continue;
                }
                stopsIn(element, stops);
                if (!stops.empty())
                {
                    out.push_back(element);
                }
            }
        }

        // Focus to the first tab stop of the landmark after (backward: before) the one holding the focus, wrapping;
        // from no focus the first (last). False, and focus stays, when no landmark would change it.
        bool cycleLandmark(const bool backward)
        {
            std::vector<Entity> marks;
            landmarks(marks);
            if (marks.empty())
            {
                return false;
            }
            Entity current = Entity::undefined();
            u32 depth = 0;
            for (Entity element = hierarchy->isElement(focused) ? focused : Entity::undefined();
                 element != Entity::undefined() && depth < MAX_DEPTH; element = hierarchy->parentOf(element), ++depth)
            {
                if (const UiStateComponent* state = stateOf(element); state && state->landmark)
                {
                    current = element;
                    break;
                }
            }
            Entity target;
            if (hierarchy->isElement(focused))
            {
                // from the landmark holding the focus, else from the focus itself, by paint position
                const u32 at = hierarchy->paintIndexOf(current != Entity::undefined() ? current : focused);
                target = backward ? marks.back() : marks.front(); // the wrap
                for (sizet i = 0; i < marks.size(); ++i)
                {
                    const Entity mark = marks[backward ? marks.size() - 1 - i : i];
                    if (backward ? hierarchy->paintIndexOf(mark) < at : hierarchy->paintIndexOf(mark) > at)
                    {
                        target = mark;
                        break;
                    }
                }
            }
            else
            {
                target = backward ? marks.back() : marks.front();
            }
            if (target == current)
            {
                return false;
            }
            std::vector<Entity> stops;
            stopsIn(target, stops);
            setFocus(stops.front());
            if (focused != stops.front())
            {
                return false;
            }
            navigated = focused;
            return true;
        }

        bool moveFocus(const bool backward)
        {
            if (focused == Entity::undefined())
            {
                return false;
            }
            std::vector<Entity> stops;
            tabStops(stops);
            if (stops.empty())
            {
                return false;
            }
            const auto count = static_cast<std::ptrdiff_t>(stops.size());
            std::ptrdiff_t index = -1;
            u32 depth = 0;
            for (Entity current = focused; current != Entity::undefined() && index < 0 && depth < MAX_DEPTH;
                 current = hierarchy->parentOf(current), ++depth)
            {
                if (const auto it = std::ranges::find(stops, current); it != stops.end())
                {
                    index = it - stops.begin();
                }
            }
            Entity target = Entity::undefined();
            if (index >= 0)
            {
                target = stops[static_cast<sizet>((index + (backward ? count - 1 : 1)) % count)];
            }
            else
            {
                // no stop holds the focus: the nearest one past it in paint order
                const u32 at = hierarchy->paintIndexOf(focused);
                const auto past =
                    std::ranges::find_if(stops, [&](const Entity stop) { return hierarchy->paintIndexOf(stop) > at; });
                if (backward)
                {
                    target = past == stops.begin() ? stops.back() : *(past - 1);
                }
                else
                {
                    target = past == stops.end() ? stops.front() : *past;
                }
            }
            if (target == focused)
            {
                return false;
            }
            setFocus(target);
            if (focused == target)
            {
                navigated = target; // the context scrolls it into view, as for an arrow key
            }
            return true;
        }

        // the undefined test first: an idle UI with no modal pays one compare per frame, not a table lookup
        [[nodiscard]] bool modalActive() const
        {
            const Entity modal = hierarchy->modalRoot();
            return modal != Entity::undefined() && hierarchy->isElement(modal);
        }

        [[nodiscard]] bool blockedByModal(const Entity element) const
        {
            const Entity modal = hierarchy->modalRoot();
            if (modal == Entity::undefined() || !hierarchy->isElement(modal) || !hierarchy->isElement(element))
            {
                return false;
            }
            const u32 modalPos = hierarchy->paintIndexOf(modal);
            const u32 pos = hierarchy->paintIndexOf(surfaceOf(element));
            return modalPos != UiHierarchyManager::NO_INDEX && pos != UiHierarchyManager::NO_INDEX && pos < modalPos;
        }

        // The first tab stop under scope in paint order (a subtree is one contiguous range of it), else scope.
        [[nodiscard]] Entity firstStopIn(const Entity scope) const
        {
            const u32 start = hierarchy->paintIndexOf(scope);
            if (start == UiHierarchyManager::NO_INDEX)
            {
                return scope;
            }
            const std::span<const Entity> paint = hierarchy->paintOrderOf();
            for (u32 i = start; i < paint.size(); ++i)
            {
                if (hierarchy->isElement(paint[i]) && isWithin(paint[i], scope) && isTabStop(paint[i]))
                {
                    return paint[i];
                }
            }
            return scope;
        }

        // A modal keeps the focus: it is dragged back when the focus is gone, destroyed or below the modal, and a
        // press or drag that began below the modal ends.
        void holdModal(const UiInputSnapshot& snapshot)
        {
            const Entity modal = hierarchy->modalRoot();
            if (!hierarchy->isElement(focused) || blockedByModal(focused))
            {
                setFocus(modal);
            }
            if (capture != Entity::undefined() && (!hierarchy->isElement(capture) || blockedByModal(capture)))
            {
                if (dragged != Entity::undefined() && hierarchy->isElement(dragged))
                {
                    UiDragEndEvent event{};
                    event.element = dragged;
                    event.position = snapshot.pointerPos;
                    event.pointerWindow = snapshot.pointerWindow;
                    queueEvent(event, dragged, UiEventKind::DragEnd);
                }
                pressed = capture = dragged = Entity::undefined();
            }
        }

        [[nodiscard]] Entity surfaceOf(Entity entity) const
        {
            Entity current = entity;
            u32 depth = 0;
            while (current != Entity::undefined() && depth < MAX_DEPTH)
            {
                ++depth;
                const Entity parent = hierarchy->parentOf(current);
                if (parent == Entity::undefined())
                {
                    return current;
                }
                current = parent;
            }
            return Entity::undefined();
        }

        // Wheel/key/text bubble from the start element through ancestors;
        // the first element with a reaction slot for the kind wins.
        [[nodiscard]] Entity bubbleTarget(Entity start, UiEventKind kind) const
        {
            Entity current = start;
            u32 depth = 0;
            while (current != Entity::undefined() && depth < MAX_DEPTH)
            {
                ++depth;
                const auto slot = reactions.find(current);
                if (slot != reactions.end() && slot->second[static_cast<u32>(kind)].fn)
                {
                    return current;
                }
                current = hierarchy->parentOf(current);
            }
            return Entity::undefined();
        }

        void setFocus(Entity element)
        {
            if (element != Entity::undefined() && blockedByModal(element))
            {
                return;
            }
            if (element == Entity::undefined() && modalActive())
            {
                element = hierarchy->modalRoot();
            }
            if (element == focused)
            {
                return;
            }
            if (focused != Entity::undefined())
            {
                UiFocusLostEvent event{};
                event.element = focused;
                queueEvent(event, focused, UiEventKind::FocusLost);
            }
            focused = element;
            focusByKeys = keyDriven;
            if (focused != Entity::undefined())
            {
                UiFocusGainedEvent event{};
                event.element = focused;
                queueEvent(event, focused, UiEventKind::FocusGained);
            }
        }
    };

    UiRouter::UiRouter(World& world, UiHierarchyManager& hierarchy) : m_state(std::make_unique<State>())
    {
        m_state->world = &world;
        m_state->hierarchy = &hierarchy;
    }

    UiRouter::~UiRouter() = default;

    void UiRouter::processFrame(const UiInputSnapshot& snapshot)
    {
        State& state = *m_state;
        state.consumed = false;
        state.handledKeys.clear();

        const bool modal = state.modalActive();
        if (modal)
        {
            state.holdModal(snapshot);
        }
        const Entity hit = state.hierarchy->hitTest(snapshot.pointerPos, snapshot.pointerWindow);
        UiPointerObservation seen;
        seen.position = snapshot.pointerPos;

        // -- pointer move: hover transitions, or drag continuation under capture
        if (state.capture != Entity::undefined())
        {
            const Vec2 delta = snapshot.pointerPos - state.lastPointer;
            const Vec2 fromPress = snapshot.pointerPos - state.pressPointer;
            const auto* capturedState = state.world->tryGetConstComponent<UiStateComponent>(state.capture);
            const float threshold =
                capturedState ? capturedState->dragThreshold * state.hierarchy->layoutScale() : 0.0f;
            const bool pastThreshold = fromPress.x * fromPress.x + fromPress.y * fromPress.y >= threshold * threshold;
            if (delta != vec2Zero() ||
                (state.dragged != Entity::undefined() && snapshot.pointerWindow != state.lastPointerWindow))
            {
                if (state.dragged == Entity::undefined() && pastThreshold)
                {
                    state.dragged = state.capture;
                    UiDragStartEvent event{};
                    event.element = state.capture;
                    event.position = snapshot.pointerPos;
                    event.pointerWindow = snapshot.pointerWindow;
                    event.delta = fromPress;
                    state.queueEvent(event, state.capture, UiEventKind::DragStart);
                }
                else if (state.dragged != Entity::undefined())
                {
                    UiDragMoveEvent event{};
                    event.element = state.capture;
                    event.position = snapshot.pointerPos;
                    event.pointerWindow = snapshot.pointerWindow;
                    event.delta = delta;
                    state.queueEvent(event, state.capture, UiEventKind::DragMove);
                }
                state.consumed = true;
            }
        }
        else
        {
            const bool hoverChanged = hit != state.hovered;
            seen.hoverChanged = hoverChanged;
            if (hoverChanged)
            {
                if (state.hovered != Entity::undefined())
                {
                    UiHoverLeaveEvent event{};
                    event.element = state.hovered;
                    event.position = snapshot.pointerPos;
                    state.queueEvent(event, state.hovered, UiEventKind::HoverLeave);
                }
                state.hovered = hit;
                if (state.hovered != Entity::undefined())
                {
                    UiHoverEnterEvent event{};
                    event.element = state.hovered;
                    event.position = snapshot.pointerPos;
                    state.queueEvent(event, state.hovered, UiEventKind::HoverEnter);
                }
            }
            if (state.hovered != Entity::undefined())
            {
                // a still pointer fires nothing: every event is an entity, and an idle UI costs nothing
                if (hoverChanged || snapshot.pointerPos != state.lastPointer)
                {
                    UiHoverMoveEvent event{};
                    event.element = state.hovered;
                    event.position = snapshot.pointerPos;
                    state.queueEvent(event, state.hovered, UiEventKind::HoverMove);
                }
                state.consumed = true;
            }
        }

        // -- buttons
        for (u8 button = 0; button < 5; ++button)
        {
            const UiPointerButton& pointerButton = snapshot.buttons[button];
            if (pointerButton.pressedThisFrame)
            {
                seen.pressed = true;
                if (hit == Entity::undefined())
                {
                    if (modal)
                    {
                        // a press outside a modal belongs to it: the focus stays and nothing below sees the press
                        state.consumed = true;
                        state.claimedButtons |= static_cast<u8>(1u << button);
                        continue;
                    }
                    // press on empty space clears focus, consumes nothing
                    state.setFocus(Entity::undefined());
                    continue;
                }
                state.consumed = true;
                state.claimedButtons |= static_cast<u8>(1u << button);
                state.activeSurface = state.surfaceOf(hit);
                if (button == 0)
                {
                    state.pressed = hit;
                    state.capture = hit;
                    state.dragged = Entity::undefined();
                    state.pressPointer = snapshot.pointerPos;
                }
                state.setFocus(hit);
                UiPointerDownEvent event{};
                event.element = hit;
                event.position = snapshot.pointerPos;
                event.button = button;
                event.modifiers = snapshot.modifiers;
                state.queueEvent(event, hit, UiEventKind::PointerDown);
            }
            if (pointerButton.releasedThisFrame)
            {
                state.claimedButtons &= static_cast<u8>(~(1u << button));
                if (button == 0 && state.capture != Entity::undefined())
                {
                    state.consumed = true;
                    if (state.dragged != Entity::undefined())
                    {
                        UiDragEndEvent dragEnd{};
                        dragEnd.element = state.capture;
                        dragEnd.position = snapshot.pointerPos;
                        dragEnd.pointerWindow = snapshot.pointerWindow;
                        state.queueEvent(dragEnd, state.capture, UiEventKind::DragEnd);
                    }
                    if (state.dragged == Entity::undefined() && hit == state.pressed)
                    {
                        UiClickEvent click{};
                        click.element = state.pressed;
                        click.position = snapshot.pointerPos;
                        click.button = button;
                        click.modifiers = snapshot.modifiers;
                        state.queueEvent(click, state.pressed, UiEventKind::Click);
                    }
                    UiPointerUpEvent up{};
                    up.element = state.capture;
                    up.position = snapshot.pointerPos;
                    up.button = button;
                    state.queueEvent(up, state.capture, UiEventKind::PointerUp);
                    state.pressed = Entity::undefined();
                    state.capture = Entity::undefined();
                    state.dragged = Entity::undefined();
                }
                else if (hit != Entity::undefined())
                {
                    UiPointerUpEvent up{};
                    up.element = hit;
                    up.position = snapshot.pointerPos;
                    up.button = button;
                    state.queueEvent(up, hit, UiEventKind::PointerUp);
                }
            }
        }

        // -- wheel: bubbles from the hovered element to the first subscriber
        if (snapshot.wheelDelta != vec2Zero())
        {
            const Entity target = state.bubbleTarget(state.hovered, UiEventKind::Wheel);
            if (target != Entity::undefined())
            {
                UiWheelEvent event{};
                event.element = target;
                event.wheelDelta = snapshot.wheelDelta;
                event.modifiers = snapshot.modifiers;
                state.queueEvent(event, target, UiEventKind::Wheel);
                state.consumed = true;
            }
        }

        // -- keys: presses (and repeats) route from focus and bubble at
        // dispatch until handled; a handled press is claimed until its
        // release, an unhandled one stays gameplay's. Releases only end claims.
        for (const UiKeyTransition& transition : snapshot.keyTransitions)
        {
            if (!transition.pressed || state.focused != Entity::undefined())
            {
                state.queueKey(transition, state.focused);
            }
        }

        // -- navigation intents: after the keys, so a key a widget handled is known; a claim ends with its intent
        state.navClaimed &= snapshot.navHeld;
        for (const UiNavTransition& transition : snapshot.navigation)
        {
            state.queueNav(transition);
        }

        // -- text: delivered to a text-editing focus; the request mirrors that
        // focus, not content
        const Entity textTarget = state.bubbleTarget(state.focused, UiEventKind::TextInput);
        state.wantsTextInput = textTarget != Entity::undefined();
        if (state.wantsTextInput && !snapshot.text.empty())
        {
            UiTextInputEvent event{};
            event.element = textTarget;
            event.text = std::string(snapshot.text);
            state.queueEvent(event, textTarget, UiEventKind::TextInput);
            state.consumed = true;
        }

        // -- composition: the input method's text in the focused input, ended when the input loses focus
        state.cancelsComposition = false;
        if (state.compositionTarget != textTarget && !state.composition.empty())
        {
            if (state.hierarchy->isElement(state.compositionTarget))
            {
                UiTextCompositionEvent ended{};
                ended.element = state.compositionTarget;
                state.queueEvent(ended, state.compositionTarget, UiEventKind::TextComposition);
            }
            state.cancelsComposition = !snapshot.composition.empty();
            state.composition.clear();
            state.compositionCursor = state.compositionSelected = 0;
        }
        state.compositionTarget = textTarget;
        if (textTarget != Entity::undefined() && !state.cancelsComposition &&
            (snapshot.composition != state.composition ||
             (!snapshot.composition.empty() &&
              (snapshot.compositionCursor != state.compositionCursor ||
               snapshot.compositionSelected != state.compositionSelected))))
        {
            UiTextCompositionEvent event{};
            event.element = textTarget;
            event.text = std::string(snapshot.composition);
            event.cursor = snapshot.compositionCursor;
            event.selected = snapshot.compositionSelected;
            state.queueEvent(event, textTarget, UiEventKind::TextComposition);
            state.composition = event.text;
            state.compositionCursor = event.cursor;
            state.compositionSelected = event.selected;
            state.consumed = true;
        }

        if (modal)
        {
            state.consumed = true;
        }
        if (state.pointerObserver)
        {
            seen.hovered = state.hovered;
            seen.moved = snapshot.pointerPos != state.lastPointer;
            seen.wheel = snapshot.wheelDelta != vec2Zero();
            if (seen.hoverChanged || seen.moved || seen.pressed || seen.wheel)
            {
                state.pointerObserver(seen);
            }
        }
        state.lastPointer = snapshot.pointerPos;
        state.lastPointerWindow = snapshot.pointerWindow;
    }

    void UiRouter::dispatch()
    {
        // Reactions may queue more (focus moves): those deliver in this pass.
        // Each entry is moved out first, so the queue may grow mid-call. A
        // chain past the cap (reactions bouncing focus between each other)
        // continues next dispatch instead of looping forever.
        constexpr sizet MAX_CHAINED = 256;
        auto& queue = m_state->queue;
        const sizet limit = queue.size() + MAX_CHAINED;
        sizet i = 0;
        for (; i < queue.size() && i < limit; ++i)
        {
            const std::function<void()> event = std::move(queue[i]);
            event();
        }
        queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(i));
    }

    bool UiRouter::consumedThisFrame() const { return m_state->consumed; }

    bool UiRouter::claimsButton(const u32 button) const { return (m_state->claimedButtons >> button) & 1u; }

    std::span<const u32> UiRouter::claimedKeys() const { return m_state->claimedKeys; }

    std::span<const u32> UiRouter::handledKeys() const { return m_state->handledKeys; }

    bool UiRouter::wantsTextInput() const { return m_state->wantsTextInput; }

    bool UiRouter::cancelsComposition() const { return m_state->cancelsComposition; }

    Entity UiRouter::activeSurface() const { return m_state->activeSurface; }

    Entity UiRouter::hovered() const { return m_state->hovered; }

    Entity UiRouter::pressed() const { return m_state->pressed; }

    Entity UiRouter::focused() const { return m_state->focused; }

    bool UiRouter::focusedByKeys() const { return m_state->focusByKeys; }

    bool UiRouter::modalActive() const { return m_state->modalActive(); }

    bool UiRouter::blockedByModal(const Entity element) const { return m_state->blockedByModal(element); }

    void UiRouter::focusInto(const Entity scope) { m_state->setFocus(m_state->firstStopIn(scope)); }

    void UiRouter::setPointerObserver(std::function<void(const UiPointerObservation&)> observer)
    {
        m_state->pointerObserver = std::move(observer);
    }

    void UiRouter::requestFocus(const Entity element) { m_state->setFocus(element); }

    void UiRouter::focusAsKeys(const Entity element)
    {
        const State::KeyDrive drive{*m_state};
        m_state->setFocus(element);
        if (m_state->focused == element && element != Entity::undefined())
        {
            m_state->focusByKeys = true; // already focused by a press: the keys take it over
        }
    }

    void UiRouter::restoreFocus(const Entity element)
    {
        m_state->setFocus(element);
        if (m_state->focused == element)
        {
            m_state->focusByKeys = false;
        }
    }

    void UiRouter::clearFocus() { m_state->setFocus(Entity::undefined()); }

    void UiRouter::setKeyClaim(KeyClaimFn fn) { m_state->keyClaim = std::move(fn); }

    void UiRouter::click(const Entity element) { m_state->queueClick(element); }

    Entity UiRouter::focusScope() const { return m_state->focusScope(); }

    void UiRouter::tabStops(std::vector<Entity>& out) const { m_state->tabStops(out); }

    void UiRouter::landmarks(std::vector<Entity>& out) const { m_state->landmarks(out); }

    // the Tab and navigation-intent entries a client calls stand in for the keys: the moves they make are key-driven
    bool UiRouter::moveFocus(const bool backward)
    {
        const State::KeyDrive drive{*m_state};
        return m_state->moveFocus(backward);
    }

    bool UiRouter::navigate(const UiNavIntent intent)
    {
        const State::KeyDrive drive{*m_state};
        return m_state->actNav({.intent = intent});
    }

    Entity UiRouter::neighbor(const UiNavIntent direction) const { return m_state->neighborOf(direction); }

    u32 UiRouter::navClaimed() const { return m_state->navClaimed; }

    Entity UiRouter::takeNavigated() { return std::exchange(m_state->navigated, Entity::undefined()); }

    void UiRouter::subscribe(const Entity element, const UiEventKind kind, ReactionFn fn, CodeOrigin origin)
    {
        SASSERTM(kind != UiEventKind::KeyDown, "UiRouter::subscribe: KeyDown reactions go through subscribeKeys.");
        if (!origin.type)
        {
            origin = CodeOrigin::of(fn);
        }
        m_state->reactions[element][static_cast<u32>(kind)] = {std::move(fn), origin};
    }

    void UiRouter::subscribeKeys(const Entity element, KeyReactionFn fn, CodeOrigin origin)
    {
        if (fn)
        {
            if (!origin.type)
            {
                origin = CodeOrigin::of(fn);
            }
            m_state->keyReactions[element] = {std::move(fn), origin};
        }
        else
        {
            m_state->keyReactions.erase(element);
        }
    }

    void UiRouter::clearReactions(const Entity element)
    {
        State& state = *m_state;
        state.reactions.erase(element);
        state.keyReactions.erase(element);
        // the element is going: no state machine may keep it, and no event names it
        if (state.focused == element)
        {
            state.focused = Entity::undefined();
        }
        if (state.hovered == element)
        {
            state.hovered = Entity::undefined();
        }
        if (state.pressed == element || state.capture == element)
        {
            state.pressed = Entity::undefined();
            state.capture = Entity::undefined();
            state.dragged = Entity::undefined();
        }
    }

    void UiRouter::setNavigationEnabled(const bool enabled) { m_state->navigationEnabled = enabled; }

    bool UiRouter::navigationEnabled() const { return m_state->navigationEnabled; }

    bool UiRouter::keyIsNavigation() const { return m_state->navigationKey; }

    u32 UiRouter::countReactionsIn(const ImageRange& image) const
    {
        u32 count = 0;
        for (const auto& slots : m_state->reactions | std::views::values)
        {
            count += static_cast<u32>(std::ranges::count_if(slots, [&image](const State::Reaction<ReactionFn>& slot)
                                                            { return image.contains(slot.origin); }));
        }
        count += static_cast<u32>(std::ranges::count_if(m_state->keyReactions | std::views::values,
                                                        [&image](const State::Reaction<KeyReactionFn>& slot)
                                                        { return image.contains(slot.origin); }));
        return count;
    }

    UiInputSnapshot inputBelow(const UiInputSnapshot& input, const UiRouter& above, const UiRouter& below,
                               std::vector<UiKeyTransition>& keys, std::vector<UiNavTransition>& navigation)
    {
        UiInputSnapshot result = input;
        const bool modal = above.modalActive();
        // a press the lower layer captured keeps its pointer, so its drag and
        // release stay whole wherever the pointer goes
        const bool pointerAbove = above.hovered() != Entity::undefined() || above.pressed() != Entity::undefined();
        if ((pointerAbove || modal) && below.pressed() == Entity::undefined())
        {
            result.pointerPos = Vec2{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
            result.wheelDelta = vec2Zero();
        }
        const std::span<const u32> handled = above.handledKeys();
        keys.clear();
        for (const UiKeyTransition& transition : input.keyTransitions)
        {
            if (!transition.pressed || (!modal && std::ranges::find(handled, transition.scancode) == handled.end()))
            {
                keys.push_back(transition);
            }
        }
        result.keyTransitions = keys;
        // an intent the upper layer acted on is its: the layers beneath neither move nor repeat on it
        navigation.clear();
        const u32 claimed = above.navClaimed();
        for (const UiNavTransition& transition : input.navigation)
        {
            if (!modal && (claimed & (1u << static_cast<u32>(transition.intent))) == 0)
            {
                navigation.push_back(transition);
            }
        }
        result.navigation = navigation;
        result.navHeld = modal ? 0 : input.navHeld & ~claimed;
        if (modal || above.wantsTextInput())
        {
            result.text = {};
            result.composition = {};
        }
        return result;
    }
} // namespace spite
