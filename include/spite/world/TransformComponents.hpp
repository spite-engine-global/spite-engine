#pragma once

#include "base/MathFunctions.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    struct TransformComponent : IEngineComponent
    {
        Vec3 position = {0.0f, 0.0f, 0.0f};
        Quat rotation = Quat::identity();
        Vec3 scale = {1.0f, 1.0f, 1.0f};
        SPITE_SCHEMA(TransformComponent, position, rotation, scale)
    };

    // Root motion that something other than the transform consumes (a character controller). An entity with this
    // component and an AnimationPlayerComponent with rootMotion set gets its root motion added here by AnimationAdvance
    // instead of written to its TransformComponent: delta is the displacement in the entity's parent space (world
    // space for a root entity) that the animation covered in `seconds` of simulated time since the consumer last took
    // it. The consumer takes both and sets them to zero (desired velocity = delta / seconds); seconds 0 means the
    // animator stood still. A consumer adds the component when it adopts an entity and removes it to give the
    // transform its root motion back. Never saved.
    struct RootMotionDeltaComponent : IDerivedComponent
    {
        Vec3 delta = {0.0f, 0.0f, 0.0f};
        f32 seconds = 0.0f;
        SPITE_OBSERVED_SCHEMA(RootMotionDeltaComponent, delta, seconds)
    };

    // Derived from the transform hierarchy each frame.
    struct TransformMatrixComponent : IDerivedComponent
    {
        Mat4 matrix = Mat4(1.0f);
        SPITE_OBSERVED_SCHEMA(TransformMatrixComponent, matrix)
    };
} // namespace spite
