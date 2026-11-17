#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <vector>

#include "base/MathFunctions.hpp"
#include "base/MathTypes.hpp"
#include "rendering/GpuSharedTypes.hpp"

// Froxel light binning: a view's frustum is cut into gridX by gridY screen tiles and gridZ depth slices,
// and every point, spot and area light is listed in each cluster its reach sphere touches (an area light's sphere is
// its rectangle's centre and its reach, so it bins like a point light and this reference takes it as one). This is the
// reference of shaders/source/light_cluster_build.comp.slang (the GPU build is tested against it); change both
// together.
//
// Cluster index = (slice * gridY + tile row) * gridX + tile column; tile row 0 is the top of the image. A light's
// index is its place in the points, then the spots, then the areas. A light of radius <= 0 reaches everywhere and sits
// in every cluster, so overflow drops it like any other. A cluster stores its first maxLights lights in light order and
// counts the rest as dropped.

namespace spite
{
    struct ClusterGrid
    {
        u32 x = 16;
        u32 y = 9;
        u32 z = 24;
        u32 maxLights = 128;

        [[nodiscard]] u32 count() const { return x * y * z; }
    };

    // What the GPU build writes next to the lists, 16 bytes of u32 it clears and atomically adds to
    struct LightClusterStats
    {
        u32 assigned; // light entries stored over every cluster
        u32 maxWanted; // most lights any cluster wanted, stored or not
        u32 overflowedClusters; // clusters that wanted more than maxLights
        u32 droppedLights; // entries wanted beyond maxLights, over every cluster
    };
    static_assert(sizeof(LightClusterStats) == 16);

    // the slice boundaries and tile rays derive from these four fields of the view's CameraData
    struct ClusterCamera
    {
        Mat4 view;
        Mat4 inverseProjection;
        bool orthographic;
        f32 nearPlane;
        f32 farPlane;

        static ClusterCamera of(const CameraData& camera)
        {
            const f32 nearPlane = std::max(camera.nearPlane, 1e-4f);
            return {.view = camera.view,
                    .inverseProjection = camera.inverseProjection,
                    .orthographic = camera.projection[3][3] > 0.5f, // a perspective projection has 0 there
                    .nearPlane = nearPlane,
                    .farPlane = std::max(camera.farPlane, nearPlane * 1.001f)};
        }
    };

    struct ClusterBounds
    {
        Vec3 min;
        Vec3 max;
    };

    struct ClusterLight
    {
        Vec3 position; // world space
        f32 radius;
    };

    // The distance in front of the eye where depth slice `boundary` of `slices` starts: exponential from near to far
    // for a perspective view (a slice is as deep, relative to its distance, as the next), linear for an orthographic
    // one (no distance makes a slice finer there).
    inline f32 clusterSliceDepth(const ClusterCamera& camera, const u32 slices, const u32 boundary)
    {
        const f32 unit = static_cast<f32>(boundary) / static_cast<f32>(slices);
        return camera.orthographic ? camera.nearPlane + (camera.farPlane - camera.nearPlane) * unit
                                   : camera.nearPlane * std::pow(camera.farPlane / camera.nearPlane, unit);
    }

    // The view-space box around one cluster: the tile's four rays cut at the slice's two depths. The rays come from
    // unprojecting at two NDC depths and intersecting the line with the depth plane, which holds for a perspective and
    // an orthographic projection alike.
    inline ClusterBounds clusterBounds(const ClusterCamera& camera, const ClusterGrid& grid, const u32 tileX,
                                       const u32 tileY, const u32 slice)
    {
        const f32 ndcX[2] = {-1.0f + 2.0f * static_cast<f32>(tileX) / static_cast<f32>(grid.x),
                             -1.0f + 2.0f * static_cast<f32>(tileX + 1) / static_cast<f32>(grid.x)};
        const f32 ndcY[2] = {1.0f - 2.0f * static_cast<f32>(tileY + 1) / static_cast<f32>(grid.y),
                             1.0f - 2.0f * static_cast<f32>(tileY) / static_cast<f32>(grid.y)};
        const f32 depths[2] = {clusterSliceDepth(camera, grid.z, slice), clusterSliceDepth(camera, grid.z, slice + 1)};
        ClusterBounds bounds{.min = Vec3(1e30f), .max = Vec3(-1e30f)};
        for (const f32 y : ndcY)
        {
            for (const f32 x : ndcX)
            {
                const Vec4 nearClip = camera.inverseProjection * Vec4(x, y, 1.0f, 1.0f);
                const Vec4 midClip = camera.inverseProjection * Vec4(x, y, 0.5f, 1.0f);
                const Vec3 a = Vec3(nearClip) / nearClip.w;
                const Vec3 b = Vec3(midClip) / midClip.w;
                for (const f32 depth : depths)
                {
                    const f32 t = (-depth - a.z) / (b.z - a.z);
                    const Vec3 corner = a + (b - a) * t;
                    bounds.min = Vec3(std::min(bounds.min.x, corner.x), std::min(bounds.min.y, corner.y),
                                      std::min(bounds.min.z, corner.z));
                    bounds.max = Vec3(std::max(bounds.max.x, corner.x), std::max(bounds.max.y, corner.y),
                                      std::max(bounds.max.z, corner.z));
                }
            }
        }
        return bounds;
    }

    // the light's reach sphere (view-space centre) touches the box
    inline bool clusterReaches(const Vec3& viewCenter, const f32 radius, const ClusterBounds& bounds)
    {
        if (radius <= 0.0f)
        {
            return true;
        }
        const Vec3 nearest(std::clamp(viewCenter.x, bounds.min.x, bounds.max.x),
                           std::clamp(viewCenter.y, bounds.min.y, bounds.max.y),
                           std::clamp(viewCenter.z, bounds.min.z, bounds.max.z));
        const Vec3 away = viewCenter - nearest;
        return dot(away, away) <= radius * radius;
    }

    // The GPU build's result: per cluster the lights it stores, in light order, and the stats.
    struct ClusterBinning
    {
        std::vector<u32> counts; // per cluster, at most maxLights
        std::vector<u32> lists; // maxLights entries per cluster, only the first counts[i] meaningful
        LightClusterStats stats{};
    };

    inline ClusterBinning binLightClusters(const ClusterCamera& camera, const ClusterGrid& grid,
                                           const std::vector<ClusterLight>& lights)
    {
        ClusterBinning result;
        result.counts.assign(grid.count(), 0);
        result.lists.assign(static_cast<sizet>(grid.count()) * grid.maxLights, 0);
        std::vector<Vec3> viewCenters;
        viewCenters.reserve(lights.size());
        for (const ClusterLight& light : lights)
        {
            viewCenters.push_back(Vec3(camera.view * Vec4(light.position, 1.0f)));
        }
        for (u32 slice = 0; slice < grid.z; ++slice)
        {
            for (u32 row = 0; row < grid.y; ++row)
            {
                for (u32 column = 0; column < grid.x; ++column)
                {
                    const u32 cluster = (slice * grid.y + row) * grid.x + column;
                    const ClusterBounds bounds = clusterBounds(camera, grid, column, row, slice);
                    u32 stored = 0;
                    u32 wanted = 0;
                    for (u32 light = 0; light < lights.size(); ++light)
                    {
                        if (!clusterReaches(viewCenters[light], lights[light].radius, bounds))
                        {
                            continue;
                        }
                        if (stored < grid.maxLights)
                        {
                            result.lists[static_cast<sizet>(cluster) * grid.maxLights + stored++] = light;
                        }
                        ++wanted;
                    }
                    result.counts[cluster] = stored;
                    result.stats.assigned += stored;
                    result.stats.maxWanted = std::max(result.stats.maxWanted, wanted);
                    if (wanted > grid.maxLights)
                    {
                        ++result.stats.overflowedClusters;
                        result.stats.droppedLights += wanted - grid.maxLights;
                    }
                }
            }
        }
        return result;
    }

    // What the clusters of a light set depend on: the count and the position and radius of every point light, then
    // every spot light, then every area light (its centre and reach; FNV-1a over their bytes). A colour, an intensity,
    // a spot's cone or a rectangle's size changes no cluster, and a change the light upload sees twice (a transform
    // written, then settled) is one change of this key.
    inline u64 lightClusterLightsKey(const std::span<const PointLight> points, const std::span<const SpotLight> spots,
                                     const std::span<const AreaLight> areas = {})
    {
        u64 key = 14695981039346656037ull;
        const auto mix = [&key](const void* data, const sizet size)
        {
            const auto* bytes = static_cast<const unsigned char*>(data);
            for (sizet i = 0; i < size; ++i)
            {
                key = (key ^ bytes[i]) * 1099511628211ull;
            }
        };
        const u32 counts[3] = {static_cast<u32>(points.size()), static_cast<u32>(spots.size()),
                               static_cast<u32>(areas.size())};
        mix(counts, sizeof(counts));
        for (const PointLight& light : points)
        {
            mix(&light.position, sizeof(Vec4));
        }
        for (const SpotLight& light : spots)
        {
            mix(&light.position, sizeof(Vec4));
        }
        for (const AreaLight& light : areas)
        {
            mix(&light.position, sizeof(Vec4));
        }
        return key;
    }

    // What a view's light clusters depend on: the build records only when this differs from the last one built. The
    // grid is fixed at startup.
    struct LightClusterInputs
    {
        Mat4 view{};
        Mat4 projection{};
        f32 nearPlane = 0.0f;
        f32 farPlane = 0.0f;
        u32 scene = 0;
        u64 lightsKey = 0; // lightClusterLightsKey of the scene's lights

        [[nodiscard]] bool operator==(const LightClusterInputs& other) const
        {
            return std::memcmp(&view, &other.view, sizeof(Mat4)) == 0 &&
                std::memcmp(&projection, &other.projection, sizeof(Mat4)) == 0 && nearPlane == other.nearPlane &&
                farPlane == other.farPlane && scene == other.scene && lightsKey == other.lightsKey;
        }
    };

    // The pass module's memory of the last build: the lists hold nothing until one was recorded, and a module
    // restart (a pipeline reload, a new buffer) starts from nothing.
    struct LightClusterBuildGate
    {
        bool built = false;
        LightClusterInputs last{};

        [[nodiscard]] bool due(const LightClusterInputs& now) const { return !built || !(last == now); }
        void commit(const LightClusterInputs& now)
        {
            built = true;
            last = now;
        }
    };
} // namespace spite
