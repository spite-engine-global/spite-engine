#pragma once

#include <array>
#include <bitset>
#include <span>
#include <vector>

#include "base/ApplicationConfig.hpp"
#include "base/MathTypes.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include "rendering/GpuSharedTypes.hpp"
#include "renderruntime/RenderRuntimeExport.hpp"

namespace spite
{
    static_assert(ShadowConfig::MAX_LIGHT_MAPS == MAX_LIGHT_SHADOW_MAPS);

    // Faces of a point light's shadow: one perspective map each, in the order +X, -X, +Y, -Y, +Z, -Z.
    inline constexpr u32 POINT_SHADOW_FACES = 6;

    // A scene light that casts shadows, as the light gather stages it: where it is in the point-then-spot order of the
    // uploaded tables (a spot's index follows every point light's), and what its shadow cameras fit from.
    struct ShadowLightCandidate
    {
        Entity entity = Entity::undefined();
        bool point = false;
        u32 tableIndex = 0;
        Vec3 position{};
        Vec3 direction{}; // a spot: the unit direction it shines along
        float radius = 0.0f; // 0 or less: unbounded, the shadow reaches shadows.maxDistance
        float outerCone = 0.0f; // a spot: radians
    };

    // The score the selection ranks a light by in a view: its reach over its distance to the camera (at most
    // LIGHT_SHADOW_MAX_SCORE), 0 for a light whose reach is outside the camera's frustum.
    inline constexpr float LIGHT_SHADOW_MAX_SCORE = 4.0f;
    SPITE_RENDER_RUNTIME_API float lightShadowScore(const ShadowLightCandidate& light, const CameraData& camera,
                                                    const ShadowConfig& config);

    // The map record of a light's face: the perspective camera (reverse-Z, position.w = 1) its cull and raster use and
    // what Light needs to look it up. A spot's single face looks along its direction and covers its outer cone; a
    // point's six faces look along the axes, each wider than 90 degrees by a guard band of the filter's reach, so
    // that the six cover the sphere with overlapping seams. The record depends only on the light, the resolution,
    // the filter and the normal offset.
    SPITE_RENDER_RUNTIME_API LightShadowMap fitLightShadow(const ShadowLightCandidate& light, u32 face,
                                                           const ShadowConfig& config, u32 scene);

    // One map slot of a view: the layer of its LightShadowMaps array. A point light holds POINT_SHADOW_FACES
    // consecutive slots, face f in slot first + f.
    struct LightShadowSlot
    {
        Entity light = Entity::undefined();
        bool point = false;
        u32 face = 0;
        u32 dirtyReasons = 0; // this frame's, as SunShadows::View::dirtyReasons
        LightShadowMap record{};
    };

    // One row of the shadowMaps table: a held map slot of a view, its VRAM cost, why it renders and how often it did.
    struct LightShadowRow
    {
        u32 view = 0;
        u32 slot = 0;
        Entity light = Entity::undefined();
        bool point = false;
        u32 face = 0;
        u32 resolution = 0;
        u64 bytes = 0; // the slot's layer of the D16 array
        float nearPlane = 0.0f;
        float farPlane = 0.0f;
        Mat4 lightViewProjection{};
        u32 dirtyReasons = 0;
        u64 renderedFrames = 0;
        u64 lastRenderedFrame = 0;

        SPITE_OBSERVED_SCHEMA(LightShadowRow, view, slot, light, point, face, resolution, bytes, nearPlane, farPlane,
                              lightViewProjection, dirtyReasons, renderedFrames, lastRenderedFrame)
    };

    // A view's point and spot light shadow maps. The camera step ranks the scene's shadowing lights by their score in
    // the view, keeps the holders that still rank within the slots and gives the others free slots (a light keeps its
    // slot, so a cached map stays valid), and writes the records and the light-to-first-slot table to the view's
    // LightShadowBuffer in frame slots that hold an older generation.
    struct LightShadowView
    {
        // what the graph compiles: the slot count (0 while no scene light casts shadows) and the maps' resolution
        struct Shape
        {
            u32 slots = 0;
            u32 resolution = 0;
            bool operator==(const Shape&) const = default;
        };

        Shape shape;
        // everything a ranking and the fits read besides the candidates' own state: the update skips a frame whose key
        // equals the last one's (and has no forced slot and no graph change)
        struct Key
        {
            Mat4 viewProjection{};
            Vec4 position{};
            u64 lightGeneration = 0;
            float normalOffset = 0.0f;
            float maxDistance = 0.0f;
            u32 pcfRadius = 0;
            u32 resolution = 0;
            u32 slots = 0;
            u32 scene = 0;
            bool active = false;
            bool operator==(const Key&) const = default;
        };
        Key memo;
        bool memoValid = false;
        // rankings run so far: flat while the scene is settled
        u64 updates = 0;
        std::array<LightShadowSlot, MAX_LIGHT_SHADOW_MAPS> slots{};
        // the first slot of each light in the point-then-spot order, NO_LIGHT_SHADOW for a light with no map
        std::vector<u32> firstMaps;
        u64 generation = 0;
        std::vector<u64> slotGenerations;
        // what the cached maps were culled with: the shadow depth bias, and lod on or off with lodBias
        std::array<float, 4> graphState{};
        // slots a shadowInvalidate marked, consumed by the next update as SHADOW_DIRTY_FORCED
        std::bitset<MAX_LIGHT_SHADOW_MAPS> forceMask{};
        // reused by the selection
        std::vector<float> scratchScores;
        std::vector<u32> scratchOrder;
        std::vector<u32> scratchChosen;
        std::vector<u32> scratchFirst;
        std::vector<u32> scratchFirstMaps;

        [[nodiscard]] std::bitset<MAX_LIGHT_SHADOW_MAPS> occupied() const
        {
            std::bitset<MAX_LIGHT_SHADOW_MAPS> mask;
            for (u32 slot = 0; slot < MAX_LIGHT_SHADOW_MAPS; ++slot)
            {
                mask.set(slot, slots[slot].light.isValid());
            }
            return mask;
        }
    };

    // Ranks the candidates in the view, places the best within shape.slots, best first (a light keeps its run unless a
    // better one claimed it; a point light needs POINT_SHADOW_FACES consecutive slots and holds all of them or none; a
    // light that finds no run is skipped and the lights behind it still fill the free slots), frees the slots of
    // lights that fell out or no longer exist, and fits every held map. pending[slot] gains the dirty reasons of the
    // slots that must render: a new map or a changed graph (graphChanged), a light whose record changed, a forced slot;
    // a freed slot's pending is cleared. An inactive view holds no maps. Returns whether the records or the table
    // changed (generation bumped).
    SPITE_RENDER_RUNTIME_API bool updateLightShadows(LightShadowView& view,
                                                     std::span<const ShadowLightCandidate> candidates, u32 lightCount,
                                                     const CameraData& camera, const ShadowConfig& config, bool active,
                                                     bool graphChanged, u32 scene,
                                                     std::span<u32, MAX_LIGHT_SHADOW_MAPS> pending);
} // namespace spite
