#pragma once

#include <memory>

#include "base/MathTypes.hpp"
#include "ecs/EcsTypes.hpp"
#include "ecs/SystemContext.hpp"
#include "physics/PhysicsExport.hpp"
#include "physics/PhysicsSettings.hpp"

namespace spite
{
    class World;

    // Registers the physics components (PhysicsComponents.hpp) process-wide, so
    // a world without a PhysicsWorld (the edit scene) can author them.
    SPITE_PHYSICS_API void registerPhysicsComponents();

    inline constexpr const char* PHYSICS_SYSTEM_NAME = "Physics";

    // One World's simulation: owns its own simulation instance,
    // publishes itself as a service of the world and registers the fixed-phase
    // "Physics" system there. Its frame phase retains changed authoring entities
    // until the next fixed step, without advancing simulation. Each fixed step creates bodies for new
    // rigidbodies, applies modified authoring and transform writes, advances
    // the simulation when a body is awake and writes moved bodies' poses into
    // TransformComponent and RigidbodyStateComponent. A body goes when its
    // RigidbodyComponent or RigidbodyStateComponent does (destruction
    // included). Destroy it before its World; worlds and their PhysicsWorlds
    // are independent of each other.
    //
    // Systems reach it as ctx.services().write<PhysicsWorld>(), which
    // serializes them with the fixed step.
    class SPITE_PHYSICS_API PhysicsWorld
    {
    public:
        explicit PhysicsWorld(World& world, const PhysicsSettings& settings = {});
        ~PhysicsWorld();
        PhysicsWorld(const PhysicsWorld&) = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;

        [[nodiscard]] const PhysicsSettings& settings() const;
        // Settings failing PhysicsSettings::valid() are refused with a warning (the constructor then uses the
        // defaults, this keeps the current ones). A gravity change wakes the sleeping bodies.
        void setSettings(const PhysicsSettings& settings);
        // Reserve before spawning a larger physics scene. Growing is supported only while no bodies exist, so
        // active bodies never lose contacts or velocity. Frame thread, outside system execution. Large grids
        // prepare bodies over several fixed steps before simulation starts.
        bool reserveBodies(u32 count);
        [[nodiscard]] bool preparingBodies() const;

        // Queue a world-space pose for the next fixed step; latest request wins.
        // Returns false if the body does not exist. Clears non-static velocities.
        bool teleport(Entity entity, const Vec3& position, const Quat& rotation);
        // Wake the body; false when the entity has no body (yet: bodies appear
        // in the fixed step after their rigidbody).
        bool setLinearVelocity(Entity entity, const Vec3& velocity);
        bool setAngularVelocity(Entity entity, const Vec3& velocity);
        bool addImpulse(Entity entity, const Vec3& impulse);

        [[nodiscard]] bool hasBody(Entity entity) const;
        [[nodiscard]] u32 bodyCount() const;
        [[nodiscard]] u32 activeBodyCount() const;
        // fixed steps that advanced the simulation; steps without an awake body skip it
        [[nodiscard]] u64 simulatedSteps() const;

    private:
        friend SPITE_PHYSICS_API void stepPhysics(SystemContext& ctx);
        void step(SystemContext& ctx);

        struct State;
        std::unique_ptr<State> m_state;
    };

    // The "Physics" system's entry: the world's PhysicsWorld runs one fixed step.
    SPITE_PHYSICS_API void stepPhysics(SystemContext& ctx);
} // namespace spite
