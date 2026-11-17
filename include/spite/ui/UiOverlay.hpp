#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string>

#include "base/ImageRange.hpp"
#include "base/MathTypes.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiExport.hpp"
#include "ui/UiLayout.hpp"

namespace spite
{
    class World;
    class SystemContext;
    class UiHierarchyManager;
    class UiRouter;
    class UiScope;

    // The bounds of a popup that has no surface to stay inside: far enough out that no edge is ever reached.
    inline constexpr UiRect UI_UNBOUNDED_BOUNDS{Vec2{-1.0e6f, -1.0e6f}, Vec2{2.0e6f, 2.0e6f}};

    // A popup to place beside an anchor inside bounds (layout pixels), unbounded by default. `margin` keeps it that
    // far inside the bounds (the popup is clamped there) and `gap` separates it from the anchor.
    struct UiPlacementRequest
    {
        Vec2 size{0.0f, 0.0f};
        UiRect anchor; // a point anchor has zero size
        UiRect bounds = UI_UNBOUNDED_BOUNDS;
        UiPlacementSide side = UiPlacementSide::Below;
        float gap = 0.0f;
        float margin = 0.0f;
    };

    // `rect` is where the popup goes; its size is the request's, capped to the bounds minus margins (`clipped`
    // says it was: the popup's contents must clip). `side` is the side used: the preferred one if the popup fits
    // there, else the opposite one if it fits, else whichever has more room. `clamped` says the popup then moved to
    // stay inside the bounds along the axes it slides on.
    struct UiPlacement
    {
        UiRect rect;
        UiPlacementSide side = UiPlacementSide::Below;
        bool flipped = false;
        bool clamped = false;
        bool clipped = false;
    };

    // Pure and total: any finite request gives a placement; a popup larger than the bounds is capped, never looped on.
    [[nodiscard]] SPITE_UI_API UiPlacement placeUiPopup(const UiPlacementRequest& request);

    // A row of the "uiOverlays" table of the UiContext service: an open overlay, or a tooltip waiting for its delay.
    struct UiOverlayRecord
    {
        Entity element = Entity::undefined();
        std::string id;
        std::string kind; // tooltip, modal
        std::string state; // open, pending (a tooltip's delay running)
        Entity target = Entity::undefined();
        std::string placed; // the side used by the last placement
        bool flipped = false;
        bool clamped = false;
        u32 stack = 0; // a modal's depth, 1 the bottom one
        Vec4 rect{0.0f, 0.0f, 0.0f, 0.0f}; // resolved x, y, w, h in UI pixels
        float elapsed = 0.0f; // seconds of world time a pending tooltip has waited
        float delay = 0.0f;
        SPITE_OBSERVED_SCHEMA(UiOverlayRecord, element, id, kind, state, target, placed, flipped, clamped, stack, rect,
                              elapsed, delay)
    };

    // The overlay layer of one UI: tooltips and modal dialogs, each a root surface a scope created
    // (UiScope::createTooltip, createModal) so they paint above the content in the same instance stream. An
    // idle layer costs nothing: no overlay open and no tooltip waiting means no scan, no system and no observer
    // work; the pointer observer is installed only while a tooltip is attached.
    //
    // Tooltips: the pointer resting on an attached element (or a descendant without a tooltip of its own) starts
    // the delay; it runs on the world's frame delta (the "UiOverlays" system, UPDATE stage), so a paused world
    // freezes it and a fixed delta replays it. The tooltip shows at the next UiContext::processInput after the
    // delay, hides when the pointer leaves, on a press and on the wheel, and never takes hits, focus or keys.
    //
    // Modals: while one is open the hierarchy's modal root is its surface (the topmost one of a stack), so hit
    // tests, scrolls and drags reach nothing painted below it; the router keeps focus inside (Tab cycles in the
    // modal), claims every key and pointer press it did not use, and Escape closes the topmost one. Closing restores
    // the focus the modal found, or when that element is gone its parent at that time, else the modal below, else
    // none. Popups painting above the modal (menus, dropdowns) stay usable.
    class SPITE_UI_API UiOverlays
    {
    public:
        // What a scope registers for an overlay root it built: the content to parent presenter widgets under,
        // for a modal the dialog panel around it, and the placement parameters.
        struct Desc
        {
            UiOverlayKind kind = UiOverlayKind::Tooltip;
            Entity content = Entity::undefined();
            Entity dialog = Entity::undefined();
            Entity bounds = Entity::undefined(); // popups stay inside its rect; undefined: the anchor's surface
            UiPlacementSide side = UiPlacementSide::Below;
            float delay = 0.5f; // a tooltip's default delay in seconds of world time
            float gap = 6.0f; // unscaled UI pixels
            float margin = 4.0f;
            bool follow = false;
            bool closeOnEscape = true;
            bool closeOnBackdrop = false;
        };

        UiOverlays(World& world, UiHierarchyManager& hierarchy, UiRouter& router);
        ~UiOverlays();
        UiOverlays(const UiOverlays&) = delete;
        UiOverlays& operator=(const UiOverlays&) = delete;

        void add(Entity root, const Desc& desc);
        // The element is being destroyed (UiScope): an overlay root closes and leaves, a tooltip target loses its
        // tooltip. True when it was an overlay root.
        bool forget(Entity element);
        [[nodiscard]] bool isOverlay(Entity root) const;
        [[nodiscard]] Entity contentOf(Entity root) const;
        [[nodiscard]] Entity dialogOf(Entity root) const;
        [[nodiscard]] bool isOpen(Entity root) const;

        // -- tooltips -------------------------------------------------------------
        // Hovering `target` for `delay` seconds shows the overlay; one overlay may serve several targets
        // (onShow tells which). A negative delay uses the overlay's default. False for an element that is not a
        // tooltip overlay. Replaces the target's tooltip.
        bool attach(Entity target, Entity overlay, float delay = -1.0f);
        void detach(Entity target);
        // Shows now (no delay) anchored to the target's rect, or to a point (the pointer's, when the overlay
        // follows). Only one tooltip shows at a time: it replaces the shown one.
        void show(Entity overlay, Entity target);
        void showAt(Entity overlay, Vec2 point);
        void hide(Entity overlay);
        // Called just before the tooltip shows, with the target (undefined for a point), so a shared overlay can
        // set its content.
        void onShow(Entity overlay, std::function<void(Entity target)> fn);

        // -- modals ---------------------------------------------------------------
        // `surface` (optional) names the element whose surface bounds the modal when it has no bounds of its own.
        void open(Entity modal, Entity surface = Entity::undefined());
        void close(Entity modal);
        void onClose(Entity modal, std::function<void()> fn);
        // The modals open, bottom first.
        [[nodiscard]] std::span<const Entity> modals() const;
        [[nodiscard]] Entity topModal() const;
        // What Escape does: closes the topmost modal, else hides the shown tooltip. The overlay it closed, or
        // undefined when nothing was open.
        Entity dismiss();

        [[nodiscard]] Entity shownTooltip() const;
        // Escape on a tooltip: hides the shown one and returns true (the key is then spent); a pending one is only
        // cancelled, so the key still acts (false).
        bool dismissTooltip();
        [[nodiscard]] u32 openCount() const;
        [[nodiscard]] bool pendingTooltip() const;

        // The "UiOverlays" system entry: advances a pending tooltip's delay by the world's frame delta.
        void advance(SystemContext& ctx);
        // The frame entry (UiContext::processInput), before the layout resolve: shows a tooltip whose delay ran
        // out.
        void update();
        // After the frame's layout resolve: refits open overlays whose content, anchor or bounds changed in it, so
        // the frame paints the new window; only the overlay roots resolve again.
        void refit();
        void retireIdleSystem();

        [[nodiscard]] u32 countCallbacksIn(const ImageRange& image) const;

    private:
        friend class UiContext;

        // Adds the "uiOverlays" table to the UiContext service; UiContext calls it once registered.
        void registerTable();

        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
