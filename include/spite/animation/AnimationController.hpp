#pragma once

#include <functional>
#include <string_view>

#include "animation/AnimationExport.hpp"
#include "base/MathTypes.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    class ServiceLocator;

    enum class AnimationParameterKind : u8
    {
        Float = 0,
        Bool = 1,
        Trigger = 2 // a one-shot: set until a transition that names it is taken, which consumes it
    };

    enum class AnimationParameterResult : u8
    {
        Ok = 0,
        NotBound = 1, // the entity has no bound controller yet (it binds at the start of the next frame), or no world
        UnknownParameter = 2, // the controller has no parameter of that name
        WrongKind = 3, // the parameter is of another kind (setFloat on a bool, trigger on a float)
        InvalidValue = 4 // a float that is not finite
    };

    // The ground under a world position, for an AnimationIkComponent with followGround: true with the ground's height
    // (world y) at the position's x and z, false where there is none. AnimationPose calls it from its worker threads,
    // so it must be safe to call concurrently and must not write the world's components.
    using GroundQuery = std::function<bool(const Vec3& position, f32& height)>;

    // How a game module drives an animation controller (an entity with an AnimationControllerComponent) from C++: it
    // sets the controller's named parameters, and the controller's transitions read them. A game sets what it knows
    // (its speed, whether it is grounded, a jump press) and never names a clip or a state. services is the world's
    // (SystemContext::services(), World::services()). Every call is safe from a system of any thread in the update
    // stages and from the frame thread outside them; a value takes effect at the next AnimationAdvance, and setting
    // the value a parameter already holds wakes nothing. What the controller is doing is read from the entity's
    // AnimationControllerStateComponent and AnimationStateComponent (normalized time, transition progress).
    class SPITE_ANIMATION_API AnimationController
    {
    public:
        static AnimationParameterResult setFloat(const ServiceLocator& services, Entity entity, std::string_view name,
                                                 f32 value);
        static AnimationParameterResult setBool(const ServiceLocator& services, Entity entity, std::string_view name,
                                                bool value);
        static AnimationParameterResult trigger(const ServiceLocator& services, Entity entity, std::string_view name);
        // Sets the ground query of the world (an empty function clears it): the foot placement hook of IK nodes with
        // followGround. Call it from the frame thread outside the update stages (a module's attachWorld). A node asks
        // again when its pose or the skin's transform changed, not when the ground alone does.
        static void setGroundQuery(ServiceLocator& services, GroundQuery query);
    };
} // namespace spite
