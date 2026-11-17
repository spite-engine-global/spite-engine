#pragma once

#include <functional>
#include <memory>
#include <optional>

#include "base/ImageRange.hpp"
#include "base/MathTypes.hpp"
#include "ecs/EcsTypes.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiEvents.hpp"
#include "ui/UiExport.hpp"

namespace spite
{
    class World;
    class UiHierarchyManager;
    class UiRouter;

    // Generic drag and drop between elements and panels. A source starts a drag with a payload, drop
    // targets accept or reject it by payload, a release over an accepting target fires one UiDropEvent on it (and
    // its DropFn) that the presenter turns into a control command. One drag is in flight at a time per UI; it costs
    // nothing while none is: no scan, no system, no observer.
    //
    // A pointer drag on a source element begins at its drag start, follows the pointer and drops at the release
    // (addSource, pointer sources replace the element's own drag reactions). A presenter whose drag the widget
    // reports (a table's rows, a pooled row) calls begin/move/drop itself. The uiDrag control command drives the same
    // calls, so a drag is inspectable (uiTree's `drag`) and injectable. Positions are the router's (hit-test space).
    class SPITE_UI_API UiDragDrop
    {
    public:
        // The payload a source gives when a drag starts on it; none refuses the drag.
        using PayloadFn = std::function<std::optional<UiDragPayload>()>;
        // Whether the target takes the payload; `hit` is the deepest element under the pointer. Empty accepts all.
        using AcceptFn = std::function<bool(const UiDragPayload& payload, Entity hit, Vec2 position)>;
        using DropFn = std::function<void(const UiDropEvent& event)>;

        // The drag in flight. `target` is the nearest drop target at or above the hit, undefined over none;
        // `accepting` is whether it takes the payload.
        struct Drag
        {
            UiDragPayload payload;
            Entity source = Entity::undefined();
            Entity target = Entity::undefined();
            Entity hit = Entity::undefined();
            bool accepting = false;
            Vec2 position{0.0f, 0.0f};
        };

        // Called with the drag on begin, on every move or change of target and accept state, and with null when it
        // ended: where a drag preview follows. One per UI; `owner` (the preview root) clears it when destroyed.
        using ChangeFn = std::function<void(const Drag* drag)>;

        UiDragDrop(World& world, UiHierarchyManager& hierarchy, UiRouter& router);
        ~UiDragDrop();
        UiDragDrop(const UiDragDrop&) = delete;
        UiDragDrop& operator=(const UiDragDrop&) = delete;

        // The element starts drags with `payload`. `pointer` makes a press-drag on it begin, follow and drop the
        // drag; without it only begin(source, ...) and uiDrag use it. Replaces an earlier registration.
        void addSource(Entity element, PayloadFn payload, bool pointer = true);
        // The element (and its descendants without a target of their own) takes drops. Adds UiDropTargetComponent.
        void addTarget(Entity element, AcceptFn accept, DropFn onDrop = {});
        // The element is being destroyed (UiScope): drops its registrations; an in-flight drag from it, or over it,
        // is cancelled or loses its target. True when it was a source, a target or the preview owner.
        bool forget(Entity element);
        [[nodiscard]] bool isSource(Entity element) const;
        [[nodiscard]] bool isTarget(Entity element) const;
        void onChange(Entity owner, ChangeFn fn);

        // Starts a drag; false while one is in flight. The second form takes the payload from the source's PayloadFn
        // (false for an element that is no source or whose function refuses).
        bool begin(Entity source, UiDragPayload payload, Vec2 position);
        bool begin(Entity source, Vec2 position);
        // The pointer moved: finds the target under it.
        void move(Vec2 position);
        // As move, with `element` as the hit and the centre of its rect as the pointer (injection).
        void moveTo(Entity element);
        // Ends the drag; an accepting target gets the event. True when it dropped.
        bool drop();
        void cancel();

        [[nodiscard]] bool active() const;
        // The in-flight drag, null when none.
        [[nodiscard]] const Drag* current() const;

        [[nodiscard]] u32 countCallbacksIn(const ImageRange& image) const;

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
