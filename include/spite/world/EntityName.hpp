#pragma once

#include <string_view>

#include "base/InternedString.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include "world/WorldExport.hpp"

namespace spite
{
    class World;

    // An entity's display name: scene data like any persisted component, the
    // only place names live. Names are labels, not keys: several entities may
    // share one (repeated model instances, duplicates); ids are the handle. A
    // field write (control's set) may leave an empty name: it reads as unnamed.
    struct EntityNameComponent : IEngineComponent
    {
        InternedString name{};
        SPITE_SCHEMA(EntityNameComponent, name)
    };

    // empty when the entity has no name
    [[nodiscard]] SPITE_WORLD_API std::string_view entityName(const World& world, Entity entity);
    // adds or overwrites the component; an empty name removes it
    SPITE_WORLD_API void setEntityName(const World& world, Entity entity, std::string_view name);
    // the first entity carrying name, undefined when none; a scan over the named
    // entities, for tools and commands, not per-frame code
    [[nodiscard]] SPITE_WORLD_API Entity findEntityByName(const World& world, std::string_view name);
} // namespace spite
