#pragma once
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/MathTypes.hpp"
#include "engine/EngineToolsExport.hpp"

namespace spite
{
    // The mesh primitives the createPrimitive command spawns: generated here, written as a GLB under the import root
    // and imported like any model. Each fits a unit box (the capsule is two tall), is centred on the origin and has
    // counter-clockwise outward faces with smooth or flat normals as the shape asks.
    struct PrimitiveMesh
    {
        std::vector<Vec3> positions, normals;
        std::vector<Vec2> uvs;
        std::vector<u16> indices;
    };

    // cube, sphere, plane (horizontal, faces +y), quad (vertical, faces +z), cylinder, cone, capsule, torus
    SPITE_ENGINE_TOOLS_API std::span<const std::string_view> primitiveKinds();
    // the kind capitalised: the name a spawned primitive's entity gets
    SPITE_ENGINE_TOOLS_API std::string primitiveLabel(std::string_view kind);
    // the empty mesh for an unknown kind
    SPITE_ENGINE_TOOLS_API PrimitiveMesh buildPrimitive(std::string_view kind);
    // a binary glTF of the primitive with one grey material, named for the kind; empty for an unknown kind
    SPITE_ENGINE_TOOLS_API std::vector<std::byte> primitiveGlb(std::string_view kind);
} // namespace spite
