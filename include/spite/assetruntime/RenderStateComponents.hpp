#pragma once

#include <memory>

#include "base/MathTypes.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include "rendering/RenderResourceHandles.hpp"

namespace spite
{
    struct OceanProfileRenderState : IDerivedComponent
    {
        u64 address = 0;
        u64 bytes = 0;
        BufferHandle buffer;
        float maxDisplacement = 0;
        SPITE_OBSERVED_SCHEMA(OceanProfileRenderState, address, bytes, maxDisplacement)
    };
    // Derived render state the asset runtime adds when it instantiates a renderable and the render runtime consumes.
    // They live here, below both, so the asset runtime includes nothing of the render runtime;
    // renderruntime/RenderingComponents.hpp still includes this header, so users of that path see them unchanged.

    // World-space AABB. The render runtime computes it each frame from the
    // asset-owned LocalBoundingBoxComponent and the entity transform; the
    // runtime's own systems (object upload, culling) consume it.
    struct WorldBoundingBoxComponent : IDerivedComponent
    {
        Vec3 min;
        Vec3 max;
        SPITE_OBSERVED_SCHEMA(WorldBoundingBoxComponent, min, max)
    };

    constexpr u32 INVALID_INSTANCE_SLOT = ~0u;

    // The renderable's stable index into the GPU object buffer, assigned in
    // place by the scene upload once the renderable is complete and held for
    // its lifetime. Content pipelines add it (INVALID) when they create the
    // renderable, so assignment never changes the archetype; removing it or
    // losing the meshlets frees the slot.
    struct GpuInstanceSlotComponent : IDerivedComponent
    {
        u32 slot = INVALID_INSTANCE_SLOT;
        SPITE_OBSERVED_SCHEMA(GpuInstanceSlotComponent, slot)
    };

    // A skinned renderable's joint palette in the JointPaletteBuffer, added in place by the scene
    // upload's skin step once the renderable's skin root has a pose, and held until the renderable goes. base is the
    // first joint of its block (the object record's paletteBase), count the joints (the submesh's palette size),
    // weightDelta its submesh's skin rows (the record's skinWeightDelta) and poseVersion the skin root's
    // SkinPaletteComponent::poseVersion the block was last written for: a renderable with no block draws its bind pose.
    // The block returns to the heap with the component.
    struct SkinPaletteBlockOwner;
    struct SkinPaletteBlockComponent : IDerivedComponent
    {
        u32 base = 0;
        u32 count = 0;
        u32 weightDelta = 0;
        u32 vertexCount = 0;
        u32 poseVersion = 0;
        // not 0: the block is the shared pose's of that id (AnimatorLod::sharedGroup), which every renderable of the
        // same submesh skin on an animator showing that pose points at; written once, never for one renderable
        u32 sharedGroup = 0;
        std::shared_ptr<SkinPaletteBlockOwner> owner;
        SPITE_OBSERVED_SCHEMA(SkinPaletteBlockComponent, SPITE_FIELD(base, OBSERVED | READONLY),
                              SPITE_FIELD(count, OBSERVED | READONLY), SPITE_FIELD(weightDelta, OBSERVED | READONLY),
                              SPITE_FIELD(vertexCount, OBSERVED | READONLY),
                              SPITE_FIELD(poseVersion, OBSERVED | READONLY),
                              SPITE_FIELD(sharedGroup, OBSERVED | READONLY))
    };
} // namespace spite
