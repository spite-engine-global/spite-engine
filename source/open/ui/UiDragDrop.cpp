#include "ui/UiDragDrop.hpp"

#include <unordered_map>

#include "ecs/World.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        constexpr i32 PREVIEW_Z = 2500; // above tooltips (2000)
        constexpr Vec2 PREVIEW_OFFSET{14.0f, 6.0f}; // beside the pointer, not under it
        constexpr float PREVIEW_PADDING = 6.0f;
    } // namespace

    struct UiDragDrop::State
    {
        struct Source
        {
            PayloadFn payload;
        };

        struct Target
        {
            AcceptFn accept;
            DropFn drop;
        };

        World* world = nullptr;
        UiHierarchyManager* hierarchy = nullptr;
        UiRouter* router = nullptr;
        std::unordered_map<Entity, Source> sources;
        std::unordered_map<Entity, Target> targets;
        std::optional<Drag> drag;
        Entity owner = Entity::undefined();
        ChangeFn changed;

        void setState(const Entity target, const UiDropState state) const
        {
            if (target != Entity::undefined() && world->isEntityAlive(target))
            {
                if (auto* component = world->tryGetComponent<UiDropTargetComponent>(target))
                {
                    component->state = state;
                }
            }
        }

        void notify() const
        {
            if (changed)
            {
                changed(drag ? &*drag : nullptr);
            }
        }

        u32 depthOf(Entity element) const
        {
            u32 depth = 0;
            for (element = hierarchy->parentOf(element); hierarchy->isElement(element);
                 element = hierarchy->parentOf(element))
            {
                ++depth;
            }
            return depth;
        }

        // A point where hit testing finds nothing (an area of elements that take no input, an image panel) has no
        // ancestry to walk, so its target is found by rect: the deepest visible target containing it
        // within its clip and no modal blocks (not the interactable flag: a container that takes no input is a target);
        // of equal depth the one painted last wins.
        Entity targetAt(const Vec2 position) const
        {
            Entity best = Entity::undefined();
            u32 bestDepth = 0;
            u32 bestPaint = 0;
            for (const auto& [element, target] : targets)
            {
                if (!hierarchy->isElement(element) || !hierarchy->isVisibleOf(element) ||
                    router->blockedByModal(element) || !hierarchy->absoluteRectOf(element).contains(position) ||
                    (hierarchy->isClippedOf(element) && !hierarchy->clipRectOf(element).contains(position)))
                {
                    continue;
                }
                const u32 depth = depthOf(element);
                const u32 paint = hierarchy->paintIndexOf(element);
                if (best == Entity::undefined() || depth > bestDepth || (depth == bestDepth && paint > bestPaint))
                {
                    best = element;
                    bestDepth = depth;
                    bestPaint = paint;
                }
            }
            return best;
        }

        // Finds the target at or above `hit` and whether it takes the payload; updates the target components.
        void settle(const Entity hit, const Vec2 position)
        {
            Drag& now = *drag;
            Entity target = Entity::undefined();
            for (Entity at = hit; hierarchy->isElement(at); at = hierarchy->parentOf(at))
            {
                if (targets.contains(at))
                {
                    target = at;
                    break;
                }
            }
            if (target == Entity::undefined() && !hierarchy->isElement(hit))
            {
                target = targetAt(position);
            }
            bool accepting = false;
            if (target != Entity::undefined())
            {
                const AcceptFn accept = targets.at(target).accept; // by value: it may cancel() or forget()
                accepting = !accept || accept(now.payload, hit, position);
                if (!drag)
                {
                    return;
                }
            }
            if (target != now.target)
            {
                setState(now.target, UiDropState::None);
            }
            setState(target, accepting ? UiDropState::Accepting : UiDropState::Rejecting);
            now.target = target;
            now.hit = hit;
            now.accepting = accepting;
            now.position = position;
            notify();
        }

        void finish()
        {
            setState(drag->target, UiDropState::None);
            drag.reset();
            notify();
        }
    };

    UiDragDrop::UiDragDrop(World& world, UiHierarchyManager& hierarchy, UiRouter& router) :
        m_state(std::make_unique<State>())
    {
        m_state->world = &world;
        m_state->hierarchy = &hierarchy;
        m_state->router = &router;
    }

    UiDragDrop::~UiDragDrop() = default;

    void UiDragDrop::addSource(const Entity element, PayloadFn payload, const bool pointer)
    {
        State& state = *m_state;
        state.sources[element] = State::Source{std::move(payload)};
        if (!pointer)
        {
            return;
        }
        state.router->subscribe(element, UiEventKind::DragStart, [this, element](const void* event)
                                { begin(element, static_cast<const UiDragStartEvent*>(event)->position); });
        // only the drag this element began: an injected or another source's drag is not ours to move or drop
        state.router->subscribe(element, UiEventKind::DragMove,
                                [this, element](const void* event)
                                {
                                    if (m_state->drag && m_state->drag->source == element)
                                    {
                                        move(static_cast<const UiDragMoveEvent*>(event)->position);
                                    }
                                });
        state.router->subscribe(element, UiEventKind::DragEnd,
                                [this, element](const void* event)
                                {
                                    if (!m_state->drag || m_state->drag->source != element)
                                    {
                                        return;
                                    }
                                    // a press the router ended because a modal opened over it cancels
                                    if (m_state->router->blockedByModal(element))
                                    {
                                        cancel();
                                        return;
                                    }
                                    move(static_cast<const UiDragEndEvent*>(event)->position);
                                    drop();
                                });
    }

    void UiDragDrop::addTarget(const Entity element, AcceptFn accept, DropFn onDrop)
    {
        State& state = *m_state;
        state.targets[element] = State::Target{std::move(accept), std::move(onDrop)};
        if (!state.world->hasComponent<UiDropTargetComponent>(element))
        {
            state.world->addComponent<UiDropTargetComponent>(element, UiDropTargetComponent{});
        }
    }

    bool UiDragDrop::forget(const Entity element)
    {
        State& state = *m_state;
        const bool source = state.sources.erase(element) != 0;
        const bool target = state.targets.erase(element) != 0;
        const bool preview = state.owner == element;
        if (preview)
        {
            state.owner = Entity::undefined();
            state.changed = nullptr;
        }
        if (state.drag)
        {
            if (state.drag->source == element)
            {
                cancel();
            }
            else if (state.drag->target == element || state.drag->hit == element)
            {
                state.drag->target = Entity::undefined();
                state.drag->hit = Entity::undefined();
                state.drag->accepting = false;
                state.notify();
            }
        }
        return source || target || preview;
    }

    bool UiDragDrop::isSource(const Entity element) const { return m_state->sources.contains(element); }

    bool UiDragDrop::isTarget(const Entity element) const { return m_state->targets.contains(element); }

    void UiDragDrop::onChange(const Entity owner, ChangeFn fn)
    {
        m_state->owner = owner;
        m_state->changed = std::move(fn);
    }

    bool UiDragDrop::begin(const Entity source, UiDragPayload payload, const Vec2 position)
    {
        State& state = *m_state;
        if (state.drag)
        {
            return false;
        }
        state.drag = Drag{.payload = std::move(payload), .source = source};
        state.settle(state.hierarchy->hitTest(position), position);
        return true;
    }

    bool UiDragDrop::begin(const Entity source, const Vec2 position)
    {
        const auto it = m_state->sources.find(source);
        if (it == m_state->sources.end() || !it->second.payload || m_state->drag)
        {
            return false;
        }
        std::optional<UiDragPayload> payload = it->second.payload();
        return payload && begin(source, std::move(*payload), position);
    }

    void UiDragDrop::move(const Vec2 position)
    {
        State& state = *m_state;
        if (state.drag)
        {
            state.settle(state.hierarchy->hitTest(position), position);
        }
    }

    void UiDragDrop::moveTo(const Entity element)
    {
        State& state = *m_state;
        if (state.drag && state.hierarchy->isElement(element) && state.hierarchy->isVisibleOf(element))
        {
            const UiRect rect = state.hierarchy->absoluteRectOf(element);
            // below an open modal the element is out of reach, as it is for the pointer
            state.settle(state.router->blockedByModal(element) ? Entity::undefined() : element,
                         rect.position + rect.size * 0.5f);
        }
    }

    bool UiDragDrop::drop()
    {
        State& state = *m_state;
        if (!state.drag)
        {
            return false;
        }
        const Drag ended = std::move(*state.drag);
        state.finish();
        if (!ended.accepting)
        {
            return false;
        }
        UiDropEvent event;
        event.element = ended.target;
        event.source = ended.source;
        event.payload = ended.payload;
        event.hit = ended.hit;
        event.position = ended.position;
        state.world->fireEvent(event);
        // by value: the reaction may re-register or drop the target
        if (const auto it = state.targets.find(ended.target); it != state.targets.end() && it->second.drop)
        {
            const DropFn onDrop = it->second.drop;
            onDrop(event);
        }
        return true;
    }

    void UiDragDrop::cancel()
    {
        if (m_state->drag)
        {
            m_state->finish();
        }
    }

    bool UiDragDrop::active() const { return m_state->drag.has_value(); }

    const UiDragDrop::Drag* UiDragDrop::current() const { return m_state->drag ? &*m_state->drag : nullptr; }

    u32 UiDragDrop::countCallbacksIn(const ImageRange& image) const
    {
        u32 count = u32{image.contains(m_state->changed)};
        for (const auto& [element, source] : m_state->sources)
        {
            count += u32{image.contains(source.payload)};
        }
        for (const auto& [element, target] : m_state->targets)
        {
            count += u32{image.contains(target.accept)} + u32{image.contains(target.drop)};
        }
        return count;
    }

    // -- UiScope ------------------------------------------------------------------

    UiDragDrop& UiScope::dragDrop() const { return m_context->dragDrop(); }

    Panel UiScope::createDragPreview(const InternedString id)
    {
        UiRectDesc rootDesc;
        rootDesc.pivot = Vec2{0.0f, 0.0f};
        rootDesc.zOffset = PREVIEW_Z;
        rootDesc.contentSize = UiContentSizeComponent{};
        UiLayoutComponent layout;
        layout.direction = UiLayoutDirection::Column;
        layout.padding = Vec4{PREVIEW_PADDING, PREVIEW_PADDING, PREVIEW_PADDING, PREVIEW_PADDING};
        rootDesc.layout = layout;
        Panel root = createPanel(id, rootDesc);
        root.setStyleClass("ui.dragPreview");
        root.setVisible(false);
        root.setInteractable(false);
        UiRectDesc labelDesc;
        labelDesc.pivot = Vec2{0.0f, 0.0f};
        labelDesc.contentSize = UiContentSizeComponent{};
        Label label = createLabel(InternedString(std::string(id.toView()) + ".text"), "", labelDesc);
        label.setParent(root);
        hierarchy().setHitTransparent(root.entity(), true);
        const UiHierarchyManager* ui = &hierarchy();
        m_context->dragDrop().onChange(
            root.entity(),
            [root, label, ui, accepting = false, rejecting = false](const UiDragDrop::Drag* drag) mutable
            {
                if (!drag)
                {
                    root.setVisible(false);
                    return;
                }
                if (label.text() != drag->payload.label)
                {
                    label.setText(drag->payload.label);
                }
                const bool nowAccepting = drag->accepting;
                const bool nowRejecting = !drag->accepting && drag->target != Entity::undefined();
                if (nowAccepting != accepting || nowRejecting != rejecting || !root.isVisible())
                {
                    accepting = nowAccepting;
                    rejecting = nowRejecting;
                    root.setStyleClass(accepting       ? "ui.dragPreview.accept"
                                           : rejecting ? "ui.dragPreview.reject"
                                                       : "ui.dragPreview");
                }
                root.setOffset(drag->position * (1.0f / ui->layoutScale()) + PREVIEW_OFFSET);
                if (!root.isVisible())
                {
                    root.setVisible(true);
                }
            });
        return root;
    }
} // namespace spite
