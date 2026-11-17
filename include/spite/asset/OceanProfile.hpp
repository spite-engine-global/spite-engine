#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

#include "asset/AssetExport.hpp"
#include "base/Expected.hpp"
#include "base/Platform.hpp"
#include "base/MathTypes.hpp"

namespace spite
{
    inline constexpr float MIN_OCEAN_TILE_METRES = 0.015625f;
    struct OceanProfile
    {
        u32 width = 0;
        u32 height = 0;
        u32 frameCount = 0;
        float tileMetres = 0;
        float framesPerSecond = 0;
        float maxDisplacementMetres = 0;
        bool loop = false;
        // Interleaved dx,dy,dz,nx,ny,nz,foam, row-major; signed linear float32.
        std::vector<float> samples;
    };

    struct OceanProfileSample
    {
        Vec3 displacement{};
        Vec3 normal{0.0f, 1.0f, 0.0f};
        float foam = 0;
    };
    // Validated decoded profile; local XZ metres, repeating spatial edges and last-to-first loop interpolation.
    SPITE_ASSET_API OceanProfileSample sampleOceanProfile(const OceanProfile& profile, float x, float z, float seconds);

    SPITE_ASSET_API Expected<OceanProfile> decodeOceanProfile(std::span<const std::byte> bytes);
    SPITE_ASSET_API Expected<std::vector<std::byte>> readOceanProfileBytes(const std::filesystem::path& path);
    // The caller resolves and authorizes the path before loading; packed profiles have no external dependencies.
    SPITE_ASSET_API Expected<OceanProfile> loadOceanProfile(const std::filesystem::path& path);
} // namespace spite
