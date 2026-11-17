#pragma once

#include "asset/AnimationData.hpp"
#include "asset/BakedAssetFormat.hpp"
#include "asset/AssetManifest.hpp"
#include "asset/BakedAnimation.hpp"
#include "asset/AssetExport.hpp"
#include "asset/EnvironmentAsset.hpp"
#include "base/Expected.hpp"
#include "base/InternedString.hpp"
#include "asset/WriteBakedFunctions.hpp"

namespace spite
{
    struct ParsedModelData;

    // Validate a texture header, exact decoded mip size and storage checksum without retaining decoded pixels.
    SPITE_ASSET_API Expected<void> validateBakedTexture(const InternedString& textureName,
                                                        const AssetManifest& manifest);

    // Load texture by NAME (resolves path via manifest): every mip level of the bake, filtered for the space the
    // texture is bound in (a chain baked for the other kind of slot is rebuilt from level 0). A file that is
    // truncated, or whose header names a chain its bytes do not hold, is an error.
    SPITE_ASSET_API Expected<std::shared_ptr<TextureAsset>> loadBakedTexture(
        const InternedString& textureName, const AssetManifest& manifest,
        TextureColorSpace space = TextureColorSpace::SRGB);

    // Load environment by NAME (resolves path via manifest): the whole baked chain. A file that is truncated or
    // whose header names a chain its bytes do not hold is an error.
    SPITE_ASSET_API Expected<std::shared_ptr<EnvironmentAsset>> loadBakedEnvironment(const InternedString& name,
                                                                                     const AssetManifest& manifest);

    // Load material by NAME (resolves path via manifest, loads textures by name)
    SPITE_ASSET_API Expected<std::shared_ptr<ParsedMaterialData>> loadBakedMaterial(const InternedString& materialName,
                                                                                    const AssetManifest& manifest);

    // Lightweight read of material descriptor only (no texture pixel data loading).
    // Returns the factors, texture names and colour spaces stored in the .spitemat file. A legacy (untagged) file
    // is an error: the project load migrates them (AssetBaker::initialize).
    SPITE_ASSET_API Expected<BakedMaterialWriteData> readBakedMaterialDescriptor(const InternedString& materialName,
                                                                                 const AssetManifest& manifest);

    // Validate all model metadata and geometry ranges and indices before loading or recovery.
    SPITE_ASSET_API Expected<void> validateBakedModelGeometry(const InternedString& modelName,
                                                              const AssetManifest& manifest);

    // Load model by NAME (resolves all paths via manifest)
    SPITE_ASSET_API Expected<std::shared_ptr<ParsedModelData>> loadBakedModelPackage(const InternedString& modelName,
                                                                                     const AssetManifest& manifest);

    // The baked skeleton, clip and skin by NAME (resolved through the manifest): plain data, the ozz archives inside as
    // bytes. What the later SpiteAnimation module takes through a loader callback; nothing here builds an ozz object.
    // A file that is truncated, of another version or ozz release, or whose header lies about its sizes is an error.
    SPITE_ASSET_API Expected<BakedSkeleton> loadBakedSkeleton(const InternedString& name,
                                                              const AssetManifest& manifest);
    // The clip is checked against the skeleton named by its entry: a clip whose skeleton is gone or no longer matches
    // is an error naming both (checkClipSkeleton).
    SPITE_ASSET_API Expected<BakedClip> loadBakedClip(const InternedString& name, const AssetManifest& manifest);
    // The per-vertex skin of a MODEL (the .spiteskin its manifest entry names); an error for a model without one.
    SPITE_ASSET_API Expected<BakedSkin> loadBakedSkin(const InternedString& modelName, const AssetManifest& manifest);

    // Per-submesh entry of the model hierarchy descriptor: identity and bounds
    // only — no geometry.
    struct ModelSubmeshDescriptor
    {
        InternedString name;
        BoundingBox localBounds;
        u32 materialIndex = 0;
    };

    // The .spiteasset file's metadata section: hierarchy, per-submesh identity
    // and the model's material names. Everything instantiation and the asset
    // browser need, decodable without touching any vertex/meshlet blob.
    struct ModelHierarchyDescriptor
    {
        InternedString name;
        InternedString sourceDocument;
        std::vector<ProcessedNode> nodes;
        u32 rootNodeIndex = 0;
        std::vector<ModelSubmeshDescriptor> submeshes;
        std::vector<InternedString> materialNames;
    };

    // Reads only the metadata ranges of the model bake file (header, TOC, node
    // tables, string table) with ranged reads — geometry blobs are never
    // touched. Fails for files missing the metadata section.
    SPITE_ASSET_API Expected<std::shared_ptr<ModelHierarchyDescriptor>> loadBakedModelDescriptor(
        const InternedString& modelName, const AssetManifest& manifest);
} // namespace spite
