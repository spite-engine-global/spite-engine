#pragma once

#include "asset/AssetHandle.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    struct OceanProfileAssetBinding : IEngineComponent
    {
        AssetHandle profileHandle;
        SPITE_SCHEMA(OceanProfileAssetBinding, SPITE_FIELD(profileHandle, READONLY | fieldAssetType("OCEAN_PROFILE")))
    };

    // Bindings name an asset by handle; they never hold GPU data or registry
    // pointers. Bindings are immutable: a change is remove+add, so the asset
    // module's lifecycle hooks rehydrate every path the same way (scene
    // deserialization, instantiation, editor edits).
    struct MeshComponent : IEngineComponent
    {
        AssetHandle meshHandle;
        SPITE_SCHEMA(MeshComponent, SPITE_FIELD(meshHandle, READONLY | fieldAssetType("MESH")))
    };

    struct MaterialComponent : IEngineComponent
    {
        AssetHandle materialHandle;
        SPITE_SCHEMA(MaterialComponent, SPITE_FIELD(materialHandle, READONLY | fieldAssetType("MATERIAL")))
    };

    // Provenance and the last imported values are persisted independently of document-operation IDs.
    struct ImportedSourceComponent : IEngineComponent
    {
        InternedString document;
        InternedString object;
        InternedString parentObject;
        InternedString sourceName;
        InternedString instanceKey;
        AssetHandle sourceMesh;
        AssetHandle sourceMaterial;
        Vec3 sourcePosition{};
        Quat sourceRotation = Quat::identity();
        Vec3 sourceScale{1.f};
        u32 sourceCamera = 0;
        u32 sourceLight = 0;
        float sourceFov = 0.f, sourceNear = 0.f, sourceFar = 0.f;
        Vec3 sourceColor{};
        float sourceIntensity = 0.f, sourceRange = 0.f, sourceInnerCone = 0.f, sourceOuterCone = 0.f;
        bool stable = false;
        bool instanceRoot = false;
        bool generatedMesh = false;
        SPITE_SCHEMA(ImportedSourceComponent, SPITE_FIELD(document, READONLY), SPITE_FIELD(object, READONLY),
                     SPITE_FIELD(parentObject, READONLY), SPITE_FIELD(sourceName, READONLY),
                     SPITE_FIELD(instanceKey, READONLY), SPITE_FIELD(sourceMesh, READONLY),
                     SPITE_FIELD(sourceMaterial, READONLY), SPITE_FIELD(sourcePosition, READONLY),
                     SPITE_FIELD(sourceRotation, READONLY), SPITE_FIELD(sourceScale, READONLY),
                     SPITE_FIELD(sourceCamera, READONLY), SPITE_FIELD(sourceLight, READONLY),
                     SPITE_FIELD(sourceFov, READONLY), SPITE_FIELD(sourceNear, READONLY),
                     SPITE_FIELD(sourceFar, READONLY), SPITE_FIELD(sourceColor, READONLY),
                     SPITE_FIELD(sourceIntensity, READONLY), SPITE_FIELD(sourceRange, READONLY),
                     SPITE_FIELD(sourceInnerCone, READONLY), SPITE_FIELD(sourceOuterCone, READONLY),
                     SPITE_FIELD(stable, READONLY), SPITE_FIELD(instanceRoot, READONLY),
                     SPITE_FIELD(generatedMesh, READONLY))
    };
} // namespace spite
