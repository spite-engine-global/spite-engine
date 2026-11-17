#pragma once
#include <span>

#include "asset/AssetExport.hpp"
#include "base/MathTypes.hpp"
#include "rendering/GpuSharedTypes.hpp"

namespace spite
{
    // The tangent frame of a vertex is the vertex normal, one tangent perpendicular to it and a
    // handedness: the bitangent is handedness * cross(normal, tangent), the direction the surface point moves as v
    // grows (the stored uv, so the V flip of the source is already in it). Vertex::packedTangent holds the tangent as
    // an octahedral direction in two 15-bit snorm fields (bits 0-14 and 15-29) and the handedness in bit 31 (set:
    // negative).
    constexpr u32 TANGENT_HANDEDNESS_BIT = 0x80000000u;

    // The unit tangent and handedness (+1 or -1) a packed tangent holds. Mirrors unpackTangent in
    // shared/bindless.slang.
    SPITE_ASSET_API Vec3 unpackTangent(u32 packed, f32& handedness);

    // The packed form of a tangent (any non-zero length) and a handedness (negative sets the bit).
    SPITE_ASSET_API u32 packTangent(const Vec3& tangent, f32 handedness);

    // Sets the handedness bit of every corner of an unindexed triangle list (three vertices a triangle, each with its
    // own copy of position, normal and uv) from its triangle's uvs: the sign of dot(cross(normal, tangent), dP/dv). The
    // importer does this before it welds equal corners, so the two sides of a mirrored uv seam, which share position,
    // normal and uv, stay apart (MikkTSpace never merges corners of opposite handedness either). A triangle with no uv
    // area leaves its corners positive.
    SPITE_ASSET_API void markCornerHandedness(std::span<Vertex> corners);

    // Fills Vertex::packedTangent of every vertex from the triangle list (three indices a triangle) over the vertices'
    // positions, normals and uvs: each triangle adds its uv-aligned tangent, weighted by its angle at the vertex, to
    // the vertices it uses, so a shared vertex gets one smooth frame. The sum is made perpendicular to the vertex
    // normal; a vertex no triangle gives a direction (zero uv area) or whose sum cancels gets a fixed perpendicular, so
    // the frame is always finite. A vertex whose packedTangent already holds a direction (low 30 bits not zero: a glTF
    // TANGENT stream) keeps it as the direction; only the handedness comes from the uvs. The result depends on the
    // input only.
    SPITE_ASSET_API void generateVertexTangents(std::span<Vertex> vertices, std::span<const u32> triangleIndices);
} // namespace spite
