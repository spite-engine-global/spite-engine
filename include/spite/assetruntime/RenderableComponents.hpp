#pragma once

#include "asset/GpuAssetFactory.hpp"
#include "base/MathTypes.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include "rendering/GpuDataTypes.hpp"

namespace spite
{
    // The renderable's meshlet block, written only by the asset module: one
    // block per (submesh, material), shared by every renderable bound to that
    // pair (GpuAssetFactory::acquireMeshlets). Move-only: the module derives
    // it from the bindings; rebind or clear the entity instead of copying it.
    struct MeshletComponent : IDerivedComponent
    {
        std::shared_ptr<MeshletData> data;

        MeshletComponent() = default;
        explicit MeshletComponent(std::shared_ptr<MeshletData> meshletData) : data(std::move(meshletData)) {}
        MeshletComponent(const MeshletComponent&) = delete;
        MeshletComponent& operator=(const MeshletComponent&) = delete;
        MeshletComponent(MeshletComponent&&) = default;
        MeshletComponent& operator=(MeshletComponent&&) = default;

        SPITE_OBSERVED_SCHEMA(MeshletComponent)
    };

    // A component holding the static, per-object information needed for rendering.
    // This is attached to an entity after its meshlets have been processed.
    struct ObjectStaticInfoComponent : IDerivedComponent
    {
        u32 firstMeshlet;
        u32 meshletCount; // level 0: what the object draws with cluster LOD off
        // the coarser clusters that follow them in the block (the cull emits them while LOD is on)
        u32 lodMeshletCount = 0;
        // Triangle id this entity's meshlet block starts at; lets CPU
        // consumers (validation oracle) resolve triangle ids.
        u32 firstTriangle = 0;
        // the submesh's uv range as UvRange packs it (zero: [0, 1]); the scene upload carries it to the object record
        u32 packedUvOffset = 0;
        u32 packedUvExtent = 0;
        // the id of the material the meshlet block was baked for (its GpuMaterial row): the shadow caster tracker finds
        // the casters of a material whose edit changes what it casts
        u32 materialId = 0;

        u32 totalMeshlets() const { return meshletCount + lodMeshletCount; }
        // baked state the asset module owns; the cached meshlet total follows its adds and removes, and the one
        // in-place write (a reimport swapping the geometry, AssetRuntime::refreshInstances) adjusts the total by the
        // difference
        SPITE_OBSERVED_SCHEMA(ObjectStaticInfoComponent, SPITE_FIELD(firstMeshlet, OBSERVED | READONLY),
                              SPITE_FIELD(meshletCount, OBSERVED | READONLY),
                              SPITE_FIELD(lodMeshletCount, OBSERVED | READONLY),
                              SPITE_FIELD(firstTriangle, OBSERVED | READONLY),
                              SPITE_FIELD(packedUvOffset, OBSERVED | READONLY),
                              SPITE_FIELD(packedUvExtent, OBSERVED | READONLY),
                              SPITE_FIELD(materialId, OBSERVED | READONLY))
    };

    // A renderable of a skinned submesh, derived with its ObjectStaticInfoComponent: the submesh's skin
    // rows and palette map. The render runtime gives it a palette block and draws it skinned once its skin root has a
    // pose; without one the renderable draws its bind pose.
    struct SkinnedRenderableComponent : IDerivedComponent
    {
        std::shared_ptr<const SubmeshSkin> skin;
        SPITE_OBSERVED_SCHEMA(SkinnedRenderableComponent)
    };

    // Derived renderable state: written by the asset module from the mesh
    // binding's sub-asset bounds; never serialized.
    struct LocalBoundingBoxComponent : IDerivedComponent
    {
        Vec3 min;
        Vec3 max;
        SPITE_OBSERVED_SCHEMA(LocalBoundingBoxComponent, min, max)
    };
} // namespace spite
