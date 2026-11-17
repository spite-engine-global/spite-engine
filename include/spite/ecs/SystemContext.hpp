#pragma once

#include <functional>
#include <span>
#include <typeinfo>

#include "base/ServiceLocator.hpp"
#include "Aspect.hpp"
#include "ComponentRegistry.hpp"
#include "EcsExport.hpp"
#include "QueryBuilder.hpp"
#include "EcsTypes.hpp"

namespace enki
{
    class TaskScheduler;
}

namespace spite
{
    class Archetype;
    class CommandBuffer;
    class EntityEventManager;
    class EntityManager;
    struct SystemDependencies;

    // A context object passed to systems during their execution. Provides the
    // world's component store, a stage CommandBuffer and the system's declared
    // dependencies; component access and queries built through it validate
    // against those dependencies. This is a temporary object: it is destroyed
    // after the system callback returns.
    class SPITE_ECS_API SystemContext
    {
    private:
        EntityManager* m_entityManager{};
        const SystemDependencies* m_dependencies{};
        CommandBuffer* cb{};
        ServiceLocator* m_services = nullptr;
        enki::TaskScheduler* m_taskScheduler = nullptr;
        const char* m_systemModuleName = nullptr;
        const char* m_systemName = nullptr;

        // EntityManager and SystemDependencies are internal; everything touching
        // their definitions lives in the .cpp
        static void assertReadDeclared(const SystemDependencies* dependencies, ComponentID id,
                                       const char* componentTypeName);
        static void assertWriteDeclared(const SystemDependencies* dependencies, ComponentID id,
                                        const char* componentTypeName);
        [[nodiscard]] void* componentWriteData(Entity entity, ComponentID componentId);
        [[nodiscard]] const void* componentReadData(Entity entity, ComponentID componentId) const;
        [[nodiscard]] bool entityHasComponent(Entity entity, ComponentID componentId) const;
        [[nodiscard]] bool entityIsValid(Entity entity) const;
        [[nodiscard]] bool entityIsRegistered(Entity entity) const;

    public:
        float deltaTime{};
        // how far the frame is between the last two fixed steps, [0, 1]; render
        // transforms interpolate by it. 0 in the fixed phase.
        float interpolationAlpha{};
        // the world's simulation is paused (stepped frames included): consumers whose state must hold while the scene
        // is frozen for inspection (auto exposure) read it
        bool simulationPaused{};

        SystemContext() = default;

        SystemContext(EntityManager* entityManager, CommandBuffer* commandBuffer, float dt,
                      const SystemDependencies* deps, enki::TaskScheduler* taskScheduler = nullptr,
                      const char* systemModuleName = nullptr, const char* systemName = nullptr,
                      ServiceLocator* services = nullptr);

        // false when the context has no declared dependencies (standalone contexts)
        [[nodiscard]] bool isComponentDeclared(ComponentID componentId, bool requiresWrite) const;

        // events
        template <typename T>
            requires t_component<std::decay_t<T>>
        void fireEvent(T&& event) const
        {
            fireEventById(getComponentId<std::decay_t<T>>(), std::addressof(event));
        }
        void fireEventById(const ComponentID componentId, const void* payload) const;
        void commitEvents() const;

        CommandBuffer& getCommandBuffer() const
        {
            SASSERT(cb);
            return *cb;
        }

        // world-bound host service container; definition known to consumers
        ServiceLocator& services() const
        {
            SASSERT(m_services);
            return *m_services;
        }

        // may be null for hosts that bind no service container
        ServiceLocator* tryGetServices() const { return m_services; }

        QueryBuilder createQueryBuilder() const;

        template <typename... T>
        std::shared_ptr<Query> query() const
        {
            return createQueryBuilder().template buildQuery<T...>();
        }

        [[nodiscard]] const SystemDependencies* getDependencies() const { return m_dependencies; }
        [[nodiscard]] const char* getSystemModuleName() const { return m_systemModuleName; }
        [[nodiscard]] const char* getSystemName() const { return m_systemName; }

        // a system must wait for every task it submits before returning (the stage waits only for the system tasks)
        enki::TaskScheduler& getTaskScheduler() const
        {
            SASSERTM(m_taskScheduler, "TaskScheduler is not available in SystemContext.");
            return *m_taskScheduler;
        }

        // Runs func(begin, end) over [0, count) on the task worker pool;
        // partitions are disjoint, ascending ranges and tasks must not depend
        // on each other. Serial fallback in contexts without a scheduler.
        void parallelFor(u32 count, u32 minRange, const std::function<void(u32 begin, u32 end)>& func) const;

        EntityEventManager& getEventManager() const;

        // Optional inspection returns empty for pending, unregistered, dead or stale entities.
        template <typename T>
        [[nodiscard]] const T* tryGetConstComponent(Entity entity) const
        {
            assertReadDeclared(m_dependencies, getComponentId<T>(), typeid(T).name());
            if (hasComponent<T>(entity))
            {
                return &getConstComponent<T>(entity);
            }
            return nullptr;
        }

        template <t_component T>
        [[nodiscard]] T* tryGetComponent(Entity entity)
        {
            assertWriteDeclared(m_dependencies, getComponentId<T>(), typeid(T).name());
            if (hasComponent<T>(entity))
            {
                return &getComponent<T>(entity);
            }
            return nullptr;
        }

        template <t_component T>
        T& getComponent(Entity entity)
        {
            const ComponentID componentId = getComponentId<T>();
            assertWriteDeclared(m_dependencies, componentId, typeid(T).name());
            return *static_cast<T*>(componentWriteData(entity, componentId));
        }

        template <t_component T>
        const T& getConstComponent(Entity entity) const
        {
            const ComponentID componentId = getComponentId<T>();
            assertReadDeclared(m_dependencies, componentId, typeid(T).name());
            return *static_cast<const T*>(componentReadData(entity, componentId));
        }

        [[nodiscard]] bool isEntityValid(Entity entity) const { return entityIsValid(entity); }
        [[nodiscard]] bool isEntityRegistered(Entity entity) const { return entityIsRegistered(entity); }

        template <t_component T>
        [[nodiscard]] bool hasComponent(Entity entity) const
        {
            return entityHasComponent(entity, getComponentId<T>());
        }

        [[nodiscard]] bool hasComponent(Entity entity, ComponentID componentId) const
        {
            return entityHasComponent(entity, componentId);
        }

        void* getComponentData(Entity entity, ComponentID componentId)
        {
            assertWriteDeclared(m_dependencies, componentId, nullptr);
            return componentWriteData(entity, componentId);
        }

        const void* getConstComponentData(Entity entity, ComponentID componentId) const
        {
            assertReadDeclared(m_dependencies, componentId, nullptr);
            return componentReadData(entity, componentId);
        }

        // getConstComponentData for several components of one entity through one lookup; out[i] is null where the
        // entity lacks ids[i]
        void getConstComponentDataByIds(Entity entity, std::span<const ComponentID> ids,
                                        std::span<const void*> out) const;

        // the entity's component composition; Aspect is a public type, unlike the
        // internal Archetype it comes from
        [[nodiscard]] Aspect getEntityAspect(Entity entity) const;
    };
} // namespace spite

// The engine writes the ECS vocabulary as `ecs::X`; publish the expected aliases.
namespace spite::ecs
{
    using spite::SystemContext;
}
