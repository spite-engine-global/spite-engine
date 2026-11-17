#pragma once

#include "base/MathFunctions.hpp"
#include "base/MathTypes.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    struct PointLightComponent : IEngineComponent
    {
        Vec3 color{1.0f};
        float intensity{1.0f};
        float radius{10.0f};
        // six cached shadow maps (one per face of a cube around the light) while the light is among the view's
        // best-scoring shadowed lights and the view's slots hold six
        bool castShadows{false};
        SPITE_SCHEMA(PointLightComponent, SPITE_FIELD(color, COLOR), intensity, radius, castShadows)
    };

    struct DirectionalLightComponent : IEngineComponent
    {
        Vec3 color{1.0f};
        float intensity{1.0f};
        // the scene's sun is the shadow-casting directional light with the lowest entity index ;
        // false by default until cached shadow maps make an idle sun free
        bool castShadows{true};
        SPITE_SCHEMA(DirectionalLightComponent, SPITE_FIELD(color, COLOR), intensity, castShadows)
    };

    struct SpotLightComponent : IEngineComponent
    {
        Vec3 color{1.0f};
        float intensity{1.0f};
        float radius{20.0f};
        float innerConeAngle{toRadians(25.0f)};
        float outerConeAngle{toRadians(35.0f)};
        // a cached shadow map while the light is among the view's best-scoring shadowed lights
        bool castShadows{false};
        SPITE_SCHEMA(SpotLightComponent, SPITE_FIELD(color, COLOR), intensity, radius, innerConeAngle, outerConeAngle,
                     castShadows)
    };

    // A rectangle that emits: width along the entity's X axis and height along its Y axis, centred on its
    // position and emitting towards its -Z like a spot light (twoSided: both faces). intensity is radiance, like an
    // emissive surface. radius is the reach sphere about the centre, which the cluster build bins like a point light's.
    // It casts no shadow.
    struct AreaLightComponent : IEngineComponent
    {
        Vec3 color{1.0f};
        float intensity{1.0f};
        float width{1.0f};
        float height{1.0f};
        float radius{10.0f};
        bool twoSided{false};
        SPITE_SCHEMA(AreaLightComponent, SPITE_FIELD(color, COLOR), intensity, width, height, radius, twoSided)
    };
} // namespace spite
