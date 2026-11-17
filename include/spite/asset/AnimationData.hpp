#pragma once

#include <cstddef>
#include <vector>

#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"

// Plain skeleton, clip and skin data, no third-party types: what the importer reads out of a rigged model, what the
// bake turns into bytes, and what the loaders hand back (the later SpiteAnimation module takes these through a
// loader callback; the ozz archives inside are opaque bytes to everything but that module's own ozz copy).
namespace spite
{
    struct JointTransform
    {
        Vec3 position{0.0f};
        Quat rotation = Quat::identity();
        Vec3 scale{1.0f};
    };

    struct RawJoint
    {
        InternedString name;
        i32 parent = -1; // index into RawSkeleton::joints, -1 for a root
        JointTransform rest; // local rest pose
        Mat4 inverseBind =
            Mat4::identity(); // model space to the joint's bind space; identity for a joint no mesh binds
    };

    // Depth-first order (children after their parent, siblings in order), so a parent's index is below its children's
    // and the ozz skeleton the bake builds numbers its joints the same way.
    struct RawSkeleton
    {
        std::vector<RawJoint> joints;
    };

    enum class CurveInterpolation : u8
    {
        Linear, // rotations are interpolated by slerp in the source
        Step,
        CubicSpline
    };

    // One animated component of one joint. times are seconds, strictly ascending; values hold 3 floats per key
    // (translation, scale) or 4 (rotation x y z w); a CubicSpline key holds in tangent, value, out tangent, in that
    // order, each of that size (glTF's layout). An empty curve means the clip does not animate the component.
    struct RawCurve
    {
        CurveInterpolation interpolation = CurveInterpolation::Linear;
        std::vector<f32> times;
        std::vector<f32> values;

        [[nodiscard]] bool empty() const { return times.empty(); }
    };

    struct RawJointCurves
    {
        RawCurve translation;
        RawCurve rotation;
        RawCurve scale;
    };

    struct RawClip
    {
        InternedString name;
        f32 duration = 0.0f; // the source's; the bake takes the later of it and the last key
        std::vector<RawJointCurves> joints; // one per RawSkeleton joint
    };

    // One vertex's skin: up to 4 influences, the palette index of the joint and its weight as unorm8 (the weights
    // sum to 255; a vertex with no influence is all zero and keeps its bind-pose position). Unused slots are 0, 0.
    struct SkinVertex
    {
        u8 joints[4] = {};
        u8 weights[4] = {};
    };
    static_assert(sizeof(SkinVertex) == 8, "SkinVertex is baked and uploaded as raw bytes");

    // A skinned submesh's stream, in the vertex order of the submesh's baked vertices.
    struct SkinStream
    {
        std::vector<u16> palette; // palette index -> skeleton joint index, ascending, at most 256
        std::vector<SkinVertex> vertices;
        // per palette entry: the box, in the joint's bind space (inverse bind times the vertex position), of the
        // vertices it weights; min above max when none does. Set by the bounds bake (SkinnedBounds.cpp).
        std::vector<BoundingBox> jointBounds;
    };

    struct BakedSkeleton
    {
        InternedString name;
        u32 ozzVersion = 0;
        u64 identity =
            0; // a hash of the joint names, parents and rest poses: a clip binds to the skeletons that share it
        std::vector<RawJoint> joints;
        std::vector<std::byte> ozz; // the ozz skeleton archive
    };

    struct BakedClip
    {
        InternedString name;
        InternedString skeleton; // the skeleton the clip was baked against
        u64 skeletonIdentity = 0;
        u32 ozzVersion = 0;
        f32 duration = 0.0f;
        u32 trackCount = 0; // one per skeleton joint
        std::vector<std::byte> ozz; // the ozz animation archive
    };

    // One skinned submesh of a model: its index in the model's submesh list and its stream.
    struct BakedSkinSubmesh
    {
        u32 submesh = 0;
        SkinStream stream;
    };

    struct BakedSkin
    {
        InternedString skeleton;
        u64 skeletonIdentity = 0;
        std::vector<BakedSkinSubmesh> submeshes;
    };
} // namespace spite
