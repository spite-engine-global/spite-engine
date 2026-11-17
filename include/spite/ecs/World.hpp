#pragma once

#include <functional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/ServiceLocator.hpp"
#include "Aspect.hpp"
#include "ComponentRegistry.hpp"
#include "EcsExport.hpp"
#include "EcsStats.hpp"
#include "QueryBuilder.hpp"
#include "EcsTypes.hpp"
#include "SystemContext.hpp"
#include "SystemDescriptor.hpp"

namespace spite
{
    class Chunk;
    class LifecycleEvents;
    struct ImageRange;

    // A handle to a world: the main world created through create(), or a staging
    // world created from it (staging worlds run no systems and share the main
    // world's component registry, services and entity-id allocator snapshot).
    // Handles are cheap value types; the world itself is host-owned and destroyed
    // through destroy().
    class SPITE_ECS_API World
    {
    private:
        struct State;
        State* m_state = nullptr;

    public:
        World() = default;

        [[nodiscard]] explicit operator bool() const;
        [[nodiscard]] bool operator==(const World&) const = default;
        [[nodiscard]] bool isStaging() const;

        // host-owned lifetime; pairs with destroy(). The service container (owned
        // by the host) is shared with staging worlds and asserted by APIs that
        // need it; a world without services supports entities only.
        [[nodiscard]] static World create(uint32_t threadCount = 0, ServiceLocator* hostServices = nullptr);
        // A private copy uses the host's already-registered component vocabulary without registering it again.
        [[nodiscard]] static World createForCopy(uint32_t threadCount, ServiceLocator* hostServices);
        void destroy();

        // reservedEntityCount: entity ids the staging build will issue, reserved
        // up front so live issuance can't collide before adoption; pass 0 when
        // the staging world only re-registers live ids
        [[nodiscard]] World createStagingWorld(sizet reservedEntityCount) const;
        // merges the staged archetypes into this world; the staging world must be
        // destroyed by the caller afterwards
        void adoptStagingWorld(World& staging) const;

        // -- host services ---------------------------------------------------------

        // the host service container bound at create(); asserts when none was bound
        [[nodiscard]] ServiceLocator& services() const;

        // lifecycle event subscriptions for this world; see LifecycleEvents for
        // the firing contract
        [[nodiscard]] LifecycleEvents& lifecycleEvents() const;

        // -- system loop -----------------------------------------------------

        void initializeSystems() const;
        // interpolationAlpha reaches systems as SystemContext::interpolationAlpha
        void update(float deltaTime, float interpolationAlpha) const;
        // One frame of several worlds in one stage loop: each stage runs in every
        // world that has it (in span order) before the next stage starts, so a
        // frame-wide stage (BEGIN_FRAME, RENDER) sees every world's earlier stages.
        // realDeltaTime, when not negative, is the deltaTime of the real-time worlds (setRealTime): the frame's
        // unscaled time, while the others get the scaled deltaTime.
        static void updateWorlds(std::span<const World> worlds, float deltaTime, float interpolationAlpha,
                                 float realDeltaTime = -1.0f);
        // A real-time world's update stages run on updateWorlds' realDeltaTime whatever the time scale (a UI world:
        // its tweens and tooltip delays keep time while the simulation is frozen). Off by default.
        void setRealTime(bool realTime) const;
        // one fixed step; the application's fixed timestep drives it before update. A paused world skips it.
        void fixedUpdate(float deltaTime) const;
        // The fixed steps stepFixedSimulation queued, all at once. Call every frame before its fixedUpdate calls,
        // with the fixed timestep: that deltaTime is also the timestep of a stepped frame's
        // STEPPED_FRAME_FIXED_STEPS (run at the start of its update, so a step requested before the update counts)
        // and what the simulation stages of a paused world's simulating frame see (interpolation 0).
        void runPendingFixedSteps(float deltaTime) const;
        [[nodiscard]] uint32_t activeEntityCount() const;

        // -- standalone contexts and buffers ---------------------------------------

        // host-serialized context for work outside the system loop (bootstrap, module
        // lifecycle, tooling); no declared dependencies. The command buffer is
        // optional; apply it with CommandBuffer::commit().
        [[nodiscard]] SystemContext createSystemContext(CommandBuffer* commandBuffer = nullptr,
                                                        float deltaTime = 0.0f) const;
        // command buffer over this world's entity store; apply with
        // CommandBuffer::commit()
        [[nodiscard]] CommandBuffer createCommandBuffer() const;

        // -- entities ----------------------------------------------------------

        [[nodiscard]] Entity createEntity() const;
        [[nodiscard]] Entity createEntity(std::span<const ComponentID> aspectIds) const;
        [[nodiscard]] std::vector<Entity> createHuskEntities(uint32_t count) const;
        // One archetype entry for the whole span. The slots are default-constructed; fill runs after that and
        // before entity-created fires, so hooks read what it wrote.
        void registerEntities(std::span<const Entity> entities, std::span<const ComponentID> aspectIds,
                              const std::function<void()>& fill = {}) const;
        [[nodiscard]] bool isEntityValid(Entity entity) const;
        [[nodiscard]] bool isEntityRegistered(Entity entity) const;
        void destroyEntity(Entity entity) const;
        void destroyEntities(std::span<const Entity> entities) const;

        // structural change; prefer the command buffer wherever possible
        void destroyEntityImmediate(Entity entity) const;
        // false for undefined, destroyed or stale-generation handles
        [[nodiscard]] bool isEntityAlive(Entity entity) const;

        // -- components ------------------------------------------------------------

        // constructs in place from the arguments, or default-constructs when none are given
        template <t_component T, typename... Args>
        void addComponent(Entity entity, Args&&... args) const
        {
            const ComponentID id = getComponentId<T>();
            if constexpr (sizeof...(Args) == 0)
            {
                static_assert(std::is_default_constructible_v<T>,
                              "addComponent without arguments requires a default-constructible component; "
                              "pass constructor arguments instead.");
                addComponentById(entity, id);
            }
            else
            {
                T payload(std::forward<Args>(args)...);
                addComponentDataById(entity, id, &payload);
            }
        }

        template <t_component T>
        void removeComponent(Entity entity) const
        {
            removeComponentById(entity, getComponentId<T>());
        }

        template <t_component T>
        [[nodiscard]] bool hasComponent(Entity entity) const
        {
            return hasComponentById(entity, getComponentId<T>());
        }

        // Component references and pointers can be invalidated by structural changes
        // anywhere in this world, including swap-compaction of another entity.
        // Copy needed values before adding or removing components, then reacquire.
        template <t_component T>
        [[nodiscard]] T& getComponent(Entity entity) const
        {
            return *static_cast<T*>(getComponentDataById(entity, getComponentId<T>()));
        }

        template <t_component T>
        [[nodiscard]] const T& getConstComponent(Entity entity) const
        {
            return *static_cast<const T*>(getConstComponentDataById(entity, getComponentId<T>()));
        }

        template <t_component T>
        [[nodiscard]] T* tryGetComponent(Entity entity) const
        {
            return static_cast<T*>(getComponentDataById(entity, getComponentId<T>()));
        }

        template <t_component T>
        [[nodiscard]] const T* tryGetConstComponent(Entity entity) const
        {
            return static_cast<const T*>(getConstComponentDataById(entity, getComponentId<T>()));
        }

        // tryGetConstComponent for each Ts through one entity lookup.
        template <t_component... Ts>
        [[nodiscard]] std::tuple<const Ts*...> tryGetConstComponents(Entity entity) const
        {
            const ComponentID ids[] = {getComponentId<Ts>()...};
            const void* data[sizeof...(Ts)];
            getConstComponentDataByIds(entity, ids, data);
            return [&]<sizet... I>(std::index_sequence<I...>)
            { return std::tuple<const Ts*...>{static_cast<const Ts*>(data[I])...}; }(std::index_sequence_for<Ts...>{});
        }

        template <t_component T>
        void enableComponent(Entity entity) const
        {
            enableComponentById(entity, getComponentId<T>());
        }

        template <t_component T>
        void disableComponent(Entity entity) const
        {
            disableComponentById(entity, getComponentId<T>());
        }

        template <t_component T>
        [[nodiscard]] bool isComponentEnabled(Entity entity) const
        {
            return isComponentEnabledById(entity, getComponentId<T>());
        }

        // -- events -----------------------------------------------------------------

        // accepts lvalues and rvalues; the payload is copy-constructed into the event entity
        template <typename T>
            requires t_component<std::decay_t<T>>
        void fireEvent(T&& event) const
        {
            fireEventById(getComponentId<std::decay_t<T>>(), std::addressof(event));
        }

        void fireEventById(ComponentID componentId, const void* payload) const;
        void commitEvents() const;

        // -- queries -------------------------------------------------------------------

        [[nodiscard]] QueryBuilder createQueryBuilder() const;

        // -- components by id ------------------------------------------------------------

        [[nodiscard]] bool hasComponentById(Entity entity, ComponentID componentId) const;
        void addComponentById(Entity entity, ComponentID componentId) const;
        void addComponentDataById(Entity entity, ComponentID componentId, const void* srcData) const;
        void removeComponentById(Entity entity, ComponentID componentId) const;
        [[nodiscard]] void* getComponentDataById(Entity entity, ComponentID componentId) const;
        [[nodiscard]] const void* getConstComponentDataById(Entity entity, ComponentID componentId) const;
        // One lookup for several components of one entity; out[i] is null where it lacks ids[i]. The pointers
        // last until the next structural change.
        void getConstComponentDataByIds(Entity entity, std::span<const ComponentID> ids,
                                        std::span<const void*> out) const;
        // Mutable access reached the component last frame or so far this frame:
        // what a consumer polling once per frame has not seen yet. False when
        // the entity lacks it.
        [[nodiscard]] bool wasComponentModifiedById(Entity entity, ComponentID componentId) const;
        // Mutable access reached the component since the last frame end (what a change-summary scan with
        // ChangeWindow::ThisFrame sees): one entity's bit of it. False when the entity lacks it.
        [[nodiscard]] bool wasComponentModifiedThisFrameById(Entity entity, ComponentID componentId) const;
        void setComponentDataById(Entity entity, ComponentID componentId, const void* srcData) const;
        void enableComponentById(Entity entity, ComponentID componentId) const;
        void disableComponentById(Entity entity, ComponentID componentId) const;
        [[nodiscard]] bool isComponentEnabledById(Entity entity, ComponentID componentId) const;

        // -- systems -------------------------------------------------------------------------

        // the module travels inside descriptor.moduleName (null = local/unnamed)
        bool registerSystem(const SystemDescriptor& descriptor) const;
        // readback of every registered system; empty on staging worlds
        [[nodiscard]] std::vector<SystemDescriptor> getSystems() const;
        // The average ms (SystemPhaseTiming::avgMs) of every phase getSystems() lists, in its order: each system's
        // update phase when it has one, then its fixed phase. Replaces out's contents; allocates only when out must
        // grow, for a reader that polls timings while the schedule (systemRevision) stands.
        // Call it on the frame thread, outside World::update.
        void readSystemTimings(std::vector<float>& out) const;
        // the clock the scheduler times systems with (SystemDescriptor::updateTiming); null restores the steady
        // clock. For tests: call it between frames, from the frame thread.
        void setSystemClock(SystemClockFn clock) const;
        // changes whenever the system schedule is rebuilt (a system added, removed or re-declared): lets a caller
        // re-validate what it derived from getSystems() without polling it
        [[nodiscard]] u64 systemRevision() const;
        // false for a required system (SystemDescriptor::required): removing it stops the frame
        bool removeSystem(const SystemName& name) const;
        // drops required systems too: the unloading module's code is going away
        void removeSystemsByModule(const char* moduleName) const;
        // systems (queued additions included, queued removals not) whose
        // functions live in image: what an unloading module's DLL still runs
        [[nodiscard]] std::vector<SystemName> findSystemsIn(const ImageRange& image) const;
        [[nodiscard]] bool isSystemEnabled(const SystemName& name) const;
        // false, changing nothing, for an unknown system, a required one (SystemDescriptor::required) to disable, or
        // one whose enabling would close a dependency cycle among the active systems (systemEnableCycle names it)
        bool setSystemEnabled(const SystemName& name, bool enabled) const;
        // "A -> B -> A": the cycle enabling the disabled system would close (a cycle of any length of runsAfter or
        // read/write orders: its systems wait for each other and never start); empty when it would not, or for an
        // unknown or an enabled one
        [[nodiscard]] std::string systemEnableCycle(const SystemName& name) const;

        // -- simulation control (pause-and-inspect) ----------------------------------

        // Pausing (or resuming) drops any pending steps.
        void setSimulationPaused(bool paused) const;
        // Pauses, then runs the simulation for the next frames update frames, each with
        // STEPPED_FRAME_FIXED_STEPS fixed steps at the start of its update, whatever time the frame took.
        // Each step call drops the other kind's pending steps.
        void stepSimulation(u32 frames) const;
        // Pauses, then the next frame runs exactly fixedSteps fixed steps
        // (runPendingFixedSteps) and simulates its update.
        void stepFixedSimulation(u32 fixedSteps) const;
        [[nodiscard]] SimulationState simulationState() const;

        // -- debug / stats -----------------------------------------------------------

        [[nodiscard]] EcsStats getStats() const;
        // The component columns (one component of one chunk) that held a write in the last finished frame
        // (updateWorlds' end), plus the entities destroyed in it (a destroy leaves no bit); entity creation counts as
        // writes: 0 means the frame modified nothing in this world. Cheap, for a once-per-frame reader; getStats
        // walks the chunks.
        [[nodiscard]] u32 columnsWrittenLastFrame() const;
        [[nodiscard]] std::vector<ArchetypeStats> getArchetypeStats() const;

        // -- debug archetype inspection -------------------------------------------------

        // chunk pointers are invalidated by any structural change on the world;
        // empty when no archetype exists for the aspect
        [[nodiscard]] std::vector<Chunk*> getArchetypeChunks(const Aspect& aspect) const;
        // chunks of every non-empty archetype that includes the aspect
        [[nodiscard]] std::vector<Chunk*> getChunksContaining(const Aspect& aspect) const;
        // per-entity component enumeration; returns the entity's full aspect count,
        // writes up to outBuffer.size() ids
        [[nodiscard]] uint32_t getEntityAspectIds(Entity entity, std::span<ComponentID> outBuffer) const;

        void removeComponents(std::span<const Entity> entities, std::span<const ComponentID> componentIds) const;
        // Entities must be alive and unique; an id an entity already holds is kept as it is (data intact, no
        // event). One structural move per source archetype for the whole span. The new slots are default-constructed;
        // fill runs after the move and before component-added fires, so hooks read what it wrote.
        void addComponents(std::span<const Entity> entities, std::span<const ComponentID> componentIds,
                           const std::function<void()>& fill) const;
    };
} // namespace spite

// The engine writes the ECS vocabulary as `ecs::X`; publish the expected aliases.
namespace spite::ecs
{
    using spite::World;
}
