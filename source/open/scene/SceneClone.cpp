#include "ecs/World.hpp"
#include "scene/SceneClone.hpp"

#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "base/Logging.hpp"
#include "ecs/Chunk.hpp"
#include "profiling/Zone.hpp"
#include "SceneInternal.hpp"
#include "world/TransformComponents.hpp"

namespace spite
{
    namespace
    {
        static_assert(std::is_trivially_copyable_v<InternedString> && std::is_trivially_copyable_v<Entity>);

        enum class CopyKind : u8
        {
            Bytes,
            Entity,
            EntityList,
            String
        };

        struct FieldCopy
        {
            CopyKind kind;
            u32 offset;
            u32 size;
        };

        // The persisted fields of a schema as copy steps, nested structs flattened, adjacent byte ranges merged:
        // exactly what the document's serialize/deserialize pair carries.
        void appendCopies(const StructSchema& schema, const u32 base, std::vector<FieldCopy>& out)
        {
            const auto add = [&out](const CopyKind kind, const u32 offset, const u32 size)
            {
                if (kind == CopyKind::Bytes && !out.empty() && out.back().kind == CopyKind::Bytes &&
                    out.back().offset + out.back().size == offset)
                {
                    out.back().size += size;
                    return;
                }
                out.push_back({kind, offset, size});
            };
            for (const FieldDesc& field : schema.fields)
            {
                if (!(field.flags & fieldFlags::PERSISTED))
                {
                    continue;
                }
                const u32 count = field.arrayCount == 0 ? 1 : field.arrayCount;
                for (u32 i = 0; i < count; ++i)
                {
                    const u32 offset = base + field.offset + i * field.elementSize;
                    switch (field.kind)
                    {
                    case FieldKind::Entity:
                        add(CopyKind::Entity, offset, sizeof(Entity));
                        break;
                    case FieldKind::EntityList:
                        add(CopyKind::EntityList, offset, 0);
                        break;
                    case FieldKind::String:
                        add(CopyKind::String, offset, 0);
                        break;
                    case FieldKind::Struct:
                        appendCopies(*field.structSchema, offset, out);
                        break;
                    case FieldKind::StructList:
                        SASSERTM(false, "cloneScene: {}.{} is a StructList, which is never a component field",
                                 schema.typeName, field.name);
                        break;
                    default:
                        add(CopyKind::Bytes, offset, field.elementSize);
                        break;
                    }
                }
            }
        }

        struct ComponentPlan
        {
            const ComponentMetadata* metadata = nullptr; // null: not scene data, never cloned
            std::vector<FieldCopy> copies;
        };

        struct Remap
        {
            struct Slot
            {
                u64 source = 0;
                Entity clone = Entity::undefined();
            };
            std::vector<Slot> byIndex; // by source entity index
            u32 dangling = 0;

            [[nodiscard]] Entity find(const Entity source) const
            {
                const u32 index = source.index();
                return index < byIndex.size() && byIndex[index].source == source.id() ? byIndex[index].clone
                                                                                      : Entity::undefined();
            }

            Entity operator()(const Entity source)
            {
                const Entity clone = find(source);
                dangling += source.isValid() && !clone.isValid() ? 1 : 0;
                return clone;
            }
        };

        void copyFields(const std::vector<FieldCopy>& copies, const u8* src, u8* dst, Remap& remap)
        {
            for (const FieldCopy& copy : copies)
            {
                const u8* from = src + copy.offset;
                u8* to = dst + copy.offset;
                switch (copy.kind)
                {
                case CopyKind::Bytes:
                    std::memcpy(to, from, copy.size);
                    break;
                case CopyKind::Entity:
                    *reinterpret_cast<Entity*>(to) = remap(*reinterpret_cast<const Entity*>(from));
                    break;
                case CopyKind::EntityList:
                    {
                        const auto& source = *reinterpret_cast<const std::vector<Entity>*>(from);
                        auto& clone = *reinterpret_cast<std::vector<Entity>*>(to);
                        clone.resize(source.size());
                        for (sizet i = 0; i < source.size(); ++i)
                        {
                            clone[i] = remap(source[i]);
                        }
                        break;
                    }
                case CopyKind::String:
                    *reinterpret_cast<std::string*>(to) = *reinterpret_cast<const std::string*>(from);
                    break;
                }
            }
        }

        struct SourceRow
        {
            const Chunk* chunk;
            u32 row;
        };

        // the source rows that become one destination archetype
        struct Group
        {
            std::vector<ComponentID> ids; // destination aspect, sorted
            std::vector<SourceRow> rows;
            u32 first = 0; // position of rows[0] in the clone order
        };
    } // namespace

    std::unordered_map<u64, Entity> cloneScene(const World& source, const EntityTransformManager& sourceTransforms,
                                               World& target, EntityTransformManager& targetTransforms,
                                               const ComponentID exclude)
    {
        SPITE_ZONE("Scene.clone");
        const ComponentID transformId = getComponentId<TransformComponent>();
        const ComponentID matrixId = getComponentId<TransformMatrixComponent>();

        std::unordered_map<ComponentID, ComponentPlan> plans;
        const auto planOf = [&plans](const ComponentID id) -> const ComponentPlan&
        {
            const auto [it, inserted] = plans.try_emplace(id);
            if (inserted)
            {
                // applyScene's rule: scene data, built through its construction policy and added by copy
                const ComponentMetadata* metadata = sceneComponentMetadata(id);
                if (metadata != nullptr && metadata->constructionPolicy != nullptr && metadata->copyPolicy != nullptr)
                {
                    it->second.metadata = metadata;
                    appendCopies(*metadata->schema, 0, it->second.copies);
                }
            }
            return it->second;
        };

        std::vector<Group> groups;
        Remap remap;
        u32 total = 0;
        {
            SPITE_ZONE("Scene.clone.group");
            std::unordered_map<Aspect, u32, Aspect::hash> groupByAspect;
            std::vector<ComponentID> ids;
            for (const Chunk* chunk : source.getChunksContaining(Aspect{}))
            {
                if (chunk->hasComponent(exclude))
                {
                    continue;
                }
                ids.clear();
                for (const ComponentID id : chunk->aspect().getComponentIds())
                {
                    if (planOf(id).metadata != nullptr)
                    {
                        ids.push_back(id);
                    }
                }
                if (chunk->hasComponent(transformId) && !std::ranges::contains(ids, matrixId))
                {
                    ids.push_back(matrixId);
                }
                std::ranges::sort(ids);
                const auto [it, inserted] = groupByAspect.try_emplace(Aspect(ids), static_cast<u32>(groups.size()));
                if (inserted)
                {
                    groups.push_back({.ids = ids});
                }
                Group& group = groups[it->second];
                for (u32 row = 0; row < chunk->size(); ++row)
                {
                    group.rows.push_back({chunk, row});
                    const u32 index = chunk->entity(row).index();
                    if (index >= remap.byIndex.size())
                    {
                        remap.byIndex.resize(static_cast<sizet>(index) + 1);
                    }
                }
                total += static_cast<u32>(chunk->size());
            }
        }

        // ids in group order: group g's entities are clones[g.first, g.first + rows)
        const std::vector<Entity> clones = target.createHuskEntities(total);
        std::vector<u32> positionByCloneIndex;
        {
            SPITE_ZONE("Scene.clone.remap");
            u32 position = 0;
            for (Group& group : groups)
            {
                group.first = position;
                for (const SourceRow& row : group.rows)
                {
                    const Entity sourceEntity = row.chunk->entity(row.row);
                    const Entity clone = clones[position];
                    remap.byIndex[sourceEntity.index()] = {sourceEntity.id(), clone};
                    if (clone.index() >= positionByCloneIndex.size())
                    {
                        positionByCloneIndex.resize(static_cast<sizet>(clone.index()) + 1, ~0u);
                    }
                    positionByCloneIndex[clone.index()] = position++;
                }
            }
        }

        {
            SPITE_ZONE("Scene.clone.register");
            for (const Group& group : groups)
            {
                const auto fill = [&]
                {
                    // the destination archetype may hold entities of the target's own: positionByCloneIndex
                    // picks out the clones
                    std::vector<const ComponentPlan*> columnPlans(group.ids.size());
                    for (sizet k = 0; k < group.ids.size(); ++k)
                    {
                        const ComponentPlan& plan = planOf(group.ids[k]);
                        columnPlans[k] = plan.metadata != nullptr ? &plan : nullptr;
                    }
                    std::vector<int> sourceColumns(group.ids.size());
                    const Chunk* lastSource = nullptr;
                    for (Chunk* chunk : target.getArchetypeChunks(Aspect(group.ids)))
                    {
                        for (u32 row = 0; row < chunk->size(); ++row)
                        {
                            const u32 index = chunk->entity(row).index();
                            if (index >= positionByCloneIndex.size() || positionByCloneIndex[index] == ~0u)
                            {
                                continue;
                            }
                            const SourceRow& from = group.rows[positionByCloneIndex[index] - group.first];
                            if (from.chunk != lastSource)
                            {
                                lastSource = from.chunk;
                                for (sizet k = 0; k < group.ids.size(); ++k)
                                {
                                    sourceColumns[k] =
                                        columnPlans[k] ? from.chunk->getComponentIndex(group.ids[k]) : -1;
                                }
                            }
                            for (sizet k = 0; k < group.ids.size(); ++k)
                            {
                                if (sourceColumns[k] < 0)
                                {
                                    continue; // the added TransformMatrix keeps its constructed value
                                }
                                const sizet size = columnPlans[k]->metadata->size;
                                copyFields(columnPlans[k]->copies,
                                           static_cast<const u8*>(from.chunk->getConstComponentArrayByIndex(
                                               static_cast<sizet>(sourceColumns[k]))) +
                                               from.row * size,
                                           static_cast<u8*>(chunk->getComponentArrayByIndex(k)) + row * size, remap);
                            }
                        }
                    }
                };
                target.registerEntities(std::span(clones).subspan(group.first, group.rows.size()), group.ids, fill);
            }
        }
        if (remap.dangling != 0)
        {
            SWARNING_LOG("cloneScene: {} entity references outside the clone remapped to undefined", remap.dangling);
        }

        std::unordered_map<u64, Entity> idMap;
        {
            SPITE_ZONE("Scene.clone.hierarchy");
            idMap.reserve(total);
            for (const Group& group : groups)
            {
                for (u32 i = 0; i < group.rows.size(); ++i)
                {
                    const Entity sourceEntity = group.rows[i].chunk->entity(group.rows[i].row);
                    const Entity clone = clones[group.first + i];
                    idMap.emplace(sourceEntity.id(), clone);
                    const Entity parent = sourceTransforms.currentParentOf(sourceEntity);
                    const Entity parentClone = remap.find(parent);
                    if (parentClone.isValid() && !targetTransforms.attach(clone, parentClone))
                    {
                        SWARNING_LOG("cloneScene: entity {} could not attach under {} - skipped", clone.id(),
                                     parentClone.id());
                    }
                }
            }
        }
        targetTransforms.applyActivity();
        return idMap;
    }
} // namespace spite
