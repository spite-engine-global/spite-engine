#pragma once

#include <unordered_map>

#include "SceneExport.hpp"
#include "ecs/ComponentMetadata.hpp"
#include "ecs/EcsTypes.hpp"
#include "world/EntityTransformManager.hpp"

namespace spite
{
    class World;
    // Clones the scene data of every source entity whose archetype lacks `exclude` into target, world to world:
    // what applyScene(collectScene(...)) of those entities builds, without a document. Scene components only (a
    // persisted field), persisted fields copied and the rest default-constructed, Entity and EntityList fields
    // remapped to the clones (a reference to an entity outside the clone -> undefined), TransformMatrix added for
    // every Transform entity, transform edges between cloned entities rebuilt. Fresh ids; one archetype entry per
    // destination aspect, entity-created fires once per entity after its data is written. Returns source id -> clone.
    // Synchronous; the source must not change structurally meanwhile.
    SPITE_SCENE_API std::unordered_map<u64, Entity> cloneScene(const World& source,
                                                               const EntityTransformManager& sourceTransforms,
                                                               World& target, EntityTransformManager& targetTransforms,
                                                               ComponentID exclude);
} // namespace spite
