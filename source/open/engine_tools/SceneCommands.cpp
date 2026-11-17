#include "ecs/World.hpp"
#include "engine/SceneCommands.hpp"
#include "SceneLayerPlacement.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <unordered_set>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "animation/AnimationWorld.hpp"
#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "ecs/Chunk.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "profiling/Zone.hpp"
#include "scene/SceneApply.hpp"
#include "scene/SceneCollect.hpp"
#include "scene/SceneStream.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiComponents.hpp"
#include "world/DisabledComponent.hpp"
#include "world/EntityName.hpp"
#include "world/EntityTransformManager.hpp"
#include "world/TransformComponents.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;

        constexpr std::string_view OWNER = "engine";
        constexpr u64 DEFAULT_SUBTREE_LIMIT = 100;
        constexpr u64 MAX_SUBTREE_LIMIT = 10000;
        // the edit world Engine registers first; the only world a scene is saved from
        constexpr std::string_view SCENE_WORLD = "scene";

        struct Target
        {
            World world;
            EntityTransformManager* transforms = nullptr;
            bool sceneWorld = false; // the edit world, the only one with layers
        };

        // Releases the held root motion even when collecting throws
        struct RootMotionHold
        {
            explicit RootMotionHold(AnimationWorld* held) : animation(held)
            {
                if (animation)
                {
                    animation->holdRootMotion();
                }
            }
            RootMotionHold(const RootMotionHold&) = delete;
            RootMotionHold& operator=(const RootMotionHold&) = delete;
            ~RootMotionHold()
            {
                if (animation)
                {
                    animation->releaseRootMotion();
                }
            }
            AnimationWorld* animation;
        };

        // The edit world's preview moves entities by root motion; that drift is not scene data, so every document made
        // of the edit world (a save, an inverse, the collected edit scene) is collected with the positions the author
        // set. Any other world (play) holds real game state, so its document is never held: editWorld is false there.
        Expected<SceneDocument> collectDocument(World& world, EntityTransformManager& transforms,
                                                const std::span<const Entity> roots, const bool editWorld)
        {
            const RootMotionHold hold(editWorld && world.services().contains<AnimationWorld>()
                                          ? &world.services().write<AnimationWorld>().get()
                                          : nullptr);
            return collectScene(world, transforms, roots);
        }

        // A saved scene loaded additively into the edit world. Dirty is the edit world's undo history
        // (ControlServer::layerDirty): a layer is dirty while its token, which only a step that touched it moves, is
        // not the one at its save or load.
        struct SceneLayer
        {
            u32 id = 0;
            std::string path; // normalized, relative to the scene root
            u64 savedAtMs = 0; // unix ms of the last save, 0 = none since the load
        };

        struct UnsavedLayer
        {
            std::string path;
            std::string document;
            u32 id = 0;
            bool active = false;
            std::vector<std::string> incomingReferences;
        };

        // The scene open in the edit world  and its layers; the open scene is layer 0 of the
        // history's layer states.
        struct OpenScene
        {
            std::string path; // normalized, relative to the scene root; empty = untitled
            u64 savedAtMs = 0; // unix ms of the last save or open, 0 = none
            std::vector<SceneLayer> layers;
            u32 nextLayer = 1; // layer ids are never reused in a session; 0 is the base (the open scene)
            u32 active = 0; // the save target
            u64 ownershipRevision = 0;
            u64 focusRevision = 0;
            // files of layers unloaded with unsaved changes, which live on only in the undo history: openScene and
            // newScene, which end it, need force while one is listed; a save of the file or the undo of the unload
            // takes it off
            std::vector<std::string> unsavedUnloads;
            std::vector<UnsavedLayer> unsavedLayerData;
            u64 recoveryCommandGeneration = 0;
            bool restoredFromRecovery = false;
        };

        void dropUnsavedUnload(const std::filesystem::path& root, OpenScene& open, const std::string& path);

        SceneLayer* findLayer(OpenScene& open, const u32 id)
        {
            const auto it = std::ranges::find(open.layers, id, &SceneLayer::id);
            return it == open.layers.end() ? nullptr : &*it;
        }

        bool layerDirty(const ControlServer& control, const SceneLayer& layer)
        {
            return control.layerDirty(SCENE_WORLD, layer.id);
        }

        u32 rootLayer(const World& world, const Entity root)
        {
            const auto* tag = world.tryGetConstComponent<SceneLayerComponent>(root);
            return tag ? tag->layer : 0;
        }

        // only roots carry the tag; layer 0 (the base) is no tag
        void setRootLayer(const World& world, const Entity root, const u32 layer)
        {
            if (auto* tag = world.tryGetComponent<SceneLayerComponent>(root))
            {
                if (layer == 0)
                {
                    world.removeComponent<SceneLayerComponent>(root);
                    return;
                }
                tag->layer = layer;
            }
            else if (layer != 0)
            {
                world.addComponent<SceneLayerComponent>(root, SceneLayerComponent{.layer = layer});
            }
        }

        u32 layerOf(const World& world, const EntityTransformManager& transforms, Entity entity)
        {
            for (Entity parent = transforms.attachedParentOf(entity); parent.isValid();
                 parent = transforms.attachedParentOf(entity))
            {
                entity = parent;
            }
            return rootLayer(world, entity);
        }

        // A root-making command's "layer" arg, refused beside a parent (the entity then joins its parent's layer).
        std::string rootLayerArg(OpenScene& open, const ControlArgs& args, const Entity parent, const u32 fallback,
                                 u32& out);

        // For redo: the request's args (keys) with the layer it resolved, so a later setActiveSaveTarget does not
        // move the redone roots. Not while replaying, whose args already carry it.
        void forwardLayer(ControlServer& control, const ControlArgs& args,
                          const std::initializer_list<const char*> keys, const u32 layer)
        {
            if (control.replayingHistory())
            {
                return;
            }
            Json forward{{"world", std::string(SCENE_WORLD)}, {"layer", layer}};
            for (const char* key : keys)
            {
                if (const std::optional<std::string> value = args.json(key))
                {
                    forward[key] = Json::parse(*value);
                }
            }
            control.recordForward(forward.dump());
        }

        // The optional "layer" arg: 0 or a loaded layer; fallback when absent.
        std::string layerArg(OpenScene& open, const ControlArgs& args, const u32 fallback, u32& out)
        {
            const std::optional<u64> layer = args.integer("layer");
            out = layer ? static_cast<u32>(*layer) : fallback;
            if (layer && (*layer > UINT32_MAX || (*layer != 0 && !findLayer(open, out))))
            {
                return "no scene layer " + std::to_string(*layer) + " (sceneLayers lists them)";
            }
            return {};
        }

        u64 nowMs()
        {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
        }

        bool isDirty(const ControlServer& control) { return control.layerDirty(SCENE_WORLD, 0); }

        void markSaved(ControlServer& control, OpenScene& open)
        {
            control.markLayerSaved(SCENE_WORLD, 0);
            open.savedAtMs = nowMs();
        }

        std::string normalizedPath(const std::string_view path)
        {
            return utf8Path(pathFromUtf8(path).lexically_normal());
        }

        // Whether two normalized scene paths name one file: the same file when both exist, else the same name (ASCII
        // case-insensitive: the file systems of Windows are).
        bool samePath(const std::filesystem::path& root, const std::string& a, const std::string& b)
        {
            if (a == b)
            {
                return true;
            }
            if (a.empty() || b.empty())
            {
                return false;
            }
            std::error_code error;
            if (std::filesystem::exists(root / a, error) && std::filesystem::exists(root / b, error))
            {
                const bool same = std::filesystem::equivalent(root / a, root / b, error);
                if (!error)
                {
                    return same;
                }
            }
            return std::ranges::equal(
                a, b, [](const char x, const char y)
                { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
        }

        std::string resolveTarget(ControlServer& control, const ControlArgs& args, Target& out)
        {
            const World* world = control.world(args);
            if (!world)
            {
                return control.unknownWorld(args);
            }
            if (!world->services().contains<EntityTransformManager>())
            {
                return "world has no transform hierarchy";
            }
            out.world = *world;
            out.transforms = &world->services().write<EntityTransformManager>().get();
            out.sceneWorld = control.worldName(args) == SCENE_WORLD;
            return {};
        }

        std::string rootLayerArg(OpenScene& open, const ControlArgs& args, const Entity parent, const u32 fallback,
                                 u32& out)
        {
            if (parent.isValid() && args.integer("layer"))
            {
                return std::string("'layer' applies only to a root: an entity under a parent is in its parent's layer");
            }
            return layerArg(open, args, fallback, out);
        }

        // The live, non-UI entity the integer arg key names.
        std::string sceneEntity(const World& world, const ControlArgs& args, const std::string_view key, Entity& out)
        {
            const std::optional<u64> id = args.integer(key);
            if (!id)
            {
                return "missing integer arg '" + std::string(key) + "'";
            }
            out = Entity(*id);
            if (!world.isEntityAlive(out))
            {
                return "no live entity " + std::to_string(*id);
            }
            if (world.hasComponent<UiIdentityComponent>(out))
            {
                return "entity " + std::to_string(*id) + " is a UI element; its UI owns it";
            }
            return {};
        }

        // The optional "parent" arg: undefined when absent, a live authored entity otherwise.
        std::string parentEntity(const World& world, const ControlArgs& args, Entity& out)
        {
            out = Entity::undefined();
            if (!args.json("parent"))
            {
                return {};
            }
            if (std::string error = sceneEntity(world, args, "parent", out); !error.empty())
            {
                return error;
            }
            return {};
        }

        struct HierarchyPromotion
        {
            Entity entity;
            bool transform;
            bool matrix;

            HierarchyPromotion(const World& world, const Entity entity) :
                entity(entity), transform(entity.isValid() && !world.hasComponent<TransformComponent>(entity)),
                matrix(entity.isValid() && !world.hasComponent<TransformMatrixComponent>(entity))
            {
            }

            void record(ControlServer& control) const
            {
                for (const auto& [needed, name] :
                     {std::pair{transform, "TransformComponent"}, std::pair{matrix, "TransformMatrixComponent"}})
                {
                    if (needed)
                    {
                        control.recordInverse("removeComponent", Json{{"id", entity.id()}, {"component", name}}.dump());
                    }
                }
            }
        };

        void writeParent(const std::string_view key, const Entity parent, IFormatWriter& out)
        {
            if (parent.isValid())
            {
                out.writeU64(key, parent.id());
            }
        }

        // JSON is the saved form; ".spsd" asks for the same-build binary cache
        bool binaryScenePath(const std::string_view path) { return path.ends_with(".spsd"); }

        // what loading met against the current schemas: defaulted, dropped,
        // unreadable fields and skipped components, one entry per kind and field
        void writeChanges(IFormatWriter& out, const SceneDocument& document, const ApplyResult& applied)
        {
            out.beginArray("changes", static_cast<u32>(document.changes.size() + applied.changes.size()));
            for (const std::vector<SceneChange>* changes : {&document.changes, &applied.changes})
            {
                for (const SceneChange& change : *changes)
                {
                    out.beginObject({});
                    out.writeString("component", change.component);
                    if (!change.field.empty())
                    {
                        out.writeString("field", change.field);
                    }
                    out.writeString("change", sceneChangeName(change.kind));
                    out.writeU32("count", change.count);
                    if (!change.detail.empty())
                    {
                        out.writeString("detail", change.detail);
                    }
                    out.endObject();
                }
            }
            out.endArray();
        }

        void recordCreated(ControlServer& control, const SceneDocument& document, const ApplyResult& applied)
        {
            for (u32 i = 0; i < document.entities.size(); ++i)
            {
                if (const auto it = applied.idMap.find(document.entities[i].sourceId); it != applied.idMap.end())
                {
                    control.recordCreated(i, it->second);
                }
            }
        }

        // The inverse that re-applies document, collected from live entities
        // about to die, under parent: command takes "document" and "parent".
        // extraArgs: more members of the args object, each with a leading comma.
        void recordReapply(ControlServer& control, const std::string_view command, const SceneDocument& document,
                           const Entity parent, const std::string_view extraArgs = {})
        {
            std::string args = R"({"document":)";
            std::string text;
            emitSceneJson(document, text);
            args += text;
            if (parent.isValid())
            {
                args += R"(,"parent":)" + std::to_string(parent.id());
            }
            args += extraArgs;
            args += '}';
            std::vector<u64> revives;
            revives.reserve(document.entities.size());
            for (const DocumentEntity& entity : document.entities)
            {
                revives.push_back(entity.sourceId);
            }
            control.recordInverse(command, args, revives);
        }

        bool hasEntityFields(const StructSchema& schema)
        {
            return std::ranges::any_of(
                schema.fields,
                [](const FieldDesc& field)
                {
                    return field.kind == FieldKind::Entity || field.kind == FieldKind::EntityList ||
                        (field.kind == FieldKind::Struct && hasEntityFields(*field.structSchema));
                });
        }

        // An inverse set for each entity field, outside the doomed entities, that names one of them
        // (an EntityList is restored whole): undo forwards the dead ids to the revived ones.
        struct ReferenceScan
        {
            const std::unordered_set<u64>& doomed;
            std::vector<std::pair<std::string, std::string>> found; // {path, value JSON} of the entity being scanned
            const World* outside = nullptr;

            bool matches(const Entity entity) const
            {
                return outside ? !doomed.contains(entity.id()) && outside->isEntityAlive(entity)
                               : doomed.contains(entity.id());
            }

            void field(const FieldDesc& desc, const std::byte* data, const std::string& path)
            {
                if (outside && !(desc.flags & fieldFlags::PERSISTED))
                    return;
                const u32 count = desc.arrayCount == 0 ? 1 : desc.arrayCount;
                for (u32 i = 0; i < count; ++i)
                {
                    const std::byte* element = data + i * desc.elementSize;
                    const std::string at = desc.arrayCount == 0 ? path : path + "[" + std::to_string(i) + "]";
                    switch (desc.kind)
                    {
                    case FieldKind::Entity:
                        if (const Entity entity = *reinterpret_cast<const Entity*>(element); matches(entity))
                        {
                            found.emplace_back(at, std::to_string(entity.id()));
                        }
                        break;
                    case FieldKind::EntityList:
                        {
                            const auto& list = *reinterpret_cast<const std::vector<Entity>*>(element);
                            if (std::ranges::any_of(list, [this](const Entity e) { return matches(e); }))
                            {
                                std::string ids = "[";
                                for (const Entity e : list)
                                {
                                    ids += (ids.size() > 1 ? "," : "") + std::to_string(e.id());
                                }
                                found.emplace_back(at, ids + "]");
                            }
                            break;
                        }
                    case FieldKind::Struct:
                        for (const FieldDesc& inner : desc.structSchema->fields)
                        {
                            field(inner, element + inner.offset, at + "." + std::string(inner.name));
                        }
                        break;
                    default:
                        break;
                    }
                }
            }
        };

        std::vector<std::string> outsideReferenceInverses(const World& world, const std::vector<Entity>& doomedList)
        {
            const std::unordered_set<u64> doomed = [&]
            {
                std::unordered_set<u64> ids;
                for (const Entity entity : doomedList)
                {
                    ids.insert(entity.id());
                }
                return ids;
            }();
            std::vector<ComponentID> ids(getRegisteredComponentCount());
            ids.resize(std::min<sizet>(ids.size(), getComponentIds(ids.data(), static_cast<u32>(ids.size()))));
            const ComponentID uiElement = getComponentId<UiIdentityComponent>();
            std::vector<std::string> inverses;
            ReferenceScan scan{doomed, {}};
            for (const ComponentID id : ids)
            {
                const ComponentMetadata& metadata = getComponentMetadata(id);
                // UI components belong to their UiContext; scene entities never carry them
                if (!metadata.schema || metadata.name.toView().starts_with("Ui") || !hasEntityFields(*metadata.schema))
                {
                    continue;
                }
                const std::string prefix(metadata.name.toView());
                for (const Chunk* chunk : world.getChunksContaining(Aspect(id)))
                {
                    if (chunk->hasComponent(uiElement))
                    {
                        continue;
                    }
                    for (const Entity entity : chunk->entities())
                    {
                        if (doomed.contains(entity.id()))
                        {
                            continue;
                        }
                        const auto* data = static_cast<const std::byte*>(world.getConstComponentDataById(entity, id));
                        scan.found.clear();
                        for (const FieldDesc& field : metadata.schema->fields)
                        {
                            scan.field(field, data + field.offset, prefix + "." + std::string(field.name));
                        }
                        for (auto& [path, value] : scan.found)
                        {
                            inverses.push_back(R"({"id":)" + std::to_string(entity.id()) + R"(,"path":")" + path +
                                               R"(","value":)" + value + "}");
                        }
                    }
                }
            }
            return inverses;
        }

        void recordDelete(ControlServer& control, const Entity entity)
        {
            control.recordInverse("deleteEntity", R"({"id":)" + std::to_string(entity.id()) + "}");
        }

        std::vector<Entity> nonUiEntities(const World& world)
        {
            std::vector<Entity> entities;
            const ComponentID uiElement = getComponentId<UiIdentityComponent>();
            for (const Chunk* chunk : world.getChunksContaining(Aspect{}))
            {
                if (!chunk->hasComponent(uiElement))
                {
                    const auto chunkEntities = chunk->entities();
                    entities.insert(entities.end(), chunkEntities.begin(), chunkEntities.end());
                }
            }
            return entities;
        }

        // Orders entities to destroy so the next creations reuse the lowest freed indices first: a re-applied
        // document then gets ascending indices in document order.
        void freeForReuseInOrder(std::vector<Entity>& entities)
        {
            std::ranges::sort(entities, [](const Entity a, const Entity b) { return a.index() > b.index(); });
        }

        std::string createEntity(ControlServer& control, OpenScene& open, const ControlArgs& args, IFormatWriter& out)
        {
            Target target;
            if (std::string error = resolveTarget(control, args, target); !error.empty())
            {
                return error;
            }
            const World& world = target.world;
            const std::optional<std::string> name = args.string("name");
            if (name && name->empty())
            {
                return "name must not be empty";
            }
            Entity parent;
            if (std::string error = parentEntity(world, args, parent); !error.empty())
            {
                return error;
            }
            u32 layer = 0;
            if (std::string error = rootLayerArg(open, args, parent, open.active, layer); !error.empty())
            {
                return error;
            }

            const HierarchyPromotion promotion(world, parent);
            const ComponentID aspect[] = {getComponentId<TransformComponent>(),
                                          getComponentId<TransformMatrixComponent>(),
                                          getComponentId<EntityNameComponent>()};
            const Entity entity = world.createEntity(std::span<const ComponentID>(aspect, name ? 3 : 2));
            if (name)
            {
                world.getComponent<EntityNameComponent>(entity).name = InternedString(*name);
            }
            if (parent.isValid())
            {
                target.transforms->attach(entity, parent);
            }
            else if (target.sceneWorld)
            {
                setRootLayer(world, entity, layer);
                forwardLayer(control, args, {"name"}, layer);
            }
            control.recordCreated(0, entity);
            recordDelete(control, entity);
            promotion.record(control);
            out.writeU64("id", entity.id());
            if (name)
            {
                out.writeString("name", *name);
            }
            writeParent("parent", parent, out);
            return {};
        }

        std::string deleteEntity(ControlServer& control, OpenScene& open, const ControlArgs& args, IFormatWriter& out)
        {
            Target target;
            if (std::string error = resolveTarget(control, args, target); !error.empty())
            {
                return error;
            }
            Entity entity;
            if (std::string error = sceneEntity(target.world, args, "id", entity); !error.empty())
            {
                return error;
            }
            EntityTransformManager& transforms = *target.transforms;
            // answered from the relation store: k commands in one pump cost no index rebuild
            const Entity parent = transforms.currentParentOf(entity);
            std::vector<Entity> doomed;
            transforms.collectSubtree(entity, doomed);
            // the inverse re-applies the subtree's scene data (names included):
            // outside references come back as sets; view-camera bindings do not
            const Entity roots[] = {entity};
            const Expected<SceneDocument> document =
                collectDocument(target.world, transforms, roots, target.sceneWorld);
            if (!document)
            {
                return document.error().message.c_str();
            }
            const std::vector<std::string> outside = outsideReferenceInverses(target.world, doomed);
            // The free list hands out the last-freed index first and undo re-creates the subtree in document
            // order, so freeing in reverse gives every entity its own index back: siblings keep their order.
            // a root comes back in its layer, the open scene included: the inverse must not fall back to the active one
            const bool layered = target.sceneWorld && !parent.isValid();
            const u32 layer = layered ? rootLayer(target.world, entity) : 0;
            control.touchLayer(target.sceneWorld ? layerOf(target.world, transforms, entity) : 0);
            std::ranges::reverse(doomed);
            target.world.destroyEntities(doomed);
            recordReapply(control, "instantiate", *document, parent,
                          layered ? R"(,"layer":)" + std::to_string(layer) : std::string());
            for (const std::string& inverse : outside)
            {
                control.recordInverse("set", inverse);
            }
            out.writeU64("id", entity.id());
            writeParent("parent", parent, out);
            out.writeU32("deleted", static_cast<u32>(doomed.size()));
            return {};
        }

        // the copy carries the scene data (persisted components), names included
        std::string duplicateEntity(ControlServer& control, OpenScene& open, const ControlArgs& args,
                                    IFormatWriter& out)
        {
            Target target;
            if (std::string error = resolveTarget(control, args, target); !error.empty())
            {
                return error;
            }
            Entity entity;
            if (std::string error = sceneEntity(target.world, args, "id", entity); !error.empty())
            {
                return error;
            }
            EntityTransformManager& transforms = *target.transforms;
            const Entity parent = transforms.currentParentOf(entity);
            const Entity roots[] = {entity};
            const Expected<SceneDocument> document =
                collectDocument(target.world, transforms, roots, target.sceneWorld);
            if (!document)
            {
                return document.error().message.c_str();
            }
            const Expected<ApplyResult> copy = applyScene(*document, target.world, transforms, parent);
            if (!copy)
            {
                return copy.error().message.c_str();
            }
            if (!parent.isValid())
            {
                setRootLayer(target.world, copy->roots.front(), rootLayer(target.world, entity));
            }
            recordCreated(control, *document, *copy);
            recordDelete(control, copy->roots.front());
            control.touchLayer(target.sceneWorld ? layerOf(target.world, transforms, entity) : 0);
            out.writeU64("id", copy->roots.front().id());
            out.writeU64("source", entity.id());
            writeParent("parent", parent, out);
            out.writeU32("entities", static_cast<u32>(document->entities.size()));
            return {};
        }

        // keeps the local transform, so the inverse restores the pose exactly
        std::string reparent(ControlServer& control, OpenScene& open, const ControlArgs& args, IFormatWriter& out)
        {
            Target target;
            if (std::string error = resolveTarget(control, args, target); !error.empty())
            {
                return error;
            }
            const World& world = target.world;
            Entity entity;
            if (std::string error = sceneEntity(world, args, "id", entity); !error.empty())
            {
                return error;
            }
            Entity parent;
            if (std::string error = parentEntity(world, args, parent); !error.empty())
            {
                return error;
            }
            if (parent == entity)
            {
                return std::string("an entity cannot be its own parent");
            }
            // the relation store answers without a compile, so a batch of
            // reparents costs no rebuilds
            EntityTransformManager& transforms = *target.transforms;
            const Entity previous = transforms.attachedParentOf(entity);
            // an entity keeps its layer when it becomes a root; the inverse names the one a root had
            const u32 previousLayer = target.sceneWorld ? layerOf(world, transforms, entity) : 0;
            u32 layer = 0;
            if (std::string error = rootLayerArg(open, args, parent, previousLayer, layer); !error.empty())
            {
                return error;
            }
            const HierarchyPromotion childPromotion(world, entity);
            const HierarchyPromotion parentPromotion(world, parent);
            if (parent.isValid())
            {
                if (!transforms.attach(entity, parent))
                {
                    return "parent " + std::to_string(parent.id()) + " is in the subtree of entity " +
                        std::to_string(entity.id());
                }
                setRootLayer(world, entity, 0);
            }
            else
            {
                transforms.detach(entity);
                if (target.sceneWorld)
                {
                    setRootLayer(world, entity, layer);
                }
            }
            // declared, so the layer it left counts as well as the one it joined
            control.touchLayer(previousLayer);
            const auto destination = target.sceneWorld ? layerOf(world, transforms, entity) : 0;
            control.touchLayer(destination);
            if (destination != previousLayer)
                ++open.ownershipRevision;
            std::string inverse = R"({"id":)" + std::to_string(entity.id());
            if (previous.isValid())
            {
                inverse += R"(,"parent":)" + std::to_string(previous.id());
            }
            else if (target.sceneWorld)
            {
                // 0 too: without it the inverse would take the layer of the parent the entity is still under
                inverse += R"(,"layer":)" + std::to_string(previousLayer);
            }
            control.recordInverse("reparent", inverse + "}");
            if (parent.isValid())
            {
                childPromotion.record(control);
                parentPromotion.record(control);
            }
            out.writeU64("id", entity.id());
            writeParent("parent", parent, out);
            writeParent("previous", previous, out);
            return {};
        }

        // the entity's own flag; its subtree follows through the manager (active = enabled and every
        // ancestor enabled). An unchanged value records nothing: no undo step, no dirty layer.
        std::string setEntityEnabled(ControlServer& control, OpenScene&, const ControlArgs& args, IFormatWriter& out)
        {
            Target target;
            if (std::string error = resolveTarget(control, args, target); !error.empty())
            {
                return error;
            }
            Entity entity;
            if (std::string error = sceneEntity(target.world, args, "id", entity); !error.empty())
            {
                return error;
            }
            const std::optional<bool> enabled = args.boolean("enabled");
            if (!enabled)
            {
                return std::string("setEntityEnabled needs enabled (true or false)");
            }
            EntityTransformManager& transforms = *target.transforms;
            const bool previous = transforms.isEntityEnabled(entity);
            if (previous != *enabled)
            {
                transforms.setEntityEnabled(entity, *enabled);
                control.touchLayer(target.sceneWorld ? layerOf(target.world, transforms, entity) : 0);
                control.recordInverse("setEntityEnabled",
                                      R"({"id":)" + std::to_string(entity.id()) + R"(,"enabled":)" +
                                          (previous ? "true" : "false") + "}");
            }
            out.writeU64("id", entity.id());
            out.writeBool("enabled", *enabled);
            out.writeBool("previous", previous);
            out.writeBool("active", transforms.isEntityActive(entity));
            return {};
        }

        // The "path" arg resolved under the scene root.
        std::string scenePath(const ControlServer& control, const ControlArgs& args, std::string& outPath,
                              std::filesystem::path& outResolved)
        {
            const std::optional<std::string> path = args.string("path");
            if (!path)
            {
                return "missing arg 'path'";
            }
            outPath = *path;
            return control.resolveFile("scene", *path, outResolved);
        }

        // The scene edit world a scene command acts on: scenes are edit-world assets.
        std::string sceneTarget(ControlServer& control, const ControlArgs& args, const std::string_view command,
                                Target& out)
        {
            const std::string worldName = control.worldName(args);
            if (worldName.empty())
            {
                return control.unknownWorld(args);
            }
            if (worldName != SCENE_WORLD)
            {
                return "scenes are edit-world assets: " + std::string(command) + " acts only on world '" +
                    std::string(SCENE_WORLD) + "', not '" + worldName + "' (play and UI worlds hold runtime state)";
            }
            return resolveTarget(control, args, out);
        }

        std::string writeSceneFile(const std::filesystem::path& file, const std::string_view path,
                                   const SceneDocument& document, sizet& bytes)
        {
            Expected<void> written = ensureDirectory(file.parent_path());
            if (written && !binaryScenePath(path))
            {
                std::string text;
                emitSceneJson(document, text);
                bytes = text.size();
                written = writeTextFileAtomically(file, text);
            }
            else if (written)
            {
                std::vector<u8> binary;
                emitSceneBinary(document, binary);
                bytes = binary.size();
                written = writeBinaryFileAtomically(
                    file, std::span(reinterpret_cast<const std::byte*>(binary.data()), binary.size()));
            }
            return written ? std::string{}
                           : "cannot write '" + std::string(path) + "': " + written.error().message.c_str();
        }

        // The layer (0: the open scene) whose file a normalized path names, if any.
        std::optional<u32> layerOwning(const std::filesystem::path& root, const OpenScene& open,
                                       const std::string& normalized)
        {
            if (samePath(root, normalized, open.path))
            {
                return 0u;
            }
            for (const SceneLayer& layer : open.layers)
            {
                if (!layer.path.empty() && samePath(root, normalized, layer.path))
                {
                    return layer.id;
                }
            }
            return std::nullopt;
        }

        // The roots a layer owns: tagged with it, or for the base (0) untagged.
        std::vector<Entity> layerRoots(const Target& target, const u32 layer)
        {
            std::vector<Entity> roots;
            const auto isRoot = [&](const Entity entity)
            { return !target.transforms->attachedParentOf(entity).isValid(); };
            if (layer != 0)
            {
                for (const Chunk* chunk :
                     target.world.getChunksContaining(Aspect(getComponentId<SceneLayerComponent>())))
                {
                    for (const Entity entity : chunk->entities())
                    {
                        if (rootLayer(target.world, entity) == layer && isRoot(entity))
                        {
                            roots.push_back(entity);
                        }
                    }
                }
                return roots;
            }
            for (const Entity entity : nonUiEntities(target.world))
            {
                if (rootLayer(target.world, entity) == 0 && isRoot(entity))
                {
                    roots.push_back(entity);
                }
            }
            return roots;
        }

        // The entities of roots' subtrees, roots first.
        std::vector<Entity> subtrees(const Target& target, const std::vector<Entity>& roots)
        {
            std::vector<Entity> entities;
            std::vector<Entity> subtree;
            for (const Entity root : roots)
            {
                target.transforms->collectSubtree(root, subtree);
                entities.insert(entities.end(), subtree.begin(), subtree.end());
            }
            return entities;
        }

        enum class SaveMode : u8
        {
            Save, // the open scene's file, or a copy at an explicit path
            SaveAs, // an explicit path, which becomes the open scene's
        };

        bool reportExternalReference(const World& world, const SceneDocument& document, IFormatWriter& out)
        {
            std::unordered_set<u64> members;
            for (const auto& entity : document.entities)
                members.insert(entity.sourceId);
            ReferenceScan scan{members, {}, &world};
            for (const auto& entity : document.entities)
            {
                for (const auto& component : entity.components)
                {
                    const ComponentID id = getComponentIdByName(component.typeName);
                    const auto& metadata = getComponentMetadata(id);
                    if (!hasEntityFields(*metadata.schema))
                        continue;
                    const auto* data =
                        static_cast<const std::byte*>(world.getConstComponentDataById(Entity(entity.sourceId), id));
                    for (const auto& field : metadata.schema->fields)
                    {
                        scan.field(field, data + field.offset,
                                   std::string(component.typeName) + "." + std::string(field.name));
                        if (!scan.found.empty())
                        {
                            out.writeU64("externalReferenceEntity", entity.sourceId);
                            out.writeString("externalReferenceField", scan.found.front().first);
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        std::string saveScene(ControlServer& control, const std::filesystem::path& root, OpenScene& open,
                              const SaveMode mode, const ControlArgs& args, IFormatWriter& out)
        {
            Target target;
            const std::string_view name = mode == SaveMode::Save ? "saveScene" : "saveSceneAs";
            if (std::string error = sceneTarget(control, args, name, target); !error.empty())
            {
                return error;
            }
            if (args.json("id"))
            {
                if (args.json("layer"))
                    return "saveScene takes id or layer, not both";
                Entity entity;
                if (std::string error = sceneEntity(target.world, args, "id", entity); !error.empty())
                {
                    return error;
                }
                std::string path;
                std::filesystem::path file;
                if (std::string error = scenePath(control, args, path, file); !error.empty())
                {
                    return error;
                }
                if (layerOwning(root, open, normalizedPath(path)))
                {
                    return "snapshot destination backs an open scene or loaded layer; choose another path";
                }
                if (!args.boolean("overwrite").value_or(false) && std::filesystem::exists(file))
                {
                    return "'" + path + "' already exists: pass overwrite=true to replace it";
                }
                const Expected<SceneDocument> document =
                    collectDocument(target.world, *target.transforms, std::span(&entity, 1), true);
                if (!document)
                {
                    return document.error().message.c_str();
                }
                sizet bytes = 0;
                if (std::string error = writeSceneFile(file, path, *document, bytes); !error.empty())
                {
                    return error;
                }
                out.writeU64("id", entity.id());
                out.writeString("path", normalizedPath(path));
                out.writeString("file", utf8Path(std::filesystem::absolute(file)));
                out.writeU32("entities", static_cast<u32>(document->entities.size()));
                out.writeU64("bytes", bytes);
                out.writeBool("openScene", false);
                return {};
            }
            // the active layer is the save target; 0 is the open scene
            u32 targetLayer = 0;
            if (std::string error = layerArg(open, args, open.active, targetLayer); !error.empty())
                return error;
            SceneLayer* layer = findLayer(open, targetLayer);
            const std::string& targetPath = layer ? layer->path : open.path;
            std::string path;
            std::filesystem::path file;
            if (mode == SaveMode::Save && !args.json("path"))
            {
                if (targetPath.empty())
                {
                    return "the scene is untitled: saveSceneAs path=<file> names it";
                }
                path = targetPath;
                file = root / path;
            }
            else if (std::string error = scenePath(control, args, path, file); !error.empty())
            {
                return error;
            }
            const std::string normalized = normalizedPath(path);
            // a save to the open scene's own file is the Save; any other file is a copy until SaveAs adopts it
            const bool openFile = samePath(root, normalized, targetPath);
            const bool adopts = mode == SaveMode::SaveAs || openFile;
            if (const std::optional<u32> owner = layerOwning(root, open, normalized); owner && *owner != targetLayer)
            {
                return "'" + path + "' is the file of scene layer " + std::to_string(*owner) +
                    ": setActiveSaveTarget layer=" + std::to_string(*owner) + " saves it";
            }
            if (mode == SaveMode::SaveAs && !openFile && !args.boolean("overwrite").value_or(false) &&
                std::filesystem::exists(file))
            {
                return "'" + path + "' already exists: pass overwrite=true to replace it";
            }
            const Expected<SceneDocument> document = open.layers.empty()
                ? collectWorldScene(target.world, *target.transforms)
                : collectDocument(target.world, *target.transforms, layerRoots(target, targetLayer), true);
            if (!document)
            {
                return document.error().message.c_str();
            }
            if (!open.layers.empty() && !args.boolean("allowExternalReferences").value_or(false) &&
                reportExternalReference(target.world, *document, out))
                return "scene has entity references outside its file that will not survive reload; "
                       "use allowExternalReferences=true to save anyway";
            sizet bytes = 0;
            if (std::string error = writeSceneFile(file, path, *document, bytes); !error.empty())
            {
                return error;
            }
            if (adopts && layer)
            {
                layer->path = normalized;
                control.markLayerSaved(SCENE_WORLD, layer->id);
                layer->savedAtMs = nowMs();
            }
            else if (adopts)
            {
                open.path = normalized;
                markSaved(control, open);
            }
            dropUnsavedUnload(root, open, normalized);
            out.writeU32("layer", targetLayer);
            out.writeString("path", normalized);
            out.writeString("file", utf8Path(std::filesystem::absolute(file)));
            out.writeU32("entities", static_cast<u32>(document->entities.size()));
            out.writeU64("bytes", bytes);
            out.writeBool("openScene", adopts && !layer);
            return {};
        }

        // The document the args name: a file under root (sets path) or, when allowed, an inline "document".
        std::string readSceneDocument(const ControlServer& control, const ControlArgs& args, const bool inlineOk,
                                      std::string& path, SceneDocument& document)
        {
            if (const std::optional<std::string> inlineDocument = inlineOk ? args.json("document") : std::nullopt)
            {
                Expected<SceneDocument> parsed = parseSceneJson(*inlineDocument);
                if (!parsed)
                {
                    return std::string("cannot parse document: ") + parsed.error().message.c_str();
                }
                document = std::move(*parsed);
                return {};
            }
            std::filesystem::path file;
            if (std::string error = scenePath(control, args, path, file); !error.empty())
            {
                return error;
            }
            const Expected<std::vector<std::byte>> bytes = readBinaryFile(file);
            if (!bytes)
            {
                return "cannot read '" + path + "': " + bytes.error().message.c_str();
            }
            const auto* raw = reinterpret_cast<const u8*>(bytes->data());
            const bool binary = bytes->size() >= sizeof(SCENE_BINARY_MAGIC) &&
                std::memcmp(raw, SCENE_BINARY_MAGIC, sizeof(SCENE_BINARY_MAGIC)) == 0;
            Expected<SceneDocument> parsed = binary
                ? parseSceneBinary(std::span(raw, bytes->size()))
                : parseSceneJson(std::string_view(reinterpret_cast<const char*>(raw), bytes->size()));
            if (!parsed)
            {
                return "cannot parse '" + path + "': " + parsed.error().message.c_str();
            }
            document = std::move(*parsed);
            return {};
        }

        // Destroys the world's scene entities, lowest index freed last so a re-applied document gets ascending
        // indices in document order. Returns how many.
        u32 clearScene(Target& target)
        {
            std::vector<Entity> doomed = nonUiEntities(target.world);
            freeForReuseInOrder(doomed);
            target.world.destroyEntities(doomed);
            return static_cast<u32>(doomed.size());
        }

        // Replaces the world's scene entities with a parsed document: only now is the current scene destroyed,
        // collected first into previous (the inverse of a load). A document that fails to apply gets the previous
        // scene back (new ids) and the error says so.
        std::string replaceScene(Target& target, const SceneLoadedFn& sceneLoaded, const SceneDocument& document,
                                 SceneDocument& previous, ApplyResult& loaded, u32& deleted)
        {
            Expected<SceneDocument> collected = collectWorldScene(target.world, *target.transforms);
            if (!collected)
            {
                return collected.error().message.c_str();
            }
            previous = std::move(*collected);
            deleted = clearScene(target);
            Expected<ApplyResult> applied = applyScene(document, target.world, *target.transforms, Entity::undefined());
            if (!applied)
            {
                // a parsed document should apply; if it does not, the world
                // gets its scene back (new ids) rather than staying empty
                clearScene(target);
                const Expected<ApplyResult> restored =
                    applyScene(previous, target.world, *target.transforms, Entity::undefined());
                if (restored && sceneLoaded)
                {
                    sceneLoaded(target.world, *restored);
                }
                return std::string(applied.error().message.c_str()) +
                    (restored ? "; the previous scene was re-applied with new entity ids"
                              : "; re-applying the previous scene failed too");
            }
            if (sceneLoaded)
            {
                sceneLoaded(target.world, *applied);
            }
            loaded = std::move(*applied);
            return {};
        }

        std::string loadScene(ControlServer& control, const SceneLoadedFn& sceneLoaded, OpenScene& open,
                              const ControlArgs& args, IFormatWriter& out)
        {
            Target target;
            if (std::string error = resolveTarget(control, args, target); !error.empty())
            {
                return error;
            }
            // the replaced entities would include the layers' and come back untagged, in the open scene
            if (target.sceneWorld && !open.layers.empty())
            {
                return std::string("scene layers are loaded: unloadSceneLayer them first, or openScene replaces the "
                                   "open scene and its layers");
            }
            std::string path;
            SceneDocument document;
            if (std::string error = readSceneDocument(control, args, true, path, document); !error.empty())
            {
                return error;
            }
            SceneDocument previous;
            ApplyResult loaded;
            u32 deleted = 0;
            if (std::string error = replaceScene(target, sceneLoaded, document, previous, loaded, deleted);
                !error.empty())
            {
                return error;
            }
            recordCreated(control, document, loaded);
            control.touchLayer(0);
            recordReapply(control, "loadScene", previous, Entity::undefined());
            if (!path.empty())
            {
                out.writeString("path", path);
            }
            out.writeU32("deleted", deleted);
            out.writeU32("entities", static_cast<u32>(document.entities.size()));
            out.writeU32("skippedComponents", document.skippedComponents + loaded.skippedComponents);
            writeChanges(out, document, loaded);
            return {};
        }

        // The guards of the commands that swap the open scene.
        std::string replaceGuard(ControlServer& control, OpenScene& open, const ControlArgs& args)
        {
            if (control.world("play"))
            {
                return "Play is active: stop it before replacing the open scene";
            }
            if (args.boolean("force").value_or(false))
            {
                return {};
            }
            std::string dirtyLayers;
            u32 dirtyCount = 0;
            for (const SceneLayer& layer : open.layers)
            {
                if (layerDirty(control, layer))
                {
                    dirtyLayers += (dirtyCount++ ? ", " : "") + std::string("scene layer ") + std::to_string(layer.id) +
                        " ('" + layer.path + "')";
                }
            }
            const char* keep = ": setActiveSaveTarget and saveScene keep them, force=true discards them";
            if (isDirty(control))
            {
                return std::string("scene has unsaved changes") +
                    (dirtyCount ? ", as " + std::string(dirtyCount == 1 ? "has " : "have ") + dirtyLayers : "") + keep;
            }
            if (dirtyCount)
            {
                return dirtyLayers + (dirtyCount == 1 ? " has" : " have") + " unsaved changes" + keep;
            }
            if (!open.unsavedUnloads.empty())
            {
                return "scene layer '" + open.unsavedUnloads.front() +
                    "' was unloaded with unsaved changes: undo loads it back while it is in the history, force=true "
                    "discards it";
            }
            return {};
        }

        // A replaced scene is a boundary: the stored inverses name entities that are gone, so the history ends here.
        // keepDirty: the world still holds the scene it had (a failed replace), so it stays as dirty as it was.
        void adoptScene(ControlServer& control, OpenScene& open, std::string path, const bool keepDirty)
        {
            // the replace destroyed every layer's entities; a failed one re-applied them untagged, in the open scene,
            // which then differs from its file
            const bool wasDirty = isDirty(control) || !open.layers.empty();
            control.clearHistory(SCENE_WORLD);
            open.path = std::move(path);
            open.layers.clear();
            open.active = 0;
            open.unsavedUnloads.clear();
            open.unsavedLayerData.clear();
            open.restoredFromRecovery = false;
            if (keepDirty && wasDirty)
            {
                control.markLayerUnsaved(SCENE_WORLD, 0);
                return;
            }
            markSaved(control, open);
        }

        std::string openSceneFile(ControlServer& control, const SceneLoadedFn& sceneLoaded, OpenScene& open,
                                  const ControlArgs& args, IFormatWriter& out)
        {
            Target target;
            if (std::string error = sceneTarget(control, args, "openScene", target); !error.empty())
            {
                return error;
            }
            if (std::string error = replaceGuard(control, open, args); !error.empty())
            {
                return error;
            }
            std::string path;
            SceneDocument document;
            if (std::string error = readSceneDocument(control, args, false, path, document); !error.empty())
            {
                return error;
            }
            SceneDocument previous;
            ApplyResult loaded;
            u32 deleted = 0;
            if (std::string error = replaceScene(target, sceneLoaded, document, previous, loaded, deleted);
                !error.empty())
            {
                adoptScene(control, open, open.path, true);
                return error;
            }
            const std::string normalized = normalizedPath(path);
            adoptScene(control, open, normalized, false);
            control.recordReplay();
            out.writeString("path", normalized);
            out.writeU32("deleted", deleted);
            out.writeU32("entities", static_cast<u32>(document.entities.size()));
            out.writeU32("skippedComponents", document.skippedComponents + loaded.skippedComponents);
            writeChanges(out, document, loaded);
            return {};
        }

        std::string newScene(ControlServer& control, const SceneLoadedFn& sceneLoaded, OpenScene& open,
                             const ControlArgs& args, IFormatWriter& out)
        {
            Target target;
            if (std::string error = sceneTarget(control, args, "newScene", target); !error.empty())
            {
                return error;
            }
            if (std::string error = replaceGuard(control, open, args); !error.empty())
            {
                return error;
            }
            const u32 deleted = clearScene(target);
            if (sceneLoaded)
            {
                sceneLoaded(target.world, ApplyResult{});
            }
            adoptScene(control, open, {}, false);
            control.recordReplay();
            out.writeU32("deleted", deleted);
            return {};
        }

        sizet countSceneEntities(const World& world)
        {
            sizet count = 0;
            const ComponentID uiElement = getComponentId<UiIdentityComponent>();
            for (const Chunk* chunk : world.getChunksContaining(Aspect{}))
            {
                if (!chunk->hasComponent(uiElement))
                {
                    count += chunk->entities().size();
                }
            }
            return count;
        }

        // Entity counts: the open scene's (layer 0) first, then each loaded layer's in registry order.
        std::vector<sizet> layerCounts(const Target& target, const OpenScene& open)
        {
            std::vector<sizet> counts(open.layers.size() + 1);
            counts.front() = countSceneEntities(target.world);
            std::unordered_map<u32, sizet> indices;
            for (sizet i = 0; i < open.layers.size(); ++i)
                indices.emplace(open.layers[i].id, i + 1);
            std::vector<Entity> descendants;
            for (const Chunk* chunk : target.world.getChunksContaining(Aspect(getComponentId<SceneLayerComponent>())))
            {
                for (const Entity root : chunk->entities())
                {
                    const auto index = indices.find(rootLayer(target.world, root));
                    if (index == indices.end() || target.transforms->attachedParentOf(root).isValid())
                        continue;
                    const auto size = target.transforms->collectSubtree(root, descendants) ? descendants.size() : 1;
                    counts[index->second] += size;
                    counts.front() -= size;
                }
            }
            return counts;
        }

        std::string sceneInfo(ControlServer& control, OpenScene& open, IFormatWriter& out, const bool includeCounts)
        {
            const World* world = control.world(SCENE_WORLD);
            if (!world)
            {
                return control.unknownWorld(SCENE_WORLD);
            }
            out.writeString("path", open.path);
            out.writeString("name",
                            open.path.empty() ? std::string("Untitled") : utf8Path(pathFromUtf8(open.path).stem()));
            out.writeBool("dirty", isDirty(control));
            const SceneLayer* active = findLayer(open, open.active);
            const std::string& activePath = active ? active->path : open.path;
            out.writeString("activePath", activePath);
            out.writeString("activeName", activePath.empty() ? "" : utf8Path(pathFromUtf8(activePath).stem()));
            out.writeBool("workspaceDirty",
                          isDirty(control) || !open.unsavedUnloads.empty() ||
                              std::ranges::any_of(open.layers,
                                                  [&](const SceneLayer& layer) { return layerDirty(control, layer); }));
            out.writeString("world", SCENE_WORLD);
            // the open scene's own entities; worldEntityCount adds the loaded layers'
            if (includeCounts)
            {
                const sizet worldCount = countSceneEntities(*world);
                sizet baseCount = worldCount;
                if (!open.layers.empty() && world->services().contains<EntityTransformManager>())
                {
                    const Target target{*world, &world->services().write<EntityTransformManager>().get(), true};
                    baseCount = layerCounts(target, open).front();
                }
                out.writeU64("entityCount", baseCount);
                out.writeU64("worldEntityCount", worldCount);
            }
            out.writeBool("playing", control.world("play") != nullptr);
            if (open.savedAtMs != 0)
            {
                out.writeU64("lastSavedMs", open.savedAtMs);
            }
            return {};
        }

        void dropUnsavedUnload(const std::filesystem::path& root, OpenScene& open, const std::string& path)
        {
            std::erase_if(open.unsavedUnloads,
                          [&](const std::string& unloaded) { return samePath(root, unloaded, path); });
            std::erase_if(open.unsavedLayerData,
                          [&](const UnsavedLayer& unloaded) { return samePath(root, unloaded.path, path); });
        }

        // A loaded layer's entry for the "layer" arg, refusing the base and unknown ids.
        std::string loadedLayer(OpenScene& open, const ControlArgs& args, SceneLayer*& out)
        {
            const std::optional<u64> id = args.integer("layer");
            if (!id)
            {
                return "missing integer arg 'layer'";
            }
            if (*id == 0)
            {
                return std::string("layer 0 is the open scene: openScene or newScene replaces it");
            }
            out = *id > UINT32_MAX ? nullptr : findLayer(open, static_cast<u32>(*id));
            return out ? std::string() : "no scene layer " + std::to_string(*id) + " (sceneLayers lists them)";
        }

        // Applies a saved scene beside the open scene as a new layer. From the undo history (replayingHistory) it
        // also takes the layer's id, and from an unload's inverse the inline document and save state.
        std::string loadSceneLayer(ControlServer& control, const std::filesystem::path& root,
                                   const SceneLoadedFn& sceneLoaded, OpenScene& open, const ControlArgs& args,
                                   IFormatWriter& out, bool recovered = false)
        {
            Target target;
            if (std::string error = sceneTarget(control, args, "loadSceneLayer", target); !error.empty())
            {
                return error;
            }
            const bool replay = control.replayingHistory() || recovered;
            const bool activate = !replay && args.boolean("activate").value_or(false);
            for (const std::string_view key : {"layer", "document", "restored", "active"})
            {
                if (!replay && args.json(key))
                {
                    return "'" + std::string(key) + "' is set only by undo and redo";
                }
            }
            std::string path;
            SceneDocument document;
            if (activate)
            {
                std::filesystem::path file;
                if (std::string error = scenePath(control, args, path, file); !error.empty())
                    return error;
                if (const auto owner = layerOwning(root, open, normalizedPath(path)))
                {
                    open.active = *owner;
                    ++open.focusRevision;
                    out.writeU32("layer", *owner);
                    out.writeBool("alreadyOpen", true);
                    return {};
                }
            }
            if (std::string error = readSceneDocument(control, args, replay, path, document); !error.empty())
            {
                return error;
            }
            const std::string normalized = normalizedPath(path.empty() ? args.string("path").value_or("") : path);
            if (const std::optional<u32> owner =
                    normalized.empty() ? std::nullopt : layerOwning(root, open, normalized))
            {
                return *owner == 0 ? "'" + normalized + "' is the open scene"
                                   : "'" + normalized + "' is loaded as scene layer " + std::to_string(*owner);
            }
            const u32 id = replay && args.integer("layer") ? static_cast<u32>(*args.integer("layer")) : open.nextLayer;
            if (findLayer(open, id) || (id == 0 && (!open.path.empty() || !layerRoots(target, 0).empty())))
            {
                return "scene layer " + std::to_string(id) + " is loaded";
            }
            Expected<ApplyResult> applied = applyScene(document, target.world, *target.transforms, Entity::undefined());
            if (!applied)
            {
                return "cannot apply '" + normalized + "': " + applied.error().message.c_str();
            }
            for (const Entity layerRoot : applied->roots)
            {
                setRootLayer(target.world, layerRoot, id);
            }
            if (sceneLoaded)
            {
                sceneLoaded(target.world, *applied);
            }
            open.nextLayer = std::max(open.nextLayer, id + 1);
            if (id == 0)
                open.path = normalized;
            else
                open.layers.push_back(SceneLayer{.id = id, .path = normalized});
            // a fresh load is its file; the undo of an unload brings back the save state the history kept; a redone
            // load applies the document loaded then, which its file may no longer hold, so it reads dirty
            const bool restored = args.boolean("restored").value_or(false);
            std::optional<UnsavedLayer> recoveredEntry;
            if (recovered && restored)
            {
                const auto found = std::ranges::find_if(open.unsavedLayerData, [&](const UnsavedLayer& layer)
                                                        { return samePath(root, layer.path, normalized); });
                if (found != open.unsavedLayerData.end())
                    recoveredEntry = *found;
            }
            control.touchLayer(id, !replay);
            if (restored)
            {
                dropUnsavedUnload(root, open, normalized);
            }
            else if (replay)
            {
                control.markLayerUnsaved(SCENE_WORLD, id);
            }
            if (activate || args.boolean("active").value_or(false))
            {
                open.active = id;
            }
            recordCreated(control, document, *applied);
            if (recoveredEntry)
            {
                Json references = Json::array();
                for (const auto& reference : recoveredEntry->incomingReferences)
                    references.push_back(Json::parse(reference));
                control.recordInverse("unloadRecoveredLayer",
                                      Json{{"layer", id},
                                           {"path", recoveredEntry->path},
                                           {"document", Json::parse(recoveredEntry->document)},
                                           {"active", recoveredEntry->active},
                                           {"references", std::move(references)}}
                                          .dump());
            }
            else
                control.recordInverse(id == 0 ? "closeSceneLayer" : "unloadSceneLayer",
                                      R"({"layer":)" + std::to_string(id) + "}");
            if (!replay)
            {
                // redo applies this document, not the file as it is then (a later save may have changed it), under
                // the same id, which later steps name; entity forwarding keys by its indices
                std::string text;
                emitSceneJson(document, text);
                control.recordForward(R"({"world":")" + std::string(SCENE_WORLD) + R"(","path":)" +
                                      Json(normalized).dump() + R"(,"layer":)" + std::to_string(id) +
                                      R"(,"document":)" + text + R"(,"active":)" + (activate ? "true" : "false") + "}");
            }
            out.writeU32("layer", id);
            out.writeString("path", normalized);
            out.writeU32("entities", static_cast<u32>(document.entities.size()));
            out.writeU32("skippedComponents", document.skippedComponents + applied->skippedComponents);
            writeChanges(out, document, *applied);
            return {};
        }

        // Destroys a layer's entities and ends it; the inverse loads it back from the collected document under its
        // id, with its save state, active flag and the references other entities held into it.
        std::string unloadSceneLayer(ControlServer& control, OpenScene& open, const ControlArgs& args,
                                     IFormatWriter& out, const bool close = false)
        {
            Target target;
            if (std::string error = sceneTarget(control, args, "unloadSceneLayer", target); !error.empty())
            {
                return error;
            }
            SceneLayer base{.path = open.path, .savedAtMs = open.savedAtMs};
            SceneLayer* layer = close && args.integer("layer") == 0 ? &base : nullptr;
            if (!layer)
            {
                if (std::string error = loadedLayer(open, args, layer); !error.empty())
                    return error;
            }
            const bool dirty = layerDirty(control, *layer);
            if (dirty && !control.replayingHistory() && !args.boolean("force").value_or(false))
                return "scene layer " + std::to_string(layer->id) + " ('" + layer->path + "') has unsaved changes";
            const std::vector<Entity> roots = layerRoots(target, layer->id);
            const Expected<SceneDocument> document = collectDocument(target.world, *target.transforms, roots, true);
            if (!document)
            {
                return document.error().message.c_str();
            }
            std::vector<Entity> doomed = subtrees(target, roots);
            const std::vector<std::string> outside = outsideReferenceInverses(target.world, doomed);
            control.touchLayer(layer->id, close && layer->id == 0);
            const bool active = open.active == layer->id;
            freeForReuseInOrder(doomed);
            target.world.destroyEntities(doomed);
            recordReapply(control, "loadSceneLayer", *document, Entity::undefined(),
                          (layer->path.empty() ? std::string{} : R"(,"path":)" + Json(layer->path).dump()) +
                              R"(,"layer":)" + std::to_string(layer->id) + R"(,"restored":true,"active":)" +
                              (active ? "true" : "false"));
            for (const std::string& inverse : outside)
            {
                control.recordInverse("set", inverse);
            }
            out.writeU32("layer", layer->id);
            out.writeString("path", layer->path);
            out.writeU32("deleted", static_cast<u32>(doomed.size()));
            out.writeBool("wasDirty", dirty);
            if (!close && dirty && !layer->path.empty() &&
                std::ranges::find(open.unsavedUnloads, layer->path) == open.unsavedUnloads.end())
            {
                open.unsavedUnloads.push_back(layer->path);
                std::string savedDocument;
                emitSceneJson(*document, savedDocument);
                open.unsavedLayerData.push_back({layer->path, std::move(savedDocument), layer->id, active, outside});
            }
            if (layer->id == 0)
            {
                open.path.clear();
                open.savedAtMs = 0;
            }
            else
                open.layers.erase(open.layers.begin() + (layer - open.layers.data()));
            if (active)
                open.active = close && !open.layers.empty() ? open.layers.front().id : 0;
            return {};
        }

        std::string setActiveSaveTarget(ControlServer& control, OpenScene& open, const ControlArgs& args,
                                        IFormatWriter& out)
        {
            if (control.worldName(args) != SCENE_WORLD)
            {
                return "scene layers live in world '" + std::string(SCENE_WORLD) + "'";
            }
            u32 layer = 0;
            if (std::string error = layerArg(open, args, 0, layer); !error.empty())
            {
                return error;
            }
            out.writeU32("previous", open.active);
            open.active = layer;
            ++open.focusRevision;
            out.writeU32("layer", layer);
            return {};
        }

        std::string sceneLayers(ControlServer& control, OpenScene& open, IFormatWriter& out, const bool includeCounts)
        {
            const World* world = control.world(SCENE_WORLD);
            if (!world)
            {
                return control.unknownWorld(SCENE_WORLD);
            }
            if (!world->services().contains<EntityTransformManager>())
            {
                return std::string("world has no transform hierarchy");
            }
            const Target target{*world, &world->services().write<EntityTransformManager>().get(), true};
            out.writeU32("active", open.active);
            out.writeU64("ownershipRevision", open.ownershipRevision);
            out.writeU64("focusRevision", open.focusRevision);
            out.beginArray("layers", static_cast<u32>(open.layers.size() + 1));
            const std::vector<sizet> counts =
                includeCounts ? layerCounts(target, open) : std::vector<sizet>(open.layers.size() + 1);
            const auto write =
                [&](const u32 id, const std::string& path, const bool dirty, const sizet count, const u64 savedAtMs)
            {
                out.beginObject({});
                out.writeU32("id", id);
                out.writeString("path", path);
                out.writeBool("active", open.active == id);
                out.writeBool("dirty", dirty);
                if (includeCounts)
                    out.writeU64("entityCount", count);
                if (savedAtMs != 0)
                {
                    out.writeU64("lastSavedMs", savedAtMs);
                }
                out.endObject();
            };
            write(0, open.path, isDirty(control), counts[0], open.savedAtMs);
            for (sizet i = 0; i < open.layers.size(); ++i)
            {
                const SceneLayer& layer = open.layers[i];
                write(layer.id, layer.path, layerDirty(control, layer), counts[i + 1], layer.savedAtMs);
            }
            out.endArray();
            out.beginArray("unsavedUnloads", static_cast<u32>(open.unsavedUnloads.size()));
            for (const std::string& path : open.unsavedUnloads)
            {
                out.writeString("", path);
            }
            out.endArray();
            return {};
        }

        // applies a scene document (JSON object) beside the scene, under parent
        std::string instantiate(ControlServer& control, const SceneLoadedFn& sceneLoaded, OpenScene& open,
                                const ControlArgs& args, IFormatWriter& out)
        {
            Target target;
            if (std::string error = resolveTarget(control, args, target); !error.empty())
            {
                return error;
            }
            Entity parent;
            if (std::string error = parentEntity(target.world, args, parent); !error.empty())
            {
                return error;
            }
            u32 layer = 0;
            if (std::string error = rootLayerArg(open, args, parent, open.active, layer); !error.empty())
            {
                return error;
            }
            if (!args.json("document") && !args.json("path"))
            {
                return "missing arg 'document' or 'path' (an inline scene document or snapshot file)";
            }
            std::string path;
            SceneDocument parsed;
            if (std::string error = readSceneDocument(control, args, true, path, parsed); !error.empty())
            {
                return error;
            }
            const SceneDocument* document = &parsed;
            const HierarchyPromotion parentPromotion(target.world, parent);
            const Expected<ApplyResult> applied = applyScene(*document, target.world, *target.transforms, parent);
            if (!applied)
            {
                return applied.error().message.c_str();
            }
            if (!parent.isValid() && target.sceneWorld)
            {
                for (const Entity root : applied->roots)
                {
                    setRootLayer(target.world, root, layer);
                }
            }
            if (!control.replayingHistory())
            {
                std::string text;
                emitSceneJson(*document, text);
                Json forward{{"world", control.worldName(args)}, {"document", Json::parse(text)}};
                if (parent.isValid())
                {
                    forward["parent"] = parent.id();
                }
                else if (target.sceneWorld)
                {
                    forward["layer"] = layer;
                }
                control.recordForward(forward.dump());
            }
            if (sceneLoaded)
            {
                sceneLoaded(target.world, *applied);
            }
            recordCreated(control, *document, *applied);
            control.touchLayer(target.sceneWorld && parent.isValid() ? layerOf(target.world, *target.transforms, parent)
                                                                     : layer);
            out.beginArray("roots", static_cast<u32>(applied->roots.size()));
            for (const Entity root : applied->roots)
            {
                recordDelete(control, root);
                out.writeU64("", root.id());
            }
            out.endArray();
            if (!applied->roots.empty())
            {
                parentPromotion.record(control);
            }
            writeParent("parent", parent, out);
            out.writeU32("entities", static_cast<u32>(document->entities.size()));
            out.writeU32("skippedComponents", document->skippedComponents + applied->skippedComponents);
            writeChanges(out, *document, *applied);
            return {};
        }

        u32 childCount(const EntityTransformManager& transforms, const Entity entity)
        {
            const std::span<const Entity> subtree = transforms.subtreeOf(entity);
            u32 count = 0;
            for (sizet i = 1; i < subtree.size(); i += transforms.subtreeOf(subtree[i]).size())
            {
                ++count;
            }
            return count;
        }

        // entity and find rows' parent and children; compile first so an edit made earlier in the frame shows
        ControlServer::EntityRelation readRelation(const World& world, const Entity entity)
        {
            if (!world.services().contains<EntityTransformManager>())
            {
                return {};
            }
            EntityTransformManager& transforms = world.services().write<EntityTransformManager>().get();
            transforms.compile();
            return {transforms.parentOf(entity), childCount(transforms, entity), transforms.isEntityEnabled(entity),
                    transforms.isEntityActive(entity)};
        }

        std::string_view nameOf(const World& world, const Entity entity)
        {
            const auto* name = world.tryGetConstComponent<EntityNameComponent>(entity);
            return name ? name->name.toView() : std::string_view{};
        }

        // path: the ancestors root first, the entity last; entities: the entity and its descendants in hierarchy
        // order down to depth levels (each row's depth is relative to the entity), at most limit rows
        std::string subtreeQuery(ControlServer& control, const ControlArgs& args, IFormatWriter& out)
        {
            const World* world = control.world(args);
            if (!world)
            {
                return control.unknownWorld(args);
            }
            if (!world->services().contains<EntityTransformManager>())
            {
                return "world has no transform hierarchy";
            }
            const std::optional<u64> id = args.integer("id");
            if (!id)
            {
                return "missing integer arg 'id'";
            }
            const Entity entity(*id);
            if (!world->isEntityAlive(entity))
            {
                return "no live entity " + std::to_string(*id);
            }
            const u64 limit = args.integer("limit").value_or(DEFAULT_SUBTREE_LIMIT);
            if (limit > MAX_SUBTREE_LIMIT)
            {
                return "limit must be an integer 0.." + std::to_string(MAX_SUBTREE_LIMIT);
            }
            const u64 maxDepth = args.integer("depth").value_or(std::numeric_limits<u64>::max());

            EntityTransformManager& transforms = world->services().write<EntityTransformManager>().get();
            transforms.compile();
            std::vector<Entity> path{entity};
            for (Entity at = transforms.parentOf(entity); at.isValid(); at = transforms.parentOf(at))
            {
                path.push_back(at);
            }
            std::ranges::reverse(path);
            out.beginArray("path", static_cast<u32>(path.size()));
            for (const Entity node : path)
            {
                out.beginObject("");
                out.writeU64("id", node.id());
                out.writeString("name", nameOf(*world, node));
                out.endObject();
            }
            out.endArray();

            // an entity outside the hierarchy (no transform) is its own subtree
            std::span<const Entity> subtree = transforms.subtreeOf(entity);
            const Entity alone[1] = {entity};
            if (subtree.empty())
            {
                subtree = alone;
            }
            const u32 rootDepth = transforms.depthOf(entity);
            std::vector<Entity> rows;
            u64 total = 0;
            for (const Entity node : subtree)
            {
                if (transforms.depthOf(node) - rootDepth > maxDepth)
                {
                    continue;
                }
                if (rows.size() < limit)
                {
                    rows.push_back(node);
                }
                ++total;
            }
            out.writeU64("total", total);
            out.beginArray("entities", static_cast<u32>(rows.size()));
            for (const Entity node : rows)
            {
                out.beginObject("");
                out.writeU64("id", node.id());
                out.writeString("name", nameOf(*world, node));
                writeParent("parent", transforms.parentOf(node), out);
                out.writeU32("children", childCount(transforms, node));
                out.writeU32("depth", transforms.depthOf(node) - rootDepth);
                out.writeBool("enabled", transforms.isEntityEnabled(node));
                out.writeBool("active", transforms.isEntityActive(node));
                out.endObject();
            }
            out.endArray();
            return {};
        }

        // addComponent/removeComponent rules control cannot know: UI elements
        // and UI components belong to their UiContext, and a transform with
        // hierarchy edges would lose them with no inverse
        std::string guardComponentEdit(const World& world, const Entity entity, const ComponentID component,
                                       const bool adding)
        {
            if (world.hasComponent<UiIdentityComponent>(entity))
            {
                return "entity " + std::to_string(entity.id()) + " is a UI element; its UI owns its components";
            }
            // the UI module's vocabulary is the "Ui" prefix
            if (const std::string_view name = getComponentName(component); name.starts_with("Ui"))
            {
                return "component '" + std::string(name) + "' belongs to the UI; scene entities never carry it";
            }
            if (component == getComponentId<SceneLayerComponent>())
            {
                return std::string("SceneLayerComponent is the scene layers' own record: reparent id layer=<id> moves "
                                   "a root to another layer");
            }
            if (component == getComponentId<InactiveComponent>())
            {
                return std::string("InactiveComponent is derived from the entity's and its ancestors' enabled state; "
                                   "it is never added or removed by hand");
            }
            if (component == getComponentId<DisabledComponent>())
            {
                return std::string("DisabledComponent is the entity's enabled flag, which its subtree's activity "
                                   "follows: setEntityEnabled id=<id> enabled=<bool> writes it");
            }
            if (adding || component != getComponentId<TransformComponent>() ||
                !world.services().contains<EntityTransformManager>())
            {
                return {};
            }
            EntityTransformManager& transforms = world.services().write<EntityTransformManager>().get();
            std::vector<Entity> subtree;
            transforms.collectSubtree(entity, subtree);
            if (transforms.attachedParentOf(entity).isValid() || subtree.size() > 1)
            {
                return "entity " + std::to_string(entity.id()) +
                    " has a parent or children, whose edges removing TransformComponent would drop; reparent or "
                    "deleteEntity instead";
            }
            return {};
        }
    } // namespace

    static void registerSceneCommandsImpl(ControlServer& control, std::filesystem::path sceneRoot,
                                          SceneLoadedFn sceneLoaded, SceneRecoveryAccess* recovery,
                                          bool registerSchemas)
    {
        if (registerSchemas)
        {
            registerComponent<SceneLayerComponent>();
            registerComponent<DisabledComponent>();
        }
        const auto open = std::make_shared<OpenScene>();
        if (const World* scene = control.world(SCENE_WORLD))
        {
            scene->services().registerService(std::make_shared<SceneLayerPlacement>(
                SceneLayerPlacement{[open](const std::optional<u64> requested) -> std::optional<u32>
                                    {
                                        const auto id = requested.value_or(open->active);
                                        if (id == 0 || (id < UINT32_MAX && findLayer(*open, static_cast<u32>(id))))
                                            return static_cast<u32>(id);
                                        return std::nullopt;
                                    }}));
        }
        if (recovery)
        {
            recovery->capture = [&control, open]() -> Expected<std::string>
            {
                const World* world = control.world(SCENE_WORLD);
                if (!world || !world->services().contains<EntityTransformManager>())
                    return std::unexpected(makeError(ErrorCode::AssetSaveFailed, "scene world is unavailable"));
                if (open->unsavedUnloads.size() != open->unsavedLayerData.size())
                    return std::unexpected(makeError(ErrorCode::AssetSaveFailed, "unloaded layer data is unavailable"));
                auto& transforms = world->services().write<EntityTransformManager>().get();
                World target = *world;
                const auto scene = collectWorldScene(target, transforms);
                if (!scene)
                    return std::unexpected(scene.error());
                std::string sceneText;
                emitSceneJson(*scene, sceneText);
                Json document{{"id", "scene"},
                              {"version", 2},
                              {"path", open->path},
                              {"savedAtMs", open->savedAtMs},
                              {"dirty", control.layerDirty(SCENE_WORLD, 0)},
                              {"active", open->active},
                              {"nextLayer", open->nextLayer},
                              {"scene", Json::parse(sceneText)},
                              {"layers", Json::array()},
                              {"rootLayers", Json::array()},
                              {"unloaded", Json::array()}};
                for (const auto& layer : open->layers)
                    document["layers"].push_back({{"id", std::to_string(layer.id)},
                                                  {"path", layer.path},
                                                  {"savedAtMs", layer.savedAtMs},
                                                  {"dirty", control.layerDirty(SCENE_WORLD, layer.id)},
                                                  {"content", Json::object()}});
                for (const auto& entity : scene->entities)
                    if (entity.parentIndex == NO_PARENT_INDEX)
                        document["rootLayers"].push_back(
                            {{"id", entity.sourceId}, {"layer", rootLayer(target, Entity(entity.sourceId))}});
                for (const auto& layer : open->unsavedLayerData)
                {
                    Json incoming = Json::array();
                    for (const auto& reference : layer.incomingReferences)
                        incoming.push_back(Json::parse(reference));
                    document["unloaded"].push_back({{"path", layer.path},
                                                    {"id", layer.id},
                                                    {"active", layer.active},
                                                    {"scene", Json::parse(layer.document)},
                                                    {"incomingReferences", std::move(incoming)}});
                }
                return Json{{"documents", Json::array({std::move(document)})}}.dump();
            };
            const auto restoreRecovery = [&control, open, sceneLoaded,
                                          sceneRoot](std::string_view text, bool validateOnly,
                                                     std::unordered_map<u64, u64>* mapping) -> Expected<void>
            {
                const auto fail = [](const char* message)
                { return std::unexpected(makeError(ErrorCode::CorruptedData, message)); };
                if (text.size() > 64 * 1024 * 1024)
                    return fail("recovery snapshot is too large");
                const Json envelope = Json::parse(text, nullptr, false);
                if (!envelope.is_object() || !envelope.contains("documents") || !envelope["documents"].is_array() ||
                    envelope["documents"].size() != 1)
                    return fail("invalid recovery documents");
                const auto& document = envelope["documents"][0];
                if (!document.is_object() || !document.contains("id") || !document["id"].is_string() ||
                    document["id"] != "scene" || !document.contains("version") ||
                    (document["version"] != 1 && document["version"] != 2) || !document.contains("scene") ||
                    !document["scene"].is_object() || !document.contains("path") || !document["path"].is_string() ||
                    !document.contains("dirty") || !document["dirty"].is_boolean() || !document.contains("savedAtMs") ||
                    !document["savedAtMs"].is_number_unsigned() || !document.contains("active") ||
                    !document["active"].is_number_unsigned() || !document.contains("nextLayer") ||
                    !document["nextLayer"].is_number_unsigned() || !document.contains("layers") ||
                    !document["layers"].is_array() || !document.contains("rootLayers") ||
                    !document["rootLayers"].is_array())
                    return fail("invalid scene recovery state");
                const World* world = control.world(SCENE_WORLD);
                if (!world || !world->services().contains<EntityTransformManager>())
                    return fail("scene world is unavailable");
                World targetWorld = *world;
                auto& transforms = world->services().write<EntityTransformManager>().get();
                if (!validateOnly)
                {
                    const auto current = collectWorldScene(targetWorld, transforms);
                    if (!current || !current->entities.empty() || !open->path.empty() || !open->layers.empty() ||
                        !open->unsavedUnloads.empty())
                        return fail("recovery requires an empty startup scene");
                }
                const auto scene = parseSceneJson(document["scene"].dump());
                if (!scene || scene->skippedComponents || !scene->changes.empty())
                    return fail("recovery scene cannot be decoded without loss");
                std::vector<SceneLayer> layers;
                std::vector<UnsavedLayer> unloaded;
                std::unordered_set<u32> ids{0};
                std::unordered_map<u32, bool> dirty;
                for (const auto& value : document["layers"])
                {
                    if (!value.is_object() || !value.contains("id") || !value["id"].is_string() ||
                        !value.contains("path") || !value["path"].is_string() || !value.contains("dirty") ||
                        !value["dirty"].is_boolean() || !value.contains("savedAtMs") ||
                        !value["savedAtMs"].is_number_unsigned())
                        return fail("invalid recovery layer");
                    const auto idText = value["id"].get<std::string>();
                    u32 id = 0;
                    const auto parsed = std::from_chars(idText.data(), idText.data() + idText.size(), id);
                    if (parsed.ec != std::errc{} || parsed.ptr != idText.data() + idText.size() || id == 0 ||
                        !ids.insert(id).second)
                        return fail("invalid recovery layer id");
                    const auto path = value["path"].get<std::string>();
                    std::filesystem::path checked;
                    if (!path.empty() && !control.resolveFile("scene", path, checked).empty())
                        return fail("invalid recovery layer path");
                    if (!path.empty() &&
                        std::ranges::any_of(layers, [&](const SceneLayer& existing)
                                            { return samePath(sceneRoot, existing.path, path); }))
                        return fail("duplicate recovery layer path");
                    layers.push_back({.id = id, .path = path, .savedAtMs = value["savedAtMs"].get<u64>()});
                    dirty.emplace(id, value["dirty"].get<bool>());
                }
                if (document["active"].get<u64>() > UINT32_MAX || document["nextLayer"].get<u64>() > UINT32_MAX)
                    return fail("invalid recovery layer selection");
                const u32 active = document["active"].get<u32>();
                const u32 next = document["nextLayer"].get<u32>();
                if (!ids.contains(active) || next == 0 || next == UINT32_MAX ||
                    std::ranges::any_of(ids, [next](u32 id) { return id >= next; }))
                    return fail("invalid recovery layer selection");
                std::unordered_map<u64, u32> rootLayers;
                for (const auto& root : document["rootLayers"])
                {
                    if (!root.is_object() || !root.contains("id") || !root["id"].is_number_unsigned() ||
                        !root.contains("layer") || !root["layer"].is_number_unsigned() ||
                        root["layer"].get<u64>() > UINT32_MAX)
                        return fail("invalid recovery root layer");
                    const auto id = root["id"].get<u64>();
                    const auto layer = root["layer"].get<u32>();
                    if (!ids.contains(layer) || !rootLayers.emplace(id, layer).second)
                        return fail("invalid recovery root layer");
                }
                sizet roots = 0;
                std::unordered_set<u64> entityIds;
                for (const auto& entity : scene->entities)
                {
                    if (!entity.sourceId || !entityIds.insert(entity.sourceId).second)
                        return fail("invalid recovery entity identity");
                    if (entity.parentIndex == NO_PARENT_INDEX)
                    {
                        ++roots;
                        if (!rootLayers.contains(entity.sourceId))
                            return fail("recovery root layer is missing");
                    }
                }
                if (roots != rootLayers.size())
                    return fail("recovery root layer is unknown");
                const auto path = document["path"].get<std::string>();
                std::filesystem::path checked;
                if (!path.empty() && !control.resolveFile("scene", path, checked).empty())
                    return fail("invalid recovery scene path");
                if (!path.empty() &&
                    std::ranges::any_of(layers,
                                        [&](const SceneLayer& layer) { return samePath(sceneRoot, layer.path, path); }))
                    return fail("recovery scene and layer paths conflict");
                if (document.contains("unloaded"))
                {
                    if (!document["unloaded"].is_array())
                        return fail("invalid unloaded recovery layers");
                    for (const auto& value : document["unloaded"])
                    {
                        if (!value.is_object() || !value.contains("path") || !value["path"].is_string() ||
                            !value.contains("id") || !value["id"].is_number_unsigned() || value["id"].get<u64>() == 0 ||
                            value["id"].get<u64>() > UINT32_MAX || !value.contains("active") ||
                            !value["active"].is_boolean() || !value.contains("scene") || !value["scene"].is_object())
                            return fail("invalid unloaded recovery layer");
                        if (document["version"] == 1 || !value.contains("incomingReferences"))
                            return fail("unloaded recovery references are unavailable");
                        const auto layerPath = value["path"].get<std::string>();
                        if (layerPath.empty() || !control.resolveFile("scene", layerPath, checked).empty())
                            return fail("invalid unloaded recovery path");
                        const auto layerId = value["id"].get<u32>();
                        if (layerId >= next || !ids.insert(layerId).second ||
                            (!path.empty() && samePath(sceneRoot, path, layerPath)) ||
                            std::ranges::any_of(layers, [&](const SceneLayer& layer)
                                                { return samePath(sceneRoot, layer.path, layerPath); }) ||
                            std::ranges::any_of(unloaded, [&](const UnsavedLayer& layer)
                                                { return samePath(sceneRoot, layer.path, layerPath); }))
                            return fail("conflicting unloaded recovery layer");
                        const auto layerText = value["scene"].dump();
                        const auto layerScene = parseSceneJson(layerText);
                        if (!layerScene || layerScene->skippedComponents || !layerScene->changes.empty())
                            return fail("unloaded recovery layer cannot be decoded without loss");
                        std::unordered_set<u64> unloadedIds;
                        for (const auto& entity : layerScene->entities)
                            if (!entity.sourceId || entityIds.contains(entity.sourceId) ||
                                !unloadedIds.insert(entity.sourceId).second)
                                return fail("invalid unloaded recovery entity identity");
                        std::vector<std::string> incoming;
                        if (value.contains("incomingReferences"))
                        {
                            if (!value["incomingReferences"].is_array())
                                return fail("invalid unloaded layer references");
                            for (const auto& reference : value["incomingReferences"])
                            {
                                if (!reference.is_object() || !reference.contains("id") ||
                                    !reference["id"].is_number_unsigned() || !reference.contains("path") ||
                                    !reference["path"].is_string() || !reference.contains("value"))
                                    return fail("invalid unloaded layer reference");
                                if (!control.isEntityReferencePath(reference["path"].get<std::string>(),
                                                                   reference["value"].is_array()))
                                    return fail("invalid unloaded layer reference path");
                                if (!entityIds.contains(reference["id"].get<u64>()))
                                    return fail("unloaded layer reference owner is unavailable");
                                const auto validReference = [&](const Json& item)
                                {
                                    return item.is_number_unsigned() &&
                                        (item.get<u64>() == 0 || entityIds.contains(item.get<u64>()) ||
                                         unloadedIds.contains(item.get<u64>()));
                                };
                                if (!(validReference(reference["value"]) ||
                                      (reference["value"].is_array() &&
                                       std::ranges::all_of(reference["value"], validReference))))
                                    return fail("unloaded layer reference target is unavailable");
                                incoming.push_back(reference.dump());
                            }
                        }
                        unloaded.push_back(
                            {layerPath, layerText, layerId, value["active"].get<bool>(), std::move(incoming)});
                    }
                }
                if (validateOnly)
                    return {};
                const auto applied = applyScene(*scene, targetWorld, transforms, Entity::undefined());
                if (!applied || applied->skippedComponents || applied->idMap.size() != scene->entities.size())
                {
                    Target target{targetWorld, &transforms, true};
                    clearScene(target);
                    return fail("cannot install recovery scene");
                }
                for (const auto& [id, layer] : rootLayers)
                {
                    const auto found = applied->idMap.find(id);
                    if (found == applied->idMap.end())
                    {
                        Target target{targetWorld, &transforms, true};
                        clearScene(target);
                        return fail("recovery root was not installed");
                    }
                    setRootLayer(targetWorld, found->second, layer);
                }
                std::unordered_set<u64> restoredIds;
                std::unordered_map<u64, u64> restoredMapping;
                if (mapping)
                    restoredMapping.reserve(applied->idMap.size());
                for (const auto& [oldId, entity] : applied->idMap)
                {
                    restoredIds.insert(entity.id());
                    if (mapping)
                        restoredMapping.emplace(oldId, entity.id());
                }
                for (const auto& layer : unloaded)
                {
                    if (layer.incomingReferences.empty())
                        continue;
                    const auto layerScene = parseSceneJson(layer.document);
                    if (!layerScene ||
                        std::ranges::any_of(layerScene->entities, [&](const DocumentEntity& entity)
                                            { return restoredIds.contains(entity.sourceId); }))
                    {
                        Target target{targetWorld, &transforms, true};
                        clearScene(target);
                        return fail("unloaded recovery entity identities are ambiguous");
                    }
                }
                for (auto& layer : unloaded)
                    for (auto& text : layer.incomingReferences)
                    {
                        auto reference = Json::parse(text);
                        reference["id"] = applied->idMap.at(reference["id"].get<u64>()).id();
                        const auto remapLive = [&](Json& value)
                        {
                            if (const auto found = applied->idMap.find(value.get<u64>()); found != applied->idMap.end())
                                value = found->second.id();
                        };
                        if (reference["value"].is_array())
                            for (auto& value : reference["value"])
                                remapLive(value);
                        else
                            remapLive(reference["value"]);
                        text = reference.dump();
                    }
                open->path = path;
                open->savedAtMs = document.value("savedAtMs", u64{0});
                open->layers = std::move(layers);
                open->active = active;
                open->nextLayer = next;
                open->unsavedLayerData = std::move(unloaded);
                open->unsavedUnloads.clear();
                for (const auto& layer : open->unsavedLayerData)
                    open->unsavedUnloads.push_back(layer.path);
                control.clearHistory(SCENE_WORLD);
                if (document["dirty"].get<bool>())
                    control.markLayerUnsaved(SCENE_WORLD, 0);
                else
                    control.markLayerSaved(SCENE_WORLD, 0);
                for (const auto& [id, wasDirty] : dirty)
                    if (wasDirty)
                        control.markLayerUnsaved(SCENE_WORLD, id);
                    else
                        control.markLayerSaved(SCENE_WORLD, id);
                if (sceneLoaded)
                    sceneLoaded(targetWorld, *applied);
                open->recoveryCommandGeneration = control.commandGeneration();
                open->restoredFromRecovery = true;
                if (mapping)
                    *mapping = std::move(restoredMapping);
                return {};
            };
            recovery->validate = [restoreRecovery](std::string_view text)
            { return restoreRecovery(text, true, nullptr); };
            recovery->restore = [restoreRecovery](std::string_view text)
            { return restoreRecovery(text, false, nullptr); };
            recovery->restoreWithMapping =
                [restoreRecovery](std::string_view text, std::unordered_map<u64, u64>& mapping)
            { return restoreRecovery(text, false, &mapping); };
            recovery->ready = [&control, open]() -> Expected<void>
            {
                const World* world = control.world(SCENE_WORLD);
                if (!world || !world->services().contains<EntityTransformManager>())
                    return std::unexpected(makeError(ErrorCode::CorruptedData, "scene world is unavailable"));
                auto& transforms = world->services().write<EntityTransformManager>().get();
                World target = *world;
                const auto current = collectWorldScene(target, transforms);
                if (!current || !current->entities.empty() || !open->path.empty() || !open->layers.empty() ||
                    !open->unsavedUnloads.empty())
                    return std::unexpected(
                        makeError(ErrorCode::AssetSaveFailed, "recovery requires an empty startup scene"));
                return {};
            };
            recovery->clearRestored = [&control, open]()
            {
                if (const World* world = control.world(SCENE_WORLD);
                    world && world->services().contains<EntityTransformManager>())
                {
                    Target target{*world, &world->services().write<EntityTransformManager>().get(), true};
                    clearScene(target);
                }
                *open = OpenScene{};
                control.clearHistory(SCENE_WORLD);
            };
        }
        const auto bind =
            [&control, open](std::string (*fn)(ControlServer&, OpenScene&, const ControlArgs&, IFormatWriter&))
        {
            return [&control, open, fn](const ControlArgs& args, IFormatWriter& out)
            { return fn(control, *open, args, out); };
        };
        constexpr ArgDesc PARENT_ARG =
            optionalArg("parent", ArgKind::Entity, "a scene entity with a TransformComponent", "none: a root");
        constexpr ArgDesc ENTITY_ARG = requiredArg("id", ArgKind::Entity, "a scene (non-UI) entity");
        constexpr ArgDesc PATH_ARG =
            requiredPath("path", "scene", "a file under the scene root; .spsd is the binary cache");
        constexpr ArgDesc SAVE_LAYER_ARG =
            optionalArg("layer", ArgKind::Integer, "the scene to save (sceneLayers)", "the active scene");
        constexpr ArgDesc EXTERNAL_REFS_ARG = optionalArg("allowExternalReferences", ArgKind::Boolean,
                                                          "save despite links that cannot survive reload", "false");
        control.setRoot("scene", sceneRoot,
                        "scene files: saveScene, saveSceneAs, loadScene, openScene, loadSceneLayer");
        control.registerUndoableCommand(
            "createEntity", OWNER, "creates an entity with a transform",
            {optionalArg("name", ArgKind::String, "the entity's name", "unnamed"), PARENT_ARG,
             optionalArg("layer", ArgKind::Integer, "the scene layer a root joins (refused with a parent)",
                         "the active layer")},
            bind(&createEntity));
        control.registerUndoableCommand("deleteEntity", OWNER, "deletes an entity and its subtree", {ENTITY_ARG},
                                        bind(&deleteEntity));
        control.registerUndoableCommand("duplicateEntity", OWNER,
                                        "copies an entity's subtree (its scene data) under the same parent",
                                        {ENTITY_ARG}, bind(&duplicateEntity));
        control.registerUndoableCommand(
            "setEntityEnabled", OWNER,
            "enables or disables an entity (DisabledComponent, saved with the scene): it and its descendants then "
            "read active=false and every query skips them; several entities go in one batch (one undo step) -> id, "
            "enabled, previous, active (enabled and every ancestor enabled); an unchanged value records no undo step",
            {ENTITY_ARG, requiredArg("enabled", ArgKind::Boolean, "true to enable, false to disable")},
            bind(&setEntityEnabled));
        control.registerUndoableCommand(
            "reparent", OWNER, "moves an entity under another, keeping its local transform",
            {ENTITY_ARG, PARENT_ARG,
             optionalArg("layer", ArgKind::Integer,
                         "the scene layer the entity joins as a root (refused with a parent)", "the layer it is in")},
            bind(&reparent));
        control.registerQuery(
            "sceneInfo", OWNER,
            "the open scene: path (empty = untitled), name, dirty (a recorded edit of its own "
            "entities since the last save or open), world, entityCount (its own entities, scene layers' excluded), "
            "worldEntityCount (the world's, layers included), playing, lastSavedMs, activePath, activeName, "
            "workspaceDirty (any open or unloaded scene has unsaved changes)",
            {optionalArg("counts", ArgKind::Boolean, "include the entity counts", "true")},
            [&control, open](const ControlArgs& args, IFormatWriter& out)
            { return sceneInfo(control, *open, out, args.boolean("counts").value_or(true)); });
        control.registerCommand(
            "saveScene", OWNER,
            "writes the requested scene layer (default: active) to its file; with a path, to that file: the scene's "
            "own file saves it, any other is a copy that leaves the open scene as it is. Refuses the play world and "
            "any other world",
            {WORLD_ARG, SAVE_LAYER_ARG, EXTERNAL_REFS_ARG,
             optionalPath("path", "scene", "a file under the scene root; .spsd is the binary cache",
                          "the open scene's path (refused while untitled: use saveSceneAs)"),
             optionalArg("id", ArgKind::Entity, "save this entity and descendants as an independent snapshot"),
             optionalArg("overwrite", ArgKind::Boolean, "replace an existing snapshot file", "false")},
            [&control, sceneRoot, open](const ControlArgs& args, IFormatWriter& out)
            { return saveScene(control, sceneRoot, *open, SaveMode::Save, args, out); });
        control.registerCommand(
            "saveSceneAs", OWNER,
            "writes the requested scene layer (default: active) to a file that becomes that scene's; refuses an "
            "existing file other than that scene's unless overwrite=true",
            {WORLD_ARG, PATH_ARG, SAVE_LAYER_ARG, EXTERNAL_REFS_ARG,
             optionalArg("overwrite", ArgKind::Boolean, "replace an existing file", "false")},
            [&control, sceneRoot, open](const ControlArgs& args, IFormatWriter& out)
            { return saveScene(control, sceneRoot, *open, SaveMode::SaveAs, args, out); });
        control.registerCommand(
            "openScene", OWNER,
            "replaces the scene world's entities with a saved scene that becomes the open scene (not dirty, undo "
            "history ended); refused while Play is active and while the open scene is dirty unless force=true",
            {WORLD_ARG, PATH_ARG, optionalArg("force", ArgKind::Boolean, "discard unsaved changes", "false")},
            [&control, sceneLoaded, open](const ControlArgs& args, IFormatWriter& out)
            { return openSceneFile(control, sceneLoaded, *open, args, out); });
        control.registerCommand(
            "newScene", OWNER,
            "empties the scene world into an untitled open scene (not dirty, undo history ended); refused while Play "
            "is active and while the open scene is dirty unless force=true",
            {WORLD_ARG, optionalArg("force", ArgKind::Boolean, "discard unsaved changes", "false")},
            [&control, sceneLoaded, open](const ControlArgs& args, IFormatWriter& out)
            { return newScene(control, sceneLoaded, *open, args, out); });
        control.registerUndoableCommand(
            "loadScene", OWNER, "replaces the world's scene entities with a saved scene",
            {optionalPath("path", "scene", "a file under the scene root; needed without document"),
             optionalArg("document", ArgKind::Json, "an inline scene document object, instead of path")},
            [&control, sceneLoaded, open](const ControlArgs& args, IFormatWriter& out)
            { return loadScene(control, sceneLoaded, *open, args, out); });
        control.registerUndoableCommand(
            "instantiate", OWNER,
            "instantiates an independent scene snapshot in the requested world, optionally under a parent",
            {optionalArg("document", ArgKind::Json, "a scene document object, instead of path"),
             optionalPath("path", "scene", "a scene snapshot file; needed without document"), PARENT_ARG,
             optionalArg("layer", ArgKind::Integer, "the scene layer the roots join (refused with a parent)",
                         "the active layer")},
            [&control, sceneLoaded, open](const ControlArgs& args, IFormatWriter& out)
            { return instantiate(control, sceneLoaded, *open, args, out); });
        // scene layers: saved scenes loaded beside the open scene (layer 0); the active one is saved
        constexpr ArgDesc LAYER_ARG = requiredArg("layer", ArgKind::Integer, "a scene layer's id (sceneLayers)");
        control.registerQuery(
            "sceneLayers", OWNER,
            "the edit world's scene layers, the open scene first as layer 0: id, path, active (the save target), "
            "dirty (a recorded edit of this layer's entities since its save or load, per layer: undoing back to "
            "the save reads clean; a manual edit back reads dirty), entityCount, lastSavedMs?; the "
            "active id; unsavedUnloads (files of layers unloaded with unsaved changes: openScene and newScene need "
            "force until a save of the file or the undo of the unload)",
            {optionalArg("counts", ArgKind::Boolean, "include the entity count for each scene", "true")},
            [&control, open](const ControlArgs& args, IFormatWriter& out)
            { return sceneLayers(control, *open, out, args.boolean("counts").value_or(true)); });
        control.registerUndoableCommand(
            "loadSceneLayer", OWNER,
            "applies a saved scene beside the open scene as a new layer (its roots tagged SceneLayerComponent) -> "
            "layer, entities, skippedComponents, changes; activate=true makes it active or focuses an already open "
            "file",
            {optionalPath("path", "scene", "the scene file to add (required outside history)"),
             optionalArg("activate", ArgKind::Boolean, "make this scene active; accept a file already open", "false"),
             optionalArg("layer", ArgKind::Integer, "undo and redo only"),
             optionalArg("document", ArgKind::Json, "undo and redo only"),
             optionalArg("restored", ArgKind::Boolean, "undo and redo only"),
             optionalArg("active", ArgKind::Boolean, "undo and redo only")},
            [&control, sceneRoot, sceneLoaded, open](const ControlArgs& args, IFormatWriter& out)
            { return loadSceneLayer(control, sceneRoot, sceneLoaded, *open, args, out); });
        control.registerUndoableCommand(
            "newSceneLayer", OWNER, "adds an untitled scene and makes it active",
            {optionalArg("layer", ArgKind::Integer, "redo only")},
            [&control, open](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                Target target;
                if (auto error = sceneTarget(control, args, "newSceneLayer", target); !error.empty())
                    return error;
                if (args.json("layer") && !control.replayingHistory())
                    return "layer is set only by redo";
                const u32 id = static_cast<u32>(args.integer("layer").value_or(open->nextLayer));
                if (id == 0 || id == UINT32_MAX || findLayer(*open, id))
                    return "scene layer is unavailable";
                open->nextLayer = std::max(open->nextLayer, id + 1);
                open->layers.push_back(SceneLayer{.id = id});
                control.touchLayer(id, true);
                control.recordForward(Json{{"layer", id}}.dump());
                control.recordInverse("closeSceneLayer", Json{{"layer", id}}.dump());
                open->active = id;
                out.writeU32("layer", id);
                return {};
            });
        constexpr ArgDesc WORKSPACE_PATH = requiredPath("path", "scene", "a .spworkspace file under the scene root");
        control.registerCommand(
            "saveSceneWorkspace", OWNER,
            "writes the list of open scene files and the active file, without writing entities",
            {WORKSPACE_PATH, optionalArg("overwrite", ArgKind::Boolean, "replace an existing workspace file", "false")},
            [&control, sceneRoot, open](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                std::string path;
                std::filesystem::path file;
                if (auto error = scenePath(control, args, path, file); !error.empty())
                    return error;
                if (!path.ends_with(".spworkspace") || layerOwning(sceneRoot, *open, normalizedPath(path)))
                    return "choose a .spworkspace file that is not an open scene";
                if (std::filesystem::exists(file) && !args.boolean("overwrite").value_or(false))
                    return "workspace already exists; pass overwrite=true to replace it";
                Json paths = Json::array();
                std::string active;
                const auto append = [&](const u32 id, const std::string& value)
                {
                    if (!value.empty())
                    {
                        paths.push_back(value);
                        if (open->active == id)
                            active = value;
                    }
                };
                if (open->path.empty() && isDirty(control))
                    return "save the untitled scene before saving the workspace";
                append(0, open->path);
                for (const auto& layer : open->layers)
                {
                    if (layer.path.empty())
                        return "save or close untitled scenes before saving the workspace";
                    append(layer.id, layer.path);
                }
                if (paths.empty())
                    return "save at least one scene file before saving the workspace";
                const auto written = writeTextFileAtomically(
                    file, Json{{"version", 1}, {"scenes", paths}, {"active", active}}.dump(2) + "\n");
                if (!written)
                    return std::string("cannot write workspace: ") + written.error().message.c_str();
                out.writeString("path", path);
                return {};
            });
        control.registerCommand(
            "openSceneWorkspace", OWNER, "adds the scene files from a workspace in one undoable step", {WORKSPACE_PATH},
            [&control, open](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                if (control.world("play"))
                    return "stop Play before opening a workspace";
                std::string path;
                std::filesystem::path file;
                if (auto error = scenePath(control, args, path, file); !error.empty())
                    return error;
                if (!path.ends_with(".spworkspace"))
                    return "choose a .spworkspace file";
                const auto text = readBinaryFile(file);
                if (!text)
                    return std::string("cannot read workspace: ") + text.error().message.c_str();
                const auto document = Json::parse(text->begin(), text->end(), nullptr, false);
                if (!document.is_object() || !document.contains("version") || document["version"] != 1 ||
                    !document.contains("scenes") || !document["scenes"].is_array() || document["scenes"].empty() ||
                    !document.contains("active") || !document["active"].is_string())
                    return "invalid scene workspace";
                Json commands = Json::array();
                const auto append = [&](const Json& value)
                {
                    commands.push_back(
                        {{"command", "loadSceneLayer"}, {"args", {{"path", value}, {"activate", true}}}});
                };
                const Json& active = document["active"];
                bool found = active == "";
                for (const Json& scene : document["scenes"])
                {
                    if (!scene.is_string() || scene == "")
                        return "invalid scene path in workspace";
                    if (scene == active)
                        found = true;
                    else
                        append(scene);
                }
                if (!found)
                    return "the active scene is missing from the workspace";
                if (active != "")
                    append(active);
                const u32 previous = open->active;
                const auto error = control.executeUndoableBatch(SCENE_WORLD, commands.dump(), out);
                if (!error.empty())
                    open->active = previous;
                return error;
            });
        const auto applyingRecoveredLayer = std::make_shared<bool>(false);
        control.registerUndoableCommand(
            "restoreRecoveredLayerData", OWNER, "internal recovered-layer history operation",
            {requiredPath("path", "scene", "layer path"), requiredArg("document", ArgKind::Json, "layer data"),
             requiredArg("layer", ArgKind::Integer, "layer id"),
             optionalArg("active", ArgKind::Boolean, "active target"),
             optionalArg("restored", ArgKind::Boolean, "recovered state")},
            [&control, sceneRoot, sceneLoaded, open, applyingRecoveredLayer](const ControlArgs& args,
                                                                             IFormatWriter& out)
            {
                if (!*applyingRecoveredLayer && !control.replayingHistory())
                    return std::string("internal recovered-layer operation");
                return loadSceneLayer(control, sceneRoot, sceneLoaded, *open, args, out, true);
            });
        control.registerUndoableCommand(
            "unloadRecoveredLayer", OWNER, "internal recovered-layer history operation",
            {requiredArg("layer", ArgKind::Integer, "layer id"), requiredPath("path", "scene", "layer path"),
             requiredArg("document", ArgKind::Json, "layer data"),
             requiredArg("references", ArgKind::Json, "references"),
             optionalArg("active", ArgKind::Boolean, "active target")},
            [&control, sceneRoot, open](const ControlArgs& args, IFormatWriter& out)
            {
                if (!control.replayingHistory())
                    return std::string("internal recovered-layer operation");
                const auto documentText = args.json("document");
                const auto referenceText = args.json("references");
                if (!documentText || !referenceText)
                    return std::string("recovered layer history is incomplete");
                const auto document = parseSceneJson(*documentText);
                if (!document)
                    return std::string("recovered layer history is incomplete");
                std::unordered_set<u64> unloadedIds;
                for (const auto& entity : document->entities)
                    unloadedIds.insert(entity.sourceId);
                UnsavedLayer saved{std::string(*args.string("path")),
                                   std::string(*documentText),
                                   static_cast<u32>(*args.integer("layer")),
                                   args.boolean("active").value_or(false),
                                   {}};
                for (auto& reference : Json::parse(*referenceText))
                {
                    reference["id"] = control.resolveHistoryEntity(SCENE_WORLD, reference["id"].get<u64>());
                    const auto forwardLive = [&](Json& value)
                    {
                        const u64 id = value.get<u64>();
                        if (id && !unloadedIds.contains(id))
                            value = control.resolveHistoryEntity(SCENE_WORLD, id);
                    };
                    if (reference["value"].is_array())
                        for (auto& value : reference["value"])
                            forwardLive(value);
                    else
                        forwardLive(reference["value"]);
                    saved.incomingReferences.push_back(reference.dump());
                }
                const auto error = unloadSceneLayer(control, *open, args, out);
                if (error.empty())
                {
                    dropUnsavedUnload(sceneRoot, *open, saved.path);
                    open->unsavedUnloads.push_back(saved.path);
                    open->unsavedLayerData.push_back(std::move(saved));
                }
                return error;
            });
        control.registerUndoableCommand(
            "restoreUnsavedLayer", OWNER,
            "restore an unloaded unsaved layer from recovery state; incoming references require an unchanged scene",
            {requiredPath("path", "scene", "the unloaded layer path"),
             optionalArg("document", ArgKind::Json, "undo and redo only"),
             optionalArg("layer", ArgKind::Integer, "undo and redo only"),
             optionalArg("active", ArgKind::Boolean, "undo and redo only")},
            [&control, sceneRoot, sceneLoaded, open, applyingRecoveredLayer](const ControlArgs& args,
                                                                             IFormatWriter& out)
            {
                const auto path = args.string("path");
                if (!path)
                    return std::string("missing layer path");
                Json forward;
                if (control.replayingHistory())
                {
                    const auto text = args.json("document");
                    const auto id = args.integer("layer");
                    if (!text || !id)
                        return std::string("recovered layer history is incomplete");
                    forward = {{"path", *path},
                               {"document", Json::parse(*text)},
                               {"layer", *id},
                               {"active", args.boolean("active").value_or(false)},
                               {"restored", true}};
                }
                else
                {
                    if (args.json("document") || args.json("layer") || args.json("active"))
                        return std::string("document, layer and active are set only by undo and redo");
                    const auto found = std::ranges::find_if(open->unsavedLayerData, [&](const UnsavedLayer& layer)
                                                            { return samePath(sceneRoot, layer.path, *path); });
                    if (found == open->unsavedLayerData.end())
                        return std::string("no recovered unsaved layer for that path");
                    if (!found->incomingReferences.empty())
                    {
                        if (!open->restoredFromRecovery ||
                            control.commandGeneration() != open->recoveryCommandGeneration)
                            return std::string("scene changed since recovery; incoming references require review");
                        const auto document = parseSceneJson(found->document);
                        const World* world = control.world(SCENE_WORLD);
                        if (!document || !world || document->entities.size() > 1000000)
                            return std::string("cannot prepare recovered layer references");
                        const UnsavedLayer saved = *found;
                        std::unordered_map<u64, u64> aliases;
                        Json revived = Json::array();
                        for (sizet i = 0; i < document->entities.size(); ++i)
                        {
                            const u64 alias =
                                std::numeric_limits<u64>::max() - 1 - ((static_cast<u64>(saved.id) << 20) | i);
                            if (world->isEntityAlive(Entity(alias)) ||
                                !aliases.emplace(document->entities[i].sourceId, alias).second)
                                return std::string("cannot reserve recovered entity aliases");
                            revived.push_back(alias);
                        }
                        Json commands = Json::array();
                        commands.push_back({{"command", "restoreRecoveredLayerData"},
                                            {"args",
                                             {{"path", saved.path},
                                              {"document", Json::parse(saved.document)},
                                              {"layer", saved.id},
                                              {"active", saved.active},
                                              {"restored", true}}}});
                        Json revives = Json::array({std::move(revived)});
                        for (const auto& text : saved.incomingReferences)
                        {
                            auto reference = Json::parse(text);
                            if (!world->isEntityAlive(Entity(reference["id"].get<u64>())))
                                return std::string("recovered reference owner is unavailable");
                            const auto remap = [&](Json& value) -> bool
                            {
                                const auto id = value.get<u64>();
                                if (const auto it = aliases.find(id); it != aliases.end())
                                    value = it->second;
                                else if (id && !world->isEntityAlive(Entity(id)))
                                    return false;
                                return true;
                            };
                            if (reference["value"].is_array())
                            {
                                for (auto& value : reference["value"])
                                    if (!remap(value))
                                        return std::string("recovered reference target is unavailable");
                            }
                            else if (!remap(reference["value"]))
                                return std::string("recovered reference target is unavailable");
                            commands.push_back({{"command", "set"}, {"args", std::move(reference)}});
                            revives.push_back(Json::array());
                        }
                        struct ApplyingGuard
                        {
                            bool& applying;
                            explicit ApplyingGuard(bool& value) : applying(value) { applying = true; }
                            ~ApplyingGuard() { applying = false; }
                        } guard(*applyingRecoveredLayer);
                        const auto error =
                            control.executeUndoableBatchWithRevives(SCENE_WORLD, commands.dump(), revives.dump(), out);
                        if (!error.empty())
                        {
                            dropUnsavedUnload(sceneRoot, *open, saved.path);
                            open->unsavedUnloads.push_back(saved.path);
                            open->unsavedLayerData.push_back(saved);
                        }
                        else
                            open->recoveryCommandGeneration = control.commandGeneration() + 1;
                        return error;
                    }
                    forward = {{"path", found->path},
                               {"document", Json::parse(found->document)},
                               {"layer", found->id},
                               {"active", found->active},
                               {"restored", true}};
                }
                const ControlArgs recoveredArgs(&forward);
                const auto error = loadSceneLayer(control, sceneRoot, sceneLoaded, *open, recoveredArgs, out, true);
                if (error.empty() && !control.replayingHistory())
                {
                    control.recordForward(forward.dump());
                    if (open->restoredFromRecovery)
                        open->recoveryCommandGeneration = control.commandGeneration() + 1;
                }
                return error;
            });
        control.registerUndoableCommand(
            "unloadSceneLayer", OWNER,
            "destroys a layer's entities and ends it -> deleted, wasDirty; refused while the layer has unsaved changes "
            "unless force=true (undo loads it back; a replay of an old log without force is refused too); the "
            "active layer's unload makes the open scene the save target",
            {LAYER_ARG, optionalArg("force", ArgKind::Boolean, "unload a layer with unsaved changes", "false")},
            [&control, open](const ControlArgs& args, IFormatWriter& out)
            { return unloadSceneLayer(control, *open, args, out); });
        control.registerUndoableCommand(
            "closeSceneLayer", OWNER, "closes one scene, leaving the others open; undo restores its contents",
            {LAYER_ARG, optionalArg("force", ArgKind::Boolean, "discard unsaved changes", "false")},
            [&control, open](const ControlArgs& args, IFormatWriter& out)
            { return unloadSceneLayer(control, *open, args, out, true); });
        control.registerCommand("setActiveSaveTarget", OWNER,
                                "makes a layer (0: the open scene) the one saveScene and saveSceneAs write -> previous",
                                {WORLD_ARG, LAYER_ARG}, bind(&setActiveSaveTarget));
        control.setComponentEditGuard(&guardComponentEdit);
        control.setHierarchyReader(&readRelation);
        control.setEntityLayerReader(
            [](const World& world, const Entity entity)
            {
                return world.services().contains<EntityTransformManager>()
                    ? layerOf(world, world.services().write<EntityTransformManager>().get(), entity)
                    : 0u;
            });
        control.registerQuery(
            "subtree", OWNER,
            "an entity's path from its root (id, name per ancestor) and its subtree in hierarchy "
            "order: id, name, parent, children (direct count), depth (below the entity), enabled, active; total "
            "counts the rows within depth",
            {WORLD_ARG, ENTITY_ARG,
             optionalArg("depth", ArgKind::Integer,
                         "levels below the entity to list; 0 is the entity "
                         "alone",
                         "all"),
             optionalArg("limit", ArgKind::Integer, "rows, 0..10000; 0 counts only", "100")},
            [&control](const ControlArgs& args, IFormatWriter& out) { return subtreeQuery(control, args, out); });
    }

    void registerSceneCommands(ControlServer& control, std::filesystem::path sceneRoot, SceneLoadedFn sceneLoaded,
                               SceneRecoveryAccess* recovery)
    {
        registerSceneCommandsImpl(control, std::move(sceneRoot), std::move(sceneLoaded), recovery, true);
    }

    void registerSceneCommandsForCopy(ControlServer& control, std::filesystem::path sceneRoot,
                                      SceneRecoveryAccess* recovery)
    {
        registerSceneCommandsImpl(control, std::move(sceneRoot), {}, recovery, false);
    }

    bool hasExternalSceneReferences(const World& world, const std::span<const Entity> entities)
    {
        return !outsideReferenceInverses(world, std::vector<Entity>(entities.begin(), entities.end())).empty();
    }

    Expected<SceneDocument> collectWorldScene(World& world, EntityTransformManager& transforms)
    {
        std::vector<Entity> roots;
        {
            SPITE_ZONE("Scene.collectWorld.roots");
            transforms.compile();
            for (const Entity entity : nonUiEntities(world))
            {
                if (!transforms.parentOf(entity).isValid())
                {
                    roots.push_back(entity);
                }
            }
        }
        return collectDocument(world, transforms, roots, true);
    }
} // namespace spite
