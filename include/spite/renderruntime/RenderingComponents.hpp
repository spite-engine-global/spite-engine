#pragma once

#include <algorithm>
#include <bit>
#include <cmath>

#include "base/MathFunctions.hpp"
#include "base/MathTypes.hpp"
#include "assetruntime/RenderStateComponents.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    struct CameraComponent : IEngineComponent
    {
        float fov = toRadians(90.f);
        float nearPlane = 0.1f;
        float farPlane = 1000.f;
        // Exposure in EV: Composite scales the scene light by 2^exposure before the tone map, whose light units stay
        // arbitrary (0 shows the light as Light wrote it). With autoExposure it is the compensation added to the
        // exposure the luminance histogram asks for.
        float exposure = 0.f;
        // The camera's view adapts its exposure to the scene's luminance (graphics.exposureMinEv/MaxEv/AdaptSeconds)
        bool autoExposure = false;
        SPITE_SCHEMA(CameraComponent, fov, nearPlane, farPlane,
                     SPITE_FIELD(exposure, fieldRange(-16.0, 16.0) | fieldStep(0.1) | fieldUnit("EV")), autoExposure)
    };

    // Per-renderable RGBA multiplied into the material albedo. It lives in the renderable's object-buffer record,
    // so changing it is one scene update record: no material, meshlet bake or archetype move. A renderable
    // without the component draws untinted. Stored as RGBA8 on the GPU: components clamp to [0, 1].
    // Attach at spawn to prepare fade-capable shadow passes before animating coverage.
    // Independent of the material's alpha/cutout and tint.
    struct RenderFadeComponent : IDerivedComponent
    {
        float opacity = 1.0f;
        SPITE_OBSERVED_SCHEMA(RenderFadeComponent, SPITE_FIELD(opacity, OBSERVED | READONLY))
    };

    struct RenderTintComponent : IEngineComponent
    {
        Vec4 color{1.0f};
        SPITE_SCHEMA(RenderTintComponent, SPITE_FIELD(color, COLOR))
    };

    // The cluster LOD policy of one renderable (the entity holding the mesh; a model root without one, a parent or a
    // child is not affected). fullDetail draws the renderable's level-0 clusters in every view and shadow pass while
    // other instances of the same mesh keep automatic selection; the shared baked mesh is not changed. Absent, or
    // fullDetail false, selects automatically. graphics.lod off draws level 0 for every renderable either way.
    struct RenderLodPolicyComponent : IEngineComponent
    {
        bool fullDetail = true;
        float lodBias = 0.0f; // Positive selects coarser LOD; negative selects finer LOD.
        SPITE_SCHEMA(RenderLodPolicyComponent, fullDetail, SPITE_FIELD(lodBias, fieldRange(-8.0, 8.0)))
    };

    inline float renderLodThresholdScale(float lodBias)
    {
        if ((std::bit_cast<u32>(lodBias) & 0x7f800000u) == 0x7f800000u)
        {
            lodBias = 0.0f;
        }
        return std::exp2(-std::clamp(lodBias, -8.0f, 8.0f));
    }

    // A world-space card uses its mesh UVs as one frame of a row-major atlas.
    struct FlipbookComponent : IEngineComponent
    {
        u32 columns = 1;
        u32 rows = 1;
        u32 frameCount = 1;
        float framesPerSecond = 24.0f;
        float elapsedSeconds = 0.0f;
        bool loop = true;
        bool playing = true;
        bool cameraFacing = false;
        SPITE_SCHEMA(FlipbookComponent, SPITE_FIELD(columns, fieldRange(1, 1024)),
                     SPITE_FIELD(rows, fieldRange(1, 1024)), SPITE_FIELD(frameCount, fieldRange(1, 1048576)),
                     SPITE_FIELD(framesPerSecond, fieldRange(0.01, 240.0)),
                     SPITE_FIELD(elapsedSeconds, fieldRange(0.0, 1000000000.0)), loop, playing, cameraFacing)
    };

    struct WaterSurfaceComponent : IEngineComponent
    {
        float amplitude = 0.5f;
        float wavelength = 20.0f;
        float directionX = 1.0f;
        float directionZ = 0.0f;
        float speed = 1.0f;
        Vec3 color{0.025f, 0.18f, 0.24f};
        float roughness = 0.18f;
        float elapsedSeconds = 0.0f;
        float previousSeconds = 0.0f;
        bool playing = true;
        bool castShadows = false;
        SPITE_SCHEMA(WaterSurfaceComponent, SPITE_FIELD(amplitude, fieldRange(0.0, 1000.0)),
                     SPITE_FIELD(wavelength, fieldRange(0.01, 100000.0)), directionX, directionZ,
                     SPITE_FIELD(speed, fieldRange(0.0, 100.0)), SPITE_FIELD(color, COLOR),
                     SPITE_FIELD(roughness, fieldRange(0.045, 1.0)),
                     SPITE_FIELD(elapsedSeconds, fieldRange(0.0, 1000000000.0)),
                     SPITE_FIELD(previousSeconds, OBSERVED | READONLY), playing, castShadows)
    };

    inline bool validWaterSurface(const WaterSurfaceComponent& water)
    {
        const auto finite = [](float v) { return (std::bit_cast<u32>(v) & 0x7f800000u) != 0x7f800000u; };
        return finite(water.amplitude) && water.amplitude >= 0.0f && water.amplitude <= 1000.0f &&
            finite(water.wavelength) && water.wavelength >= 0.01f && water.wavelength <= 100000.0f &&
            finite(water.directionX) && finite(water.directionZ) && water.directionX >= -1.0f &&
            water.directionX <= 1.0f && water.directionZ >= -1.0f && water.directionZ <= 1.0f &&
            (water.directionX * water.directionX + water.directionZ * water.directionZ) >= 1e-6f &&
            finite(water.speed) && water.speed >= 0.0f && water.speed <= 100.0f && finite(water.color.x) &&
            finite(water.color.y) && finite(water.color.z) && water.color.x >= 0.0f && water.color.x <= 1.0f &&
            water.color.y >= 0.0f && water.color.y <= 1.0f && water.color.z >= 0.0f && water.color.z <= 1.0f &&
            finite(water.roughness) && water.roughness >= 0.045f && water.roughness <= 1.0f &&
            finite(water.elapsedSeconds) && water.elapsedSeconds >= 0.0f && water.elapsedSeconds <= 1000000000.0f &&
            finite(water.previousSeconds) && water.previousSeconds >= 0.0f && water.previousSeconds <= 1000000000.0f;
    }

    inline bool validFlipbook(const FlipbookComponent& effect)
    {
        const auto finite = [](float v) { return (std::bit_cast<u32>(v) & 0x7f800000u) != 0x7f800000u; };
        return effect.columns != 0 && effect.columns <= 1024 && effect.rows != 0 && effect.rows <= 1024 &&
            effect.frameCount != 0 &&
            static_cast<u64>(effect.frameCount) <= static_cast<u64>(effect.columns) * effect.rows &&
            finite(effect.framesPerSecond) && effect.framesPerSecond >= 0.01f && effect.framesPerSecond <= 240.0f &&
            finite(effect.elapsedSeconds) && effect.elapsedSeconds >= 0.0f && effect.elapsedSeconds <= 1000000000.0f;
    }

    inline u32 flipbookFrame(const FlipbookComponent& effect)
    {
        if (!validFlipbook(effect))
        {
            return 0;
        }
        const double position = static_cast<double>(effect.elapsedSeconds) * effect.framesPerSecond;
        if (effect.loop)
        {
            return static_cast<u32>(std::fmod(position, static_cast<double>(effect.frameCount)));
        }
        return static_cast<u32>(std::min(position, static_cast<double>(effect.frameCount - 1)));
    }

    constexpr u32 UNTINTED = 0xFFFFFFFFu;

    inline u32 packTint(const Vec4& color)
    {
        const auto byte = [](const float value)
        { return static_cast<u32>(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f); };
        return byte(color.x) | byte(color.y) << 8 | byte(color.z) << 16 | byte(color.w) << 24;
    }
} // namespace spite
