#include "ui/UiOverlay.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "base/Handle.hpp"
#include "base/ServiceLocator.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "ecs/LifecycleEvents.hpp"
#include "ecs/SystemContext.hpp"
#include "ecs/World.hpp"
#include "ui/UiEvents.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        constexpr u32 KEY_ESCAPE = 41; // HID scancode
        constexpr u32 MAX_DEPTH = 64;
        constexpr float EPSILON = 0.01f;
        // A pointer-anchored tooltip clears the cursor image drawn below its hotspot (unscaled pixels).
        constexpr float CURSOR_EXTENT = 16.0f;

        constexpr UiPlacementSide opposite(const UiPlacementSide side)
        {
            switch (side)
            {
            case UiPlacementSide::Below:
                return UiPlacementSide::Above;
            case UiPlacementSide::Above:
                return UiPlacementSide::Below;
            case UiPlacementSide::Right:
                return UiPlacementSide::Left;
            case UiPlacementSide::Left:
                return UiPlacementSide::Right;
            }
            return side;
        }

        constexpr std::string_view sideName(const UiPlacementSide side)
        {
            switch (side)
            {
            case UiPlacementSide::Below:
                return "below";
            case UiPlacementSide::Above:
                return "above";
            case UiPlacementSide::Right:
                return "right";
            case UiPlacementSide::Left:
                return "left";
            }
            return "";
        }

        float clampTo(const float value, const float low, const float high)
        {
            return std::max(low, std::min(value, high));
        }

        InternedString childId(const InternedString id, const std::string_view suffix)
        {
            return InternedString(std::string(id.toView()) + std::string(suffix));
        }

        UiRectDesc topLeft()
        {
            UiRectDesc desc;
            desc.pivot = Vec2{0.0f, 0.0f};
            return desc;
        }

        // A column that sizes to its items, padded on all sides.
        UiRectDesc paddedColumn(const float padding)
        {
            UiRectDesc desc = topLeft();
            desc.contentSize = UiContentSizeComponent{};
            UiLayoutComponent layout;
            layout.direction = UiLayoutDirection::Column;
            layout.padding = Vec4{padding, padding, padding, padding};
            desc.layout = layout;
            return desc;
        }
    } // namespace

    UiPlacement placeUiPopup(const UiPlacementRequest& request)
    {
        const Vec2 low{request.bounds.position.x + request.margin, request.bounds.position.y + request.margin};
        const Vec2 area{std::max(0.0f, request.bounds.size.x - 2.0f * request.margin),
                        std::max(0.0f, request.bounds.size.y - 2.0f * request.margin)};
        const Vec2 high{low.x + area.x, low.y + area.y};
        UiPlacement out;
        out.rect.size = Vec2{std::min(request.size.x, area.x), std::min(request.size.y, area.y)};
        out.clipped = out.rect.size != request.size;
        const Vec2 size = out.rect.size;
        const UiRect& anchor = request.anchor;
        const float gap = request.gap;

        const auto room = [&](const UiPlacementSide side)
        {
            switch (side)
            {
            case UiPlacementSide::Below:
                return high.y - (anchor.position.y + anchor.size.y + gap);
            case UiPlacementSide::Above:
                return anchor.position.y - gap - low.y;
            case UiPlacementSide::Right:
                return high.x - (anchor.position.x + anchor.size.x + gap);
            case UiPlacementSide::Left:
                return anchor.position.x - gap - low.x;
            }
            return 0.0f;
        };
        const auto need = [&](const UiPlacementSide side)
        { return side == UiPlacementSide::Below || side == UiPlacementSide::Above ? size.y : size.x; };

        UiPlacementSide side = request.side;
        // the preferred side first; the opposite one when it fits better (it fits, or has more room)
        if (room(side) < need(side) && room(opposite(side)) > room(side))
        {
            side = opposite(side);
            out.flipped = true;
        }
        out.side = side;

        Vec2 position{};
        switch (side)
        {
        case UiPlacementSide::Below:
            position = Vec2{anchor.position.x, anchor.position.y + anchor.size.y + gap};
            break;
        case UiPlacementSide::Above:
            position = Vec2{anchor.position.x, anchor.position.y - gap - size.y};
            break;
        case UiPlacementSide::Right:
            position = Vec2{anchor.position.x + anchor.size.x + gap, anchor.position.y};
            break;
        case UiPlacementSide::Left:
            position = Vec2{anchor.position.x - gap - size.x, anchor.position.y};
            break;
        }
        const Vec2 clamped{clampTo(position.x, low.x, high.x - size.x), clampTo(position.y, low.y, high.y - size.y)};
        out.clamped = std::abs(clamped.x - position.x) > EPSILON || std::abs(clamped.y - position.y) > EPSILON;
        out.rect.position = clamped;
        return out;
    }

    // -- UiOverlays ---------------------------------------------------------------

    struct UiOverlays::State
    {
        // What a placement was computed from: a change of any part re-places the overlay.
        struct Fit
        {
            Vec2 content{0.0f, 0.0f};
            UiRect anchor;
            UiRect bounds;
            // within a rounding step: a rect round-trips through the unscaled pixels it is stored in
            [[nodiscard]] bool near(const Fit& other) const
            {
                const auto close = [](const Vec2 a, const Vec2 b)
                { return std::abs(a.x - b.x) <= EPSILON && std::abs(a.y - b.y) <= EPSILON; };
                return close(content, other.content) && close(anchor.position, other.anchor.position) &&
                    close(anchor.size, other.anchor.size) && close(bounds.position, other.bounds.position) &&
                    close(bounds.size, other.bounds.size);
            }
        };

        struct Overlay
        {
            Desc desc;
            Entity root = Entity::undefined();
            bool open = false;
            bool byHover = false; // a tooltip a delay showed (it hides when the pointer leaves or presses)
            bool byFocus = false; // ... the delay of keyboard focus: it hides when the focus moves
            bool atPoint = false;
            Vec2 point{0.0f, 0.0f};
            Entity target = Entity::undefined(); // tooltip target
            Entity surface = Entity::undefined(); // modal: the element whose surface bounds it
            Entity returnFocus = Entity::undefined();
            Entity returnParent = Entity::undefined();
            Fit fit;
            std::function<void(Entity)> onShow;
            std::function<void()> onClose;
        };

        struct Pending
        {
            Entity target = Entity::undefined();
            Entity overlay = Entity::undefined();
            float elapsed = 0.0f;
            float delay = 0.0f;
            bool active = false;
            bool due = false;
            bool byFocus = false; // armed by keyboard focus on the target, not by the pointer resting on it
        };

        World* world = nullptr;
        UiHierarchyManager* hierarchy = nullptr;
        UiRouter* router = nullptr;
        std::unordered_map<Entity, Overlay> overlays;
        std::unordered_map<Entity, Entity> tooltipOf; // target -> overlay root
        std::vector<Entity> stack; // open modals, bottom first
        Entity shown = Entity::undefined(); // the shown tooltip's root
        Entity hoverOwner = Entity::undefined(); // the target whose tooltip the pointer rests on
        Entity focusOwner = Entity::undefined(); // the target holding keyboard focus (or an ancestor of it)
        Entity focusSeen = Entity::undefined(); // the focus update() last compared
        Vec2 pointer{0.0f, 0.0f}; // as the observer last saw it
        Pending pending;
        bool systemOn = false;
        u64 fitGeneration = ~u64{0};
        Handle removedSubscription;

        Overlay* find(const Entity root)
        {
            const auto it = overlays.find(root);
            return it == overlays.end() ? nullptr : &it->second;
        }

        [[nodiscard]] float scale() const { return hierarchy->layoutScale(); }

        [[nodiscard]] bool laidOut(const Entity element) const
        {
            return hierarchy->isElement(element) && hierarchy->paintIndexOf(element) != UiHierarchyManager::NO_INDEX;
        }

        [[nodiscard]] Entity surfaceOf(Entity element) const
        {
            for (u32 depth = 0; depth < MAX_DEPTH; ++depth)
            {
                const Entity parent = hierarchy->parentOf(element);
                if (parent == Entity::undefined())
                {
                    return element;
                }
                element = parent;
            }
            return Entity::undefined();
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

        void setSystem(const bool on)
        {
            systemOn = on;
            world->setSystemEnabled(SystemName{nullptr, "UiOverlays"}, on);
        }

        void setVisible(const Entity element, const bool visible) const
        {
            world->getComponent<UiStateComponent>(element).visible = visible;
            hierarchy->markLayoutDirty(element);
        }

        // Writes the element's rect when it differs (px = laid-out pixels, the rect holds unscaled ones).
        void setRect(const Entity element, const Vec2 position, const Vec2 size) const
        {
            auto& rect = world->getComponent<UiRectComponent>(element);
            const float inverse = 1.0f / scale();
            const Vec2 offset = position * inverse;
            const Vec2 extent = size * inverse;
            const auto close = [](const Vec2 a, const Vec2 b)
            { return std::abs(a.x - b.x) <= EPSILON && std::abs(a.y - b.y) <= EPSILON; };
            if (close(rect.offset, offset) && close(rect.size, extent) && rect.anchor == Vec2{0.0f, 0.0f} &&
                rect.pivot == Vec2{0.0f, 0.0f})
            {
                return;
            }
            rect.anchor = Vec2{0.0f, 0.0f};
            rect.pivot = Vec2{0.0f, 0.0f};
            rect.offset = offset;
            rect.size = extent;
            hierarchy->markLayoutDirty(element);
        }

        void sync(const Overlay& overlay, const UiPlacement* placement = nullptr) const
        {
            auto& state = world->getComponent<UiOverlayComponent>(overlay.root);
            state.open = overlay.open;
            state.target =
                overlay.open && overlay.desc.kind == UiOverlayKind::Tooltip ? overlay.target : Entity::undefined();
            if (placement)
            {
                state.placed = placement->side;
                state.flipped = placement->flipped;
                state.clamped = placement->clamped;
            }
            const auto at = std::ranges::find(stack, overlay.root);
            state.stack = at == stack.end() ? 0u : static_cast<u32>(at - stack.begin()) + 1u;
        }

        // The rect the popup stays inside, in laid-out pixels.
        [[nodiscard]] UiRect boundsOf(const Overlay& overlay) const
        {
            if (laidOut(overlay.desc.bounds))
            {
                return hierarchy->absoluteRectOf(overlay.desc.bounds);
            }
            Entity anchored = overlay.desc.kind == UiOverlayKind::Modal ? overlay.surface
                : overlay.target != Entity::undefined()                 ? overlay.target
                                                                        : router->hovered();
            if (hierarchy->isElement(anchored) && !isWithin(anchored, overlay.root))
            {
                const Entity surface = surfaceOf(anchored);
                if (laidOut(surface))
                {
                    return hierarchy->absoluteRectOf(surface);
                }
            }
            if (overlay.desc.kind == UiOverlayKind::Modal)
            {
                const auto& rect = world->getConstComponent<UiRectComponent>(overlay.root);
                return UiRect{rect.offset * scale(), rect.size * scale()};
            }
            return UI_UNBOUNDED_BOUNDS;
        }

        [[nodiscard]] Fit fitOf(const Overlay& overlay) const
        {
            Fit fit;
            if (laidOut(overlay.desc.content))
            {
                fit.content = hierarchy->absoluteRectOf(overlay.desc.content).size;
            }
            fit.bounds = boundsOf(overlay);
            if (overlay.desc.kind == UiOverlayKind::Tooltip)
            {
                if (overlay.atPoint)
                {
                    fit.anchor = UiRect{overlay.point, Vec2{0.0f, CURSOR_EXTENT * scale()}};
                }
                else if (laidOut(overlay.target))
                {
                    fit.anchor = hierarchy->absoluteRectOf(overlay.target);
                }
            }
            return fit;
        }

        void place(Overlay& overlay)
        {
            // a popup opens in the OS window of what it is anchored to
            const Entity anchored = overlay.desc.kind == UiOverlayKind::Modal ? overlay.surface
                : overlay.target != Entity::undefined()                       ? overlay.target
                                                                              : router->hovered();
            if (hierarchy->isElement(anchored) && !isWithin(anchored, overlay.root))
            {
                if (const u64 window = hierarchy->windowOf(anchored); window != hierarchy->windowOf(overlay.root))
                {
                    hierarchy->bindRootToWindow(overlay.root, window);
                }
            }
            const Fit fit = fitOf(overlay);
            overlay.fit = fit;
            const float pixel = scale();
            if (overlay.desc.kind == UiOverlayKind::Tooltip)
            {
                UiPlacementRequest request;
                request.size = fit.content;
                request.anchor = fit.anchor;
                request.bounds = fit.bounds;
                request.side = overlay.desc.side;
                request.gap = overlay.desc.gap * pixel;
                request.margin = overlay.desc.margin * pixel;
                const UiPlacement placement = placeUiPopup(request);
                setRect(overlay.root, placement.rect.position, placement.rect.size);
                sync(overlay, &placement);
                return;
            }
            // a modal: the backdrop covers the bounds, the dialog is centered by its anchor and capped to them
            setRect(overlay.root, fit.bounds.position, fit.bounds.size);
            const float margin = overlay.desc.margin * pixel;
            const Vec2 area{std::max(0.0f, fit.bounds.size.x - 2.0f * margin),
                            std::max(0.0f, fit.bounds.size.y - 2.0f * margin)};
            const Vec2 size{std::min(fit.content.x, area.x), std::min(fit.content.y, area.y)};
            auto& dialog = world->getComponent<UiRectComponent>(overlay.desc.dialog);
            const Vec2 unscaled = size * (1.0f / pixel);
            if (std::abs(dialog.size.x - unscaled.x) > EPSILON || std::abs(dialog.size.y - unscaled.y) > EPSILON)
            {
                dialog.size = unscaled;
                hierarchy->markLayoutDirty(overlay.desc.dialog);
            }
            UiPlacement placement;
            placement.clamped = size != fit.content;
            sync(overlay, &placement);
        }

        // -- tooltips ---------------------------------------------------------

        void showTooltip(Overlay& overlay, const Entity target, const bool atPoint, const Vec2 point,
                         const bool byHover, const bool byFocus = false)
        {
            if (shown != Entity::undefined() && shown != overlay.root)
            {
                if (Overlay* other = find(shown))
                {
                    hideTooltip(*other);
                }
            }
            if (overlay.open && overlay.target != target && hierarchy->isElement(overlay.target))
            {
                if (auto* previous = world->tryGetComponent<UiTooltipComponent>(overlay.target))
                {
                    previous->shown = false;
                }
            }
            overlay.target = target;
            overlay.atPoint = atPoint;
            overlay.point = point;
            overlay.byHover = byHover;
            overlay.byFocus = byFocus;
            if (overlay.onShow)
            {
                const auto fn = overlay.onShow; // it may replace itself
                fn(target);
            }
            const bool wasOpen = overlay.open;
            overlay.open = true;
            shown = overlay.root;
            if (!wasOpen)
            {
                setVisible(overlay.root, true);
                hierarchy->setHitTransparent(overlay.root, true);
            }
            if (auto* tooltip =
                    hierarchy->isElement(target) ? world->tryGetComponent<UiTooltipComponent>(target) : nullptr)
            {
                tooltip->shown = true;
            }
            // the content measures at the next resolve; the placement reads that size
            hierarchy->markLayoutDirty(overlay.desc.content);
            hierarchy->resolveDirty();
            place(overlay);
            hierarchy->resolveDirty();
        }

        void hideTooltip(Overlay& overlay)
        {
            if (!overlay.open)
            {
                return;
            }
            overlay.open = false;
            overlay.byHover = false;
            overlay.byFocus = false;
            if (shown == overlay.root)
            {
                shown = Entity::undefined();
            }
            hierarchy->setHitTransparent(overlay.root, false);
            if (world->isEntityAlive(overlay.root))
            {
                setVisible(overlay.root, false);
                sync(overlay);
            }
            if (auto* tooltip = hierarchy->isElement(overlay.target)
                    ? world->tryGetComponent<UiTooltipComponent>(overlay.target)
                    : nullptr)
            {
                tooltip->shown = false;
            }
        }

        [[nodiscard]] Entity ownerOf(Entity hovered) const
        {
            for (u32 depth = 0; hovered != Entity::undefined() && depth < MAX_DEPTH; ++depth)
            {
                if (tooltipOf.contains(hovered))
                {
                    return hovered;
                }
                hovered = hierarchy->parentOf(hovered);
            }
            return Entity::undefined();
        }

        void cancelPending() { pending = Pending{}; }

        // The focus moved since the last frame: a tooltip it showed hides; keyboard focus (a key, not a press or a
        // call) on a target arms that target's tooltip, shown anchored to its rect after the delay.
        void syncFocus()
        {
            const Entity focus = router->focused();
            if (focus == focusSeen)
            {
                return;
            }
            focusSeen = focus;
            focusOwner = router->focusedByKeys() ? ownerOf(focus) : Entity::undefined();
            Overlay* current = shown == Entity::undefined() ? nullptr : find(shown);
            if (pending.active && pending.byFocus)
            {
                cancelPending();
            }
            if (current && current->byFocus)
            {
                hideTooltip(*current);
                current = nullptr;
            }
            if (focusOwner != Entity::undefined())
            {
                cancelPending();
                if (current && current->byHover)
                {
                    hideTooltip(*current);
                }
                arm(focusOwner, true);
            }
        }

        void arm(const Entity owner, const bool byFocus = false)
        {
            const auto it = tooltipOf.find(owner);
            const auto* tooltip = world->tryGetConstComponent<UiTooltipComponent>(owner);
            if (it == tooltipOf.end() || !tooltip)
            {
                return;
            }
            pending = Pending{owner, it->second, 0.0f, tooltip->delay, true, tooltip->delay <= 0.0f, byFocus};
            if (!pending.due && !systemOn)
            {
                setSystem(true);
            }
        }

        // The pointer moved onto another tooltip target (or off all): the shown tooltip hides, the new delay starts.
        void setHoverOwner(const Entity owner)
        {
            if (owner == hoverOwner)
            {
                return;
            }
            hoverOwner = owner;
            cancelPending();
            if (Overlay* current = shown == Entity::undefined() ? nullptr : find(shown); current && current->byHover)
            {
                hideTooltip(*current);
            }
            if (owner != Entity::undefined())
            {
                arm(owner);
            }
        }

        // A root destroyed with a raw world call (UiScope::destroy forgets it first): the hit floor, the stack and
        // the tables must not keep pointing at it.
        void onRootRemoved(const Entity root)
        {
            const auto it = overlays.find(root);
            if (it == overlays.end())
            {
                return;
            }
            if (it->second.open && it->second.desc.kind == UiOverlayKind::Modal)
            {
                std::erase(stack, root);
                hierarchy->setModalRoot(stack.empty() ? Entity::undefined() : stack.back());
            }
            if (shown == root)
            {
                shown = Entity::undefined();
            }
            hierarchy->setHitTransparent(root, false);
            if (pending.overlay == root)
            {
                cancelPending();
            }
            std::erase_if(tooltipOf, [root](const auto& entry) { return entry.second == root; });
            overlays.erase(it);
            syncObserver();
        }

        void observe(const UiPointerObservation& seen)
        {
            pointer = seen.position;
            Overlay* current = shown == Entity::undefined() ? nullptr : find(shown);
            if (seen.pressed || seen.wheel)
            {
                // a press or scroll dismisses the tooltip; it comes back after the pointer re-enters a target
                cancelPending();
                if (current && current->byHover)
                {
                    hideTooltip(*current);
                }
            }
            if (seen.hoverChanged)
            {
                setHoverOwner(ownerOf(seen.hovered));
            }
            else if (seen.moved && current && current->byHover && current->desc.follow && current->open)
            {
                current->point = seen.position;
                current->atPoint = true;
                place(*current);
            }
        }

        // The pointer observer lives only while a tooltip is attached, so a UI without tooltips pays no per-frame
        // pointer work.
        void syncObserver()
        {
            if (tooltipOf.empty())
            {
                router->setPointerObserver(nullptr);
                hoverOwner = focusOwner = focusSeen = Entity::undefined();
                cancelPending();
            }
            else
            {
                router->setPointerObserver([this](const UiPointerObservation& seen) { observe(seen); });
            }
        }

        void detachTarget(const Entity target, const bool alive)
        {
            const auto it = tooltipOf.find(target);
            if (it == tooltipOf.end())
            {
                return;
            }
            if (Overlay* overlay = find(it->second); overlay && overlay->open && overlay->target == target)
            {
                hideTooltip(*overlay);
            }
            tooltipOf.erase(it);
            if (hoverOwner == target)
            {
                hoverOwner = Entity::undefined();
            }
            if (focusOwner == target)
            {
                focusOwner = Entity::undefined();
            }
            if (pending.target == target)
            {
                cancelPending();
            }
            if (alive)
            {
                world->removeComponent<UiTooltipComponent>(target);
            }
            syncObserver();
        }

        // -- modals -----------------------------------------------------------

        void openModal(Overlay& overlay, const Entity surface)
        {
            if (overlay.open)
            {
                return;
            }
            overlay.surface = surface;
            const Entity focus = router->focused();
            const bool outside = hierarchy->isElement(focus) && !isWithin(focus, overlay.root);
            overlay.returnFocus = outside ? focus : Entity::undefined();
            overlay.returnParent = outside ? hierarchy->parentOf(focus) : Entity::undefined();
            if (hoverOwner != Entity::undefined() || shown != Entity::undefined())
            {
                // tooltips below the modal end with the hover they belonged to
                cancelPending();
                if (Overlay* tip = shown == Entity::undefined() ? nullptr : find(shown))
                {
                    hideTooltip(*tip);
                }
            }
            overlay.open = true;
            stack.push_back(overlay.root);
            setVisible(overlay.root, true);
            hierarchy->setModalRoot(overlay.root);
            hierarchy->markLayoutDirty(overlay.desc.content);
            place(overlay); // the backdrop's rect
            hierarchy->resolveDirty(); // the content measures
            place(overlay); // the dialog's size
            hierarchy->resolveDirty();
            if (stack.size() > 1)
            {
                // later opened is higher: raise the root when it was created to paint below the modal it covers
                const Entity below = stack[stack.size() - 2];
                if (hierarchy->paintIndexOf(overlay.root) < hierarchy->paintIndexOf(below))
                {
                    auto& rect = world->getComponent<UiRectComponent>(overlay.root);
                    rect.zOffset = world->getConstComponent<UiRectComponent>(below).zOffset + 1;
                    hierarchy->markLayoutDirty();
                    hierarchy->resolveDirty();
                }
            }
            router->focusInto(overlay.root);
            sync(overlay);
        }

        [[nodiscard]] bool interactable(const Entity element) const
        {
            const auto* state = world->tryGetConstComponent<UiStateComponent>(element);
            return !state || state->interactable;
        }

        void closeModal(Overlay& overlay, const bool notify)
        {
            if (!overlay.open)
            {
                return;
            }
            overlay.open = false;
            std::erase(stack, overlay.root);
            hierarchy->setModalRoot(stack.empty() ? Entity::undefined() : stack.back());
            const bool alive = world->isEntityAlive(overlay.root);
            if (alive)
            {
                setVisible(overlay.root, false);
                sync(overlay);
                for (const Entity open : stack)
                {
                    if (Overlay* other = find(open))
                    {
                        sync(*other);
                    }
                }
            }
            hierarchy->resolveDirty(); // the closed root is hidden for the candidates' visibility test
            const Entity focus = router->focused();
            // a focus that already left (a popup above, a request elsewhere) stays; one inside returns
            if (!hierarchy->isElement(focus) || isWithin(focus, overlay.root))
            {
                Entity back = Entity::undefined();
                for (const Entity candidate : {overlay.returnFocus, overlay.returnParent})
                {
                    if (hierarchy->isElement(candidate) && !isWithin(candidate, overlay.root) &&
                        !router->blockedByModal(candidate) &&
                        (!laidOut(candidate) || hierarchy->isVisibleOf(candidate)) && interactable(candidate))
                    {
                        back = candidate;
                        break;
                    }
                }
                if (back != Entity::undefined())
                {
                    router->restoreFocus(back);
                }
                else if (!stack.empty())
                {
                    router->focusInto(stack.back());
                }
                else
                {
                    router->clearFocus();
                }
            }
            overlay.returnFocus = overlay.returnParent = Entity::undefined();
            if (notify && overlay.onClose)
            {
                const auto fn = overlay.onClose; // it may replace itself or destroy the modal
                fn();
            }
        }

        [[nodiscard]] bool anyOpen() const { return !stack.empty() || shown != Entity::undefined(); }

        void refitOne(const Entity root)
        {
            Overlay* overlay = find(root);
            if (!overlay || !overlay->open || !world->isEntityAlive(root) ||
                (overlay->desc.kind == UiOverlayKind::Modal && !world->isEntityAlive(overlay->desc.dialog)))
            {
                return;
            }
            if (overlay->desc.kind == UiOverlayKind::Tooltip && !overlay->atPoint &&
                (!hierarchy->isElement(overlay->target) || !laidOut(overlay->target) ||
                 !hierarchy->isVisibleOf(overlay->target)))
            {
                hideTooltip(*overlay); // its target went away or hid
                return;
            }
            if (!fitOf(*overlay).near(overlay->fit))
            {
                place(*overlay);
            }
        }

        // After the frame's resolve: an overlay whose content, anchor or bounds changed gets its window refit, and
        // only the overlay roots resolve again (the dirty set holds nothing else).
        void refit()
        {
            if (!anyOpen() || hierarchy->layoutGeneration() == fitGeneration)
            {
                return;
            }
            for (size_t i = 0; i < stack.size(); ++i)
            {
                refitOne(stack[i]);
            }
            if (shown != Entity::undefined())
            {
                refitOne(shown);
            }
            hierarchy->resolveDirty();
            fitGeneration = hierarchy->layoutGeneration();
        }
    };

    UiOverlays::UiOverlays(World& world, UiHierarchyManager& hierarchy, UiRouter& router) :
        m_state(std::make_unique<State>())
    {
        m_state->world = &world;
        m_state->hierarchy = &hierarchy;
        m_state->router = &router;
        State* state = m_state.get();
        m_state->removedSubscription = world.lifecycleEvents().onComponentRemoved(
            getComponentId<UiRectComponent>(), [state](const Entity entity) { state->onRootRemoved(entity); });
    }

    UiOverlays::~UiOverlays()
    {
        // the router and hierarchy outlive this object and must not keep pointing into it
        m_state->world->lifecycleEvents().unsubscribe(m_state->removedSubscription);
        m_state->router->setPointerObserver(nullptr);
        m_state->hierarchy->setModalRoot(Entity::undefined());
    }

    void UiOverlays::add(const Entity root, const Desc& desc)
    {
        State& state = *m_state;
        State::Overlay& overlay = state.overlays[root];
        overlay = State::Overlay{};
        overlay.desc = desc;
        overlay.root = root;
        if (desc.kind != UiOverlayKind::Modal)
        {
            return;
        }
        // Escape closes the topmost modal: keys bubble from the trapped focus up to the root
        state.router->subscribeKeys(
            root,
            [this, root](const UiKeyDownEvent& event)
            {
                State::Overlay* modal = m_state->find(root);
                if (!modal || !modal->open || !modal->desc.closeOnEscape || event.scancode != KEY_ESCAPE ||
                    event.repeat ||
                    (event.modifiers & (UiKeyModifier::CTRL | UiKeyModifier::ALT | UiKeyModifier::SUPER)) != 0)
                {
                    return false;
                }
                close(root);
                return true;
            });
        // a click on the dimmed backdrop itself (the dialog takes its own hits)
        state.router->subscribe(root, UiEventKind::Click,
                                [this, root](const void*)
                                {
                                    const State::Overlay* modal = m_state->find(root);
                                    if (modal && modal->open && modal->desc.closeOnBackdrop)
                                    {
                                        close(root);
                                    }
                                });
    }

    bool UiOverlays::forget(const Entity element)
    {
        State& state = *m_state;
        state.detachTarget(element, false);
        State::Overlay* overlay = state.find(element);
        if (!overlay)
        {
            return false;
        }
        if (overlay->desc.kind == UiOverlayKind::Modal)
        {
            state.closeModal(*overlay, false);
        }
        else
        {
            state.hideTooltip(*overlay);
        }
        std::vector<Entity> targets;
        for (const auto& [target, root] : state.tooltipOf)
        {
            if (root == element)
            {
                targets.push_back(target);
            }
        }
        for (const Entity target : targets)
        {
            state.detachTarget(target, state.world->isEntityAlive(target));
        }
        if (state.pending.overlay == element)
        {
            state.cancelPending();
        }
        state.overlays.erase(element);
        return true;
    }

    bool UiOverlays::isOverlay(const Entity root) const { return m_state->overlays.contains(root); }

    Entity UiOverlays::contentOf(const Entity root) const
    {
        const auto it = m_state->overlays.find(root);
        return it == m_state->overlays.end() ? Entity::undefined() : it->second.desc.content;
    }

    Entity UiOverlays::dialogOf(const Entity root) const
    {
        const auto it = m_state->overlays.find(root);
        return it == m_state->overlays.end() ? Entity::undefined() : it->second.desc.dialog;
    }

    bool UiOverlays::isOpen(const Entity root) const
    {
        const auto it = m_state->overlays.find(root);
        return it != m_state->overlays.end() && it->second.open;
    }

    bool UiOverlays::attach(const Entity target, const Entity overlay, const float delay)
    {
        State& state = *m_state;
        const State::Overlay* found = state.find(overlay);
        if (!found || found->desc.kind != UiOverlayKind::Tooltip || !state.hierarchy->isElement(target))
        {
            return false;
        }
        const float resolved = std::max(0.0f, delay < 0.0f ? found->desc.delay : delay);
        const auto existing = state.tooltipOf.find(target);
        if (existing != state.tooltipOf.end() && existing->second == overlay)
        {
            // the same tooltip again: a shown one stays shown, a running delay keeps its elapsed time
            if (auto* component = state.world->tryGetComponent<UiTooltipComponent>(target))
            {
                component->delay = resolved;
            }
            if (state.pending.active && state.pending.target == target)
            {
                state.pending.delay = resolved;
                state.pending.due = state.pending.elapsed >= resolved;
            }
            return true;
        }
        if (existing != state.tooltipOf.end())
        {
            state.detachTarget(target, true);
        }
        UiTooltipComponent component;
        component.overlay = overlay;
        component.delay = resolved;
        state.world->addComponent<UiTooltipComponent>(target, component);
        state.tooltipOf[target] = overlay;
        state.syncObserver();
        // the pointer may already rest on the target: the hover change that arms it has passed
        if (const Entity hovered = state.router->hovered(); state.hierarchy->isElement(hovered))
        {
            state.setHoverOwner(state.ownerOf(hovered));
        }
        return true;
    }

    void UiOverlays::detach(const Entity target)
    {
        m_state->detachTarget(target, m_state->hierarchy->isElement(target));
    }

    void UiOverlays::show(const Entity overlay, const Entity target)
    {
        State& state = *m_state;
        State::Overlay* found = state.find(overlay);
        if (found && found->desc.kind == UiOverlayKind::Tooltip && state.hierarchy->isElement(target))
        {
            state.cancelPending();
            state.showTooltip(*found, target, false, Vec2{0.0f, 0.0f}, false);
        }
    }

    void UiOverlays::showAt(const Entity overlay, const Vec2 point)
    {
        State& state = *m_state;
        State::Overlay* found = state.find(overlay);
        if (found && found->desc.kind == UiOverlayKind::Tooltip)
        {
            state.cancelPending();
            state.showTooltip(*found, Entity::undefined(), true, point, false);
        }
    }

    void UiOverlays::hide(const Entity overlay)
    {
        State& state = *m_state;
        if (State::Overlay* found = state.find(overlay); found && found->desc.kind == UiOverlayKind::Tooltip)
        {
            state.hideTooltip(*found);
        }
    }

    void UiOverlays::onShow(const Entity overlay, std::function<void(Entity)> fn)
    {
        if (State::Overlay* found = m_state->find(overlay))
        {
            found->onShow = std::move(fn);
        }
    }

    void UiOverlays::open(const Entity modal, const Entity surface)
    {
        State& state = *m_state;
        if (State::Overlay* found = state.find(modal); found && found->desc.kind == UiOverlayKind::Modal)
        {
            state.openModal(*found, surface);
        }
    }

    void UiOverlays::close(const Entity modal)
    {
        State& state = *m_state;
        if (State::Overlay* found = state.find(modal); found && found->desc.kind == UiOverlayKind::Modal)
        {
            state.closeModal(*found, true);
        }
    }

    void UiOverlays::onClose(const Entity modal, std::function<void()> fn)
    {
        if (State::Overlay* found = m_state->find(modal))
        {
            found->onClose = std::move(fn);
        }
    }

    std::span<const Entity> UiOverlays::modals() const { return m_state->stack; }

    Entity UiOverlays::topModal() const { return m_state->stack.empty() ? Entity::undefined() : m_state->stack.back(); }

    Entity UiOverlays::dismiss()
    {
        State& state = *m_state;
        if (!state.stack.empty())
        {
            const Entity top = state.stack.back();
            close(top);
            return top;
        }
        if (state.shown != Entity::undefined())
        {
            const Entity tip = state.shown;
            hide(tip);
            return tip;
        }
        return Entity::undefined();
    }

    Entity UiOverlays::shownTooltip() const { return m_state->shown; }

    bool UiOverlays::dismissTooltip()
    {
        State& state = *m_state;
        state.cancelPending();
        if (state.shown == Entity::undefined())
        {
            return false;
        }
        hide(state.shown);
        return true;
    }

    u32 UiOverlays::openCount() const
    {
        return static_cast<u32>(m_state->stack.size()) + (m_state->shown != Entity::undefined() ? 1u : 0u);
    }

    bool UiOverlays::pendingTooltip() const { return m_state->pending.active; }

    void UiOverlays::advance(SystemContext& ctx)
    {
        State::Pending& pending = m_state->pending;
        if (!pending.active || pending.due)
        {
            return;
        }
        pending.elapsed += ctx.deltaTime;
        pending.due = pending.elapsed >= pending.delay;
    }

    void UiOverlays::update()
    {
        State& state = *m_state;
        if (!state.tooltipOf.empty())
        {
            state.syncFocus();
        }
        if (state.pending.active && state.pending.due)
        {
            const State::Pending due = state.pending;
            state.cancelPending();
            State::Overlay* overlay = state.find(due.overlay);
            // the pointer (or the keyboard focus) must still rest on the target, and no modal may cover it
            const Entity resting = due.byFocus ? state.focusOwner : state.hoverOwner;
            if (overlay && resting == due.target && state.hierarchy->isElement(due.target))
            {
                state.showTooltip(*overlay, due.target, !due.byFocus && overlay->desc.follow, state.pointer, true,
                                  due.byFocus);
            }
        }
        retireIdleSystem();
    }

    void UiOverlays::refit() { m_state->refit(); }

    void UiOverlays::retireIdleSystem()
    {
        if (m_state->systemOn && !m_state->pending.active)
        {
            m_state->setSystem(false);
        }
    }

    u32 UiOverlays::countCallbacksIn(const ImageRange& image) const
    {
        u32 count = 0;
        for (const auto& [root, overlay] : m_state->overlays)
        {
            count += u32{image.contains(overlay.onShow)} + u32{image.contains(overlay.onClose)};
        }
        return count;
    }

    void UiOverlays::registerTable()
    {
        m_state->world->services().registerTable<UiContext>(
            "uiOverlays", UiOverlayRecord::spiteSchema(),
            [](const UiContext& ui, const TableRowSink& sink)
            {
                const State& state = *ui.overlays().m_state;
                const World& world = *state.world;
                const auto emit = [&](const Entity root, const std::string_view stateName, const float elapsed,
                                      const float delay, const Entity pendingTarget)
                {
                    const auto it = state.overlays.find(root);
                    if (it == state.overlays.end())
                    {
                        return;
                    }
                    const State::Overlay& overlay = it->second;
                    const auto* identity = world.tryGetConstComponent<UiIdentityComponent>(root);
                    const auto* component = world.tryGetConstComponent<UiOverlayComponent>(root);
                    UiOverlayRecord record;
                    record.element = root;
                    record.id = identity ? std::string(identity->id.toView()) : std::string{};
                    record.kind = overlay.desc.kind == UiOverlayKind::Modal ? "modal" : "tooltip";
                    record.state = stateName;
                    record.target = pendingTarget != Entity::undefined() ? pendingTarget : overlay.target;
                    if (component)
                    {
                        record.placed = sideName(component->placed);
                        record.flipped = component->flipped;
                        record.clamped = component->clamped;
                        record.stack = component->stack;
                    }
                    if (stateName == "open" && state.laidOut(root))
                    {
                        const UiRect rect = state.hierarchy->absoluteRectOf(root);
                        record.rect = Vec4{rect.position.x, rect.position.y, rect.size.x, rect.size.y};
                    }
                    record.elapsed = elapsed;
                    record.delay = delay;
                    sink(&record);
                };
                for (const Entity modal : state.stack)
                {
                    emit(modal, "open", 0.0f, 0.0f, Entity::undefined());
                }
                if (state.shown != Entity::undefined())
                {
                    emit(state.shown, "open", 0.0f, 0.0f, Entity::undefined());
                }
                if (state.pending.active)
                {
                    emit(state.pending.overlay, "pending", state.pending.elapsed, state.pending.delay,
                         state.pending.target);
                }
            });
    }

    // -- UiScope ------------------------------------------------------------------

    UiOverlays& UiScope::overlays() const { return m_context->overlays(); }

    Tooltip UiScope::createTooltip(const InternedString id, const UiTooltipDesc& desc)
    {
        UiRectDesc rootDesc = topLeft();
        rootDesc.zOffset = desc.zOffset;
        Panel root = createPanel(id, rootDesc);
        root.setStyleClass("ui.tooltip");
        auto& state = m_world->getComponent<UiStateComponent>(root.entity());
        state.visible = false;
        state.interactable = false;
        state.clipsChildren = true;
        Panel content = createPanel(childId(id, ".content"), paddedColumn(desc.padding));
        content.setParent(root);
        if (desc.label)
        {
            UiRectDesc labelDesc = topLeft();
            labelDesc.contentSize = UiContentSizeComponent{};
            createLabel(childId(id, ".text"), "", labelDesc).setParent(content);
        }
        UiOverlayComponent component;
        component.kind = UiOverlayKind::Tooltip;
        component.side = component.placed = desc.side;
        component.follow = desc.follow;
        m_world->addComponent<UiOverlayComponent>(root.entity(), component);

        UiOverlays::Desc overlay;
        overlay.kind = UiOverlayKind::Tooltip;
        overlay.content = content.entity();
        overlay.bounds = desc.bounds.isValid() ? desc.bounds.entity() : Entity::undefined();
        overlay.side = desc.side;
        overlay.delay = desc.delay;
        overlay.gap = desc.gap;
        overlay.margin = desc.margin;
        overlay.follow = desc.follow;
        m_context->overlays().add(root.entity(), overlay);
        return Tooltip(this, root.entity(), m_generation);
    }

    Modal UiScope::createModal(const InternedString id, const UiModalDesc& desc)
    {
        UiRectDesc rootDesc = topLeft();
        rootDesc.zOffset = desc.zOffset;
        Panel root = createPanel(id, rootDesc);
        root.setStyleClass("ui.modal");
        auto& state = m_world->getComponent<UiStateComponent>(root.entity());
        state.visible = false;
        state.focusScope = true;
        UiRectDesc dialogDesc;
        dialogDesc.anchor = Vec2{0.5f, 0.5f};
        Panel dialog = createPanel(childId(id, ".dialog"), dialogDesc);
        dialog.setStyleClass("ui.modal.dialog");
        dialog.setAccessibleRole(desc.alert ? UiRole::AlertDialog : UiRole::Dialog);
        m_world->getComponent<UiStateComponent>(dialog.entity()).clipsChildren = true;
        dialog.setParent(root);
        Panel content = createPanel(childId(id, ".content"), paddedColumn(desc.padding));
        content.setParent(dialog);
        UiOverlayComponent component;
        component.kind = UiOverlayKind::Modal;
        component.closeOnEscape = desc.closeOnEscape;
        component.closeOnBackdrop = desc.closeOnBackdrop;
        m_world->addComponent<UiOverlayComponent>(root.entity(), component);

        UiOverlays::Desc overlay;
        overlay.kind = UiOverlayKind::Modal;
        overlay.content = content.entity();
        overlay.dialog = dialog.entity();
        overlay.bounds = desc.bounds.isValid() ? desc.bounds.entity() : Entity::undefined();
        overlay.margin = desc.margin;
        overlay.closeOnEscape = desc.closeOnEscape;
        overlay.closeOnBackdrop = desc.closeOnBackdrop;
        m_context->overlays().add(root.entity(), overlay);
        return Modal(this, root.entity(), m_generation);
    }

    // -- Tooltip ------------------------------------------------------------------

    Panel Tooltip::content() const
    {
        return isValid() ? Panel(scope(), scope()->overlays().contentOf(entity()), m_generation) : Panel{};
    }

    Label Tooltip::label() const
    {
        if (!isValid())
        {
            return {};
        }
        for (const Entity child : scope()->hierarchy().childrenOf(scope()->overlays().contentOf(entity())))
        {
            if (scope()->world().getConstComponent<UiIdentityComponent>(child).kind == UiWidgetKind::Label)
            {
                return Label(scope(), child, m_generation);
            }
        }
        return {};
    }

    void Tooltip::attach(const Widget& target, const float delay)
    {
        if (isValid() && target.isValid())
        {
            scope()->overlays().attach(target.entity(), entity(), delay);
        }
    }

    void Tooltip::detach(const Widget& target)
    {
        if (isValid() && target.isValid())
        {
            scope()->overlays().detach(target.entity());
        }
    }

    void Tooltip::show(const Widget& anchor)
    {
        if (isValid() && anchor.isValid())
        {
            scope()->overlays().show(entity(), anchor.entity());
        }
    }

    void Tooltip::showAt(const Vec2 point)
    {
        if (isValid())
        {
            scope()->overlays().showAt(entity(), point);
        }
    }

    void Tooltip::hide()
    {
        if (isValid())
        {
            scope()->overlays().hide(entity());
        }
    }

    bool Tooltip::isShown() const { return isValid() && scope()->overlays().isOpen(entity()); }

    void Tooltip::onShow(std::function<void(Entity target)> fn)
    {
        if (isValid())
        {
            scope()->overlays().onShow(entity(), std::move(fn));
        }
    }

    // -- Modal --------------------------------------------------------------------

    Panel Modal::dialog() const
    {
        return isValid() ? Panel(scope(), scope()->overlays().dialogOf(entity()), m_generation) : Panel{};
    }

    Panel Modal::content() const
    {
        return isValid() ? Panel(scope(), scope()->overlays().contentOf(entity()), m_generation) : Panel{};
    }

    void Modal::open(const Widget& surface)
    {
        if (isValid())
        {
            scope()->overlays().open(entity(), surface.isValid() ? surface.entity() : Entity::undefined());
        }
    }

    void Modal::close()
    {
        if (isValid())
        {
            scope()->overlays().close(entity());
        }
    }

    bool Modal::isOpen() const { return isValid() && scope()->overlays().isOpen(entity()); }

    void Modal::onClose(std::function<void()> fn)
    {
        if (isValid())
        {
            scope()->overlays().onClose(entity(), std::move(fn));
        }
    }
} // namespace spite
