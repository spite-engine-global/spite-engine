#pragma once

#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    // The entity's own enabled flag: present means disabled. Scene data, saved and copied to the Play
    // clone as a fieldless tag; EntityTransformManager derives InactiveComponent from it for the entity and every
    // descendant, and that is what queries skip. Write it through EntityTransformManager::setEntityEnabled.
    struct DisabledComponent : IEngineComponent
    {
        SPITE_PERSISTED_TAG_SCHEMA(DisabledComponent)
    };
} // namespace spite
