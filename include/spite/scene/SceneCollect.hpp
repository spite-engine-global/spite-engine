#pragma once

#include <span>
#include <vector>

#include "SceneDocument.hpp"
#include "SceneExport.hpp"
#include "base/Expected.hpp"
#include "ecs/EcsTypes.hpp"
#include "world/EntityTransformManager.hpp"

namespace spite
{
    class World;
    // Collects the given roots and their transform subtrees into a document.
    // Entity order is parents before children; parentIndex is the document
    // position of the parent (NO_PARENT_INDEX for roots). Entities without a
    // transform become standalone roots. Only scene data is collected: components
    // whose schema has a persisted field. Overlapping roots are
    // collected once. sourceId is the live entity id, so entity references
    // inside payloads need no rewrite at collect; apply remaps them.
    SPITE_SCENE_API Expected<SceneDocument> collectScene(World& world, EntityTransformManager& transforms,
                                                         std::span<const Entity> roots);
} // namespace spite
