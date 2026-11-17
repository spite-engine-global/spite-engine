#pragma once

#include <span>

#include "asset/AnimationData.hpp"
#include "asset/AssetExport.hpp"
#include "base/Expected.hpp"

// The baked skeleton (.spiteskel), clip (.spiteanim) and skin (.spiteskin) files: raw little-endian structs like
// the model file, no padding (static_asserts below), and one function per direction. SpiteAsset links the ozz
// offline builders; only these bytes leave it (the archives inside are ozz's own format).
namespace spite
{
    // The first asset version that has the three files. A skeleton and clip reader takes any version from here to
    // SPITE_ASSET_VERSION.
    constexpr u32 SPITE_ASSET_VERSION_ANIMATION = 7;
    // The first version whose skin file carries joint bounds, and whose skinned models hold bounds and meshlet spheres
    // that cover every pose of their clips and every blend of them with each other and the rest pose
    // and of a clip with itself at another time, up to MAX_BLEND_CLIP_LAYERS layers. A skin
    // file below it is refused: its model's bounds are the bind pose's or the clip poses' alone,
    // which would cull a posed or blending character, so it says to bake again.
    constexpr u32 SPITE_ASSET_VERSION_SKIN_BOUNDS = 10;

    // The most clip layers one blend of poses mixes at runtime (clips at any times, one clip more than once too; the
    // rest pose is one more layer): the two motions of a transition, each a blend tree with at most three children
    // playing (MAX_BLEND_TREE_ACTIVE in AnimationControllerAsset.hpp). The skinned bounds bake covers every blend of
    // this many layers; a runtime that mixes more must raise it and bake again.
    constexpr u32 MAX_BLEND_CLIP_LAYERS = 6;

    // What a model imported with usesLayers (ModelImportSettings) has bounds for beyond the clips' own blends
    // a layered pose takes each joint from any clip and adds at most LAYER_ADDITIVE_MAX additive layers,
    // each at a weight of at most LAYER_ADDITIVE_WEIGHT_MAX. A layered runtime must clamp to both, or raise them and
    // bake the models again.
    constexpr u32 LAYER_ADDITIVE_MAX = 2;
    constexpr f32 LAYER_ADDITIVE_WEIGHT_MAX = 1.0f;

    // The vendored ozz release (major << 16 | minor << 8 | patch) the archives inside the files were written by.
    // An ozz upgrade changes the archive format: it changes this, rebakes every skeleton and clip and bumps
    // SPITE_ASSET_VERSION (a static_assert in AnimationBake.cpp pins the archive versions this value stands for).
    constexpr u32 BAKED_OZZ_VERSION = (0u << 16) | (17u << 8) | 0u;

    // What a clip bake reads from the import settings.
    constexpr f32 DEFAULT_CLIP_SAMPLE_RATE = 60.0f;
    struct ClipBakeOptions
    {
        // Hz: CUBICSPLINE curves are resampled at this rate (ozz has no cubic interpolation)
        f32 sampleRate = DEFAULT_CLIP_SAMPLE_RATE;
        // scale tracks are dropped: every joint keeps its rest scale (a smaller clip when the rig never scales)
        bool stripScaleTracks = false;
    };

    struct SpiteSkeletonHeader
    {
        char magic[4] = {'S', 'P', 'S', 'K'};
        u32 version = 0;
        u32 ozzVersion = 0;
        u32 jointCount = 0;
        u64 identity = 0;
        u64 ozzSize = 0;
        u32 nameLength = 0; // the skeleton's name, right after the header
        u32 reserved = 0;
    };
    static_assert(sizeof(SpiteSkeletonHeader) == 4 + 5 * 4 + 2 * 8, "SpiteSkeletonHeader must have no padding");

    // then jointCount of these, the joint names (nameLength bytes each, in joint order), the ozz skeleton archive
    struct SpiteSkeletonJoint
    {
        i32 parent = -1;
        u32 nameLength = 0;
        JointTransform rest;
        f32 inverseBind[16] = {};
    };
    static_assert(sizeof(JointTransform) == 40);
    static_assert(sizeof(SpiteSkeletonJoint) == 2 * 4 + 40 + 16 * 4, "SpiteSkeletonJoint must have no padding");

    // header, the clip's name, the skeleton's name, the ozz animation archive
    struct SpiteClipHeader
    {
        char magic[4] = {'S', 'P', 'A', 'N'};
        u32 version = 0;
        u32 ozzVersion = 0;
        u32 trackCount = 0;
        f32 duration = 0.0f;
        u32 nameLength = 0;
        u32 skeletonNameLength = 0;
        u32 reserved = 0;
        u64 skeletonIdentity = 0;
        u64 ozzSize = 0;
    };
    static_assert(sizeof(SpiteClipHeader) == 4 + 7 * 4 + 2 * 8, "SpiteClipHeader must have no padding");

    // header, the skeleton's name, submeshCount of SpiteSkinSubmesh, then each submesh's palette (u16 joints), stream
    // (8 bytes per vertex, SkinVertex) and joint bounds (paletteSize boxes of 6 floats, min then max) at the absolute
    // offsets the table gives, each on a 4-byte boundary. The streams run in the vertex order of the same submesh in
    // the model file; a submesh without a skin has no entry.
    struct SpiteSkinHeader
    {
        char magic[4] = {'S', 'P', 'S', 'N'};
        u32 version = 0;
        u32 submeshCount = 0;
        u32 skeletonNameLength = 0;
        u64 skeletonIdentity = 0;
    };
    static_assert(sizeof(SpiteSkinHeader) == 4 + 3 * 4 + 8, "SpiteSkinHeader must have no padding");

    struct SpiteSkinSubmesh
    {
        u32 submesh = 0;
        u32 vertexCount = 0;
        u32 paletteSize = 0;
        u32 reserved = 0;
        u64 paletteOffset = 0;
        u64 streamOffset = 0;
        u64 boundsOffset = 0;
    };
    static_assert(sizeof(BoundingBox) == 6 * 4);
    static_assert(sizeof(SpiteSkinSubmesh) == 4 * 4 + 3 * 8, "SpiteSkinSubmesh must have no padding");

    // A hash of the joint count, names, parents and rest poses: two skeletons with one value are interchangeable for a
    // clip (the inverse binds belong to the skin, not to the identity).
    SPITE_ASSET_API u64 skeletonIdentity(const RawSkeleton& skeleton);

    // Builds the ozz skeleton of a RawSkeleton (depth-first, at most 1024 joints, unique non-empty names) and
    // writes the file. Deterministic: equal inputs give equal bytes in every configuration and on every thread.
    SPITE_ASSET_API Expected<std::vector<std::byte>> encodeSkeleton(const RawSkeleton& skeleton, InternedString name);

    // Bakes a clip against its skeleton into the file. Every joint gets all three tracks: a component the clip does
    // not animate holds the rest pose (an empty ozz track would sample identity). STEP curves are expanded with a key
    // just before the next, CUBICSPLINE curves are resampled at options.sampleRate, and a rotation step wider than
    // the nlerp of ozz can follow within 5e-5 radians is split by slerped keys.
    SPITE_ASSET_API Expected<std::vector<std::byte>> encodeClip(const RawClip& clip, const RawSkeleton& skeleton,
                                                                InternedString skeletonName,
                                                                const ClipBakeOptions& options = {});

    SPITE_ASSET_API std::vector<std::byte> encodeSkin(const BakedSkin& skin);

    // The readers check magic, version, the ozz release, every size against the bytes there are and, for a skeleton,
    // the joint order; a file that fails is an error naming what was wrong (a different ozz release says to rebake).
    SPITE_ASSET_API Expected<BakedSkeleton> decodeSkeleton(std::span<const std::byte> bytes);
    SPITE_ASSET_API Expected<BakedClip> decodeClip(std::span<const std::byte> bytes);
    SPITE_ASSET_API Expected<BakedSkin> decodeSkin(std::span<const std::byte> bytes);

    // A clip plays on the skeletons that share the identity and the joint count of the one it was baked against; any
    // other is an error that names the clip and both skeletons, and is logged (the visible failure).
    SPITE_ASSET_API Expected<void> checkClipSkeleton(const BakedClip& clip, const BakedSkeleton& skeleton);
    // the same check from what it reads of the skeleton, for a caller that kept those instead of the file
    SPITE_ASSET_API Expected<void> checkClipSkeleton(const BakedClip& clip, InternedString skeleton, u64 identity,
                                                     sizet jointCount);

    // A skin's palette indexes joints of its skeleton; same rule for the identity.
    SPITE_ASSET_API Expected<void> checkSkinSkeleton(const BakedSkin& skin, const BakedSkeleton& skeleton);

    // What the skinned bounds bake pads every pose with for blends, per joint: for any blend
    // of at most MAX_BLEND_CLIP_LAYERS layers of the clips' poses at any times, plus the rest pose, weights summing to
    // 1, the blended model matrix differs from the weights' mix of the layers' model matrices by at most `linear` (the
    // spectral norm of the 3x3 part) and `translation` (length). The ground-plane travel of a root joint is held at
    // the clip's first pose, as the bake holds it (up axis Y, no layered or IK flag). `reach` (the largest distance of
    // a skinned vertex from its joint in the joint's bind space) only steers which poses stand for the others.
    // Exposed so a test can hold the bound against an exact blend; the bake itself calls the same code.
    struct BlendStray
    {
        double linear = 0.0;
        double translation = 0.0;
    };
    SPITE_ASSET_API Expected<std::vector<BlendStray>> skinnedBlendStrays(const BakedSkeleton& skeleton,
                                                                         std::span<const BakedClip> clips,
                                                                         double reach);
} // namespace spite
