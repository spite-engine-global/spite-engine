#pragma once

#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include "rendering/Environment.hpp"

namespace spite
{
    // The scene's background and ambient light. The active environment is the enabled one (not on an inactive
    // entity) with the lowest entity index, like the sun , so it survives additive scene layers:
    // adding one with a lower index or disabling the active one switches it. It reaches the GPU as one record, written
    // only when it changes. The colour mode shows `tint`, which the other modes multiply into what they show;
    // `intensity` scales the radiance of every mode. The procedural mode draws the gradient below plus the sun's disc
    // into the same cube the hdri mode bakes, so the sky and the lighting read it alike: the sun is the scene's sun
    // (the shadow-casting directional light with the lowest entity index, the one the sun shadows use), and the cube is
    // drawn again only when the sun or one of the procedural fields changes. With a sun the procedural sky is an
    // Earth-like atmosphere (Rayleigh and Mie scattering, ozone; include/spite/rendering/Atmosphere.hpp) lit by it,
    // from the sun's direction alone: blue by day, a warm horizon towards a low sun, a fading twilight after it, and
    // the scene's sun light has the atmosphere's transmittance on it (warmer and dimmer as it sinks).
    // The gradient colours are the sky of a scene with no sun and, at 3%, the floor of the night under one; `ground` is
    // the albedo of the ground below the horizon.
    struct EnvironmentComponent : IEngineComponent
    {
        EnvironmentMode mode{EnvironmentMode::Gradient};
        // the environment asset of the hdri mode (importEnvironment's name)
        InternedString hdri{};
        float intensity{1.0f};
        // the turn of the environment about +Y in radians, baked into the cube the hdri mode shows
        float rotationY{0.0f};
        Vec3 tint{1.0f};
        float ambientIntensity{1.0f};
        // the gradient mode's colours by the ray's height: zenith above the horizon, ground below it
        Vec3 zenith{0.25f, 0.45f, 0.85f};
        Vec3 horizon{0.75f, 0.85f, 0.95f};
        Vec3 ground{0.30f, 0.28f, 0.25f};
        // the procedural sun disc's angular radius in radians, and its radiance (HDR: it is what the sun's glint in
        // reflections and bloom is made of; 0 hides the disc)
        float sunDiscSize{0.05f};
        float sunDiscIntensity{20.0f};
        // the irradiance the atmosphere is lit with, the scale of the sky's brightness (the sun light is its own
        // component). The atmosphere is the reference one (Hillaire 2020: clean air, aerosol optical depth 0.005),
        // whose zenith is 0.0145 of the sun's irradiance in luminance where a clear day's (Preetham, turbidity 2) is
        // about 0.05, so 10 against a sun light of intensity 3 gives the ratio of sky to sunlit surface that a clear
        // day has. 0 turns the atmosphere off: the sky is the gradient as it is, the disc keeps its radiance and the
        // sun light is not tinted.
        float sunIrradiance{10.0f};
        bool affectsLighting{true};
        SPITE_SCHEMA(EnvironmentComponent, mode, hdri, intensity, rotationY, SPITE_FIELD(tint, COLOR), ambientIntensity,
                     SPITE_FIELD(zenith, COLOR), SPITE_FIELD(horizon, COLOR), SPITE_FIELD(ground, COLOR), sunDiscSize,
                     sunDiscIntensity, sunIrradiance, affectsLighting)
    };

    // the record the GPU reads for an environment; none is all zero, so a scene without one writes nothing. The
    // procedural mode carries the scene's sun (`towardSun` is its unit direction, ignored while `hasSun` is false);
    // the other modes carry none, so a sun that moves under them changes nothing.
    inline EnvironmentData environmentRecord(const EnvironmentComponent& environment, const bool hasSun = false,
                                             const Vec3& towardSun = Vec3(0.0f))
    {
        EnvironmentData record{};
        if (environment.mode == EnvironmentMode::None)
        {
            return record;
        }
        record.mode = static_cast<u32>(environment.mode);
        record.tintIntensity = Vec4(environment.tint, environment.intensity);
        record.zenith = Vec4(environment.zenith, 0.0f);
        record.horizon = Vec4(environment.horizon, 0.0f);
        record.ground = Vec4(environment.ground, 0.0f);
        record.parameters = Vec4(environment.rotationY, environment.ambientIntensity, environment.sunDiscSize,
                                 environment.affectsLighting ? 1.0f : 0.0f);
        if (environment.mode == EnvironmentMode::Procedural && hasSun)
        {
            record.sun = Vec4(towardSun, environment.sunDiscIntensity);
            record.sunIrradiance = environment.sunIrradiance;
        }
        return record;
    }
} // namespace spite
