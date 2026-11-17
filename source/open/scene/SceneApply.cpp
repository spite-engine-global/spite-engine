#include "ecs/World.hpp"
#include "scene/SceneApply.hpp"

#include "base/Logging.hpp"
#include "profiling/Zone.hpp"
#include "SceneInternal.hpp"
#include "serialization/Serializer.hpp"
#include "world/TransformComponents.hpp"

namespace spite
{
    namespace
    {
        // per-entity phases are summed into counters (a zone per entity would drown the collector); the clock is read
        // only while a profiling session is armed
        struct PhaseClock
        {
            bool armed = prof::sessionArmed();
            u64 total = 0;
            u64 start = 0;
            void begin() { start = armed ? prof::profilerNowNs() : 0; }
            void end() { total += armed ? prof::profilerNowNs() - start : 0; }
        };

        void applyComponents(const SceneDocument& document, World& world, const std::unordered_map<u64, u64>& remap,
                             ApplyResult& result)
        {
            PhaseClock lookup;
            PhaseClock deserialize;
            PhaseClock add;
            for (const DocumentEntity& entity : document.entities)
            {
                lookup.begin();
                const auto liveIt = result.idMap.find(entity.sourceId);
                if (liveIt == result.idMap.end())
                {
                    continue; // unreachable: idMap is filled for every document entity
                }
                const Entity live = liveIt->second;
                lookup.end();

                for (const DocumentComponent& component : entity.components)
                {
                    const ComponentMetadata* metadata = sceneComponentMetadata(component.typeName);
                    if (metadata == nullptr)
                    {
                        SWARNING_LOG("applyScene: unknown component {} - skipped", component.typeName);
                        ++result.skippedComponents;
                        recordChange(result.changes, component.typeName, {}, SceneChangeKind::Skipped,
                                     "not scene data in this process");
                        continue;
                    }

                    deserialize.begin();
                    ScopedInstance instance(*metadata);
                    std::string error;
                    u32 dangling = 0;
                    const bool deserialized = deserializeFields(*metadata->schema, instance.get(), component.payload,
                                                                error, &remap, &dangling);
                    deserialize.end();
                    if (!deserialized)
                    {
                        SWARNING_LOG("applyScene: corrupt payload for component {} - skipped ({})", component.typeName,
                                     error);
                        ++result.skippedComponents;
                        recordChange(result.changes, component.typeName, {}, SceneChangeKind::Skipped, error);
                        continue;
                    }
                    if (dangling != 0)
                    {
                        SWARNING_LOG("applyScene: {} dangling entity references in component {} remapped to "
                                     "undefined",
                                     dangling, component.typeName);
                    }
                    add.begin();
                    world.addComponentDataById(live, metadata->id, instance.get());
                    add.end();
                }
            }
            SPITE_COUNTER("Scene.apply.idLookupNs", lookup.total);
            SPITE_COUNTER("Scene.apply.deserializeNs", deserialize.total);
            SPITE_COUNTER("Scene.apply.addComponentNs", add.total);
        }
    } // namespace

    Expected<ApplyResult> applyScene(const SceneDocument& document, World& world, EntityTransformManager& transforms,
                                     const Entity parent)
    {
        SPITE_ZONE("Scene.apply");
        ApplyResult result;
        std::unordered_map<u64, u64> remap;

        {
            SPITE_ZONE("Scene.apply.createEntities");
            PhaseClock create;
            PhaseClock maps;
            result.roots.reserve(document.entities.size());
            for (const DocumentEntity& entity : document.entities)
            {
                create.begin();
                const Entity live = world.createEntity();
                create.end();
                maps.begin();
                result.idMap.emplace(entity.sourceId, live);
                remap.emplace(entity.sourceId, live.id());
                maps.end();
                if (entity.parentIndex == NO_PARENT_INDEX)
                {
                    result.roots.push_back(live);
                }
            }
            SPITE_COUNTER("Scene.apply.createEntityNs", create.total);
            SPITE_COUNTER("Scene.apply.idMapEmplaceNs", maps.total);
        }

        {
            SPITE_ZONE("Scene.apply.components");
            applyComponents(document, world, remap, result);
        }

        // The transform manager indexes entities carrying Transform + Matrix;
        // the matrix is runtime-derived (never serialized), so apply restores
        // it for every transform-bearing entity.
        const ComponentID transformId = getComponentId<TransformComponent>();
        const ComponentID matrixId = getComponentId<TransformMatrixComponent>();
        {
            SPITE_ZONE("Scene.apply.matrices");
            for (const auto& [sourceId, live] : result.idMap)
            {
                if (world.hasComponentById(live, transformId) && !world.hasComponentById(live, matrixId))
                {
                    world.addComponentById(live, matrixId);
                }
            }
        }

        std::vector<std::pair<Entity, Entity>> attachPairs;
        {
            SPITE_ZONE("Scene.apply.attachPairs");
            for (size_t i = 0; i < document.entities.size(); ++i)
            {
                const DocumentEntity& entity = document.entities[i];
                const Entity live = result.idMap.at(entity.sourceId);

                if (entity.parentIndex != NO_PARENT_INDEX)
                {
                    if (entity.parentIndex >= document.entities.size())
                    {
                        SWARNING_LOG("applyScene: parent index {} out of range - entity left unattached",
                                     entity.parentIndex);
                        continue;
                    }
                    attachPairs.emplace_back(live, result.idMap.at(document.entities[entity.parentIndex].sourceId));
                }
                else if (parent.isValid())
                {
                    attachPairs.emplace_back(live, parent);
                }
            }
        }
        SPITE_ZONE("Scene.apply.attach");
        // the compiled index lags attach, so the warning keys on attach's verdict
        for (const auto& [child, parentEntity] : attachPairs)
        {
            if (!transforms.attach(child, parentEntity))
            {
                SWARNING_LOG("applyScene: entity {} could not attach under {} - skipped", child.id(),
                             parentEntity.id());
            }
        }
        transforms.applyActivity();

        return result;
    }
} // namespace spite
