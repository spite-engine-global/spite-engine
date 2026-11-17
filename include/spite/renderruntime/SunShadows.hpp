#pragma once

#include <array>
#include <span>
#include <vector>

#include "base/ApplicationConfig.hpp"
#include "base/MathTypes.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include "rendering/GpuSharedTypes.hpp"
#include "rendering/RenderingSingletons.hpp"
#include "renderruntime/LightShadows.hpp"
#include "renderruntime/RenderRuntimeExport.hpp"

namespace spite
{
    static_assert(ShadowConfig::MAX_CASCADES == MAX_SHADOW_CASCADES);

    // A cascade's radius rounds up to this, so float noise in the slice bounds never changes it.
    inline constexpr float SHADOW_RADIUS_QUANTUM = 1.0f / 64.0f;

    // The practical split scheme over [nearPlane, farPlane]: out[i] = lambda * logarithmic + (1 - lambda) * uniform,
    // for out.size() - 1 cascades; out.front() = nearPlane, out.back() = farPlane.
    SPITE_RENDER_RUNTIME_API void practicalSplits(float nearPlane, float farPlane, float lambda, std::span<float> out);

    // The smallest sphere around the view-depth slice [sliceNear, sliceFar] of a symmetric perspective frustum. Its
    // centre lies on the view axis, so it is the same for every camera rotation about that axis.
    struct ViewSliceSphere
    {
        float centerDistance = 0.0f; // along the view direction from the camera
        float radius = 0.0f;
    };
    SPITE_RENDER_RUNTIME_API ViewSliceSphere viewSliceSphere(float sliceNear, float sliceFar, float tanHalfFovY,
                                                             float aspect);

    // The light space of a sun, from its direction alone: back = -direction, up from the world axis least parallel
    // to it. A light-space coordinate is (dot(right, p), dot(up, p), dot(back, p)).
    struct ShadowLightBasis
    {
        Vec3 right;
        Vec3 up;
        Vec3 back;
    };
    SPITE_RENDER_RUNTIME_API ShadowLightBasis shadowLightBasis(const Vec3& lightDirection);

    // Fits a view's cascades for the sun travelling along lightDirection : practical splits over
    // [camera near, min(camera far, maxDistance)], each slice (extended back over the previous cascade's blend band)
    // bounded by its view-axis sphere, the radius padded so the snapped sphere still holds the slice and quantised,
    // the centre snapped to the light-space texel grid on all three axes. Each record's camera is orthographic:
    // x and y span +-radius, view depth [-radius, radius] about the centre maps to reverse-Z depth [0, 1] with the
    // light side at 1; position.xyz = forward.xyz = lightDirection (position.w = 0); nearPlane = -radius,
    // farPlane = radius. A record depends only on the basis, the snapped centre, the radius, the resolution and
    // camera.scene, so a camera move or roll that keeps the snapped centre leaves it bit-identical. Returns the
    // cascade count written (config.cascadeCount; 0 for a degenerate camera range).
    SPITE_RENDER_RUNTIME_API u32 fitShadowCascades(const CameraData& camera, const Vec3& lightDirection,
                                                   const ShadowConfig& config,
                                                   std::span<ShadowCascade, MAX_SHADOW_CASCADES> out);

    // A sun map for a sphere (the probe capture view's): the one cascade out[0], fitted like the others
    // (same basis, padding, quantised radius, snapped centre) around the sphere, its split range [0, 2 radius] so the
    // mask's cascade choice by view depth takes it for every pixel within the sphere. Returns 1 (0 for a degenerate
    // radius or a config with no cascades). A record depends only on the basis, the snapped centre, the radius, the
    // resolution and `scene`: a sphere that moves less than a texel leaves it bit-identical.
    SPITE_RENDER_RUNTIME_API u32 fitShadowSphere(const Vec3& center, float radius, const Vec3& lightDirection,
                                                 const ShadowConfig& config, u32 scene,
                                                 std::span<ShadowCascade, MAX_SHADOW_CASCADES> out);

    struct ShadowCascadeStats;
    struct ShadowCascadeStatsSingleton;

    // Whether an object with these world bounds reaches the cascade's caster volume: the planes of its cull, which
    // keep everything toward the light (the near plane is dropped), so a caster that pancakes onto the map counts.
    SPITE_RENDER_RUNTIME_API bool shadowVolumeReaches(const CameraData& cascadeCamera, const Vec3& boundsMin,
                                                      const Vec3& boundsMax);

    // One row of the shadowCascades table : CPU columns, the dirty reasons and the pass results
    // (ShadowCascadeStats; the GPU columns arrive a few frames late).
    struct ShadowCascadeRow
    {
        u32 view = 0;
        u32 cascade = 0;
        Entity sun = Entity::undefined();
        float splitNear = 0.0f;
        float splitFar = 0.0f;
        Vec3 center{};
        float radius = 0.0f;
        float texelWorldSize = 0.0f;
        Mat4 lightViewProjection{};
        u32 dirtyReasons = 0;
        u64 renderedFrames = 0;
        u64 lastRenderedFrame = 0;
        u32 visibleMeshlets = 0;
        u32 overflowed = 0;
        u64 measuredFrame = 0;

        SPITE_OBSERVED_SCHEMA(ShadowCascadeRow, view, cascade, sun, splitNear, splitFar, center, radius, texelWorldSize,
                              lightViewProjection, dirtyReasons, renderedFrames, lastRenderedFrame, visibleMeshlets,
                              overflowed, measuredFrame)
    };

    // Every view's sun cascades: the scene upload of the view's world fits them each frame and writes them to the
    // view's ShadowCascadeBuffer only in frame slots that hold an older fit. A view without a sun has none.
    struct SPITE_RENDER_RUNTIME_API SunShadows
    {
        struct View
        {
            std::array<ShadowCascade, MAX_SHADOW_CASCADES> cascades{};
            u32 cascadeCount = 0;
            Entity sun = Entity::undefined();
            // bumped when the fit changes; slotGenerations[slot] is what that frame slot's buffer holds
            u64 generation = 0;
            std::vector<u64> slotGenerations;
            // this frame's dirty reasons per cascade, set by the frame's cascade update and the object step
            std::array<u32, MAX_SHADOW_CASCADES> dirtyReasons{};
            // what the last fit was made for: a change is the LIGHT or GRAPH reason
            Vec3 lightDirection{};
            std::array<float, 2> depthBias{};
            // the cluster LOD state the cached cascades were culled with: lod on (1) or off (0), and lodBias
            std::array<float, 2> lodState{};
            // set by invalidate, consumed by the next cascade update as SHADOW_DIRTY_FORCED on every cascade
            bool forcePending = false;
            // the view's point and spot light shadow maps
            LightShadowView lights;
        };

        std::vector<View> views;
        // the passes' per-cascade results, owned by the runtime's services; null without a render runtime
        ShadowCascadeStatsSingleton* stats = nullptr;
        // frame-slot buffer writes so far: flat on a quiescent scene
        u64 uploads = 0;
        // the same for the light shadow maps' buffers
        u64 mapUploads = 0;

        [[nodiscard]] ShadowCascadeRow row(u32 view, u32 cascade) const;
        // Marks the view's cascades dirty for the next frame (reason forced); the cascades it marked.
        u32 invalidate(u32 view);

        // The shadowMaps row of a slot a light holds in the view.
        [[nodiscard]] LightShadowRow mapRow(u32 view, u32 slot) const;
        // Marks the maps of one light of the view, or of every light for an undefined entity, dirty for the next frame
        // (reason forced); the slots it marked.
        u32 invalidateMaps(u32 view, Entity light = Entity::undefined());
    };
} // namespace spite
