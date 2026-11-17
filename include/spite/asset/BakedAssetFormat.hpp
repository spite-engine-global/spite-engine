#pragma once

#include "asset/AssetData.hpp"
#include "asset/TextureMips.hpp"
#include "base/MathTypes.hpp"

namespace spite
{
    // Bumped whenever the .spiteasset layout or what a bake means changes; loaders reject other versions (2: meshlet
    // bounds and normal cones; 3: cluster LOD, meshlets carry errors and a level, the level-0 meshlets come first; 4:
    // the vertex is 24 bytes, with a packed tangent frame; 5: the cluster LOD of a model of several submeshes keeps
    // the seams between them closed; 6: a submesh's TOC entry carries the uv range its vertices' unorm uvs span,
    // UvRange; 7: skeleton, clip and skin files (BakedAnimation.hpp) with the ozz release in their headers; a model
    // file keeps the layout of 6, so a static model bakes to the bytes of 6 apart from this field; 8: a skinned
    // submesh's bounds and meshlet spheres cover every pose of the model's clips and its meshlet cones are off, and
    // the skin file carries joint bounds; 9: the bounds and spheres of a skinned submesh also cover every blend of its
    // clips and the rest pose (a crossfade, a weight below 1): only the skin file refuses an older version, a model
    // file of 7 or 8 loads as it is, with the bounds of the bind pose or of the clip poses alone, and a skinned one
    // wants a re-bake); 10: the bounds and spheres of a skinned submesh also cover the blends of a clip with itself
    // at another time and of up to MAX_BLEND_CLIP_LAYERS clips (a blend tree, a transition between two): only the
    // skin file refuses an older version, as with 9; 11: the cluster LOD of a skinned submesh weighs how far its skin
    // moves each vertex when the joints bend: a file of 10 loads as it is, its bent limbs collapsing at
    // level 1 and above, so a skinned model wants a re-bake); 12: the cluster LOD locks every vertex on a uv,
    // handedness or normal seam of a flat surface: a file of 11 loads as it is, with such seams
    // collapsing, so a model with them wants a re-bake). A version-2 file still loads, and so does every version below
    // 5: its meshlets are all level 0 (a version-3 or 4 hierarchy may crack at a material seam; re-bake them), and a
    // file of version 3 or lower holds 20-byte vertices, which load with their tangents generated from the uvs. A file
    // below 6 has the range [0, 1]. An ozz upgrade bumps this too: the archives in the skeleton and clip files change
    // with it.
    // Version 13 appends optional imported-node properties after the string table when header.reserved is one.
    // Version 14 stores a source-identity string-table offset in each node's reserved word.
    // Version 15 matches cross-instance LOD seams in model space, within float roundoff; older files still load,
    // rebake for the fix.
    // Version 16 keeps each group's outline in place in the cluster LOD's position-only fallback; older files still
    // load, rebake for the fix.
    // Version 17 places a skinned submesh at the root and each repeated placement once when matching the version-15
    // seams; older files still load, rebake for the fix.
    constexpr u32 SPITE_ASSET_VERSION = 17;
    constexpr u32 SPITE_ASSET_VERSION_NO_UV_RANGE = 5;
    constexpr u32 SPITE_ASSET_VERSION_NO_SEAM_LOCKS = 4;
    constexpr u32 SPITE_ASSET_VERSION_NO_TANGENTS = 3;
    constexpr u32 SPITE_ASSET_VERSION_NO_LOD = 2;

    // Baked structs are written as raw bytes, so none may have compiler padding (indeterminate bytes would make
    // equal sources bake to different files): each names its alignment gap as a zeroed reserved field, and a
    // static_assert on its size fails when a new field reintroduces padding.
    struct SpiteAssetHeader
    {
        char magic[4] = {'S', 'P', 'A', 'S'};
        u32 version = SPITE_ASSET_VERSION;
        u32 subAssetCount = 0;
        u32 nodeCount = 0;
        u32 rootNodeIndex = 0;
        u32 reserved = 0;

        u64 nodeMeshIndicesOffset = 0;
        u64 nodeMeshIndicesSize = 0;
        u64 nodeChildrenIndicesOffset = 0;
        u64 nodeChildrenIndicesSize = 0;

        u64 stringTableOffset = 0;
        u64 stringTableSize = 0;
    };
    static_assert(sizeof(SpiteAssetHeader) == 4 + 5 * 4 + 6 * 8, "SpiteAssetHeader must have no padding");

    struct SpiteAssetTOCEntry
    {
        u32 subAssetId = 0;
        BoundingBox localBounds;

        u32 materialIndex = 0;
        u32 meshletCount = 0;
        u32 uvOffset = 0; // UvRange::packedOffset (version 6; zero before: the range [0, 1])
        u64 meshletsDataOffset = 0;

        u64 vertexDataOffset = 0;
        u64 vertexDataSize = 0;
        u64 meshletVertexDataOffset = 0;
        u64 meshletVertexDataSize = 0;
        u64 meshletTriangleDataOffset = 0;
        u64 meshletTriangleDataSize = 0;

        u64 nameOffset = 0;
        u32 nameLength = 0;
        u32 uvExtent = 0; // UvRange::packedExtent
    };
    static_assert(sizeof(SpiteAssetTOCEntry) == 4 + sizeof(BoundingBox) + 3 * 4 + 8 * 8 + 2 * 4,
                  "SpiteAssetTOCEntry must have no padding");

    struct SpiteMaterialData
    {
        Vec4 albedoFactor;
        InternedString albedoTextureName;
        InternedString normalTextureName;
        InternedString metallicRoughnessTextureName;
    };

    // The .spitetex layout: this header, then `mipLevels` tightly packed levels, largest first, down to the extent the
    // header's width and height give each level (mipExtent), each level in the header's format. 2: the chain and
    // mipFilter (the version 1 byte was reserved zero, so a version 1 file reads as one level filtered as SRGB and
    // still loads). 3: the format byte names the level format, RGBA8 or a BC format of bakedTextureFormatKnown, each
    // level whole 4x4 blocks (bakedTextureLevelBytes); a file below 3 is RGBA8 whatever its format byte says.
    // 4: storage is 0 (raw) or 1 (zlib); the decoded size is derived from the format, extent and mip count.
    constexpr u32 SPITE_TEXTURE_VERSION = 4;

    struct SpiteTextureHeader
    {
        char magic[4] = {'S', 'P', 'T', 'X'};
        u32 version = SPITE_TEXTURE_VERSION;
        u8 format = BAKED_TEXTURE_FORMAT_R8G8B8A8_SRGB;
        u8 mipFilter = static_cast<u8>(BakedTextureFilter::SRGB); // how the levels past the first were averaged
        u8 storage = 0;
        u8 reserved = 0;
        u32 width;
        u32 height;
        u32 mipLevels;
    };
    static_assert(sizeof(SpiteTextureHeader) == 4 + 4 + 4 + 3 * 4, "SpiteTextureHeader must have no padding");

    // The format of the levels a header describes: the format byte of a version 3 file, RGBA8 below it.
    inline u8 bakedTextureFormat(const SpiteTextureHeader& header)
    {
        return header.version >= 3 ? header.format : BAKED_TEXTURE_FORMAT_R8G8B8A8_SRGB;
    }

    // The level count a texture file's header promises, 0 when it cannot describe a chain: a version this build does
    // not know, a zero extent, more levels than the extent has, an unknown filter or format. A version 1 file holds one
    // level. Readers size every buffer from this, never from the raw mipLevels field of an untrusted file.
    inline u32 bakedTextureMipLevels(const SpiteTextureHeader& header)
    {
        const u32 levels = header.version == 1                               ? 1
            : header.version >= 2 && header.version <= SPITE_TEXTURE_VERSION ? header.mipLevels
                                                                             : 0;
        const bool valid = header.width != 0 && header.height != 0 && levels != 0 &&
            levels <= textureMipCount(header.width, header.height) &&
            header.mipFilter <= static_cast<u8>(BakedTextureFilter::SRGB_ALPHA) &&
            bakedTextureFormatKnown(bakedTextureFormat(header));
        return valid ? levels : 0;
    }

    struct SerializedNode
    {
        TransformData localTransform;
        u32 meshIndexCount;
        u32 meshIndicesOffset;
        u32 childrenCount;
        u32 childrenIndicesOffset;

        u64 nameOffset;
        u32 nameLength;
        u32 reserved = 0;
    };
    static_assert(sizeof(SerializedNode) == sizeof(TransformData) + 4 * 4 + 8 + 4 + 4,
                  "SerializedNode must have no padding");

} // namespace spite
