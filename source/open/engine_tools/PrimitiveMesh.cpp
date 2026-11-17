#include "engine/PrimitiveMesh.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <nlohmann/json.hpp>

namespace spite
{
    namespace
    {
        constexpr std::array<std::string_view, 8> KINDS{"cube",     "sphere", "plane",   "quad",
                                                        "cylinder", "cone",   "capsule", "torus"};
        constexpr u32 SEGMENTS = 32;
        constexpr f32 PI = 3.14159265358979f;
        constexpr f32 HALF = 0.5f;

        // One point of a revolved profile: its radius and height, its outward normal in the (radius, height) plane.
        struct Ring
        {
            f32 radius, y, normalRadius, normalY;
        };

        // A face given by its normal and two tangents with u x v = normal, so the corners run counter-clockwise
        void addFace(PrimitiveMesh& mesh, const Vec3& normal, const Vec3& u, const Vec3& v, const f32 half)
        {
            const u16 first = static_cast<u16>(mesh.positions.size());
            const Vec3 centre = normal * half;
            const std::array<Vec3, 4> corners{centre - u * half - v * half, centre + u * half - v * half,
                                              centre + u * half + v * half, centre - u * half + v * half};
            const std::array<Vec2, 4> uvs{Vec2(0.0f, 1.0f), Vec2(1.0f, 1.0f), Vec2(1.0f, 0.0f), Vec2(0.0f, 0.0f)};
            for (u32 i = 0; i < 4; ++i)
            {
                mesh.positions.push_back(corners[i]);
                mesh.normals.push_back(normal);
                mesh.uvs.push_back(uvs[i]);
            }
            for (const u16 i : {0, 1, 2, 0, 2, 3})
                mesh.indices.push_back(static_cast<u16>(first + i));
        }

        // Revolves each strip about y, from its first ring to its last: with the strip running bottom to top on the
        // outside the faces look outward. A ring of radius 0 is a pole, whose degenerate triangle is dropped.
        void revolve(PrimitiveMesh& mesh, const std::vector<std::vector<Ring>>& strips)
        {
            for (const std::vector<Ring>& strip : strips)
            {
                const u16 first = static_cast<u16>(mesh.positions.size());
                const u32 stride = SEGMENTS + 1;
                for (u32 i = 0; i < strip.size(); ++i)
                    for (u32 s = 0; s <= SEGMENTS; ++s)
                    {
                        const f32 phi = 2.0f * PI * static_cast<f32>(s) / SEGMENTS;
                        const f32 c = std::cos(phi), n = std::sin(phi);
                        const Ring& ring = strip[i];
                        mesh.positions.emplace_back(ring.radius * c, ring.y, ring.radius * n);
                        mesh.normals.emplace_back(ring.normalRadius * c, ring.normalY, ring.normalRadius * n);
                        mesh.uvs.emplace_back(1.0f - static_cast<f32>(s) / SEGMENTS,
                                              1.0f - static_cast<f32>(i) / static_cast<f32>(strip.size() - 1));
                    }
                for (u32 i = 0; i + 1 < strip.size(); ++i)
                    for (u32 s = 0; s < SEGMENTS; ++s)
                    {
                        const auto a = static_cast<u16>(first + i * stride + s), b = static_cast<u16>(a + stride),
                                   c = static_cast<u16>(b + 1), d = static_cast<u16>(a + 1);
                        if (strip[i + 1].radius > 0.0f)
                            mesh.indices.insert(mesh.indices.end(), {a, b, c});
                        if (strip[i].radius > 0.0f)
                            mesh.indices.insert(mesh.indices.end(), {a, c, d});
                    }
            }
        }

        // the arc of a sphere of radius HALF about (0, centreY) from angle from to to (radians above the equator)
        void arc(std::vector<Ring>& strip, const f32 centreY, const f32 from, const f32 to, const u32 steps)
        {
            for (u32 i = 0; i <= steps; ++i)
            {
                const f32 a = from + (to - from) * static_cast<f32>(i) / static_cast<f32>(steps);
                const f32 radius = HALF * std::cos(a);
                strip.push_back(
                    {radius < 1e-6f ? 0.0f : radius, centreY + HALF * std::sin(a), std::cos(a), std::sin(a)});
            }
        }
    } // namespace

    std::span<const std::string_view> primitiveKinds() { return KINDS; }

    std::string primitiveLabel(const std::string_view kind)
    {
        std::string label(kind);
        if (!label.empty())
            label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
        return label;
    }

    PrimitiveMesh buildPrimitive(const std::string_view kind)
    {
        PrimitiveMesh mesh;
        const Vec3 x(1.0f, 0.0f, 0.0f), y(0.0f, 1.0f, 0.0f), z(0.0f, 0.0f, 1.0f);
        if (kind == "cube")
        {
            addFace(mesh, x, y, z, HALF);
            addFace(mesh, -x, z, y, HALF);
            addFace(mesh, y, z, x, HALF);
            addFace(mesh, -y, x, z, HALF);
            addFace(mesh, z, x, y, HALF);
            addFace(mesh, -z, y, x, HALF);
        }
        else if (kind == "plane")
        {
            // one face of the cube's size, pushed to the middle
            addFace(mesh, y, z, x, HALF);
            for (Vec3& position : mesh.positions)
                position.y = 0.0f;
        }
        else if (kind == "quad")
        {
            addFace(mesh, z, x, y, HALF);
            for (Vec3& position : mesh.positions)
                position.z = 0.0f;
        }
        else if (kind == "sphere")
        {
            std::vector<Ring> strip;
            arc(strip, 0.0f, -PI / 2, PI / 2, 16);
            revolve(mesh, {strip});
        }
        else if (kind == "capsule")
        {
            std::vector<Ring> strip;
            arc(strip, -HALF, -PI / 2, 0.0f, 8);
            arc(strip, HALF, 0.0f, PI / 2, 8);
            revolve(mesh, {strip});
        }
        else if (kind == "cylinder")
        {
            revolve(mesh,
                    {{{0.0f, -HALF, 0.0f, -1.0f}, {HALF, -HALF, 0.0f, -1.0f}},
                     {{HALF, -HALF, 1.0f, 0.0f}, {HALF, HALF, 1.0f, 0.0f}},
                     {{HALF, HALF, 0.0f, 1.0f}, {0.0f, HALF, 0.0f, 1.0f}}});
        }
        else if (kind == "cone")
        {
            // the side's normal is perpendicular to the slope from the rim (radius HALF) to the tip (height 1)
            const f32 length = std::sqrt(1.0f + HALF * HALF);
            const f32 nr = 1.0f / length, ny = HALF / length;
            revolve(mesh,
                    {{{0.0f, -HALF, 0.0f, -1.0f}, {HALF, -HALF, 0.0f, -1.0f}},
                     {{HALF, -HALF, nr, ny}, {0.0f, HALF, nr, ny}}});
        }
        else if (kind == "torus")
        {
            // a circle of tube radius 0.15 about a ring of radius 0.35, so the whole fits the unit box
            std::vector<Ring> strip;
            for (u32 i = 0; i <= 16; ++i)
            {
                const f32 a = 2.0f * PI * static_cast<f32>(i) / 16.0f;
                strip.push_back({0.35f + 0.15f * std::cos(a), 0.15f * std::sin(a), std::cos(a), std::sin(a)});
            }
            revolve(mesh, {strip});
        }
        return mesh;
    }

    std::vector<std::byte> primitiveGlb(const std::string_view kind)
    {
        const PrimitiveMesh mesh = buildPrimitive(kind);
        if (mesh.indices.empty())
            return {};
        std::vector<u8> binary;
        const auto add = [&](const void* data, const sizet size)
        {
            const auto* first = static_cast<const u8*>(data);
            binary.insert(binary.end(), first, first + size);
        };
        std::vector<f32> positions, normals, uvs;
        Vec3 low(1e9f), high(-1e9f);
        for (sizet i = 0; i < mesh.positions.size(); ++i)
        {
            const Vec3& p = mesh.positions[i];
            positions.insert(positions.end(), {p.x, p.y, p.z});
            normals.insert(normals.end(), {mesh.normals[i].x, mesh.normals[i].y, mesh.normals[i].z});
            uvs.insert(uvs.end(), {mesh.uvs[i].x, mesh.uvs[i].y});
            low = Vec3(std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z));
            high = Vec3(std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z));
        }
        const sizet positionBytes = positions.size() * 4, uvBytes = uvs.size() * 4,
                    indexBytes = mesh.indices.size() * 2;
        add(positions.data(), positionBytes);
        add(normals.data(), positionBytes);
        add(uvs.data(), uvBytes);
        add(mesh.indices.data(), indexBytes);
        const sizet bufferLength = binary.size();
        while (binary.size() % 4)
            binary.push_back(0);

        using Json = nlohmann::json;
        const std::string label = primitiveLabel(kind);
        const Json json = {
            {"asset", {{"version", "2.0"}, {"generator", "SPITE primitive"}}},
            {"scene", 0},
            {"scenes", Json::array({{{"nodes", Json::array({0})}}})},
            {"nodes", Json::array({{{"name", label}, {"mesh", 0}}})},
            {"meshes",
             Json::array({{{"name", label},
                           {"primitives",
                            Json::array({{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                                          {"indices", 3},
                                          {"material", 0}}})}}})},
            {"materials",
             Json::array(
                 {{{"name", "PrimitiveGrey"},
                   {"pbrMetallicRoughness",
                    {{"baseColorFactor", {0.8, 0.8, 0.8, 1.0}}, {"metallicFactor", 0.0}, {"roughnessFactor", 0.5}}}}})},
            {"buffers", Json::array({{{"byteLength", bufferLength}}})},
            {"bufferViews",
             Json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", positionBytes}},
                          {{"buffer", 0}, {"byteOffset", positionBytes}, {"byteLength", positionBytes}},
                          {{"buffer", 0}, {"byteOffset", 2 * positionBytes}, {"byteLength", uvBytes}},
                          {{"buffer", 0}, {"byteOffset", 2 * positionBytes + uvBytes}, {"byteLength", indexBytes}}})},
            {"accessors",
             Json::array(
                 {{{"bufferView", 0},
                   {"componentType", 5126},
                   {"count", mesh.positions.size()},
                   {"type", "VEC3"},
                   {"min", {low.x, low.y, low.z}},
                   {"max", {high.x, high.y, high.z}}},
                  {{"bufferView", 1}, {"componentType", 5126}, {"count", mesh.positions.size()}, {"type", "VEC3"}},
                  {{"bufferView", 2}, {"componentType", 5126}, {"count", mesh.positions.size()}, {"type", "VEC2"}},
                  {{"bufferView", 3}, {"componentType", 5123}, {"count", mesh.indices.size()}, {"type", "SCALAR"}}})}};
        std::string text = json.dump();
        while (text.size() % 4)
            text.push_back(' ');
        std::vector<std::byte> glb;
        const auto addBytes = [&](const void* data, const sizet size)
        {
            const auto* first = static_cast<const std::byte*>(data);
            glb.insert(glb.end(), first, first + size);
        };
        const u32 head[] = {0x46546c67u, 2u, static_cast<u32>(12 + 8 + text.size() + 8 + binary.size()),
                            static_cast<u32>(text.size()), 0x4e4f534au};
        const u32 binaryHead[] = {static_cast<u32>(binary.size()), 0x004e4942u};
        addBytes(head, sizeof(head));
        addBytes(text.data(), text.size());
        addBytes(binaryHead, sizeof(binaryHead));
        addBytes(binary.data(), binary.size());
        return glb;
    }
} // namespace spite
