#pragma once

#include <cmath>

#include "base/MathTypes.hpp"
#include "base/Schema.hpp"

namespace spite
{
    // Simulation tuning of one PhysicsWorld; the defaults are the simulation's own.
    struct PhysicsSettings
    {
        struct BodyPairCache
        {
            i32 maxInFlightBodyPairs = 16384;
            float maxDeltaPositionSq = 0.000001f;
            float cosMaxDeltaRotationDiv2 = 0.9998477f;
            bool useBodyPairContactCache = true;
            SPITE_SCHEMA(BodyPairCache, maxInFlightBodyPairs, maxDeltaPositionSq, cosMaxDeltaRotationDiv2,
                         useBodyPairContactCache)
        };

        struct Contact
        {
            float speculativeContactDistance = 0.02f;
            float penetrationSlop = 0.02f;
            float manifoldTolerance = 0.001f;
            float maxPenetrationDistance = 0.2f;
            float contactNormalCosMaxDeltaRotation = 0.9961947f;
            float contactPointPreserveLambdaMaxDistSq = 0.0001f;
            SPITE_SCHEMA(Contact, speculativeContactDistance, penetrationSlop, manifoldTolerance,
                         maxPenetrationDistance, contactNormalCosMaxDeltaRotation, contactPointPreserveLambdaMaxDistSq)
        };

        struct Solver
        {
            u32 numVelocitySteps = 10;
            u32 numPositionSteps = 2;
            float baumgarte = 0.2f;
            SPITE_SCHEMA(Solver, numVelocitySteps, numPositionSteps, baumgarte)
        };

        struct Sleeping
        {
            float timeBeforeSleep = 0.5f;
            float pointVelocitySleepThreshold = 0.03f;
            bool allowSleeping = true;
            SPITE_SCHEMA(Sleeping, timeBeforeSleep, pointVelocitySleepThreshold, allowSleeping)
        };

        struct CollisionDetection
        {
            bool checkActiveEdges = true;
            bool useManifoldReduction = true;
            SPITE_SCHEMA(CollisionDetection, checkActiveEdges, useManifoldReduction)
        };

        struct Ccd
        {
            float linearCastThreshold = 0.75f;
            float linearCastMaxPenetration = 0.25f;
            SPITE_SCHEMA(Ccd, linearCastThreshold, linearCastMaxPenetration)
        };

        struct Behavior
        {
            float minVelocityForRestitution = 1.0f;
            bool deterministicSimulation = true;
            bool constraintWarmStart = true;
            bool useLargeIslandSplitter = true;
            i32 stepListenersBatchSize = 8;
            SPITE_SCHEMA(Behavior, minVelocityForRestitution, deterministicSimulation, constraintWarmStart,
                         useLargeIslandSplitter, stepListenersBatchSize)
        };

        BodyPairCache bodyPairCache;
        Contact contact;
        Solver solver;
        Sleeping sleeping;
        CollisionDetection collisionDetection;
        Ccd ccd;
        Behavior behavior;
        Vec3 gravity = {0.0f, -9.81f, 0.0f};

        // What a file or a command may set (valid(); the config field table declares the same bounds): the
        // simulation's own domains, with the counts that size memory or loops and every float bounded so a value
        // cannot poison broad-phase boxes or velocities.
        //
        // Every in-flight pair is 8 bytes. Bound the per-job queue to 512 KiB; larger scene constraint buffers use
        // PhysicsWorld's temporary allocator fallback. Jolt's own default is 16384 pairs.
        static constexpr i32 MAX_IN_FLIGHT_BODY_PAIRS = 1 << 16;
        static constexpr u32 MAX_SOLVER_STEPS = 255; // Jolt keeps a step count in a byte and asserts below 256
        static constexpr i32 MAX_STEP_LISTENER_BATCH = 1024;
        static constexpr float MAX_DISTANCE = 1.0f; // m and m^2: contact distances, tolerances, cache movement
        static constexpr float MAX_PENETRATION_DISTANCE = 10.0f; // m
        static constexpr float MAX_SLEEP_TIME = 3600.0f; // s
        static constexpr float MAX_SPEED = 1000.0f; // m/s sleep and restitution thresholds; m/s^2 per gravity axis
        static constexpr float MAX_CAST_THRESHOLD = 100.0f; // share of the inner radius
        static constexpr float MAX_CAST_PENETRATION = 1.0f; // share of the inner radius

        [[nodiscard]] bool valid() const
        {
            // Release builds with /fp:fast, where comparisons with a NaN are not reliably false: finite is explicit
            const auto within = [](const float value, const float max)
            { return std::isfinite(value) && value >= 0.0f && value <= max; };
            const auto cosine = [](const float value)
            { return std::isfinite(value) && value >= -1.0f && value <= 1.0f; };
            const auto gravityAxis = [](const float value)
            { return std::isfinite(value) && std::abs(value) <= MAX_SPEED; };
            return bodyPairCache.maxInFlightBodyPairs >= 1 &&
                bodyPairCache.maxInFlightBodyPairs <= MAX_IN_FLIGHT_BODY_PAIRS &&
                within(bodyPairCache.maxDeltaPositionSq, MAX_DISTANCE) &&
                cosine(bodyPairCache.cosMaxDeltaRotationDiv2) &&
                within(contact.speculativeContactDistance, MAX_DISTANCE) &&
                within(contact.penetrationSlop, MAX_DISTANCE) && within(contact.manifoldTolerance, MAX_DISTANCE) &&
                within(contact.maxPenetrationDistance, MAX_PENETRATION_DISTANCE) &&
                cosine(contact.contactNormalCosMaxDeltaRotation) &&
                within(contact.contactPointPreserveLambdaMaxDistSq, MAX_DISTANCE) && solver.numVelocitySteps >= 1 &&
                solver.numVelocitySteps <= MAX_SOLVER_STEPS && solver.numPositionSteps <= MAX_SOLVER_STEPS &&
                within(solver.baumgarte, 1.0f) && within(sleeping.timeBeforeSleep, MAX_SLEEP_TIME) &&
                within(sleeping.pointVelocitySleepThreshold, MAX_SPEED) &&
                within(ccd.linearCastThreshold, MAX_CAST_THRESHOLD) &&
                within(ccd.linearCastMaxPenetration, MAX_CAST_PENETRATION) &&
                within(behavior.minVelocityForRestitution, MAX_SPEED) && behavior.stepListenersBatchSize >= 1 &&
                behavior.stepListenersBatchSize <= MAX_STEP_LISTENER_BATCH && gravityAxis(gravity.x) &&
                gravityAxis(gravity.y) && gravityAxis(gravity.z);
        }

        SPITE_SCHEMA(PhysicsSettings, bodyPairCache, contact, solver, sleeping, collisionDetection, ccd, behavior,
                     gravity)
    };
} // namespace spite
