#pragma once

#include <functional>
#include <variant>
#include <vector>

#include "base/Handle.hpp"
#include "base/ImageRange.hpp"
#include "Aspect.hpp"
#include "ComponentMetadata.hpp"
#include "EcsExport.hpp"
#include "EcsTypes.hpp"

namespace spite
{
    // Subscription registry for entity/component lifecycle events, owned by the
    // EntityManager of each world. Firing contract:
    //  - created fires once when an entity is registered in an archetype; the
    //    aspect is part of the event, so creation and staging adoption carry no
    //    per-component events
    //  - added fires once the component exists and is constructed; data pushed
    //    through addComponentData lands before the event, data delivered through
    //    a command buffer commit is written after it
    //  - removed fires before the data is destroyed and stays readable
    //  - destroying an entity fires removed for every subscribed component of
    //    its aspect, then destroyed
    //  - husk reservation/recycling, enable/disable and data writes fire nothing
    //
    // Events dispatch outside the EntityManager lock, so hooks may read the
    // world. Hooks must not perform immediate structural changes (use a command
    // buffer) and must not subscribe/unsubscribe while events are dispatching.
    // Subscribe/unsubscribe from the host-serialized context.
    class SPITE_ECS_API LifecycleEvents
    {
    public:
        using EntityCreatedFn = std::function<void(Entity, const Aspect&)>;
        using EntityDestroyedFn = std::function<void(Entity)>;
        using ComponentFn = std::function<void(Entity)>;

        [[nodiscard]] Handle onEntityCreated(EntityCreatedFn fn);
        [[nodiscard]] Handle onEntityDestroyed(EntityDestroyedFn fn);
        [[nodiscard]] Handle onComponentAdded(ComponentID id, ComponentFn fn);
        [[nodiscard]] Handle onComponentRemoved(ComponentID id, ComponentFn fn);

        // a stale or unknown subscription is ignored
        void unsubscribe(Handle subscription);

        // -- firing (EntityManager only) --------------------------------------

        void fireEntityCreated(Entity entity, const Aspect& aspect) const;
        void fireEntityDestroyed(Entity entity) const;
        void fireComponentAdded(ComponentID id, Entity entity) const;
        void fireComponentRemoved(ComponentID id, Entity entity) const;

        [[nodiscard]] bool hasEntityCreatedSubscribers() const;
        [[nodiscard]] bool hasEntityDestroyedSubscribers() const;
        [[nodiscard]] bool hasComponentAddedSubscribers() const;
        [[nodiscard]] bool hasComponentRemovedSubscribers() const;
        [[nodiscard]] bool hasComponentRemovedSubscribers(ComponentID id) const;

        // subscriptions whose code lives in the image: what a module that
        // unloads failed to unsubscribe
        [[nodiscard]] u32 countCallbacksIn(const ImageRange& image) const;

    private:
        enum class Kind : u8
        {
            eEntityCreated,
            eEntityDestroyed,
            eComponentAdded,
            eComponentRemoved
        };

        // EntityDestroyedFn and ComponentFn share the void(Entity) signature, so
        // the variant only separates it from the two-argument created callback;
        // Kind distinguishes the one-argument kinds
        using OneArgFn = std::function<void(Entity)>;

        struct Subscriber
        {
            u32 slot; // back-reference into m_slots, fixed up when swap-removal moves the entry
            Kind kind;
            ComponentID componentId = INVALID_COMPONENT_ID; // component kinds only
            std::variant<EntityCreatedFn, OneArgFn> fn;
        };

        // a handle's index is a stable slot mapping to the subscriber's current
        // position in the dense m_subscribers dispatch array
        struct Slot
        {
            u32 dense = 0;
            u32 generation = 0; // 0 while free
        };

        [[nodiscard]] Handle subscribe(Kind kind, ComponentID componentId, std::variant<EntityCreatedFn, OneArgFn> fn);

        std::vector<Subscriber> m_subscribers;
        std::vector<Slot> m_slots;
        std::vector<u32> m_freeSlots;
        // issued once per subscription and never reused, so a stale handle can
        // never alias the subscription that later occupies its slot
        u32 m_nextGeneration = 1;
    };
} // namespace spite
