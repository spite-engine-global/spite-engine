#include "ecs/World.hpp"
#include "scene/SceneCollect.hpp"

#include <unordered_map>

#include "SceneInternal.hpp"
#include "profiling/Zone.hpp"
#include "serialization/Serializer.hpp"

namespace spite
{
    namespace
    {
        void appendEntityComponents(World& world, const Entity entity, LayoutHashes& hashes, DocumentEntity& out)
        {
            std::vector<ComponentID> aspectIds(64);
            u32 count = world.getEntityAspectIds(entity, aspectIds);
            if (count > aspectIds.size())
            {
                aspectIds.resize(count);
                count = world.getEntityAspectIds(entity, aspectIds);
            }

            for (const ComponentID id : std::span(aspectIds.data(), count))
            {
                const ComponentMetadata* metadata = sceneComponentMetadata(id);
                if (metadata == nullptr)
                {
                    continue;
                }

                DocumentComponent component;
                component.typeName = metadata->name.toView().data();
                component.layoutHash = hashes.of(*metadata);
                serializeFields(*metadata->schema, world.getConstComponentDataById(entity, id), component.payload);
                out.components.push_back(std::move(component));
            }
        }
    } // namespace

    Expected<SceneDocument> collectScene(World& world, EntityTransformManager& transforms,
                                         const std::span<const Entity> roots)
    {
        SPITE_ZONE("Scene.collect");
        SceneDocument document;
        std::unordered_map<u64, u32> docPositionById;

        const auto alreadyCollected = [&docPositionById](const Entity entity)
        { return docPositionById.find(entity.id()) != docPositionById.end(); };

        {
            SPITE_ZONE("Scene.collect.structure");
            std::vector<Entity> subtree;
            for (const Entity root : roots)
            {
                if (!world.isEntityValid(root))
                {
                    return std::unexpected(
                        makeError(ErrorCode::SceneCollectFailed, "collectScene: invalid root entity"));
                }

                if (transforms.collectSubtree(root, subtree))
                {
                    for (const Entity entity : subtree)
                    {
                        if (alreadyCollected(entity))
                        {
                            continue; // overlapping roots collect once
                        }
                        docPositionById.emplace(entity.id(), static_cast<u32>(document.entities.size()));

                        DocumentEntity entry;
                        entry.sourceId = entity.id();
                        const Entity parent = transforms.currentParentOf(entity);
                        if (parent.isValid())
                        {
                            const auto it = docPositionById.find(parent.id());
                            if (it != docPositionById.end())
                            {
                                entry.parentIndex = it->second;
                            }
                        }
                        document.entities.push_back(std::move(entry));
                    }
                }
                else if (!alreadyCollected(root))
                {
                    docPositionById.emplace(root.id(), static_cast<u32>(document.entities.size()));

                    DocumentEntity entry;
                    entry.sourceId = root.id();
                    document.entities.push_back(std::move(entry));
                }
            }
        }

        LayoutHashes hashes;
        SPITE_ZONE("Scene.collect.serialize");
        for (DocumentEntity& entry : document.entities)
        {
            appendEntityComponents(world, Entity(entry.sourceId), hashes, entry);
        }

        return document;
    }
} // namespace spite
