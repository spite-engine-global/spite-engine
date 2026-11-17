#include "engine/AnimationCommands.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/NumberArg.hpp"
#include "animation/AnimationWorld.hpp"
#include "animation/AnimationControllerAsset.hpp"
#include "asset/BakedAnimation.hpp"
#include "asset/LoadBakedFunctions.hpp"
#include "asset/AssetManifest.hpp"
#include "assetruntime/AssetComponents.hpp"
#include "assetruntime/AssetRuntime.hpp"
#include "assetruntime/RenderStateComponents.hpp"
#include "base/ServiceLocator.hpp"
#include "control/ControlServer.hpp"
#include "ecs/Aspect.hpp"
#include "ecs/Chunk.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "ecs/World.hpp"
#include "physics/PhysicsComponents.hpp"
#include "serialization/FormatWalk.hpp"
#include "world/EntityName.hpp"
#include "world/EntityTransformManager.hpp"
#include "world/TransformComponents.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using Binding = AnimationStateComponent::Binding;

        constexpr std::string_view OWNER = "engine";
        constexpr u64 DEFAULT_LIMIT = 100;
        constexpr u64 MAX_LIMIT = 10000;
        constexpr u32 LISTED_JOINT_NAMES = 24;
        constexpr u64 MAX_SHARE_PHASES = 1024;

        constexpr ArgDesc ID_ARG =
            requiredArg("id", ArgKind::Entity, "a skinned entity (SkinComponent): the root of a rigged model");
        constexpr ArgDesc CLIP_ARG = optionalArg(
            "clip", ArgKind::String, "a baked clip of the entity's skeleton (clips lists them)", "the current clip");

        // One animator, the row of the animators table and the animator query.
        struct AnimatorRow
        {
            Entity entity = Entity::undefined();
            std::string name;
            InternedString skeleton;
            u32 joints = 0;
            bool posed = false;
            u32 poseVersion = 0;
            bool player = false;
            InternedString clip;
            std::string binding;
            f32 duration = 0.0f;
            f32 time = 0.0f;
            f32 normalizedTime = 0.0f;
            f32 speed = 1.0f;
            f32 weight = 1.0f;
            f32 blend = 0.0f;
            bool playing = false;
            bool loop = false;
            bool rootMotion = false;
            InternedString controller;
            std::string controllerBinding;
            InternedString state;
            InternedString nextState;
            std::string controllerReason;
            u32 updateInterval = 1; // frames between the animator's updates (AnimationLodSettings)
            u32 sharedGroup = 0; // the shared pose it shows, 0 for its own
            u32 sharedPhase = 0;
            SPITE_OBSERVED_SCHEMA(AnimatorRow, entity, name, skeleton, joints, posed, poseVersion, player, clip,
                                  binding, duration, time, normalizedTime, speed, weight, blend, playing, loop,
                                  rootMotion, controller, controllerBinding, state, nextState, controllerReason,
                                  updateInterval, sharedGroup, sharedPhase)
        };

        // One joint of one animator, the row of the joints table and the joints query.
        struct JointRow
        {
            Entity entity = Entity::undefined();
            u32 joint = 0;
            InternedString name;
            i32 parent = -1;
            InternedString parentName;
            bool posed = false;
            Vec3 position{0.0f};
            Quat rotation = Quat::identity();
            Vec3 scale{1.0f};
            Mat4 model = Mat4::identity();
            f32 palette[12] = {};
            SPITE_OBSERVED_SCHEMA(JointRow, entity, joint, name, parent, parentName, posed, position, rotation, scale,
                                  model, palette)
        };

        // One bone attachment, the row of the attachments table and the attachments query.
        struct AttachmentRow
        {
            Entity entity = Entity::undefined();
            std::string name;
            Entity target = Entity::undefined();
            std::string targetName;
            InternedString joint;
            Vec3 position{0.0f};
            Quat rotation = Quat::identity();
            Vec3 scale{1.0f};
            std::string binding;
            std::string reason;
            i32 jointIndex = -1;
            SPITE_OBSERVED_SCHEMA(AttachmentRow, entity, name, target, targetName, joint, position, rotation, scale,
                                  binding, reason, jointIndex)
        };

        std::string_view viewOf(const InternedString text)
        {
            return text.isValid() ? text.toView() : std::string_view{};
        }

        std::string describe(const Entity entity) { return std::to_string(entity.id()); }

        std::string animationWorldOf(ControlServer& control, const ControlArgs& args, const World*& world)
        {
            world = control.world(args);
            if (!world)
            {
                return control.unknownWorld(args);
            }
            if (!world->services().contains<AnimationWorld>())
            {
                return "world '" + control.worldName(args) + "' has no animation (only the edit and play worlds do)";
            }
            return {};
        }

        std::string skinnedEntity(const World& world, const ControlArgs& args, Entity& out)
        {
            const std::optional<u64> id = args.integer("id");
            if (!id)
            {
                return "missing integer arg 'id'";
            }
            out = Entity(*id);
            if (!world.isEntityAlive(out))
            {
                return "no live entity " + std::to_string(*id);
            }
            if (!world.hasComponent<SkinComponent>(out))
            {
                return "entity " + describe(out) +
                    " has no SkinComponent: only the root of a rigged model animates (animators lists them)";
            }
            return {};
        }

        std::string compatibleSkeleton(const World& world, const AssetRuntime& assets, Entity entity,
                                       InternedString candidate, std::vector<InternedString>& clips)
        {
            if (!candidate.isValid() || !world.services().contains<AnimationAssetLoader>())
                return "skeleton or animation loader is missing";
            const InternedString current = world.getConstComponent<SkinComponent>(entity).skeleton;
            const auto loader = world.services().read<AnimationAssetLoader>();
            if (!loader->loadSkeleton || !loader->loadClip)
                return "animation loader cannot load skeletons or clips";
            const auto oldRig = loader->loadSkeleton(current);
            const auto newRig = loader->loadSkeleton(candidate);
            if (!oldRig || !newRig)
                return !oldRig ? oldRig.error().message : newRig.error().message;
            if (oldRig->identity != newRig->identity || oldRig->joints.size() != newRig->joints.size() ||
                oldRig->zUp != newRig->zUp || oldRig->usesLayers != newRig->usesLayers ||
                oldRig->usesIk != newRig->usesIk)
                return "skeleton joint identity or import conventions differ";
            for (sizet i = 0; i < oldRig->joints.size(); ++i)
            {
                const JointInfo& a = oldRig->joints[i];
                const JointInfo& b = newRig->joints[i];
                if (a.name != b.name || a.parent != b.parent || a.position != b.position || a.rotation != b.rotation ||
                    a.scale != b.scale || a.inverseBind != b.inverseBind)
                    return "skeleton rest pose or inverse bind differs";
            }
            const auto skeleton = loadBakedSkeleton(candidate, assets.manifest());
            if (!skeleton)
                return skeleton.error().message;
            if (!world.services().contains<EntityTransformManager>())
                return "world has no transform hierarchy";
            const auto transforms = world.services().read<EntityTransformManager>();
            std::vector<Entity> subtree;
            if (!transforms->collectSubtree(entity, subtree))
                return "cannot inspect the skin hierarchy";
            for (Entity child : subtree)
            {
                Entity owner = child;
                while (owner.isValid() && owner != entity && !world.hasComponent<SkinComponent>(owner))
                    owner = transforms->attachedParentOf(owner);
                if (owner != entity)
                    continue;
                const auto* mesh = world.tryGetConstComponent<MeshComponent>(child);
                if (!mesh || !mesh->meshHandle.isValid())
                    continue;
                const auto model = assets.manifest().getModelEntry(mesh->meshHandle.name);
                if (!model)
                    return "a descendant mesh model is missing";
                if (model->skin.isValid())
                {
                    const auto skin = loadBakedSkin(mesh->meshHandle.name, assets.manifest());
                    if (!skin)
                        return skin.error().message;
                    if (const auto fits = checkSkinSkeleton(*skin, *skeleton); !fits)
                        return fits.error().message;
                }
            }
            auto addClip = [&](InternedString clip) -> bool
            {
                if (!clip.isValid())
                    return true;
                if (!loader->loadClip(clip, candidate))
                    return false;
                if (std::ranges::find(clips, clip) == clips.end())
                    clips.push_back(clip);
                return true;
            };
            if (const auto* player = world.tryGetConstComponent<AnimationPlayerComponent>(entity);
                player && !addClip(player->clip))
                return "the active clip is missing or incompatible";
            if (const auto* controller = world.tryGetConstComponent<AnimationControllerComponent>(entity))
            {
                if (!loader->loadController)
                    return "controller loader is unavailable";
                const auto text = loader->loadController(controller->controller);
                if (!text)
                    return text.error().message;
                const auto def = parseAnimationController(*text, controller->controller, addClip);
                if (!def)
                    return def.error().message;
                const auto hasJoint = [&](InternedString name)
                {
                    return !name.isValid() ||
                        std::ranges::any_of(newRig->joints, [&](const JointInfo& joint) { return joint.name == name; });
                };
                for (const auto& layer : def->layers)
                {
                    if (!newRig->usesLayers || !hasJoint(layer.mask.root))
                        return "controller layer is incompatible with the skeleton";
                    for (const auto& joint : layer.mask.joints)
                        if (!hasJoint(joint.joint))
                            return "controller layer references a missing joint";
                }
            }
            const auto hasJoint = [&](InternedString name)
            {
                return !name.isValid() ||
                    std::ranges::any_of(newRig->joints, [&](const JointInfo& joint) { return joint.name == name; });
            };
            for (const Chunk* chunk : world.getChunksContaining(Aspect(getComponentId<BoneAttachmentComponent>())))
            {
                const auto* attachments = chunk->readColumn<BoneAttachmentComponent>();
                for (sizet i = 0; i < chunk->size(); ++i)
                {
                    const auto& attachment = attachments[i];
                    if (attachment.target == entity && !hasJoint(attachment.joint))
                        return "a bone attachment references a missing joint";
                }
            }
            for (const Chunk* chunk : world.getChunksContaining(Aspect(getComponentId<AnimationIkComponent>())))
            {
                const auto* nodes = chunk->readColumn<AnimationIkComponent>();
                for (sizet i = 0; i < chunk->size(); ++i)
                {
                    const auto& ik = nodes[i];
                    if (ik.skin == entity &&
                        (!newRig->usesIk || !hasJoint(ik.start) || !hasJoint(ik.mid) || !hasJoint(ik.end)))
                        return "an IK node is incompatible with the skeleton";
                }
            }
            return {};
        }

        std::string playerOf(const World& world, const Entity entity)
        {
            return world.hasComponent<AnimationPlayerComponent>(entity) ? std::string{}
                                                                        : "entity " + describe(entity) +
                    " has no AnimationPlayerComponent (playAnimation with a clip adds one)";
        }

        // the finite float the number arg holds; absent leaves out empty; an error text for a value no float holds
        std::string floatArg(const ControlArgs& args, const char* key, std::optional<f32>& out)
        {
            out.reset();
            if (!args.json(key))
            {
                return {};
            }
            const std::optional<double> value = numberArg(args, key);
            if (!value || !std::isfinite(*value) || std::abs(*value) > static_cast<double>(FLT_MAX))
            {
                return std::string("'") + key + "' must be a finite number a float holds";
            }
            out = static_cast<f32>(*value);
            return {};
        }

        // the clip's manifest name when it exists and was baked for the entity's skeleton
        std::string checkedClip(const AssetRuntime& assets, const std::string& clip, const Entity entity,
                                const InternedString skeleton, InternedString& out)
        {
            const InternedString name(clip);
            const std::optional<AssetManifest::AnimationEntry> entry = assets.manifest().getAnimationEntry(name);
            if (!entry)
            {
                return "no clip '" + clip + "' (clips lists the baked ones)";
            }
            if (entry->skeleton == skeleton)
            {
                out = name;
                return {};
            }
            const auto baked = loadBakedClip(name, assets.manifest());
            const auto rig = loadBakedSkeleton(skeleton, assets.manifest());
            const std::string mismatch = "clip '" + clip + "' is baked for skeleton '" +
                std::string(viewOf(entry->skeleton)) + "', not the skeleton '" + std::string(viewOf(skeleton)) + "'";
            if (!baked || !rig)
                return mismatch + ": " + (!baked ? baked.error().message : rig.error().message);
            if (const auto fits = checkClipSkeleton(*baked, *rig); !fits)
                return mismatch + ": " + fits.error().message;
            out = name;
            return {};
        }

        const char* bindingName(const Binding binding)
        {
            switch (binding)
            {
            case Binding::Bound:
                return "bound";
            case Binding::Rejected:
                return "rejected";
            default:
                return "unbound";
            }
        }

        const char* controllerReasonName(const AnimationControllerStateComponent::Reason reason)
        {
            using Reason = AnimationControllerStateComponent::Reason;
            switch (reason)
            {
            case Reason::LoadFailed:
                return "loadFailed";
            case Reason::NotEnabled:
                return "notEnabled";
            case Reason::UnknownJoint:
                return "unknownJoint";
            default:
                return "none";
            }
        }

        const char* parameterKindName(const AnimationParameterKind kind)
        {
            return kind == AnimationParameterKind::Float ? "float"
                : kind == AnimationParameterKind::Bool   ? "bool"
                                                         : "trigger";
        }

        std::vector<Entity> skinnedEntities(const World& world)
        {
            std::vector<Entity> entities;
            for (const Chunk* chunk : world.getChunksContaining(Aspect(getComponentId<SkinComponent>())))
            {
                const std::span<const Entity> held = chunk->entities();
                entities.insert(entities.end(), held.begin(),
                                held.begin() + static_cast<std::ptrdiff_t>(chunk->size()));
            }
            std::ranges::sort(entities, {}, &Entity::id);
            return entities;
        }

        AnimatorRow animatorRow(const World& world, const AnimationWorld& animation, const Entity entity)
        {
            AnimatorRow row;
            row.entity = entity;
            row.name = std::string(entityName(world, entity));
            row.skeleton = world.getConstComponent<SkinComponent>(entity).skeleton;
            if (const auto* palette = world.tryGetConstComponent<SkinPaletteComponent>(entity))
            {
                row.joints = palette->jointCount;
                row.poseVersion = palette->poseVersion;
            }
            row.posed = !animation.palette(entity).empty();
            row.binding = "none";
            if (const auto* player = world.tryGetConstComponent<AnimationPlayerComponent>(entity))
            {
                row.player = true;
                row.clip = player->clip;
                row.time = player->time;
                row.speed = player->speed;
                row.weight = player->weight;
                row.playing = player->playing;
                row.loop = player->loop;
                row.rootMotion = player->rootMotion;
                row.binding = "unbound";
            }
            if (const auto* state = world.tryGetConstComponent<AnimationStateComponent>(entity))
            {
                row.binding = bindingName(state->binding);
                row.duration = state->duration;
                row.normalizedTime = state->normalizedTime;
                row.blend = state->blend;
            }
            row.controllerBinding = "none";
            row.controllerReason = "none";
            if (const auto* controller = world.tryGetConstComponent<AnimationControllerComponent>(entity))
            {
                row.controller = controller->controller;
                row.controllerBinding = "unbound";
            }
            if (const auto* controller = world.tryGetConstComponent<AnimationControllerStateComponent>(entity))
            {
                row.controllerBinding = bindingName(controller->binding);
                row.state = controller->state;
                row.nextState = controller->nextState;
                row.controllerReason = controllerReasonName(controller->reason);
            }
            const AnimatorLod lod = animation.animatorLod(entity);
            row.updateInterval = lod.interval;
            row.sharedGroup = lod.sharedGroup;
            row.sharedPhase = lod.sharedPhase;
            return row;
        }

        std::vector<JointRow> jointRows(const AnimationWorld& animation, const Entity entity)
        {
            const std::span<const JointInfo> joints = animation.joints(entity);
            const std::span<const PaletteJoint> palette = animation.palette(entity);
            std::vector<JointRow> rows(joints.size());
            for (u32 i = 0; i < rows.size(); ++i)
            {
                JointRow& row = rows[i];
                const JointInfo& info = joints[i];
                row.entity = entity;
                row.joint = i;
                row.name = info.name;
                row.parent = info.parent;
                if (info.parent >= 0)
                {
                    row.parentName = joints[static_cast<sizet>(info.parent)].name;
                }
                row.posed = animation.jointLocalTransform(entity, i, row.position, row.rotation, row.scale);
                if (row.posed)
                {
                    animation.jointModelTransform(entity, i, row.model);
                    if (i < palette.size())
                    {
                        std::memcpy(row.palette, palette[i].row, sizeof(row.palette));
                    }
                }
                else
                {
                    row.position = info.position;
                    row.rotation = info.rotation;
                    row.scale = info.scale;
                }
            }
            return rows;
        }

        void writeRow(const StructSchema& schema, const void* row, IFormatWriter& out)
        {
            out.beginObject({});
            writeValue(schema, row, out, FieldSet::All);
            out.endObject();
        }

        // skeleton -> its clips, by name
        std::map<std::string, std::vector<std::string>> clipsBySkeleton(const AssetManifest& manifest)
        {
            std::map<std::string, std::vector<std::string>> bySkeleton;
            for (const auto& [name, entry] : manifest.getAnimations())
            {
                bySkeleton[std::string(viewOf(entry.skeleton))].emplace_back(name.toView());
            }
            for (auto& [skeleton, clips] : bySkeleton)
            {
                std::ranges::sort(clips);
            }
            return bySkeleton;
        }

        std::string clipsQuery(const AssetRuntime& assets, const ControlArgs& args, IFormatWriter& out)
        {
            const AssetManifest& manifest = assets.manifest();
            const std::optional<std::string> skeleton = args.string("skeleton");
            if (skeleton && !manifest.getSkeletonEntry(InternedString(*skeleton)))
            {
                return "no skeleton '" + *skeleton + "' (skeletons lists the baked ones)";
            }
            std::vector<std::pair<std::string_view, AssetManifest::AnimationEntry>> rows;
            const std::unordered_map<InternedString, AssetManifest::AnimationEntry> clips = manifest.getAnimations();
            for (const auto& [name, entry] : clips)
            {
                if (!skeleton || viewOf(entry.skeleton) == *skeleton)
                {
                    rows.emplace_back(name.toView(), entry);
                }
            }
            std::ranges::sort(rows, {}, [](const auto& row) { return row.first; });
            out.beginArray("clips", static_cast<u32>(rows.size()));
            for (const auto& [name, entry] : rows)
            {
                out.beginObject({});
                out.writeString("name", name);
                out.writeString("skeleton", viewOf(entry.skeleton));
                out.writeString("model", viewOf(entry.model));
                out.writeF64("duration", entry.duration);
                out.writeU32("tracks", entry.trackCount);
                out.endObject();
            }
            out.endArray();
            return {};
        }

        std::string skeletonsQuery(const AssetRuntime& assets, IFormatWriter& out)
        {
            const AssetManifest& manifest = assets.manifest();
            const std::map<std::string, std::vector<std::string>> clips = clipsBySkeleton(manifest);
            std::vector<std::pair<std::string_view, AssetManifest::SkeletonEntry>> rows;
            const std::unordered_map<InternedString, AssetManifest::SkeletonEntry> skeletons = manifest.getSkeletons();
            for (const auto& [name, entry] : skeletons)
            {
                rows.emplace_back(name.toView(), entry);
            }
            std::ranges::sort(rows, {}, [](const auto& row) { return row.first; });
            out.beginArray("skeletons", static_cast<u32>(rows.size()));
            for (const auto& [name, entry] : rows)
            {
                out.beginObject({});
                out.writeString("name", name);
                out.writeString("model", viewOf(entry.model));
                out.writeU32("joints", entry.jointCount);
                const auto fitting = clips.find(std::string(name));
                const std::span<const std::string> names = fitting != clips.end()
                    ? std::span<const std::string>(fitting->second)
                    : std::span<const std::string>();
                out.beginArray("clips", static_cast<u32>(names.size()));
                for (const std::string& clip : names)
                {
                    out.writeString("", clip);
                }
                out.endArray();
                out.endObject();
            }
            out.endArray();
            return {};
        }

        std::string animatorsQuery(ControlServer& control, const ControlArgs& args, IFormatWriter& out)
        {
            const World* world = nullptr;
            if (std::string error = animationWorldOf(control, args, world); !error.empty())
            {
                return error;
            }
            const u64 limit = args.integer("limit").value_or(DEFAULT_LIMIT);
            if (limit > MAX_LIMIT)
            {
                return "limit must be 0.." + std::to_string(MAX_LIMIT);
            }
            const auto animation = world->services().read<AnimationWorld>();
            const std::vector<Entity> entities = skinnedEntities(*world);
            out.beginArray("animators", static_cast<u32>(std::min<u64>(limit, entities.size())));
            for (u64 i = 0; i < limit && i < entities.size(); ++i)
            {
                const AnimatorRow row = animatorRow(*world, *animation, entities[i]);
                writeRow(AnimatorRow::spiteSchema(), &row, out);
            }
            out.endArray();
            out.writeU32("total", static_cast<u32>(entities.size()));
            return {};
        }

        std::string animatorQuery(ControlServer& control, const AssetRuntime& assets, const ControlArgs& args,
                                  IFormatWriter& out)
        {
            const World* world = nullptr;
            Entity entity;
            if (std::string error = animationWorldOf(control, args, world); !error.empty())
            {
                return error;
            }
            if (std::string error = skinnedEntity(*world, args, entity); !error.empty())
            {
                return error;
            }
            const auto animation = world->services().read<AnimationWorld>();
            const AnimatorRow row = animatorRow(*world, *animation, entity);
            writeValue(AnimatorRow::spiteSchema(), &row, out, FieldSet::All);
            const std::vector<AnimationParameterInfo> parameters = animation->parameters(entity);
            out.beginArray("parameters", static_cast<u32>(parameters.size()));
            for (const AnimationParameterInfo& parameter : parameters)
            {
                out.beginObject({});
                out.writeString("name", viewOf(parameter.name));
                out.writeString("type", parameterKindName(parameter.kind));
                if (parameter.kind == AnimationParameterKind::Float)
                {
                    out.writeF32("value", parameter.value);
                }
                else
                {
                    out.writeBool("value", parameter.value != 0.0f);
                }
                out.endObject();
            }
            out.endArray();
            const std::vector<AnimationLayerInfo> layers = animation->layers(entity);
            out.beginArray("layers", static_cast<u32>(layers.size()));
            for (const AnimationLayerInfo& layer : layers)
            {
                out.beginObject({});
                out.writeString("name", viewOf(layer.name));
                out.writeString("mode", layer.additive ? "additive" : "override");
                out.writeBool("enabled", layer.enabled);
                out.writeF32("weight", layer.weight);
                out.writeString("state", viewOf(layer.state));
                out.writeString("nextState", viewOf(layer.nextState));
                out.writeF32("normalizedTime", layer.normalizedTime);
                out.writeF32("blend", layer.blend);
                out.endObject();
            }
            out.endArray();
            // where the player's root motion goes: nowhere (off), the entity's transform, or a RootMotionDeltaComponent
            // a character controller takes
            out.writeString("rootMotionMode",
                            !row.rootMotion                                             ? "off"
                                : world->hasComponent<RootMotionDeltaComponent>(entity) ? "delta"
                                                                                        : "transform");
            const Vec3 applied = animation->rootMotionApplied(entity);
            out.beginArray("rootMotionApplied", 3);
            out.writeF32("", applied.x);
            out.writeF32("", applied.y);
            out.writeF32("", applied.z);
            out.endArray();
            const std::map<std::string, std::vector<std::string>> clips = clipsBySkeleton(assets.manifest());
            const auto fitting = clips.find(std::string(viewOf(row.skeleton)));
            out.beginArray("clips", fitting != clips.end() ? static_cast<u32>(fitting->second.size()) : 0u);
            if (fitting != clips.end())
            {
                for (const std::string& clip : fitting->second)
                {
                    out.writeString("", clip);
                }
            }
            out.endArray();
            return {};
        }

        std::string jointsQuery(ControlServer& control, const ControlArgs& args, IFormatWriter& out)
        {
            const World* world = nullptr;
            Entity entity;
            if (std::string error = animationWorldOf(control, args, world); !error.empty())
            {
                return error;
            }
            if (std::string error = skinnedEntity(*world, args, entity); !error.empty())
            {
                return error;
            }
            const std::optional<std::string> name = args.string("joint");
            const std::optional<u64> index = args.integer("index");
            if (name && index)
            {
                return "pass joint (a name) or index, not both";
            }
            const auto animation = world->services().read<AnimationWorld>();
            const std::vector<JointRow> rows = jointRows(*animation, entity);
            const InternedString skeleton = world->getConstComponent<SkinComponent>(entity).skeleton;
            if (rows.empty())
            {
                return "entity " + describe(entity) + "'s skin '" + std::string(viewOf(skeleton)) +
                    "' is not bound: it binds at the start of the next frame, and a skeleton that failed to load is "
                    "named in the log";
            }
            const JointRow* only = nullptr;
            if (index)
            {
                if (*index >= rows.size())
                {
                    return "skeleton '" + std::string(viewOf(skeleton)) + "' has " + std::to_string(rows.size()) +
                        " joints, no joint " + std::to_string(*index);
                }
                only = &rows[static_cast<sizet>(*index)];
            }
            else if (name)
            {
                const auto found = std::ranges::find(rows, InternedString(*name), &JointRow::name);
                if (found == rows.end())
                {
                    std::string names;
                    for (u32 i = 0; i < rows.size() && i < LISTED_JOINT_NAMES; ++i)
                    {
                        names += (i ? ", " : "") + std::string(viewOf(rows[i].name));
                    }
                    return "skeleton '" + std::string(viewOf(skeleton)) + "' has no joint '" + *name + "' (" + names +
                        (rows.size() > LISTED_JOINT_NAMES ? ", ..." : "") + ")";
                }
                only = &*found;
            }
            out.writeU64("id", entity.id());
            out.writeString("skeleton", viewOf(skeleton));
            out.beginArray("joints", only ? 1u : static_cast<u32>(rows.size()));
            for (const JointRow& row : rows)
            {
                if (!only || &row == only)
                {
                    writeRow(JointRow::spiteSchema(), &row, out);
                }
            }
            out.endArray();
            return {};
        }

        void writeLod(const char* key, const AnimationLodSettings& lod, const bool viewer, IFormatWriter& out)
        {
            out.beginObject(key);
            out.writeBool("enabled", lod.enabled);
            out.beginArray("bands", static_cast<u32>(std::size(lod.bands)));
            for (const f32 band : lod.bands)
            {
                out.writeF32("", band);
            }
            out.endArray();
            out.writeF32("radius", lod.radius);
            out.writeU32("shareFrom", lod.shareFrom);
            out.writeU32("sharePhases", lod.sharePhases);
            out.writeBool("viewer", viewer);
            out.endObject();
        }

        // skin.instances and skin.vertices: the skinned renderables of the world that hold a joint palette on the GPU
        // and the vertices they draw; skin.cacheBytes is 0 for good: skinning is palette skinning in the shaders and
        // no skinned-vertex cache exists (animation.md)
        std::string statsQuery(ControlServer& control, const ControlArgs& args, IFormatWriter& out)
        {
            const World* world = nullptr;
            if (std::string error = animationWorldOf(control, args, world); !error.empty())
            {
                return error;
            }
            const auto animation = world->services().read<AnimationWorld>();
            const AnimationStats stats = animation->stats();
            out.writeU32("anim.active", stats.active);
            out.writeU32("anim.sampled", stats.sampled);
            out.writeU32("anim.posed", stats.posed);
            out.writeU32("anim.controllers", stats.controllers);
            out.writeU32("anim.deferred", stats.deferred);
            out.writeU32("anim.sharedSampled", stats.sharedSampled);
            out.writeU32("anim.sharedGroups", stats.sharedGroups);
            out.writeBool("advancing", animation->advancing());
            writeLod("lod", animation->lod(), animation->hasViewer(), out);
            u32 skinInstances = 0;
            u64 skinVertices = 0;
            for (const Chunk* chunk : world->getChunksContaining(Aspect(getComponentId<SkinPaletteBlockComponent>())))
            {
                const auto blocks = chunk->readColumn<SkinPaletteBlockComponent>();
                for (sizet i = 0; i < chunk->size(); ++i)
                {
                    ++skinInstances;
                    skinVertices += blocks[i].vertexCount;
                }
            }
            out.writeU32("skin.instances", skinInstances);
            out.writeU64("skin.vertices", skinVertices);
            out.writeU64("skin.cacheBytes", 0);
            out.writeU32("animators", static_cast<u32>(skinnedEntities(*world).size()));
            out.writeU32("skeletons", animation->skeletonCount());
            return {};
        }

        void recordSetOf(ControlServer& control, const Entity entity, const char* component, const char* field,
                         Json value)
        {
            control.recordInverse(
                "set",
                Json{{"id", entity.id()}, {"path", std::string(component) + "." + field}, {"value", std::move(value)}}
                    .dump());
        }

        void recordSet(ControlServer& control, const Entity entity, const char* field, Json value)
        {
            recordSetOf(control, entity, "AnimationPlayerComponent", field, std::move(value));
        }

        // the inverse of an edit of the player: a set per changed field; the time of a seek or stop is not one. A
        // changed clip goes back through playAnimation (then pauseAnimation when it was paused), which also drops a
        // crossfade that was asked for and not bound yet.
        void recordRestore(ControlServer& control, const Entity entity, const AnimationPlayerComponent& before,
                           const AnimationPlayerComponent& now)
        {
            if (before.clip != now.clip && before.clip.isValid())
            {
                control.recordInverse(
                    "playAnimation",
                    Json{{"id", entity.id()}, {"clip", std::string(viewOf(before.clip))}, {"time", before.time}}
                        .dump());
                if (!before.playing)
                {
                    control.recordInverse("pauseAnimation", Json{{"id", entity.id()}}.dump());
                }
            }
            else
            {
                if (before.clip != now.clip)
                {
                    recordSet(control, entity, "clip", std::string());
                }
                if (before.time != now.time)
                {
                    recordSet(control, entity, "time", before.time);
                }
                if (before.playing != now.playing)
                {
                    recordSet(control, entity, "playing", before.playing);
                }
            }
            if (before.speed != now.speed)
            {
                recordSet(control, entity, "speed", before.speed);
            }
        }

        // writes the player only when a field differs: a mutable access marks the component edited and wakes the
        // animator, so a command that changes nothing leaves the scene quiescent
        void storePlayer(const World& world, const Entity entity, const AnimationPlayerComponent& before,
                         const AnimationPlayerComponent& now)
        {
            if (before.clip != now.clip || before.time != now.time || before.speed != now.speed ||
                before.playing != now.playing)
            {
                AnimationPlayerComponent& player = world.getComponent<AnimationPlayerComponent>(entity);
                player.clip = now.clip;
                player.time = now.time;
                player.speed = now.speed;
                player.playing = now.playing;
            }
        }

        void recordSeek(ControlServer& control, const Entity entity, const f32 time)
        {
            control.recordInverse("seekAnimation", Json{{"id", entity.id()}, {"time", time}}.dump());
        }

        void writePrevious(const AnimationPlayerComponent& before, IFormatWriter& out)
        {
            out.beginObject("previous");
            out.writeString("clip", viewOf(before.clip));
            out.writeF32("time", before.time);
            out.writeF32("speed", before.speed);
            out.writeBool("playing", before.playing);
            out.endObject();
        }

        // the clip's duration when the entity's player has it bound; the refusal says why not
        std::string boundDuration(const World& world, const Entity entity, f32& duration)
        {
            const AnimationPlayerComponent& player = world.getConstComponent<AnimationPlayerComponent>(entity);
            if (!player.clip.isValid())
            {
                return "entity " + describe(entity) + " names no clip (playAnimation with a clip sets one)";
            }
            const std::string clip =
                "entity " + describe(entity) + "'s clip '" + std::string(viewOf(player.clip)) + "'";
            const auto* state = world.tryGetConstComponent<AnimationStateComponent>(entity);
            if (state && state->binding == Binding::Rejected)
            {
                return clip +
                    " was rejected when it was bound (unreadable or baked for another skeleton): the log "
                    "says why";
            }
            if (!state || state->binding != Binding::Bound)
            {
                return clip + " is not bound yet: a new player or clip binds at the start of the next frame";
            }
            duration = state->duration;
            return {};
        }

        std::string playAnimation(ControlServer& control, const AssetRuntime& assets, const ControlArgs& args,
                                  IFormatWriter& out)
        {
            const World* world = nullptr;
            Entity entity;
            if (std::string error = animationWorldOf(control, args, world); !error.empty())
            {
                return error;
            }
            if (std::string error = skinnedEntity(*world, args, entity); !error.empty())
            {
                return error;
            }
            std::optional<f32> fade;
            std::optional<f32> time;
            if (std::string error = floatArg(args, "fade", fade); !error.empty())
            {
                return error;
            }
            if (std::string error = floatArg(args, "time", time); !error.empty())
            {
                return error;
            }
            if (fade && *fade < 0.0f)
            {
                return std::string("'fade' is seconds, 0 or more");
            }
            InternedString clip;
            const std::optional<std::string> clipName = args.string("clip");
            if (clipName)
            {
                const InternedString skeleton = world->getConstComponent<SkinComponent>(entity).skeleton;
                if (std::string error = checkedClip(assets, *clipName, entity, skeleton, clip); !error.empty())
                {
                    return error;
                }
            }
            const AnimationPlayerComponent* existing = world->tryGetConstComponent<AnimationPlayerComponent>(entity);
            if (!existing)
            {
                if (!clipName)
                {
                    return "entity " + describe(entity) + " has no AnimationPlayerComponent: pass clip to add one";
                }
                world->addComponent<AnimationPlayerComponent>(
                    entity, AnimationPlayerComponent{.clip = clip, .time = time.value_or(0.0f)});
                control.recordInverse("removeComponent",
                                      Json{{"id", entity.id()}, {"component", "AnimationPlayerComponent"}}.dump());
                out.writeU64("id", entity.id());
                out.writeString("clip", *clipName);
                out.writeBool("playing", true);
                out.writeBool("faded", false);
                out.writeBool("added", true);
                return {};
            }
            if (!clipName && !existing->clip.isValid())
            {
                return "entity " + describe(entity) + " names no clip: pass clip";
            }
            const AnimationPlayerComponent before = *existing;
            AnimationPlayerComponent now = before;
            bool faded = false;
            bool cut = false;
            if (clipName && clip != before.clip)
            {
                if (fade && *fade > 0.0f)
                {
                    const auto animation = world->services().write<AnimationWorld>();
                    faded = animation->crossfade(entity, clip, *fade, time.value_or(0.0f));
                }
                cut = !faded;
                now.clip = clip;
                now.time = time.value_or(0.0f);
            }
            else if (time)
            {
                now.time = *time;
            }
            else if (!before.loop)
            {
                const auto* state = world->tryGetConstComponent<AnimationStateComponent>(entity);
                if (state && state->duration > 0.0f && before.time >= state->duration)
                {
                    now.time = 0.0f;
                }
            }
            now.playing = true;
            storePlayer(*world, entity, before, now);
            if (cut)
            {
                // a crossfade asked for earlier and not bound yet (the undo of a fading play lands here) is superseded
                const auto animation = world->services().write<AnimationWorld>();
                animation->cancelCrossfade(entity);
            }
            recordRestore(control, entity, before, now);
            out.writeU64("id", entity.id());
            out.writeString("clip", viewOf(now.clip));
            out.writeBool("playing", true);
            out.writeBool("faded", faded);
            writePrevious(before, out);
            return {};
        }

        // The inverse addComponent of an AnimationControllerComponent records: the player AnimationBind adds to an
        // entity that has none comes with the controller, so its undo takes both away. Nothing to remove (the add was
        // undone before a bind ran) is fine.
        std::string removeAnimationPlayer(ControlServer& control, const ControlArgs& args, IFormatWriter& out)
        {
            const World* world = nullptr;
            if (std::string error = animationWorldOf(control, args, world); !error.empty())
            {
                return error;
            }
            const std::optional<u64> id = args.integer("id");
            if (!id)
            {
                return std::string("missing integer arg 'id'");
            }
            const Entity entity(*id);
            if (!world->isEntityAlive(entity))
            {
                return "no live entity " + std::to_string(*id);
            }
            const AnimationPlayerComponent* held = world->tryGetConstComponent<AnimationPlayerComponent>(entity);
            if (held)
            {
                control.recordInverse("addComponent",
                                      Json{{"id", entity.id()},
                                           {"component", "AnimationPlayerComponent"},
                                           {"value",
                                            {{"clip", std::string(viewOf(held->clip))},
                                             {"time", held->time},
                                             {"speed", held->speed},
                                             {"weight", held->weight},
                                             {"playing", held->playing},
                                             {"loop", held->loop},
                                             {"rootMotion", held->rootMotion}}}}
                                          .dump());
                world->removeComponent<AnimationPlayerComponent>(entity);
            }
            out.writeU64("id", entity.id());
            out.writeBool("removed", held != nullptr);
            return {};
        }

        std::string pauseAnimation(ControlServer& control, const ControlArgs& args, IFormatWriter& out)
        {
            const World* world = nullptr;
            Entity entity;
            if (std::string error = animationWorldOf(control, args, world); !error.empty())
            {
                return error;
            }
            if (std::string error = skinnedEntity(*world, args, entity); !error.empty())
            {
                return error;
            }
            if (std::string error = playerOf(*world, entity); !error.empty())
            {
                return error;
            }
            const AnimationPlayerComponent before = world->getConstComponent<AnimationPlayerComponent>(entity);
            AnimationPlayerComponent now = before;
            now.playing = false;
            storePlayer(*world, entity, before, now);
            recordRestore(control, entity, before, now);
            out.writeU64("id", entity.id());
            out.writeBool("playing", false);
            writePrevious(before, out);
            return {};
        }

        std::string stopAnimation(ControlServer& control, const ControlArgs& args, IFormatWriter& out)
        {
            const World* world = nullptr;
            Entity entity;
            if (std::string error = animationWorldOf(control, args, world); !error.empty())
            {
                return error;
            }
            if (std::string error = skinnedEntity(*world, args, entity); !error.empty())
            {
                return error;
            }
            if (std::string error = playerOf(*world, entity); !error.empty())
            {
                return error;
            }
            const AnimationPlayerComponent before = world->getConstComponent<AnimationPlayerComponent>(entity);
            f32 duration = 0.0f;
            bool shown = false;
            // an animator already stopped at its start has nothing to rewind
            if ((before.playing || before.time != 0.0f) && boundDuration(*world, entity, duration).empty())
            {
                const auto animation = world->services().write<AnimationWorld>();
                shown = animation->evaluateAt(entity, 0.0f);
            }
            AnimationPlayerComponent now = before;
            now.time = 0.0f;
            now.playing = false;
            storePlayer(*world, entity, before, now);
            if (before.playing)
            {
                recordSet(control, entity, "playing", true);
            }
            if (before.time != now.time)
            {
                if (shown)
                {
                    recordSeek(control, entity, before.time);
                }
                else
                {
                    recordSet(control, entity, "time", before.time);
                }
            }
            out.writeU64("id", entity.id());
            out.writeBool("playing", false);
            out.writeF32("time", 0.0f);
            writePrevious(before, out);
            return {};
        }

        std::string setAnimationSpeed(ControlServer& control, const ControlArgs& args, IFormatWriter& out)
        {
            const World* world = nullptr;
            Entity entity;
            if (std::string error = animationWorldOf(control, args, world); !error.empty())
            {
                return error;
            }
            if (std::string error = skinnedEntity(*world, args, entity); !error.empty())
            {
                return error;
            }
            if (std::string error = playerOf(*world, entity); !error.empty())
            {
                return error;
            }
            std::optional<f32> speed;
            if (std::string error = floatArg(args, "speed", speed); !error.empty())
            {
                return error;
            }
            if (!speed)
            {
                return std::string("missing number arg 'speed'");
            }
            const AnimationPlayerComponent before = world->getConstComponent<AnimationPlayerComponent>(entity);
            AnimationPlayerComponent now = before;
            now.speed = *speed;
            storePlayer(*world, entity, before, now);
            recordRestore(control, entity, before, now);
            out.writeU64("id", entity.id());
            out.writeF32("speed", *speed);
            writePrevious(before, out);
            return {};
        }

        std::string seekAnimation(ControlServer& control, const ControlArgs& args, IFormatWriter& out)
        {
            const World* world = nullptr;
            Entity entity;
            if (std::string error = animationWorldOf(control, args, world); !error.empty())
            {
                return error;
            }
            if (std::string error = skinnedEntity(*world, args, entity); !error.empty())
            {
                return error;
            }
            if (std::string error = playerOf(*world, entity); !error.empty())
            {
                return error;
            }
            std::optional<f32> time;
            std::optional<f32> normalized;
            if (std::string error = floatArg(args, "time", time); !error.empty())
            {
                return error;
            }
            if (std::string error = floatArg(args, "normalized", normalized); !error.empty())
            {
                return error;
            }
            if (time.has_value() == normalized.has_value())
            {
                return std::string("seekAnimation needs time (seconds) or normalized (0..1), one of them");
            }
            if (normalized && (*normalized < 0.0f || *normalized > 1.0f))
            {
                return std::string("'normalized' is 0..1 of the clip");
            }
            f32 duration = 0.0f;
            if (std::string error = boundDuration(*world, entity, duration); !error.empty())
            {
                return error;
            }
            const AnimationPlayerComponent& player = world->getConstComponent<AnimationPlayerComponent>(entity);
            const AnimationPlayerComponent before = player;
            const f32 seconds = time ? *time : *normalized * duration;
            {
                const auto animation = world->services().write<AnimationWorld>();
                if (!animation->evaluateAt(entity, seconds))
                {
                    return "entity " + describe(entity) + "'s clip cannot be evaluated (it is not bound)";
                }
            }
            if (before.time != player.time)
            {
                recordSeek(control, entity, before.time);
            }
            out.writeU64("id", entity.id());
            out.writeString("clip", viewOf(player.clip));
            out.writeF32("time", player.time);
            out.writeF32("normalizedTime", duration > 0.0f ? player.time / duration : 0.0f);
            out.writeF32("duration", duration);
            writePrevious(before, out);
            return {};
        }

        Json parameterJson(const AnimationParameterInfo& parameter)
        {
            return parameter.kind == AnimationParameterKind::Float ? Json(parameter.value)
                                                                   : Json(parameter.value != 0.0f);
        }

        std::string setAnimationParameter(ControlServer& control, const ControlArgs& args, IFormatWriter& out)
        {
            const World* world = nullptr;
            Entity entity;
            if (std::string error = animationWorldOf(control, args, world); !error.empty())
            {
                return error;
            }
            if (std::string error = skinnedEntity(*world, args, entity); !error.empty())
            {
                return error;
            }
            const std::optional<std::string> name = args.string("name");
            if (!name)
            {
                return std::string("missing string arg 'name'");
            }
            const auto animation = world->services().read<AnimationWorld>();
            const std::vector<AnimationParameterInfo> parameters = animation->parameters(entity);
            if (parameters.empty())
            {
                return "entity " + describe(entity) +
                    " has no bound controller: it needs an AnimationControllerComponent whose asset loaded (animator "
                    "reports controllerBinding), bound at the start of the next frame";
            }
            const auto found = std::ranges::find_if(parameters, [&name](const AnimationParameterInfo& parameter)
                                                    { return viewOf(parameter.name) == *name; });
            if (found == parameters.end())
            {
                std::string names;
                for (const AnimationParameterInfo& parameter : parameters)
                {
                    names += (names.empty() ? "" : ", ") + std::string(viewOf(parameter.name));
                }
                return "controller has no parameter '" + *name + "' (" + names + ")";
            }
            f32 value = 0.0f;
            if (found->kind == AnimationParameterKind::Float)
            {
                std::optional<f32> number;
                if (args.boolean("value"))
                {
                    return "parameter '" + *name + "' is a float: value is a number, not true or false";
                }
                if (std::string error = floatArg(args, "value", number); !error.empty())
                {
                    return error;
                }
                if (!number)
                {
                    return "parameter '" + *name + "' is a float: value is a number";
                }
                value = *number;
            }
            else
            {
                const std::optional<bool> flag = args.boolean("value");
                if (!flag)
                {
                    return "parameter '" + *name + "' is a " + parameterKindName(found->kind) + ": value is true" +
                        (found->kind == AnimationParameterKind::Trigger ? " (sets it) or false (clears it)"
                                                                        : " or false");
                }
                value = *flag ? 1.0f : 0.0f;
            }
            const AnimationParameterInfo before = *found;
            if (const AnimationParameterResult result = animation->setParameter(entity, *name, found->kind, value);
                result != AnimationParameterResult::Ok)
            {
                return "parameter '" + *name + "' was refused (the controller changed under the call)";
            }
            AnimationParameterInfo now = before;
            now.value = value;
            if (before.value != now.value)
            {
                control.recordInverse(
                    "setAnimationParameter",
                    Json{{"id", entity.id()}, {"name", *name}, {"value", parameterJson(before)}}.dump());
            }
            out.writeU64("id", entity.id());
            out.writeString("name", *name);
            out.writeString("type", parameterKindName(before.kind));
            if (before.kind == AnimationParameterKind::Float)
            {
                out.writeF32("value", now.value);
                out.writeF32("previous", before.value);
            }
            else
            {
                out.writeBool("value", now.value != 0.0f);
                out.writeBool("previous", before.value != 0.0f);
            }
            return {};
        }

        std::string previewAnimation(ControlServer& control, const ControlArgs& args, IFormatWriter& out)
        {
            const World* world = nullptr;
            if (std::string error = animationWorldOf(control, args, world); !error.empty())
            {
                return error;
            }
            if (control.worldName(args) == "play")
            {
                return std::string("the play world always advances its animators: preview is for the edit world");
            }
            const auto animation = world->services().write<AnimationWorld>();
            const bool now = args.boolean("on").value_or(!animation->advancing());
            const bool before = animation->advancing();
            animation->setAdvancing(now);
            out.writeBool("advancing", now);
            out.writeBool("previous", before);
            return {};
        }

        using AttachmentState = BoneAttachmentStateComponent;

        const char* reasonName(const AttachmentState::Reason reason)
        {
            switch (reason)
            {
            case AttachmentState::Reason::NoTarget:
                return "noTarget";
            case AttachmentState::Reason::NoSkin:
                return "noSkin";
            case AttachmentState::Reason::UnknownJoint:
                return "unknownJoint";
            case AttachmentState::Reason::NotChild:
                return "notChild";
            default:
                return "none";
            }
        }

        std::vector<Entity> attachmentEntities(const World& world)
        {
            std::vector<Entity> entities;
            for (const Chunk* chunk : world.getChunksContaining(Aspect(getComponentId<BoneAttachmentComponent>())))
            {
                const std::span<const Entity> held = chunk->entities();
                entities.insert(entities.end(), held.begin(),
                                held.begin() + static_cast<std::ptrdiff_t>(chunk->size()));
            }
            std::ranges::sort(entities, {}, &Entity::id);
            return entities;
        }

        AttachmentRow attachmentRow(const World& world, const Entity entity)
        {
            const BoneAttachmentComponent& attachment = world.getConstComponent<BoneAttachmentComponent>(entity);
            AttachmentRow row;
            row.entity = entity;
            row.name = std::string(entityName(world, entity));
            row.target = attachment.target;
            if (attachment.target.isValid() && world.isEntityAlive(attachment.target))
            {
                row.targetName = std::string(entityName(world, attachment.target));
            }
            row.joint = attachment.joint;
            row.position = attachment.position;
            row.rotation = attachment.rotation;
            row.scale = attachment.scale;
            row.binding = "unbound";
            row.reason = "none";
            if (const auto* state = world.tryGetConstComponent<AttachmentState>(entity))
            {
                row.binding = bindingName(state->binding);
                row.reason = reasonName(state->reason);
                row.jointIndex = state->jointIndex;
            }
            return row;
        }

        std::string attachmentsQuery(ControlServer& control, const ControlArgs& args, IFormatWriter& out)
        {
            const World* world = nullptr;
            if (std::string error = animationWorldOf(control, args, world); !error.empty())
            {
                return error;
            }
            const u64 limit = args.integer("limit").value_or(DEFAULT_LIMIT);
            if (limit > MAX_LIMIT)
            {
                return "limit must be 0.." + std::to_string(MAX_LIMIT);
            }
            const std::optional<u64> target = args.integer("target");
            std::vector<Entity> entities = attachmentEntities(*world);
            if (target)
            {
                std::erase_if(
                    entities, [&](const Entity entity)
                    { return world->getConstComponent<BoneAttachmentComponent>(entity).target.id() != *target; });
            }
            out.beginArray("attachments", static_cast<u32>(std::min<u64>(limit, entities.size())));
            for (u64 i = 0; i < limit && i < entities.size(); ++i)
            {
                const AttachmentRow row = attachmentRow(*world, entities[i]);
                writeRow(AttachmentRow::spiteSchema(), &row, out);
            }
            out.endArray();
            out.writeU32("total", static_cast<u32>(entities.size()));
            return {};
        }

        // a JSON array of count finite numbers, when the arg is given
        std::string numbersArg(const ControlArgs& args, const char* key, const u32 count, f32* out, bool& given)
        {
            given = false;
            const std::optional<std::string> text = args.json(key);
            if (!text)
            {
                return {};
            }
            const Json value = Json::parse(*text, nullptr, false);
            if (!value.is_array() || value.size() != count)
            {
                return std::string("'") + key + "' must be an array of " + std::to_string(count) + " numbers";
            }
            for (u32 i = 0; i < count; ++i)
            {
                const double number = value[i].is_number() ? value[i].get<double>() : HUGE_VAL;
                if (!std::isfinite(number) || std::abs(number) > static_cast<double>(FLT_MAX))
                {
                    return std::string("'") + key + "' must hold finite numbers a float holds";
                }
                out[i] = static_cast<f32>(number);
            }
            given = true;
            return {};
        }

        // what the call leaves out stays as it was
        std::string setAnimationLod(ControlServer& control, const ControlArgs& args, IFormatWriter& out)
        {
            const World* world = nullptr;
            if (std::string error = animationWorldOf(control, args, world); !error.empty())
            {
                return error;
            }
            const auto animation = world->services().write<AnimationWorld>();
            const AnimationLodSettings before = animation->lod();
            AnimationLodSettings lod = before;
            lod.enabled = args.boolean("enabled").value_or(lod.enabled);
            bool given = false;
            f32 bands[std::size(AnimationLodSettings{}.bands)] = {};
            if (std::string error = numbersArg(args, "bands", static_cast<u32>(std::size(bands)), bands, given);
                !error.empty())
            {
                return error;
            }
            if (given)
            {
                for (u32 i = 0; i < std::size(bands); ++i)
                {
                    if (bands[i] < 0.0f || (i > 0 && bands[i] < bands[i - 1]))
                    {
                        return std::string("'bands' must be ascending distances of 0 or more metres");
                    }
                    lod.bands[i] = bands[i];
                }
            }
            std::optional<f32> radius;
            if (std::string error = floatArg(args, "radius", radius); !error.empty())
            {
                return error;
            }
            if (radius && *radius < 0.0f)
            {
                return std::string("'radius' must be 0 or more metres");
            }
            lod.radius = radius.value_or(lod.radius);
            const std::optional<u64> shareFrom = args.integer("shareFrom");
            if (shareFrom && *shareFrom > ANIMATION_LOD_SLOWEST_INTERVAL)
            {
                return "'shareFrom' must be 0.." + std::to_string(ANIMATION_LOD_SLOWEST_INTERVAL);
            }
            lod.shareFrom = shareFrom ? static_cast<u32>(*shareFrom) : lod.shareFrom;
            const std::optional<u64> phases = args.integer("sharePhases");
            if (phases && *phases > MAX_SHARE_PHASES)
            {
                return "'sharePhases' must be 0.." + std::to_string(MAX_SHARE_PHASES);
            }
            lod.sharePhases = phases ? static_cast<u32>(*phases) : lod.sharePhases;
            animation->setLod(lod);
            writeLod("lod", lod, animation->hasViewer(), out);
            writeLod("previous", before, animation->hasViewer(), out);
            return {};
        }

        Json vectorJson(const Vec3& value) { return Json::array({value.x, value.y, value.z}); }
        Json quatJson(const Quat& value) { return Json::array({value.x, value.y, value.z, value.w}); }

        // Makes an entity follow a joint of a skinned entity (BoneAttachmentComponent), or edits the attachment it has:
        // what the call leaves out stays as the attachment holds it (identity for a new one). Everything the bind would
        // refuse later is refused here, naming the entity and the joint.
        std::string attachToBone(ControlServer& control, const ControlArgs& args, IFormatWriter& out)
        {
            const World* world = nullptr;
            if (std::string error = animationWorldOf(control, args, world); !error.empty())
            {
                return error;
            }
            const std::optional<u64> id = args.integer("id");
            if (!id)
            {
                return std::string("missing integer arg 'id'");
            }
            const Entity child(*id);
            if (!world->isEntityAlive(child))
            {
                return "no live entity " + std::to_string(*id);
            }
            if (!world->hasComponent<TransformComponent>(child))
            {
                return "entity " + describe(child) +
                    " has no TransformComponent: only a transform entity can follow a bone";
            }
            const std::optional<std::string> jointName = args.string("joint");
            if (!jointName || jointName->empty())
            {
                return std::string("missing string arg 'joint' (joints lists a skeleton's joints)");
            }
            const bool hasHierarchy = world->services().contains<EntityTransformManager>();
            const auto parentOf = [&](const Entity entity)
            { return world->services().read<EntityTransformManager>()->currentParentOf(entity); };
            Entity target = hasHierarchy ? parentOf(child) : Entity::undefined();
            if (const std::optional<u64> given = args.integer("target"))
            {
                target = Entity(*given);
            }
            if (!target.isValid())
            {
                return "entity " + describe(child) +
                    " has no parent to follow: pass target, the skinned entity, and reparent the entity under it";
            }
            if (!world->isEntityAlive(target))
            {
                return "no live target entity " + describe(target);
            }
            if (!world->hasComponent<SkinComponent>(target))
            {
                return "target entity " + describe(target) +
                    " has no SkinComponent: only the root of a rigged model animates (animators lists them)";
            }
            const auto animation = world->services().read<AnimationWorld>();
            const std::span<const JointInfo> joints = animation->joints(target);
            const InternedString skeleton = world->getConstComponent<SkinComponent>(target).skeleton;
            if (joints.empty())
            {
                return "target entity " + describe(target) + "'s skin '" + std::string(viewOf(skeleton)) +
                    "' is not bound: it binds at the start of the next frame, and a skeleton that failed to load is "
                    "named in the log";
            }
            const InternedString joint(*jointName);
            if (std::ranges::find(joints, joint, &JointInfo::name) == joints.end())
            {
                std::string names;
                for (u32 i = 0; i < joints.size() && i < LISTED_JOINT_NAMES; ++i)
                {
                    names += (i ? ", " : "") + std::string(viewOf(joints[i].name));
                }
                return "entity " + describe(target) + " (skeleton '" + std::string(viewOf(skeleton)) +
                    "') has no joint '" + *jointName + "' (" + names +
                    (joints.size() > LISTED_JOINT_NAMES ? ", ..." : "") + ")";
            }
            if (hasHierarchy && parentOf(child) != target)
            {
                return "entity " + describe(child) + " is not a child of target entity " + describe(target) +
                    ": reparent it first (reparent id=" + describe(child) + " parent=" + describe(target) +
                    "), its transform is placed in the target's space";
            }
            if (const auto* body = world->tryGetConstComponent<RigidbodyComponent>(child);
                body && body->bodyType == RigidbodyComponent::BodyType::Dynamic)
            {
                return "entity " + describe(child) +
                    " has a dynamic rigidbody: a dynamic body is a world-space root and cannot follow a bone (make it "
                    "kinematic or static)";
            }

            const BoneAttachmentComponent* held = world->tryGetConstComponent<BoneAttachmentComponent>(child);
            const BoneAttachmentComponent before = held ? *held : BoneAttachmentComponent{};
            BoneAttachmentComponent now = before;
            now.target = target;
            now.joint = joint;
            f32 values[4];
            bool given = false;
            if (std::string error = numbersArg(args, "position", 3, values, given); !error.empty())
            {
                return error;
            }
            if (given)
            {
                now.position = Vec3(values[0], values[1], values[2]);
            }
            if (std::string error = numbersArg(args, "rotation", 4, values, given); !error.empty())
            {
                return error;
            }
            if (given)
            {
                const Quat turn(values[0], values[1], values[2], values[3]);
                if (!(quatLength(turn) > 0.0f))
                {
                    return std::string("'rotation' is a quaternion [x, y, z, w] of non-zero length");
                }
                now.rotation = normalize(turn);
            }
            if (std::string error = numbersArg(args, "scale", 3, values, given); !error.empty())
            {
                return error;
            }
            if (given)
            {
                now.scale = Vec3(values[0], values[1], values[2]);
            }

            const bool changed = !held || before.target != now.target || before.joint != now.joint ||
                before.position != now.position || before.rotation != now.rotation || before.scale != now.scale;
            if (!held)
            {
                // the bind places the child at the bone, so undo gives its transform back as well as the component
                const TransformComponent prior = world->getConstComponent<TransformComponent>(child);
                world->addComponent<BoneAttachmentComponent>(child, now);
                control.recordInverse("removeComponent",
                                      Json{{"id", child.id()}, {"component", "BoneAttachmentComponent"}}.dump());
                recordSetOf(control, child, "TransformComponent", "position", vectorJson(prior.position));
                recordSetOf(control, child, "TransformComponent", "rotation", quatJson(prior.rotation));
                recordSetOf(control, child, "TransformComponent", "scale", vectorJson(prior.scale));
            }
            else if (changed)
            {
                // the inverse is a set per changed field: it takes whatever the attachment held, a joint that no
                // longer exists included. Only a field that differs is written, so a call that changes nothing leaves
                // the scene quiescent.
                BoneAttachmentComponent& edited = world->getComponent<BoneAttachmentComponent>(child);
                const char* const component = "BoneAttachmentComponent";
                if (before.target != now.target)
                {
                    edited.target = now.target;
                    recordSetOf(control, child, component, "target", before.target.id());
                }
                if (before.joint != now.joint)
                {
                    edited.joint = now.joint;
                    recordSetOf(control, child, component, "joint", std::string(viewOf(before.joint)));
                }
                if (before.position != now.position)
                {
                    edited.position = now.position;
                    recordSetOf(control, child, component, "position", vectorJson(before.position));
                }
                if (before.rotation != now.rotation)
                {
                    edited.rotation = now.rotation;
                    recordSetOf(control, child, component, "rotation", quatJson(before.rotation));
                }
                if (before.scale != now.scale)
                {
                    edited.scale = now.scale;
                    recordSetOf(control, child, component, "scale", vectorJson(before.scale));
                }
            }
            out.writeU64("id", child.id());
            out.writeU64("target", target.id());
            out.writeString("joint", *jointName);
            out.writeBool("added", !held);
            out.writeBool("changed", changed);
            return {};
        }
    } // namespace

    namespace
    {
        // a name that becomes a path is whitelisted: components of letters, digits, '_', '-' and '.', joined
        // by '/', none empty, none starting or ending with a dot, none a Windows device name (also with an extension)
        bool safeControllerName(const std::string& name)
        {
            static constexpr std::string_view DEVICES[] = {"CON", "PRN", "AUX", "NUL", "CONIN", "CONOUT", "CLOCK"};
            if (name.empty() || name.size() > 240)
            {
                return false;
            }
            sizet start = 0;
            while (start <= name.size())
            {
                sizet end = name.find('/', start);
                end = end == std::string::npos ? name.size() : end;
                const std::string_view part(name.data() + start, end - start);
                if (part.empty() || part.front() == '.' || part.back() == '.')
                {
                    return false;
                }
                for (const char c : part)
                {
                    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-' || c == '.';
                    if (!ok)
                    {
                        return false;
                    }
                }
                std::string stem(part.substr(0, part.find('.')));
                std::ranges::transform(stem, stem.begin(),
                                       [](const char c) { return static_cast<char>(std::toupper(c)); });
                const bool numbered = stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
                    stem[3] >= '0' && stem[3] <= '9';
                if (numbered || std::ranges::find(DEVICES, std::string_view(stem)) != std::end(DEVICES))
                {
                    return false;
                }
                start = end + 1;
            }
            return true;
        }
    } // namespace

    std::function<Expected<std::string>(InternedString)> animationControllerReader(std::filesystem::path root)
    {
        return [root = std::move(root)](const InternedString name) -> Expected<std::string>
        {
            const std::string text(viewOf(name));
            if (!safeControllerName(text))
            {
                return std::unexpected(makeError(ErrorCode::InvalidPath,
                                                 "'" + text +
                                                     "' is not a name the animation root takes (letters, digits, _ - . "
                                                     "and / between folders; no leading or trailing dot, no device "
                                                     "name)"));
            }
            std::ifstream file(root / std::filesystem::path(text), std::ios::binary);
            if (!file)
            {
                return std::unexpected(
                    makeError(ErrorCode::FileNotFound, "no file '" + text + "' under the animation root"));
            }
            return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        };
    }
    bool animatorsAdvance(const World& world)
    {
        return !world.services().contains<AnimationWorld>() || world.services().read<AnimationWorld>()->advancing();
    }

    void animatorBones(const World& world, const Entity animator, std::vector<AnimatorBone>& out)
    {
        if (!world.isEntityAlive(animator) || !world.hasComponent<SkinComponent>(animator) ||
            !world.services().contains<AnimationWorld>())
        {
            return;
        }
        const auto animation = world.services().read<AnimationWorld>();
        const std::span<const JointInfo> joints = animation->joints(animator);
        const Mat4 root = world.hasComponent<TransformMatrixComponent>(animator)
            ? world.getConstComponent<TransformMatrixComponent>(animator).matrix
            : Mat4(1.0f);
        const auto place = [&root](const Mat4& model)
        {
            const Vec4 at = root * Vec4(model[3][0], model[3][1], model[3][2], 1.0f);
            return Vec3(at.x, at.y, at.z);
        };
        const Vec3 origin = place(Mat4(1.0f));
        std::vector<Vec3> heads(joints.size());
        for (u32 i = 0; i < joints.size(); ++i)
        {
            Mat4 model(1.0f);
            if (!animation->jointModelTransform(animator, i, model))
            {
                return;
            }
            heads[i] = place(model);
        }
        for (u32 i = 0; i < joints.size(); ++i)
        {
            const i32 parent = joints[i].parent;
            out.push_back(
                {.from = parent >= 0 ? heads[static_cast<sizet>(parent)] : origin, .to = heads[i], .joint = i});
        }
    }

    void registerAnimationTables(const World& world)
    {
        ServiceLocator& services = world.services();
        services.registerTable<AnimationWorld>("animators", AnimatorRow::spiteSchema(),
                                               [world](const AnimationWorld& animation, const TableRowSink& sink)
                                               {
                                                   for (const Entity entity : skinnedEntities(world))
                                                   {
                                                       const AnimatorRow row = animatorRow(world, animation, entity);
                                                       sink(&row);
                                                   }
                                               });
        services.registerTable<AnimationWorld>("attachments", AttachmentRow::spiteSchema(),
                                               [world](const AnimationWorld&, const TableRowSink& sink)
                                               {
                                                   for (const Entity entity : attachmentEntities(world))
                                                   {
                                                       const AttachmentRow row = attachmentRow(world, entity);
                                                       sink(&row);
                                                   }
                                               });
        services.registerTable<AnimationWorld>("joints", JointRow::spiteSchema(),
                                               [world](const AnimationWorld& animation, const TableRowSink& sink)
                                               {
                                                   for (const Entity entity : skinnedEntities(world))
                                                   {
                                                       for (const JointRow& row : jointRows(animation, entity))
                                                       {
                                                           sink(&row);
                                                       }
                                                   }
                                               });
    }

    void registerAnimationCommands(ControlServer& control, AssetRuntime& assets)
    {
        control.registerQuery("skeletonCompatibility", OWNER,
                              "whether a skeleton can replace an entity's current rig; compatible is a boolean and "
                              "reason is empty when compatible or explains the refusal",
                              {WORLD_ARG, ID_ARG, requiredArg("skeleton", ArgKind::String, "candidate skeleton name")},
                              [&control, &assets](const ControlArgs& args, IFormatWriter& out)
                              {
                                  const World* world = nullptr;
                                  if (std::string error = animationWorldOf(control, args, world); !error.empty())
                                      return error;
                                  Entity entity;
                                  if (std::string error = skinnedEntity(*world, args, entity); !error.empty())
                                      return error;
                                  std::vector<InternedString> clips;
                                  const std::string reason =
                                      compatibleSkeleton(*world, assets, entity,
                                                         InternedString(args.string("skeleton").value_or("")), clips);
                                  out.writeBool("compatible", reason.empty());
                                  out.writeString("reason", reason);
                                  return std::string{};
                              });
        control.registerUndoableCommand(
            "setSkinSkeleton", OWNER,
            "switches an entity to a compatible baked skeleton; on a change replies id (the entity) and skeleton "
            "(the new name), while an unchanged binding returns no fields",
            {WORLD_ARG, ID_ARG, requiredArg("skeleton", ArgKind::String, "candidate skeleton name")},
            [&control, &assets](const ControlArgs& args, IFormatWriter& out)
            {
                const World* world = nullptr;
                if (std::string error = animationWorldOf(control, args, world); !error.empty())
                    return error;
                Entity entity;
                if (std::string error = skinnedEntity(*world, args, entity); !error.empty())
                    return error;
                const InternedString next(args.string("skeleton").value_or(""));
                const InternedString before = world->getConstComponent<SkinComponent>(entity).skeleton;
                if (before == next)
                    return std::string{};
                const u64 generation = assets.manifest().generation();
                std::vector<InternedString> clips;
                if (std::string error = compatibleSkeleton(*world, assets, entity, next, clips); !error.empty())
                    return error;
                {
                    const auto animation = world->services().write<AnimationWorld>();
                    if (const auto ready = animation->prepareSkeleton(next, clips); !ready)
                        return ready.error().message;
                    if (assets.manifest().generation() != generation)
                    {
                        animation->cancelPreparedSkeleton(next);
                        return std::string("assets changed while preparing the skeleton; try again");
                    }
                    if (const auto committed = animation->commitPreparedSkeleton(entity, before, next); !committed)
                    {
                        animation->cancelPreparedSkeleton(next);
                        return committed.error().message;
                    }
                }
                control.recordInverse("setSkinSkeleton",
                                      Json{{"id", entity.id()}, {"skeleton", std::string(before.toView())}}.dump());
                out.writeU64("id", entity.id());
                out.writeString("skeleton", next.toView());
                return std::string{};
            });
        control.registerQuery(
            "clips", OWNER, "the baked animation clips: name, skeleton, model, duration (seconds), tracks",
            {optionalArg("skeleton", ArgKind::String, "only the clips baked for this skeleton", "every skeleton")},
            [&assets](const ControlArgs& args, IFormatWriter& out) { return clipsQuery(assets, args, out); });
        control.registerQuery("skeletons", OWNER, "the baked skeletons: name, model, joints, the clips that fit each",
                              {}, [&assets](const ControlArgs&, IFormatWriter& out)
                              { return skeletonsQuery(assets, out); });
        control.registerQuery(
            "animators", OWNER,
            "the skinned entities of a world with their player and bind state (clip, binding, time, speed, weight, "
            "playing, loop, joints, poseVersion), updateInterval (frames between its updates, by distance to the "
            "viewer: setAnimationLod) and sharedGroup / sharedPhase (the shared pose it shows, 0 for its own); the "
            "AnimationWorld_animators snapshot table has the same rows",
            {WORLD_ARG, optionalArg("limit", ArgKind::Integer, "rows, 0..10000; 0 counts only", "100")},
            [&control](const ControlArgs& args, IFormatWriter& out) { return animatorsQuery(control, args, out); });
        control.registerQuery(
            "animator", OWNER,
            "one skinned entity's animator row (updateInterval, sharedGroup and sharedPhase included), its "
            "controller's parameters and layers, rootMotionMode (off|transform|delta) and rootMotionApplied "
            "[x,y,z], and the clips that fit its skeleton",
            {WORLD_ARG, ID_ARG}, [&control, &assets](const ControlArgs& args, IFormatWriter& out)
            { return animatorQuery(control, assets, args, out); });
        control.registerQuery(
            "joints", OWNER,
            "an animator's joints in skeleton order: name, parent (index and name), local position, rotation "
            "[x,y,z,w] and scale, model (16 floats, column-major, in the skin root's space) and palette (12 floats: "
            "model x inverse bind, three rows of four) in the current pose; posed false means no pose yet, the "
            "local transform is the rest pose; the AnimationWorld_joints snapshot table has the same rows",
            {WORLD_ARG, ID_ARG, optionalArg("joint", ArgKind::String, "only the joint of this name", "every joint"),
             optionalArg("index", ArgKind::Integer, "only the joint of this index", "every joint")},
            [&control](const ControlArgs& args, IFormatWriter& out) { return jointsQuery(control, args, out); });
        control.registerQuery(
            "attachments", OWNER,
            "the bone attachments of a world (BoneAttachmentComponent): the attached entity, its target, joint name, "
            "offset (position, rotation [x,y,z,w], scale), binding (bound, unbound while the target's skin is not "
            "bound, rejected for a joint the skeleton lacks or an entity that is not a child of its target), the "
            "reason "
            "and the joint index it resolved to; total counts every row before the limit; the "
            "AnimationWorld_attachments snapshot table has the same rows",
            {WORLD_ARG,
             optionalArg("target", ArgKind::Entity, "only the attachments that follow this skinned entity",
                         "every target"),
             optionalArg("limit", ArgKind::Integer, "rows, 0..10000; 0 counts only", "100")},
            [&control](const ControlArgs& args, IFormatWriter& out) { return attachmentsQuery(control, args, out); });
        control.registerQuery(
            "animationStats", OWNER,
            "what the animation systems did in the world's last frame: anim.active, anim.sampled, anim.posed (zero "
            "while paused or when no animator plays), skin.instances and skin.vertices (the skinned renderables that "
            "hold a joint palette on the GPU, and their vertices; a renderable gets its palette once its skeleton has "
            "a "
            "pose), skin.cacheBytes (always 0: skinning is palette skinning in the shaders, there is no skinned-vertex "
            "cache), animators, skeletons; anim.deferred (playing animators the update rate skipped), "
            "anim.sharedSampled (shared poses sampled), anim.sharedGroups (shared poses alive) and lod (the "
            "setAnimationLod settings, viewer: whether the world has a camera to measure distances from)",
            {WORLD_ARG},
            [&control](const ControlArgs& args, IFormatWriter& out) { return statsQuery(control, args, out); });

        control.registerUndoableCommand(
            "playAnimation", OWNER,
            "starts or resumes an animator; with a clip it plays that one (an entity with no player gets one), with "
            "fade seconds crossfading from the current clip, restarting at time (default 0); without a clip it "
            "resumes the current one where it is",
            {ID_ARG, CLIP_ARG,
             optionalArg("fade", ArgKind::Number, "crossfade seconds from the clip that plays, 0 or more",
                         "a hard cut"),
             optionalArg("time", ArgKind::Number, "the playhead in seconds to start at",
                         "0 for a new clip, the playhead for the current one")},
            [&control, &assets](const ControlArgs& args, IFormatWriter& out)
            { return playAnimation(control, assets, args, out); });
        control.registerUndoableCommand(
            "removeAnimationPlayer", OWNER,
            "removes an entity's AnimationPlayerComponent when it has one, and does nothing when it has none: the "
            "inverse that adding an AnimationControllerComponent to an entity without a player records (AnimationBind "
            "gives the controller its player; one that has a player gets its clip, time, speed, loop and playing back "
            "by sets), so undoing the add leaves the scene as it was; replies id and removed",
            {requiredArg("id", ArgKind::Entity, "the entity whose player goes")},
            [&control](const ControlArgs& args, IFormatWriter& out)
            { return removeAnimationPlayer(control, args, out); });
        control.setComponentAddedHook(
            [&control](const World& world, const Entity entity, const ComponentID component)
            {
                if (component != getComponentId<AnimationControllerComponent>())
                {
                    return;
                }
                if (const auto* player = world.tryGetConstComponent<AnimationPlayerComponent>(entity))
                {
                    // the bind makes the controller's initial state the player's clip, time, speed and loop: the undo
                    // puts the author's values back, ahead of the removal of the controller
                    recordSet(control, entity, "clip", std::string(viewOf(player->clip)));
                    recordSet(control, entity, "time", player->time);
                    recordSet(control, entity, "speed", player->speed);
                    recordSet(control, entity, "loop", player->loop);
                    recordSet(control, entity, "playing", player->playing);
                }
                else
                {
                    control.recordInverse("removeAnimationPlayer", Json{{"id", entity.id()}}.dump());
                }
            });
        control.registerUndoableCommand("pauseAnimation", OWNER, "stops an animator's playhead where it is", {ID_ARG},
                                        [&control](const ControlArgs& args, IFormatWriter& out)
                                        { return pauseAnimation(control, args, out); });
        control.registerUndoableCommand("stopAnimation", OWNER,
                                        "stops an animator and rewinds it to the clip's start pose, shown at once",
                                        {ID_ARG}, [&control](const ControlArgs& args, IFormatWriter& out)
                                        { return stopAnimation(control, args, out); });
        control.registerUndoableCommand(
            "setAnimationSpeed", OWNER, "sets an animator's speed multiplier (negative plays backwards)",
            {ID_ARG, requiredArg("speed", ArgKind::Number, "the playhead's seconds per second"),
             optionalArg("merge", ArgKind::String,
                         "a drag's key: calls on the same entity with the key of the newest undo step fold into it, "
                         "one step for a whole drag",
                         "none: each change is a step")},
            [&control](const ControlArgs& args, IFormatWriter& out) { return setAnimationSpeed(control, args, out); });
        control.registerUndoableCommand(
            "seekAnimation", OWNER,
            "puts an animator's pose at a time of its clip at once, also in a paused world (a loop wraps the time, a "
            "once-clip clamps it); refused while the clip is not bound",
            {ID_ARG, optionalArg("time", ArgKind::Number, "seconds into the clip; needs this or normalized"),
             optionalArg("normalized", ArgKind::Number, "0..1 of the clip's duration; needs this or time"),
             optionalArg("merge", ArgKind::String,
                         "a scrub's key: seeks of the same entity with the key of the newest undo step fold into it, "
                         "one step for a whole scrub",
                         "none: each seek that moves the playhead is a step")},
            [&control](const ControlArgs& args, IFormatWriter& out) { return seekAnimation(control, args, out); });
        control.registerUndoableCommand(
            "setAnimationParameter", OWNER,
            "sets a parameter of an animator's controller (a number for a float, true or false for a bool, true sets a "
            "trigger and false clears it): runtime state of the animation world, not saved; the controller's "
            "transitions act on it while the world advances; animator lists the parameters and the controller's state",
            {ID_ARG, requiredArg("name", ArgKind::String, "the parameter's name (animator lists them)"),
             requiredArg("value", ArgKind::Json, "a number (float parameter) or true / false (bool, trigger)"),
             optionalArg("merge", ArgKind::String,
                         "a drag's key: calls on the same entity with the key of the newest undo step fold into it, "
                         "one step for a whole drag",
                         "none: each change is a step")},
            [&control](const ControlArgs& args, IFormatWriter& out)
            { return setAnimationParameter(control, args, out); });
        control.registerUndoableCommand(
            "attachToBone", OWNER,
            "makes an entity follow a joint of a skinned entity: it adds a BoneAttachmentComponent (or edits the one "
            "the entity has; what the call leaves out stays as it was, identity for a new one) and AnimationWorld "
            "places the entity at the joint's model matrix times the offset in the target's space, whenever the pose "
            "changes and once when it binds. The entity must be a direct child of the target (reparent first) and "
            "may carry a kinematic or static rigidbody, never a dynamic one; the joint must exist in the target's "
            "bound skeleton (joints lists them); the call refuses what the bind would refuse, naming the entity and "
            "the joint; it replies id, target, joint, added (true when it created the attachment) and changed (false "
            "when the call changed nothing and recorded no step); attachments reports the binding",
            {requiredArg("id", ArgKind::Entity, "the entity that follows the joint: a child of the target"),
             requiredArg("joint", ArgKind::String, "the joint's name in the target's skeleton (joints lists them)"),
             optionalArg("target", ArgKind::Entity, "the skinned entity (SkinComponent) whose joint it follows",
                         "the entity's parent"),
             optionalArg("position", ArgKind::Json, "[x, y, z] in the joint's frame", "the attachment's, else 0 0 0"),
             optionalArg("rotation", ArgKind::Json, "quaternion [x, y, z, w] in the joint's frame, normalised",
                         "the attachment's, else identity"),
             optionalArg("scale", ArgKind::Json, "[x, y, z] in the joint's frame", "the attachment's, else 1 1 1"),
             optionalArg("merge", ArgKind::String,
                         "a drag's key: calls on the same entity with the key of the newest undo step fold into it, "
                         "one step for a whole drag",
                         "none: each change is a step")},
            [&control](const ControlArgs& args, IFormatWriter& out) { return attachToBone(control, args, out); });
        control.registerViewCommand(
            "setAnimationLod", OWNER,
            "sets how often a world's playing animators update by their distance to the camera of the view that draws "
            "the world: nearer than bands[0] metres every frame, bands[1] every 2, bands[2] every 4, beyond (or "
            "outside the view by more than radius) every 8; a skipped frame's time is owed, so a playhead lands where "
            "it would have. Animators updating every shareFrom frames or slower that play one clip at full weight "
            "show one shared pose per clip and phase (sharePhases per clip, sampled once, one palette block). Off by "
            "default; runtime state, not saved and not undoable; acts only while the world advances; animators and "
            "animationStats report it; replies lod and previous",
            {WORLD_ARG, optionalArg("enabled", ArgKind::Boolean, "the update rate by distance", "as it is"),
             optionalArg("bands", ArgKind::Json, "three ascending distances in metres", "as they are (15, 30, 60)"),
             optionalArg("radius", ArgKind::Number, "metres around an animator the view test keeps visible",
                         "as it is (2)"),
             optionalArg("shareFrom", ArgKind::Integer, "the interval (1, 2, 4, 8) from which poses are shared, 0 none",
                         "as it is (4)"),
             optionalArg("sharePhases", ArgKind::Integer, "shared poses per clip, 0..1024, 0 none", "as it is (32)")},
            [&control](const ControlArgs& args, IFormatWriter& out) { return setAnimationLod(control, args, out); });
        control.registerViewCommand(
            "previewAnimation", OWNER,
            "turns the edit world's animators' playheads on or off: the edit world is scrub-only (seekAnimation and "
            "the transport commands pose it, but time stays where it was put, so a saved scene holds no drift) until "
            "preview is on; turning it off puts every controller back in its initial state (initial state's clip at "
            "time 0, parameters at their defaults, no transition) and takes back what root motion moved the entities "
            "by, so a save after preview holds no drift (a save during it holds their positions without the root "
            "motion); the play "
            "world always advances (refused there); editor state, not saved and not undoable; replies advancing (now) "
            "and previous (before this call)",
            {WORLD_ARG,
             optionalArg("on", ArgKind::Boolean, "true plays the playing animators, false freezes them",
                         "the opposite of now")},
            [&control](const ControlArgs& args, IFormatWriter& out) { return previewAnimation(control, args, out); });
    }
} // namespace spite
