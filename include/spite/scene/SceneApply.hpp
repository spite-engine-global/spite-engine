#pragma once

#include <unordered_map>
#include <vector>

#include "SceneDocument.hpp"
#include "SceneExport.hpp"
#include "base/Expected.hpp"
#include "ecs/EcsTypes.hpp"
#include "world/EntityTransformManager.hpp"

namespace spite
{
    class World;
    struct ApplyResult
    {
        std::unordered_map<u64, Entity> idMap; // document sourceId -> live entity
        std::vector<Entity> roots; // live entities of the document roots, in document order
        u32 skippedComponents = 0; // unknown or undecodable components, each logged
        std::vector<SceneChange> changes; // the skips, per component (SceneChangeKind::Skipped)
    };

    // Applies a document into the caller's world: creates the entities, fills
    // their bound components (entity references remapped sourceId -> live,
    // dangling -> undefined with a warning), then rebuilds the transform
    // hierarchy. When parent is valid, document roots attach under it;
    // otherwise they stay unattached. Components this process does not register
    // as scene data are skipped with a warning. Payloads are decoded positionally
    // under the current schemas, so a document must not outlive a layout change:
    // parseSceneBinary refuses a file written under another layout and
    // parseSceneJson restamps by field name (SceneStream.hpp). Synchronous; call
    // from the main thread.
    SPITE_SCENE_API Expected<ApplyResult> applyScene(const SceneDocument& document, World& world,
                                                     EntityTransformManager& transforms, Entity parent);
} // namespace spite
