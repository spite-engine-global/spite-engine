#pragma once

#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "base/ImageRange.hpp"
#include "ecs/EcsTypes.hpp"
#include "ui/UiEvents.hpp"
#include "ui/UiExport.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiInputSnapshot.hpp"

namespace spite
{
    class World;

    // What the router saw of the pointer this frame, for the overlay layer's observer: only a frame in which
    // something happened is reported.
    struct UiPointerObservation
    {
        Entity hovered = Entity::undefined();
        Vec2 position{0.0f, 0.0f};
        bool hoverChanged = false;
        bool moved = false;
        bool pressed = false; // a button went down this frame
        bool wheel = false;
    };

    // The transient interaction core: hover, capture/press/drag, focus, and
    // active-surface state machines over the resolved layout. Consumes a UI
    // input snapshot per frame, produces typed UI events at the dispatch
    // point — never mid-hit-test. Dispatch fires each queued event through
    // the world's event mechanism and, from the same pass, invokes the
    // presenter reaction registered for (element, kind). Reactions that
    // destroy elements are safe: tables are purged at destroy time, so later
    // queued events for a dead element deliver as ECS events only.
    class SPITE_UI_API UiRouter
    {
    public:
        // Payload pointer is typed by the event kind's payload struct.
        using ReactionFn = std::function<void(const void*)>;
        // Returns whether the element handled the key (event.element is the
        // element). An unhandled press bubbles on and, if nothing handles it
        // and keyboard navigation does not act on it, stays gameplay's.
        using KeyReactionFn = std::function<bool(const UiKeyDownEvent&)>;
        // Asked first for every key press and navigation key, before the focus chain: true claims it, so nothing in
        // the chain, no navigation and no gameplay acts on it (Escape on a shown tooltip).
        using KeyClaimFn = std::function<bool(const UiKeyTransition&)>;

        UiRouter(World& world, UiHierarchyManager& hierarchy);
        ~UiRouter();
        UiRouter(const UiRouter&) = delete;
        UiRouter& operator=(const UiRouter&) = delete;

        // Hit test against the last resolved layout, advance the state
        // machines, queue events. Does not dispatch.
        void processFrame(const UiInputSnapshot& snapshot);

        // The dispatch point: fire queued events as ECS events and invoke
        // reactions; clears the queue.
        void dispatch();

        // True when the router claimed input this frame (hover over an
        // element, active capture, or a handled key, delivered text or wheel).
        // Truthful reporting only — honoring it is the host's contract.
        [[nodiscard]] bool consumedThisFrame() const;

        // Controls the UI holds from gameplay: a mouse button pressed over an
        // element, and a key press a key reaction handled, each until its
        // release. Hover claims nothing; a press that began outside the UI is
        // never claimed. Key claims settle at dispatch.
        [[nodiscard]] bool claimsButton(u32 button) const;
        [[nodiscard]] std::span<const u32> claimedKeys() const;
        // Keys whose press a key reaction handled in this frame, including a tap released in the same frame
        // (no claim remains then); inputBelow drops these presses for the layers beneath.
        [[nodiscard]] std::span<const u32> handledKeys() const;

        // Adapter-facing request: text input is active because the focus
        // chain edits text (has a TextInput reaction). Mirrors InputSystem's
        // cursor-request pattern.
        [[nodiscard]] bool wantsTextInput() const;
        // True when this frame ended a composition the input method still holds: the input it was shown in lost
        // focus or went away. The adapter cancels it in the platform (SDL_ClearComposition). A composition that
        // ended by commit does not set it: the platform already ended it.
        [[nodiscard]] bool cancelsComposition() const;

        [[nodiscard]] Entity activeSurface() const;
        // Interaction state for the view: hovered freezes while a press holds
        // capture; pressed is the captured element until the primary button
        // is released, wherever the pointer is.
        [[nodiscard]] Entity hovered() const;
        [[nodiscard]] Entity pressed() const;
        [[nodiscard]] Entity focused() const;

        // Focus moves by pointer or explicitly; queues FocusLost/Gained.
        void requestFocus(Entity element);
        // As requestFocus, for a widget giving the focus back to where it came from (a menu or dialog that closed):
        // not keyboard navigation, so focusedByKeys is false and no tooltip arms on the restored element.
        void restoreFocus(Entity element);
        // As requestFocus, the way keyboard navigation moves it (a control client's "focus as keys would"):
        // focusedByKeys is true, so the focused look shows and a tooltip of keyboard focus arms.
        void focusAsKeys(Entity element);
        void clearFocus();

        // Queues a primary click's PointerDown, PointerUp and Click at the element's centre, as the Enter key does on
        // a focused button; they fire at the next dispatch. No hit test: the element need not be on top.
        void click(Entity element);

        // Keyboard navigation acts on key presses nothing in the focus chain
        // handled, and only then claims them: Tab and Shift+Tab move focus
        // (moveFocus), Escape clears it, Enter, keypad Enter and Space click a
        // focused Button tab stop that has a click or press reaction (PointerDown,
        // PointerUp and Click at its centre, as a primary click). Ctrl, Alt and
        // Super chords are never navigation. Navigation fires no KeyDown event.
        // Disabled, it acts on nothing and those keys stay gameplay's (the
        // editor's UI while a game plays); key reactions and moveFocus still
        // work. Enabled by default.
        void setNavigationEnabled(bool enabled);
        [[nodiscard]] bool navigationEnabled() const;
        // True while a key reaction runs for the key a navigation intent stands for (a pad's South as Return), not a
        // key the keyboard pressed: a widget that records keys tells the two apart.
        [[nodiscard]] bool keyIsNavigation() const;
        // The focus scope: the nearest focusScope (or landmark) element at or
        // above the focus, else the focus's root surface; undefined without
        // focus.
        [[nodiscard]] Entity focusScope() const;
        // The landmarks (UiStateComponent::landmark) the landmark intents visit, in paint order from the last
        // resolve: visible, not behind a modal and holding a tab stop. NextLandmark and PreviousLandmark (F6 and
        // Shift+F6, and the tab intents no widget took) focus the first tab stop of the one after or before the
        // landmark holding the focus (else the focus's paint position), wrapping; with no focus, of the first or
        // last. They do nothing when that is the landmark holding the focus.
        void landmarks(std::vector<Entity>& out) const;
        // The focus scope's tab stops (UiStateComponent::tabStop, visible and
        // interactable) in paint order, from the last resolve.
        void tabStops(std::vector<Entity>& out) const;
        // Focus to the tab stop after (backward: before) the one holding focus
        // (the focus or its nearest tab-stop ancestor, so Tab leaves a tree
        // from any of its rows), wrapping within the focus scope; from a focus
        // no stop holds, the nearest stop past it in paint order. False, and
        // focus stays, without focus or when it would not change.
        bool moveFocus(bool backward);

        // Directional navigation: the intents of the snapshot (UiNavIntent, from input actions, so
        // remapping applies) act on the focus after the frame's keys, and only with one (like keyboard navigation,
        // and disabled with it). A direction with an explicit neighbour (UiNavigationComponent, on the focus or an
        // ancestor) focuses it: the author's way out of a widget that takes every arrow; an override on an ancestor
        // beats the focused widget's own keys for a pad or remapped source (the keyboard's arrow reaches the widget
        // first), a refused one falls back to the search below. Otherwise the intent goes to the focus chain as the key
        // it stands for (Up is the Up arrow, Accept Enter, Cancel Escape, NextTab/PreviousTab Ctrl+PageDown/PageUp),
        // unless the keyboard just delivered that very key, so a menu, tree, table, slider or text input keeps its own
        // keys. Unhandled, a direction focuses the nearest tab stop that way in the focus scope, from the stop holding
        // the focus (focus on none enters the scope at its first stop); Accept clicks a focused button and Cancel
        // clears the focus as Enter and Escape do; the tab intents, unhandled, cycle the landmarks as
        // NextLandmark/PreviousLandmark (F6, Shift+F6) do, with or without focus. A key a reaction handled this
        // frame (or still holds) is not also an intent. What acted is claimed while the intent is held (navClaimed); a
        // modal claims all. navigate acts now, outside a frame (the focus command, tests): whether anything did; no
        // claim.
        bool navigate(UiNavIntent intent);
        // What a press of the direction would focus now; undefined when nothing.
        [[nodiscard]] Entity neighbor(UiNavIntent direction) const;
        // Bit per UiNavIntent that acted and is still held (the snapshot's navHeld): the adapter keeps their
        // controls off gameplay, inputBelow keeps them off the layers beneath.
        [[nodiscard]] u32 navClaimed() const;
        // The element directional navigation last focused, once: the context scrolls it into view.
        Entity takeNavigated();

        // Whether the focus last moved because of a key or a navigation intent (Tab, F6, an arrow, a pad), not a press
        // or a call: what the tooltips of keyboard focus follow.
        [[nodiscard]] bool focusedByKeys() const;

        // A modal surface is open (UiHierarchyManager::modalRoot): hit tests reach only what paints at or above it,
        // focus cannot leave it (a request for an element below it is ignored, clearing it lands on the modal's
        // root), and the router claims every key press and pointer button. The topmost layer's modal also hides
        // the pointer, wheel, keys and text from the layers below (inputBelow).
        [[nodiscard]] bool modalActive() const;
        // Whether the open modal blocks the element: it sits on a surface painted below the modal's.
        [[nodiscard]] bool blockedByModal(Entity element) const;
        // Focus to the first tab stop under `scope` in paint order (from the last resolve), else to `scope`.
        void focusInto(Entity scope);
        // Observes the frames in which the pointer moved, hover changed, a button went down or the wheel turned;
        // null removes it. The overlay layer installs it while tooltips exist, so an idle UI pays nothing.
        void setPointerObserver(std::function<void(const UiPointerObservation&)> observer);

        // Presenter-facing reaction slots, one per (element, kind); a new
        // subscribe replaces the previous. Bubbling (wheel/key/text) walks
        // ancestors until a slot exists. KeyDown takes subscribeKeys. A
        // wrapper that hides the caller's function inside its own callable
        // passes that function's origin, so countReactionsIn attributes the
        // slot to the caller's image; by default it is fn's own.
        void subscribe(Entity element, UiEventKind kind, ReactionFn fn, CodeOrigin origin = {});
        // The element's KeyDown slot. Presses and repeats route from focus at
        // dispatch through ancestors until a reaction returns handled; only
        // then the ECS event fires (after the reaction) and the press is
        // claimed.
        void subscribeKeys(Entity element, KeyReactionFn fn, CodeOrigin origin = {});
        void setKeyClaim(KeyClaimFn fn);
        // The element is being destroyed: drops its slots, and its focus,
        // hover and press without events.
        void clearReactions(Entity element);
        // reaction slots whose code lives in the image: what a module that
        // unloads left behind
        [[nodiscard]] u32 countReactionsIn(const ImageRange& image) const;

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };

    // Input layering (UI layers stacked over one target): what the
    // lower layer gets once the upper one processed `input` this frame. The
    // pointer and wheel are hidden while the upper layer hovers or presses,
    // unless the lower layer holds a press; key presses the upper layer
    // handled this frame (held or tapped) and the text it takes are dropped. Button edges pass through,
    // so a press over the upper layer clears the lower layer's focus.
    // `keys` and `navigation` back the result's key and intent transitions; an intent the upper layer acted on, and
    // every one while its modal is open, does not pass.
    [[nodiscard]] SPITE_UI_API UiInputSnapshot inputBelow(const UiInputSnapshot& input, const UiRouter& above,
                                                          const UiRouter& below, std::vector<UiKeyTransition>& keys,
                                                          std::vector<UiNavTransition>& navigation);
} // namespace spite
