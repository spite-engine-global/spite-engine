#include "physics/PhysicsWorld.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyPair.h>
#include <Jolt/Physics/Collision/GroupFilter.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/ContactConstraintManager.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/Assert.hpp"
#include "base/Handle.hpp"
#include "base/Logging.hpp"
#include "base/MathFunctions.hpp"
#include "base/ServiceLocator.hpp"
#include "ecs/Chunk.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "ecs/LifecycleEvents.hpp"
#include "ecs/Query.hpp"
#include "ecs/World.hpp"
#include "physics/PhysicsComponents.hpp"
#include "profiling/Zone.hpp"
#include "world/EntityTransformManager.hpp"
#include "world/TransformComponents.hpp"

namespace spite
{
    namespace
    {
        void joltTrace(const char* format, ...)
        {
            char buffer[1024];
            va_list args;
            va_start(args, format);
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            SINFO_LOG("jolt: {}", buffer);
        }

#ifdef JPH_ENABLE_ASSERTS
        bool joltAssertFailed(const char* expression, const char* message, const char* file, const JPH::uint line)
        {
            SASSERTM(false, "jolt: {} ({}) at {}:{}", expression, message ? message : "", file, line);
            return false;
        }
#endif

        // Process-wide simulation setup: once per module load, shared
        // by every PhysicsWorld.
        struct JoltRuntime
        {
            JoltRuntime()
            {
                JPH::RegisterDefaultAllocator();
                JPH::Trace = &joltTrace;
                JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = &joltAssertFailed;)
                JPH::Factory::sInstance = new JPH::Factory();
                JPH::RegisterTypes();
            }

            ~JoltRuntime()
            {
                JPH::UnregisterTypes();
                delete JPH::Factory::sInstance;
                JPH::Factory::sInstance = nullptr;
            }
        };

        const JoltRuntime g_joltRuntime;

        // Default pools; a host can reserve a larger empty physics world before spawning its scene.
        constexpr u32 MAX_BODIES = 16384;
        constexpr u32 MAX_BODY_PAIRS = 32768;
        constexpr u32 MAX_CONTACT_CONSTRAINTS = 20000;
        constexpr sizet TEMP_ALLOCATOR_BYTES = 20 * 1024 * 1024;

        // The default scene's working buffers fit the fast arena: the body pair buffer
        // (a setting), the contact constraint array (864 B each: cMaxContactConstraintsLimit is ~0u /
        // sizeof(ContactConstraint), so the quotient bounds the size from above) and the island and splitter buffers,
        // 4 to 8 bytes per body or contact (no body is LinearCast, which would add a CCD
        // buffer per such body): allowed 2 MiB. Larger reserved scenes spill to the heap instead of aborting.
        constexpr sizet CONTACT_CONSTRAINT_BYTES = ~0u / JPH::ContactConstraintManager::cMaxContactConstraintsLimit;
        constexpr sizet TEMP_ALLOCATOR_OTHER_BYTES = 2 * 1024 * 1024;
        static_assert(sizeof(JPH::BodyPair) * PhysicsSettings::MAX_IN_FLIGHT_BODY_PAIRS +
                          MAX_CONTACT_CONSTRAINTS * CONTACT_CONSTRAINT_BYTES + TEMP_ALLOCATOR_OTHER_BYTES <=
                      TEMP_ALLOCATOR_BYTES);

        // Refuse tuning outside Jolt's supported numeric domains and retain the current settings.
        bool acceptable(const PhysicsSettings& settings)
        {
            const bool valid = settings.valid();
            if (!valid)
            {
                SWARNING_LOG("physics: refused settings outside PhysicsSettings::valid(); keeping the current ones");
            }
            return valid;
        }

        enum class BroadPhaseLayer : u8
        {
            NonMoving = 0,
            Moving = 1,
            Count
        };

        bool isValidObjectLayer(const JPH::ObjectLayer layer) { return layer < NUM_OBJECT_LAYERS; }

        class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
        {
        public:
            JPH::uint GetNumBroadPhaseLayers() const override { return static_cast<JPH::uint>(BroadPhaseLayer::Count); }

            JPH::BroadPhaseLayer GetBroadPhaseLayer(const JPH::ObjectLayer layer) const override
            {
                const bool nonMoving = layer == static_cast<JPH::ObjectLayer>(ObjectLayer::NonMoving);
                return JPH::BroadPhaseLayer(
                    static_cast<u8>(nonMoving ? BroadPhaseLayer::NonMoving : BroadPhaseLayer::Moving));
            }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
            const char* GetBroadPhaseLayerName(const JPH::BroadPhaseLayer layer) const override
            {
                return layer.GetValue() == static_cast<u8>(BroadPhaseLayer::NonMoving) ? "NonMoving" : "Moving";
            }
#endif
        };

        class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
        {
        public:
            bool ShouldCollide(const JPH::ObjectLayer layer, const JPH::BroadPhaseLayer broadPhaseLayer) const override
            {
                if (!isValidObjectLayer(layer))
                {
                    return false;
                }
                return layer != static_cast<JPH::ObjectLayer>(ObjectLayer::NonMoving) ||
                    broadPhaseLayer.GetValue() == static_cast<u8>(BroadPhaseLayer::Moving);
            }
        };

        class ObjectPairFilter final : public JPH::ObjectLayerPairFilter
        {
        public:
            bool ShouldCollide(const JPH::ObjectLayer lhs, const JPH::ObjectLayer rhs) const override
            {
                constexpr auto nonMoving = static_cast<JPH::ObjectLayer>(ObjectLayer::NonMoving);
                return isValidObjectLayer(lhs) && isValidObjectLayer(rhs) && (lhs != nonMoving || rhs != nonMoving);
            }
        };

        // A body's group id is its layer bit and its subgroup id its collision
        // mask: two bodies collide when each one's mask accepts the other's layer.
        class CollisionMaskFilter final : public JPH::GroupFilter
        {
        public:
            bool CanCollide(const JPH::CollisionGroup& lhs, const JPH::CollisionGroup& rhs) const override
            {
                return (lhs.GetGroupID() & rhs.GetSubGroupID()) != 0 && (rhs.GetGroupID() & lhs.GetSubGroupID()) != 0;
            }
        };

        JPH::Vec3 toJolt(const Vec3& value) { return {value.x, value.y, value.z}; }
        JPH::Quat toJolt(const Quat& value) { return {value.x, value.y, value.z, value.w}; }
        Vec3 fromJolt(const JPH::Vec3 value) { return {value.GetX(), value.GetY(), value.GetZ()}; }
        Quat fromJolt(const JPH::Quat value) { return {value.GetX(), value.GetY(), value.GetZ(), value.GetW()}; }

        JPH::PhysicsSettings toJolt(const PhysicsSettings& settings)
        {
            JPH::PhysicsSettings jolt;
            jolt.mMaxInFlightBodyPairs = settings.bodyPairCache.maxInFlightBodyPairs;
            jolt.mBodyPairCacheMaxDeltaPositionSq = settings.bodyPairCache.maxDeltaPositionSq;
            jolt.mBodyPairCacheCosMaxDeltaRotationDiv2 = settings.bodyPairCache.cosMaxDeltaRotationDiv2;
            jolt.mUseBodyPairContactCache = settings.bodyPairCache.useBodyPairContactCache;
            jolt.mSpeculativeContactDistance = settings.contact.speculativeContactDistance;
            jolt.mPenetrationSlop = settings.contact.penetrationSlop;
            jolt.mManifoldTolerance = settings.contact.manifoldTolerance;
            jolt.mMaxPenetrationDistance = settings.contact.maxPenetrationDistance;
            jolt.mContactNormalCosMaxDeltaRotation = settings.contact.contactNormalCosMaxDeltaRotation;
            jolt.mContactPointPreserveLambdaMaxDistSq = settings.contact.contactPointPreserveLambdaMaxDistSq;
            jolt.mNumVelocitySteps = settings.solver.numVelocitySteps;
            jolt.mNumPositionSteps = settings.solver.numPositionSteps;
            jolt.mBaumgarte = settings.solver.baumgarte;
            jolt.mTimeBeforeSleep = settings.sleeping.timeBeforeSleep;
            jolt.mPointVelocitySleepThreshold = settings.sleeping.pointVelocitySleepThreshold;
            jolt.mAllowSleeping = settings.sleeping.allowSleeping;
            jolt.mCheckActiveEdges = settings.collisionDetection.checkActiveEdges;
            jolt.mUseManifoldReduction = settings.collisionDetection.useManifoldReduction;
            jolt.mLinearCastThreshold = settings.ccd.linearCastThreshold;
            jolt.mLinearCastMaxPenetration = settings.ccd.linearCastMaxPenetration;
            jolt.mMinVelocityForRestitution = settings.behavior.minVelocityForRestitution;
            jolt.mDeterministicSimulation = settings.behavior.deterministicSimulation;
            jolt.mConstraintWarmStart = settings.behavior.constraintWarmStart;
            jolt.mUseLargeIslandSplitter = settings.behavior.useLargeIslandSplitter;
            jolt.mStepListenersBatchSize = settings.behavior.stepListenersBatchSize;
            return jolt;
        }

        enum class ShapeKind : u8
        {
            Box,
            Sphere,
            Capsule
        };

        // What a body is built from; a body whose description changes is rebuilt.
        struct BodyDesc
        {
            RigidbodyComponent::BodyType bodyType = RigidbodyComponent::BodyType::Dynamic;
            u16 objectLayer = 0;
            u16 collisionMask = 0;
            u8 allowedDofs = 0;
            bool isSensor = false;
            bool collideKinematicVsNonDynamic = false;
            bool useGravity = true;
            bool allowSleeping = true;
            float mass = 0.0f;
            float friction = 0.0f;
            float restitution = 0.0f;
            float linearDamping = 0.0f;
            float angularDamping = 0.0f;
            ShapeKind shape = ShapeKind::Box;
            Vec3 size; // box: half extents; sphere: (radius); capsule: (radius, half height)
            Vec3 offset;

            bool operator==(const BodyDesc&) const = default;
        };

        u8 allowedDofs(const RigidbodyComponent& rb)
        {
            const bool locks[] = {rb.lockLinearX,  rb.lockLinearY,  rb.lockLinearZ,
                                  rb.lockAngularX, rb.lockAngularY, rb.lockAngularZ};
            u8 dofs = 0;
            for (u32 i = 0; i < 6; ++i)
            {
                dofs |= locks[i] ? 0 : static_cast<u8>(1u << i);
            }
            return dofs;
        }

        std::optional<BodyDesc> describe(const SystemContext& ctx, const Entity entity, const RigidbodyComponent& rb)
        {
            BodyDesc desc{.bodyType = rb.bodyType,
                          .objectLayer = rb.objectLayer,
                          .collisionMask = rb.collisionMask,
                          .allowedDofs = allowedDofs(rb),
                          .isSensor = rb.isSensor,
                          .collideKinematicVsNonDynamic = rb.collideKinematicVsNonDynamic,
                          .useGravity = rb.useGravity,
                          .allowSleeping = rb.allowSleeping,
                          .mass = rb.mass,
                          .friction = rb.friction,
                          .restitution = rb.restitution,
                          .linearDamping = rb.linearDamping,
                          .angularDamping = rb.angularDamping};
            if (const auto* box = ctx.tryGetConstComponent<BoxColliderComponent>(entity))
            {
                desc.shape = ShapeKind::Box;
                desc.size = box->halfExtents;
                desc.offset = box->offset;
            }
            else if (const auto* sphere = ctx.tryGetConstComponent<SphereColliderComponent>(entity))
            {
                desc.shape = ShapeKind::Sphere;
                desc.size = {sphere->radius, 0.0f, 0.0f};
                desc.offset = sphere->offset;
            }
            else if (const auto* capsule = ctx.tryGetConstComponent<CapsuleColliderComponent>(entity))
            {
                desc.shape = ShapeKind::Capsule;
                desc.size = {capsule->radius, capsule->halfHeight, 0.0f};
                desc.offset = capsule->offset;
            }
            else
            {
                return std::nullopt;
            }
            return desc;
        }

        JPH::ShapeRefC makeShape(const BodyDesc& desc)
        {
            constexpr float MIN_SIZE = 1.0e-3f;
            JPH::ShapeRefC shape;
            switch (desc.shape)
            {
            case ShapeKind::Box:
                {
                    const JPH::Vec3 halfExtents = JPH::Vec3::sMax(toJolt(desc.size), JPH::Vec3::sReplicate(MIN_SIZE));
                    shape =
                        new JPH::BoxShape(halfExtents, std::min(JPH::cDefaultConvexRadius, halfExtents.ReduceMin()));
                    break;
                }
            case ShapeKind::Sphere:
                shape = new JPH::SphereShape(std::max(desc.size.x, MIN_SIZE));
                break;
            case ShapeKind::Capsule:
                shape = new JPH::CapsuleShape(std::max(desc.size.y, MIN_SIZE), std::max(desc.size.x, MIN_SIZE));
                break;
            }
            if (desc.offset != Vec3{0.0f, 0.0f, 0.0f})
            {
                shape = new JPH::RotatedTranslatedShape(toJolt(desc.offset), JPH::Quat::sIdentity(), shape);
            }
            return shape;
        }

        JPH::EMotionType motionType(const RigidbodyComponent::BodyType type)
        {
            switch (type)
            {
            case RigidbodyComponent::BodyType::Static:
                return JPH::EMotionType::Static;
            case RigidbodyComponent::BodyType::Kinematic:
                return JPH::EMotionType::Kinematic;
            case RigidbodyComponent::BodyType::Dynamic:
                break;
            }
            return JPH::EMotionType::Dynamic;
        }

        struct Pose
        {
            Vec3 position{0.0f, 0.0f, 0.0f};
            Quat rotation = Quat::identity();
            Vec3 scale{1.0f, 1.0f, 1.0f};
        };

        Pose compose(const Pose& parent, const TransformComponent& local)
        {
            return {parent.position + rotate(parent.rotation, parent.scale * local.position),
                    normalize(parent.rotation * local.rotation), parent.scale * local.scale};
        }

        bool samePose(const Vec3& position, const Quat& rotation, const Vec3& otherPosition, const Quat& otherRotation)
        {
            return lengthSquared(position - otherPosition) <= 1.0e-10f &&
                std::abs(dot(rotation, otherRotation)) >= 1.0f - 1.0e-6f;
        }
    } // namespace

    struct PhysicsWorld::State
    {
        struct Body
        {
            JPH::BodyID id;
            BodyDesc desc;
        };

        World world;
        PhysicsSettings settings;
        JPH::TempAllocatorImplWithMallocFallback tempAllocator{TEMP_ALLOCATOR_BYTES};
        // no worker threads: jobs run on the stepping thread until they move to the engine scheduler
        JPH::JobSystemThreadPool jobSystem{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 0};
        BroadPhaseLayers broadPhaseLayers;
        ObjectVsBroadPhaseFilter objectVsBroadPhaseFilter;
        ObjectPairFilter objectPairFilter;
        JPH::Ref<CollisionMaskFilter> collisionMaskFilter = new CollisionMaskFilter();
        JPH::PhysicsSystem system;
        // removal hooks run wherever a component goes, the fixed step on a worker
        std::mutex mutex;
        std::unordered_map<Entity, Body> bodies;
        std::unordered_set<Entity> pendingAuthoring;
        std::unordered_map<Entity, std::pair<Vec3, Quat>> pendingTeleports;
        u64 simulatedSteps = 0;
        u32 creationBudget = 0;
        bool preparing = false;
        // kinematic bodies MoveKinematic gave a velocity last step: Jolt keeps a kinematic body's velocity, so one that
        // got where it was sent and is not sent again would drift on
        std::vector<JPH::BodyID> kinematicMoving;
        Handle rigidbodyRemoved;
        Handle stateRemoved;

        explicit State(const World& owner, const PhysicsSettings& initial, u32 capacity = MAX_BODIES) : world(owner)
        {
            const u32 pairs = capacity > MAX_BODIES ? capacity * 2 : MAX_BODY_PAIRS;
            const u32 contacts = capacity > MAX_BODIES ? capacity * 2 : MAX_CONTACT_CONSTRAINTS;
            system.Init(capacity, 0, pairs, contacts, broadPhaseLayers, objectVsBroadPhaseFilter, objectPairFilter);
            apply(initial);
        }

        ~State()
        {
            JPH::BodyInterface& bodyInterface = system.GetBodyInterfaceNoLock();
            for (const auto& [entity, body] : bodies)
            {
                bodyInterface.RemoveBody(body.id);
                bodyInterface.DestroyBody(body.id);
            }
        }

        void apply(const PhysicsSettings& next)
        {
            const bool gravityChanged = next.gravity != settings.gravity;
            settings = next;
            system.SetPhysicsSettings(toJolt(settings));
            system.SetGravity(toJolt(settings.gravity));
            if (gravityChanged)
            {
                // Jolt leaves sleeping bodies asleep: they would hang in the air until something touched them
                JPH::BodyInterface& bodyInterface = system.GetBodyInterfaceNoLock();
                for (const auto& [entity, body] : bodies)
                {
                    if (body.desc.bodyType == RigidbodyComponent::BodyType::Dynamic && body.desc.useGravity)
                    {
                        bodyInterface.ActivateBody(body.id);
                    }
                }
            }
        }

        JPH::BodyID create(const Entity entity, const BodyDesc& desc, const Vec3& position, const Quat& rotation,
                           const Vec3& linearVelocity, const Vec3& angularVelocity)
        {
            const JPH::ObjectLayer layer = isValidObjectLayer(desc.objectLayer)
                ? desc.objectLayer
                : static_cast<JPH::ObjectLayer>(ObjectLayer::Moving);
            JPH::BodyCreationSettings creation(makeShape(desc), toJolt(position), toJolt(rotation),
                                               motionType(desc.bodyType), layer);
            creation.mFriction = desc.friction;
            creation.mRestitution = desc.restitution;
            creation.mLinearDamping = desc.linearDamping;
            creation.mAngularDamping = desc.angularDamping;
            creation.mGravityFactor = desc.useGravity ? 1.0f : 0.0f;
            creation.mIsSensor = desc.isSensor;
            creation.mCollideKinematicVsNonDynamic = desc.collideKinematicVsNonDynamic;
            creation.mAllowSleeping = desc.allowSleeping;
            creation.mAllowedDOFs = static_cast<JPH::EAllowedDOFs>(desc.allowedDofs);
            creation.mLinearVelocity = toJolt(linearVelocity);
            creation.mAngularVelocity = toJolt(angularVelocity);
            creation.mUserData = entity.id();
            creation.mCollisionGroup = JPH::CollisionGroup(collisionMaskFilter, 1u << layer, desc.collisionMask);
            if (desc.bodyType == RigidbodyComponent::BodyType::Dynamic && desc.mass > 0.0f)
            {
                creation.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                creation.mMassPropertiesOverride.mMass = desc.mass;
            }
            const JPH::EActivation activation = desc.bodyType == RigidbodyComponent::BodyType::Static
                ? JPH::EActivation::DontActivate
                : JPH::EActivation::Activate;
            const JPH::BodyID id = system.GetBodyInterfaceNoLock().CreateAndAddBody(creation, activation);
            if (id.IsInvalid())
            {
                SERROR_LOG("physics: no room for the body of entity {} ({} bodies)", entity.id(),
                           system.GetMaxBodies());
            }
            return id;
        }

        void destroy(const JPH::BodyID id)
        {
            JPH::BodyInterface& bodyInterface = system.GetBodyInterfaceNoLock();
            bodyInterface.RemoveBody(id);
            bodyInterface.DestroyBody(id);
        }

        void onRemoved(const Entity entity)
        {
            std::scoped_lock lock(mutex);
            pendingAuthoring.erase(entity);
            pendingTeleports.erase(entity);
            if (const auto it = bodies.find(entity); it != bodies.end())
            {
                destroy(it->second.id);
                bodies.erase(it);
            }
        }

        void collectAuthoring(SystemContext& ctx)
        {
            const ComponentID watched[] = {getComponentId<RigidbodyComponent>(), getComponentId<BoxColliderComponent>(),
                                           getComponentId<SphereColliderComponent>(),
                                           getComponentId<CapsuleColliderComponent>()};
            ctx.createQueryBuilder()
                .with<Read<RigidbodyComponent>, Read<RigidbodyStateComponent>>()
                .buildQuery()
                ->forEachChangedChunkConst(watched, ChangeWindow::SinceLastFrame,
                                           [&](const Chunk* chunk)
                                           {
                                               std::bitset<DEFAULT_CHUNK_CAPACITY> changed;
                                               for (const ComponentID id : watched)
                                               {
                                                   const int index = chunk->getComponentIndex(id);
                                                   if (index >= 0)
                                                       changed |= chunk->modifiedSinceLastFrameByIndex(
                                                           static_cast<sizet>(index));
                                               }
                                               for (sizet row = 0; row < chunk->size(); ++row)
                                                   if (changed.test(row))
                                                       pendingAuthoring.insert(chunk->entity(row));
                                           });
        }

        JPH::BodyID find(const Entity entity) const
        {
            const auto it = bodies.find(entity);
            return it == bodies.end() ? JPH::BodyID() : it->second.id;
        }
    };

    void registerPhysicsComponents()
    {
        registerComponent<RigidbodyComponent>();
        registerComponent<BoxColliderComponent>();
        registerComponent<SphereColliderComponent>();
        registerComponent<CapsuleColliderComponent>();
        registerComponent<RigidbodyStateComponent>();
    }

    PhysicsWorld::PhysicsWorld(World& world, const PhysicsSettings& settings) :
        m_state(std::make_unique<State>(world, acceptable(settings) ? settings : PhysicsSettings{}))
    {
        registerPhysicsComponents();
        registerComponent<TransformComponent>();
        registerComponent<TransformMatrixComponent>();
        world.services().registerService(this);

        world.registerSystem(SystemDescriptor{
            .name = PHYSICS_SYSTEM_NAME,
            .fn =
                [](SystemContext& ctx)
            {
                auto physics = ctx.services().write<PhysicsWorld>();
                std::scoped_lock lock(physics->m_state->mutex);
                physics->m_state->collectAuthoring(ctx);
            },
            .fixedUpdateFn = &stepPhysics,
            .update =
                SystemPhaseDescriptor{
                    .reads = {getComponentId<RigidbodyComponent>(), getComponentId<RigidbodyStateComponent>(),
                              getComponentId<BoxColliderComponent>(), getComponentId<SphereColliderComponent>(),
                              getComponentId<CapsuleColliderComponent>(), getComponentId<InactiveComponent>()},
                    .stage = CoreExecutionStages::CLEANUP},
            .fixedUpdate = SystemPhaseDescriptor{
                .reads = {getComponentId<RigidbodyComponent>(), getComponentId<BoxColliderComponent>(),
                          getComponentId<SphereColliderComponent>(), getComponentId<CapsuleColliderComponent>(),
                          getComponentId<TransformMatrixComponent>(), getComponentId<InactiveComponent>()},
                .writes = {getComponentId<TransformComponent>(), getComponentId<RigidbodyStateComponent>()}}});

        LifecycleEvents& events = world.lifecycleEvents();
        m_state->rigidbodyRemoved = events.onComponentRemoved(
            getComponentId<RigidbodyComponent>(), [this](const Entity entity) { m_state->onRemoved(entity); });
        m_state->stateRemoved = events.onComponentRemoved(getComponentId<RigidbodyStateComponent>(),
                                                          [this](const Entity entity) { m_state->onRemoved(entity); });
    }

    PhysicsWorld::~PhysicsWorld()
    {
        const World& world = m_state->world;
        LifecycleEvents& events = world.lifecycleEvents();
        events.unsubscribe(m_state->rigidbodyRemoved);
        events.unsubscribe(m_state->stateRemoved);
        world.removeSystem({nullptr, PHYSICS_SYSTEM_NAME});
        world.services().unregisterService<PhysicsWorld>();
    }

    const PhysicsSettings& PhysicsWorld::settings() const { return m_state->settings; }

    bool PhysicsWorld::reserveBodies(const u32 count)
    {
        constexpr u32 MAX_RESERVED_BODIES = 1u << 20;
        if (count > MAX_RESERVED_BODIES)
            return false;
        std::unique_ptr<State> previous;
        {
            std::scoped_lock lock(m_state->mutex);
            if (count <= m_state->system.GetMaxBodies())
            {
                if (m_state->bodies.empty())
                {
                    m_state->kinematicMoving.clear();
                    m_state->creationBudget = count > MAX_BODIES ? 4096 : 0;
                    m_state->preparing = m_state->creationBudget != 0;
                }
                return true;
            }
            if (!m_state->bodies.empty())
                return false;
            auto next = std::make_unique<State>(m_state->world, m_state->settings, std::bit_ceil(count));
            next->rigidbodyRemoved = m_state->rigidbodyRemoved;
            next->stateRemoved = m_state->stateRemoved;
            next->simulatedSteps = m_state->simulatedSteps;
            next->creationBudget = 4096;
            next->preparing = true;
            previous = std::move(m_state);
            m_state = std::move(next);
        }
        return true;
    }

    bool PhysicsWorld::preparingBodies() const
    {
        std::scoped_lock lock(m_state->mutex);
        return m_state->preparing;
    }

    void PhysicsWorld::setSettings(const PhysicsSettings& settings)
    {
        if (!acceptable(settings))
        {
            return;
        }
        std::scoped_lock lock(m_state->mutex);
        m_state->apply(settings);
    }

    bool PhysicsWorld::teleport(const Entity entity, const Vec3& position, const Quat& rotation)
    {
        std::scoped_lock lock(m_state->mutex);
        if (!m_state->bodies.contains(entity))
            return false;
        m_state->pendingTeleports.insert_or_assign(entity, std::pair{position, rotation});
        return true;
    }

    bool PhysicsWorld::setLinearVelocity(const Entity entity, const Vec3& velocity)
    {
        std::scoped_lock lock(m_state->mutex);
        const JPH::BodyID id = m_state->find(entity);
        if (!id.IsInvalid())
        {
            auto& bodies = m_state->system.GetBodyInterfaceNoLock();
            const auto value = toJolt(velocity);
            bodies.SetLinearVelocity(id, value);
            if (value.IsNearZero() && bodies.GetMotionType(id) != JPH::EMotionType::Static)
                bodies.ActivateBody(id);
        }
        return !id.IsInvalid();
    }

    bool PhysicsWorld::setAngularVelocity(const Entity entity, const Vec3& velocity)
    {
        std::scoped_lock lock(m_state->mutex);
        const JPH::BodyID id = m_state->find(entity);
        if (!id.IsInvalid())
        {
            auto& bodies = m_state->system.GetBodyInterfaceNoLock();
            const auto value = toJolt(velocity);
            bodies.SetAngularVelocity(id, value);
            if (value.IsNearZero() && bodies.GetMotionType(id) != JPH::EMotionType::Static)
                bodies.ActivateBody(id);
        }
        return !id.IsInvalid();
    }

    bool PhysicsWorld::addImpulse(const Entity entity, const Vec3& impulse)
    {
        std::scoped_lock lock(m_state->mutex);
        const JPH::BodyID id = m_state->find(entity);
        if (!id.IsInvalid())
        {
            m_state->system.GetBodyInterfaceNoLock().AddImpulse(id, toJolt(impulse));
        }
        return !id.IsInvalid();
    }

    bool PhysicsWorld::hasBody(const Entity entity) const
    {
        std::scoped_lock lock(m_state->mutex);
        return m_state->bodies.contains(entity);
    }

    u32 PhysicsWorld::bodyCount() const
    {
        std::scoped_lock lock(m_state->mutex);
        return static_cast<u32>(m_state->bodies.size());
    }

    u32 PhysicsWorld::activeBodyCount() const { return m_state->system.GetNumActiveBodies(JPH::EBodyType::RigidBody); }

    u64 PhysicsWorld::simulatedSteps() const { return m_state->simulatedSteps; }

    void PhysicsWorld::step(SystemContext& ctx)
    {
        State& state = *m_state;
        std::scoped_lock lock(state.mutex);
        JPH::BodyInterface& bodyInterface = state.system.GetBodyInterfaceNoLock();
        CommandBuffer& commands = ctx.getCommandBuffer();

        ServiceLocator& services = ctx.services();
        std::optional<ReadService<EntityTransformManager>> transforms;
        if (services.contains<EntityTransformManager>())
        {
            transforms.emplace(services.read<EntityTransformManager>());
        }
        const auto parentOf = [&](const Entity entity)
        { return transforms ? (*transforms)->parentOf(entity) : Entity::undefined(); };
        const auto worldPose = [&](const Entity entity)
        {
            Entity chain[64];
            u32 depth = 0;
            for (Entity at = entity; at != Entity::undefined() && depth < std::size(chain); at = parentOf(at))
            {
                chain[depth++] = at;
            }
            Pose pose;
            while (depth > 0)
            {
                if (const auto* local = ctx.tryGetConstComponent<TransformComponent>(chain[--depth]))
                {
                    pose = compose(pose, *local);
                }
            }
            return pose;
        };

        // bodies that lost their rigidbody or collider
        const auto dropBody = [&](const Entity entity)
        {
            if (const auto it = state.bodies.find(entity); it != state.bodies.end())
            {
                state.destroy(it->second.id);
                state.bodies.erase(it);
            }
            commands.removeComponent<RigidbodyStateComponent>(entity);
        };
        for (const Entity entity : ctx.createQueryBuilder()
                                       .with<Read<RigidbodyStateComponent>>()
                                       .without<RigidbodyComponent>()
                                       .includeInactive()
                                       .buildQuery()
                                       ->view<Entity>())
        {
            dropBody(entity);
        }
        for (const Entity entity :
             ctx.createQueryBuilder()
                 .with<Read<RigidbodyStateComponent>, Read<RigidbodyComponent>>()
                 .without<BoxColliderComponent, SphereColliderComponent, CapsuleColliderComponent>()
                 .includeInactive()
                 .buildQuery()
                 ->view<Entity>())
        {
            dropBody(entity);
        }

        // inactive entities lose their body and velocity; the creation below makes a fresh one on re-enable
        for (const Entity entity : ctx.createQueryBuilder()
                                       .with<Read<RigidbodyStateComponent>, Read<InactiveComponent>>()
                                       .buildQuery()
                                       ->view<Entity>())
        {
            dropBody(entity);
        }

        // Prepare a large grid in batches and hold simulation until all bodies exist.
        u32 created = 0;
        bool pending = false;
        // new rigidbodies
        for (auto [entity, rb] : ctx.createQueryBuilder()
                                     .with<Read<RigidbodyComponent>, Read<TransformComponent>>()
                                     .without<RigidbodyStateComponent>()
                                     .buildQuery()
                                     ->view<Entity, Read<RigidbodyComponent>>())
        {
            const std::optional<BodyDesc> desc = describe(ctx, entity, rb);
            if (!desc || state.bodies.contains(entity))
            {
                continue;
            }
            if (state.creationBudget && created >= state.creationBudget)
            {
                pending = true;
                break;
            }
            const Pose pose = worldPose(entity);
            const JPH::BodyID id = state.create(entity, *desc, pose.position, pose.rotation, {}, {});
            if (id.IsInvalid())
            {
                continue;
            }
            state.bodies.emplace(entity, State::Body{id, *desc});
            ++created;
            commands.addComponent(
                entity,
                RigidbodyStateComponent{.awake = desc->bodyType != RigidbodyComponent::BodyType::Static,
                                        .bodyId = id.GetIndexAndSequenceNumber()});
        }

        state.preparing = pending;
        if (pending)
            return;
        state.creationBudget = 0;

        if (state.bodies.empty())
        {
            return;
        }

        // a kinematic body sent last step has arrived: it stops unless a transform write sends it on. Before the
        // rebuild below, which keeps the velocity (a rebuilt body would carry the move's velocity on under a new id)
        for (const JPH::BodyID id : state.kinematicMoving)
        {
            bodyInterface.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
        }
        state.kinematicMoving.clear();

        // Frame updates retain authoring edits until a fixed step consumes them; other ECS consumers keep their
        // ordinary frame windows. Rebuild only when the retained description differs, preserving velocity.
        state.collectAuthoring(ctx);
        for (const Entity entity : state.pendingAuthoring)
        {
            const auto it = state.bodies.find(entity);
            if (it == state.bodies.end())
                continue;
            const auto& rb = ctx.getConstComponent<RigidbodyComponent>(entity);
            const std::optional<BodyDesc> desc = describe(ctx, entity, rb);
            if (!desc || *desc == it->second.desc)
            {
                continue;
            }
            const JPH::BodyID old = it->second.id;
            JPH::RVec3 position;
            JPH::Quat rotation;
            bodyInterface.GetPositionAndRotation(old, position, rotation);
            const Vec3 linearVelocity = fromJolt(bodyInterface.GetLinearVelocity(old));
            const Vec3 angularVelocity = fromJolt(bodyInterface.GetAngularVelocity(old));
            state.destroy(old);
            const JPH::BodyID id =
                state.create(entity, *desc, fromJolt(position), fromJolt(rotation), linearVelocity, angularVelocity);
            if (id.IsInvalid())
            {
                state.bodies.erase(it);
                commands.removeComponent<RigidbodyStateComponent>(entity);
                continue;
            }
            it->second = {id, *desc};
            ctx.getComponent<RigidbodyStateComponent>(entity).bodyId = id.GetIndexAndSequenceNumber();
        }
        state.pendingAuthoring.clear();

        // transform writes reach kinematic bodies and teleporting ones
        for (auto [entity, rb, body] :
             ctx.createQueryBuilder()
                 .with<Read<RigidbodyComponent>, Read<RigidbodyStateComponent>, Read<TransformMatrixComponent>>()
                 .modified<TransformMatrixComponent>()
                 .buildQuery()
                 ->view<Entity, Read<RigidbodyComponent>, Read<RigidbodyStateComponent>>())
        {
            const bool teleport = rb.transformBehaviourMode == RigidbodyComponent::TransformBehaviourMode::Teleport;
            if (!teleport && rb.bodyType != RigidbodyComponent::BodyType::Kinematic)
            {
                continue;
            }
            const JPH::BodyID id(body.bodyId);
            const Pose target = worldPose(entity);
            JPH::RVec3 position;
            JPH::Quat rotation;
            bodyInterface.GetPositionAndRotation(id, position, rotation);
            if (samePose(target.position, target.rotation, fromJolt(position), fromJolt(rotation)))
            {
                continue;
            }
            if (teleport)
            {
                const bool isStatic = rb.bodyType == RigidbodyComponent::BodyType::Static;
                bodyInterface.SetPositionAndRotation(id, toJolt(target.position), toJolt(target.rotation),
                                                     isStatic ? JPH::EActivation::DontActivate
                                                              : JPH::EActivation::Activate);
                if (!isStatic)
                {
                    bodyInterface.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
                }
            }
            else
            {
                bodyInterface.MoveKinematic(id, toJolt(target.position), toJolt(target.rotation), ctx.deltaTime);
                state.kinematicMoving.push_back(id);
            }
        }

        const auto teleportedWorldPose = [&](Entity entity)
        {
            Entity chain[64];
            u32 depth = 0;
            for (Entity at = entity; at != Entity::undefined() && depth < std::size(chain); at = parentOf(at))
                chain[depth++] = at;
            Pose pose;
            while (depth > 0)
            {
                const Entity at = chain[--depth];
                if (const auto* local = ctx.tryGetConstComponent<TransformComponent>(at))
                    pose = compose(pose, *local);
                if (const auto it = state.pendingTeleports.find(at); it != state.pendingTeleports.end())
                {
                    pose.position = it->second.first;
                    pose.rotation = it->second.second;
                }
            }
            return pose;
        };

        for (const auto& [entity, target] : state.pendingTeleports)
        {
            const auto it = state.bodies.find(entity);
            if (it == state.bodies.end())
                continue;
            const bool isStatic = it->second.desc.bodyType == RigidbodyComponent::BodyType::Static;
            bodyInterface.SetPositionAndRotation(it->second.id, toJolt(target.first), toJolt(target.second),
                                                 isStatic ? JPH::EActivation::DontActivate
                                                          : JPH::EActivation::Activate);
            if (!isStatic)
                bodyInterface.SetLinearAndAngularVelocity(it->second.id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            Vec3 position = target.first;
            Quat rotation = target.second;
            if (const Entity parent = parentOf(entity); parent != Entity::undefined())
            {
                const Pose parentPose = teleportedWorldPose(parent);
                const Quat toLocal = inverse(parentPose.rotation);
                const Vec3 local = rotate(toLocal, position - parentPose.position);
                position = {local.x / parentPose.scale.x, local.y / parentPose.scale.y, local.z / parentPose.scale.z};
                rotation = normalize(toLocal * rotation);
            }
            auto& transform = ctx.getComponent<TransformComponent>(entity);
            transform.position = position;
            transform.rotation = rotation;
        }
        state.pendingTeleports.clear();

        if (state.system.GetNumActiveBodies(JPH::EBodyType::RigidBody) == 0)
        {
            return;
        }
        {
            SPITE_ZONE("JoltUpdate");
            state.system.Update(ctx.deltaTime, 1, &state.tempAllocator, &state.jobSystem);
        }
        ++state.simulatedSteps;

        // moved bodies write back: awake ones, and the ones that fell asleep this step
        const JPH::BodyLockInterfaceNoLock& locks = state.system.GetBodyLockInterfaceNoLock();
        ctx.createQueryBuilder()
            .with<Read<RigidbodyComponent>, Write<RigidbodyStateComponent>, Write<TransformComponent>>()
            .buildQuery()
            ->forEachChunk(
                [&](Chunk* chunk)
                {
                    const std::span<const Entity> entities = chunk->entities();
                    const RigidbodyComponent* rigidbodies = chunk->readColumn<RigidbodyComponent>();
                    RigidbodyStateComponent* states = chunk->mutableColumn<RigidbodyStateComponent>();
                    TransformComponent* transforms = chunk->mutableColumn<TransformComponent>();
                    for (sizet i = 0; i < entities.size(); ++i)
                    {
                        if (rigidbodies[i].bodyType == RigidbodyComponent::BodyType::Static)
                        {
                            continue;
                        }
                        RigidbodyStateComponent& bodyState = states[i];
                        const JPH::BodyLockRead bodyLock(locks, JPH::BodyID(bodyState.bodyId));
                        if (!bodyLock.Succeeded() || bodyLock.GetBody().GetUserData() != entities[i].id())
                        {
                            // copied with its entity: the body is not this entity's
                            commands.removeComponent<RigidbodyStateComponent>(entities[i]);
                            continue;
                        }
                        const JPH::Body& body = bodyLock.GetBody();
                        const bool awake = body.IsActive();
                        if (!awake && !bodyState.awake)
                        {
                            continue;
                        }
                        Vec3 position = fromJolt(body.GetPosition());
                        Quat rotation = fromJolt(body.GetRotation());
                        if (const Entity parent = parentOf(entities[i]); parent != Entity::undefined())
                        {
                            const Pose parentPose = worldPose(parent);
                            const Quat toLocal = inverse(parentPose.rotation);
                            const Vec3 local = rotate(toLocal, position - parentPose.position);
                            position = {local.x / parentPose.scale.x, local.y / parentPose.scale.y,
                                        local.z / parentPose.scale.z};
                            rotation = normalize(toLocal * rotation);
                        }
                        TransformComponent& transform = transforms[i];
                        if (transform.position != position || transform.rotation != rotation)
                        {
                            transform.position = position;
                            transform.rotation = rotation;
                            chunk->markModified<TransformComponent>(i);
                        }
                        bodyState.linearVelocity = fromJolt(body.GetLinearVelocity());
                        bodyState.angularVelocity = fromJolt(body.GetAngularVelocity());
                        bodyState.awake = awake;
                        chunk->markModified<RigidbodyStateComponent>(i);
                    }
                });
    }

    void stepPhysics(SystemContext& ctx)
    {
        ServiceLocator* services = ctx.tryGetServices();
        if (!services || !services->contains<PhysicsWorld>())
        {
            return;
        }
        services->write<PhysicsWorld>().get().step(ctx);
    }
} // namespace spite
