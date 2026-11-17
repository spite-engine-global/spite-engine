#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <vector>

#include "base/MathTypes.hpp"
#include "base/Schema.hpp"
#include "rendering/GpuSharedTypes.hpp"

// Cluster LOD selection: what the meshlet cull selects clusters from and what it counts. The GPU side is
// shaders/source/cull_meshlets.comp.slang (exceedsBudget, isLodSelected); change both together.

namespace spite
{
    // Levels the counters keep apart; a deeper cluster adds to the last one
    constexpr u32 LOD_STAT_LEVELS = 16;

    // What the meshlet cull's atomics write next to the visible list (the shader's LOD_STAT_* offsets), cleared each
    // frame it counts: the clusters and triangles drawn per level, and what level 0 alone would have drawn for the same
    // views of the same instances (its clusters that pass the frustum, cone and Hi-Z tests, selected or not). The
    // object cull adds the clusters it dropped for lack of candidate capacity (both phases) to droppedCandidates.
    struct LodStatsGpu
    {
        u32 clusters[LOD_STAT_LEVELS];
        u32 triangles[LOD_STAT_LEVELS];
        u32 level0Clusters;
        u32 level0Triangles;
        u32 droppedCandidates;
        u32 _pad[5];
    };
    static_assert(sizeof(LodStatsGpu) == 160);

    // The eye a view's clusters are selected from, with the error budget folded into the scale: an error e over a
    // sphere at distance d is over budget when e * scale > d (an orthographic view has no distance: d is 1).
    struct LodCamera
    {
        Vec3 eye{};
        f32 nearPlane = 0.1f;
        f32 scale = 0.0f;
        bool orthographic = false;
    };

    // The budget is 2^bias pixels of pixelHeight rows: the display's for a game view (a render scale below 1 draws
    // fewer rows than the display shows, so the same error spans fewer rendered pixels), the shadow map's for a cascade
    inline LodCamera lodCameraOf(const CameraData& camera, const f32 pixelHeight, const f32 bias)
    {
        const f32 pixelsPerUnit = std::abs(camera.projection[1][1]) * 0.5f * pixelHeight;
        return {.eye = Vec3(camera.position),
                .nearPlane = camera.nearPlane,
                .scale = pixelsPerUnit / std::exp2(bias),
                .orthographic = camera.projection[3][3] > 0.5f};
    }

    // One row of the lodStats table: a view's cluster LOD as the last landed count measured it
    struct LodRow
    {
        u32 view = 0;
        bool enabled = false; // graphics.lod
        f32 bias = 0.0f; // the view's budget in stops
        bool frozen = false;
        u64 measuredFrame = 0; // the frame the counts describe, 0 until one lands
        u32 clusters = 0; // drawn
        u32 triangles = 0; // drawn
        u32 level0Clusters = 0;
        u32 level0Triangles = 0;
        u32 levels = 0; // the deepest level drawn, plus one
        f32 triangleRatio = 0.0f; // triangles / level0Triangles
        u32 droppedCandidates = 0; // clusters the object cull dropped for lack of candidate capacity

        SPITE_OBSERVED_SCHEMA(LodRow, view, enabled, bias, frozen, measuredFrame, clusters, triangles, level0Clusters,
                              level0Triangles, levels, triangleRatio, droppedCandidates)
    };

    // One row of the lodLevels table: what a view drew of one level of the cluster DAG
    struct LodLevelRow
    {
        u32 view = 0;
        u32 level = 0;
        u32 clusters = 0;
        u32 triangles = 0;

        SPITE_OBSERVED_SCHEMA(LodLevelRow, view, level, clusters, triangles)
    };

    struct LodView
    {
        std::optional<f32> bias; // empty: graphics.lodBias
        bool frozen = false;
        LodCamera live; // the view's camera of the last frame (the scene upload writes it)
        LodCamera held; // what a frozen view selects from: the eye of the camera at the freeze
        std::array<LodCamera, MAX_SHADOW_CASCADES> cascades{}; // the view's sun cascades' (one error pixel is a texel)
        std::array<LodCamera, MAX_LIGHT_SHADOW_MAPS> maps{}; // the view's light shadow maps' (a texel each)
        LodStatsGpu gpu{};
        u64 measuredFrame = 0;
        bool dropsReported = false;

        // A landed count replaces the last; true once per run of counts with drops (the engine logs it then)
        [[nodiscard]] bool land(const LodStatsGpu& counts, const u64 frame)
        {
            gpu = counts;
            measuredFrame = frame;
            const bool report = counts.droppedCandidates != 0 && !dropsReported;
            dropsReported = counts.droppedCandidates != 0;
            return report;
        }
    };

    // Per render view: the selection camera, its budget, the freeze, and the counts the cull reads back. The counts
    // cost a clear, atomics and a readback per frame, so they are produced only while a consumer wants them: a reader
    // of the rows asks for WANT_FRAMES frames (the shadow cascade stats' pattern), the oracle checks pin them.
    struct LodSingleton
    {
        static constexpr u64 WANT_FRAMES = 32;

        u64 frame = 0; // bumped once per frame by the scene upload
        mutable u64 wantedUntil = 0;
        bool pinned = false;
        // graphics.lod and graphics.lodBias as of the last frame (the scene upload copies them)
        bool enabled = false;
        f32 configBias = 0.0f;
        std::vector<LodView> views;

        [[nodiscard]] bool wanted() const { return pinned || (wantedUntil != 0 && frame <= wantedUntil); }
        void want() const { wantedUntil = frame + WANT_FRAMES; }
        [[nodiscard]] f32 biasOf(const u32 view) const { return views[view].bias.value_or(configBias); }
        // A frozen view holds the eye (and the near plane and projection kind) but not the budget: its scale follows
        // the live bias and display height, so a bias change or a resize still moves the selection
        [[nodiscard]] LodCamera selectionCamera(const u32 view) const
        {
            const LodView& state = views[view];
            if (!state.frozen)
            {
                return state.live;
            }
            LodCamera camera = state.held;
            camera.scale = state.live.scale;
            return camera;
        }

        [[nodiscard]] LodRow row(const u32 view) const
        {
            want();
            const LodView& shown = views[view];
            LodRow row{.view = view,
                       .enabled = enabled,
                       .bias = biasOf(view),
                       .frozen = shown.frozen,
                       .measuredFrame = shown.measuredFrame,
                       .level0Clusters = shown.gpu.level0Clusters,
                       .level0Triangles = shown.gpu.level0Triangles,
                       .droppedCandidates = shown.gpu.droppedCandidates};
            for (u32 level = 0; level < LOD_STAT_LEVELS; ++level)
            {
                row.clusters += shown.gpu.clusters[level];
                row.triangles += shown.gpu.triangles[level];
                row.levels = shown.gpu.clusters[level] > 0 ? level + 1 : row.levels;
            }
            row.triangleRatio = row.level0Triangles > 0
                ? static_cast<f32>(row.triangles) / static_cast<f32>(row.level0Triangles)
                : 0.0f;
            return row;
        }

        [[nodiscard]] LodLevelRow levelRow(const u32 view, const u32 level) const
        {
            return {.view = view,
                    .level = level,
                    .clusters = views[view].gpu.clusters[level],
                    .triangles = views[view].gpu.triangles[level]};
        }
    };
} // namespace spite
