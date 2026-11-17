#pragma once

#include "base/MathTypes.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    enum class ObjectLayer : u16
    {
        NonMoving = 0,
        Moving = 1,
        MovingKinematic = 2,
        Trigger = 3,
        Debris = 4,
        Bullet = 5,
        Character = 6,
        Vehicle = 7,
        Custom0 = 8,
        Custom1 = 9,
        Custom2 = 10,
        Custom3 = 11,
        Custom4 = 12,
        Custom5 = 13,
        Custom6 = 14,
        Custom7 = 15
    };

    inline constexpr u16 NUM_OBJECT_LAYERS = 16;

    namespace CollisionLayers
    {
        inline constexpr u16 ALL_LAYERS = 0xFFFF;

        inline constexpr u16 layerMask(ObjectLayer layer) { return static_cast<u16>(1u << static_cast<u16>(layer)); }
    } // namespace CollisionLayers

    // Authoring: what the body is. Physics only reads it; a change rebuilds the
    // body with its velocity kept. Simulation state lives in
    // RigidbodyStateComponent. The entity's TransformComponent places the body;
    // a collider component gives its shape (box, then sphere, then capsule wins).
    struct RigidbodyComponent : IEngineComponent
    {
        enum class BodyType : u8
        {
            Static = 0,
            Kinematic = 1,
            Dynamic = 2
        };
        SPITE_ENUM_NAMES_IN_CLASS(BodyType, Static, Kinematic, Dynamic)

        // How a transform write reaches the body: PhysicsDriven moves kinematic
        // bodies toward it and leaves dynamic and static bodies to the
        // simulation. At fixed synchronization, Teleport applies a changed pose
        // and clears non-static linear and angular velocity. This is a persistent
        // mode, not a one-shot operation; restore the prior mode after use.
        enum class TransformBehaviourMode : u8
        {
            PhysicsDriven = 0,
            Teleport = 1
        };
        SPITE_ENUM_NAMES_IN_CLASS(TransformBehaviourMode, PhysicsDriven, Teleport)

        float mass = 1.0f;
        float friction = 0.5f;
        float restitution = 0.3f;
        float linearDamping = 0.05f;
        float angularDamping = 0.05f;
        BodyType bodyType = BodyType::Dynamic;
        u16 objectLayer = static_cast<u16>(ObjectLayer::Moving);
        u16 collisionMask = CollisionLayers::ALL_LAYERS;
        bool isSensor = false;
        bool collideKinematicVsNonDynamic = false;
        bool useGravity = true;
        bool allowSleeping = true;
        bool lockLinearX = false;
        bool lockLinearY = false;
        bool lockLinearZ = false;
        bool lockAngularX = false;
        bool lockAngularY = false;
        bool lockAngularZ = false;
        TransformBehaviourMode transformBehaviourMode = TransformBehaviourMode::PhysicsDriven;
        // restitution and the dampings: Jolt's documented range, 0 to 1
        SPITE_SCHEMA(RigidbodyComponent, SPITE_FIELD(mass, fieldUnit("kg")), friction,
                     SPITE_FIELD(restitution, fieldRange(0.0, 1.0) | fieldStep(0.01)),
                     SPITE_FIELD(linearDamping, fieldRange(0.0, 1.0) | fieldStep(0.01)),
                     SPITE_FIELD(angularDamping, fieldRange(0.0, 1.0) | fieldStep(0.01)), bodyType, objectLayer,
                     collisionMask, isSensor, collideKinematicVsNonDynamic, useGravity, allowSleeping, lockLinearX,
                     lockLinearY, lockLinearZ, lockAngularX, lockAngularY, lockAngularZ, transformBehaviourMode)
    };

    // Primitive dimensions and offsets are in physics world units and ignore
    // entity and parent transform scale. Offset is body-local and rotates with
    // the body; the primitive itself has no separate collider rotation.
    struct BoxColliderComponent : IEngineComponent
    {
        Vec3 halfExtents = {0.5f, 0.5f, 0.5f};
        Vec3 offset = {0.0f, 0.0f, 0.0f};
        SPITE_SCHEMA(BoxColliderComponent, halfExtents, offset)
    };

    struct SphereColliderComponent : IEngineComponent
    {
        float radius = 0.5f;
        Vec3 offset = {0.0f, 0.0f, 0.0f};
        SPITE_SCHEMA(SphereColliderComponent, radius, offset)
    };

    struct CapsuleColliderComponent : IEngineComponent
    {
        float radius = 0.5f;
        float halfHeight = 1.0f;
        Vec3 offset = {0.0f, 0.0f, 0.0f};
        SPITE_SCHEMA(CapsuleColliderComponent, radius, halfHeight, offset)
    };

    // Physics-owned: added when the body is created, written after each fixed
    // step for bodies that moved, removed with the body. Velocities change
    // through PhysicsWorld, never by writing this component.
    struct RigidbodyStateComponent : IDerivedComponent
    {
        Vec3 linearVelocity = {0.0f, 0.0f, 0.0f};
        Vec3 angularVelocity = {0.0f, 0.0f, 0.0f};
        bool awake = false;
        u32 bodyId = 0; // the simulation's body handle: runtime state, outside the schema
        SPITE_OBSERVED_SCHEMA(RigidbodyStateComponent, SPITE_FIELD(linearVelocity, OBSERVED | READONLY),
                              SPITE_FIELD(angularVelocity, OBSERVED | READONLY),
                              SPITE_FIELD(awake, OBSERVED | READONLY))
    };
} // namespace spite
