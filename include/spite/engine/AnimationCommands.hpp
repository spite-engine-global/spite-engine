#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "base/Expected.hpp"
#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "ecs/EcsTypes.hpp"
#include "engine/EngineToolsExport.hpp"

namespace spite
{
    class AssetRuntime;
    class ControlServer;
    class World;

    // Registers the animation queries and commands (owner "engine") on control. An animator is the root of a rigged
    // model: the entity with a SkinComponent (its AnimationPlayerComponent, when it has one, is the transport state).
    // Every entity arg is `id`; the world is the request's (the edit world by default, "play" while playing). Numbers
    // are finite floats. A refusal names what was wrong: no live entity, no SkinComponent, no AnimationPlayerComponent,
    // no such clip, a clip baked for another skeleton, a clip not bound yet.
    //   clips [skeleton]                  -> clips: [{name, skeleton, model, duration (seconds), tracks}] by name
    //   skeletons                         -> skeletons: [{name, model, joints, clips: [names]}] by name
    //   animators [limit]                 -> animators: [animator row], total; limit 0..10000, default 100, 0 counts
    //   animator id                       -> the animator row (below) and clips: the clips that fit its skeleton
    //       row: entity, name, skeleton, joints (0: the skin is not bound), posed (a pose exists), poseVersion,
    //       player (has an AnimationPlayerComponent), clip, binding (none|unbound|bound|rejected), duration, time,
    //       normalizedTime, speed, weight, blend (crossfade progress), playing, loop, rootMotion; and, for an entity
    //       with an AnimationControllerComponent, controller (the asset name), controllerBinding (none|unbound|bound|
    //       rejected), state (the state in force: the source while a transition runs), nextState (the state a
    //       running transition leads to, empty when none; blend is its progress, normalizedTime the clip's),
    //       controllerReason (why a controller is rejected: none|loadFailed (the asset or a clip did not load, the log
    //       names it)|notEnabled (the asset has layers and the skin's model was not imported with usesLayers, see
    //       importSettings)|unknownJoint (a layer's mask names a joint the skeleton lacks))
    //       the animator query adds parameters: [{name, type (float|bool|trigger), value}] of a bound controller and
    //       layers: [{name, mode (override|additive), enabled (false: an additive layer past the limit of 2, which
    //       the baked bounds hold), weight (the clamped one the pose uses: 0..1, an additive one at most 1; a layer
    //       with no weight is paused), state, nextState, normalizedTime, blend}] in the asset's order. A layer's
    //       weight is a constant or a float parameter, so setAnimationParameter drives it; rootMotionMode (off|
    //       transform|delta: where the player's rootMotion goes, the entity's position or a RootMotionDeltaComponent
    //       that a character controller takes) and rootMotionApplied [x,y,z] (what root motion added to the entity's
    //       position, in its parent's space; the editor's preview taking back what it moved)
    //   joints id [joint|index]           -> id, skeleton, joints: [joint row] in skeleton order (a parent before its
    //       children), or the one named; names are unique per skeleton (an unnamed source node is nodes[i])
    //       row: entity, joint (index), name, parent (index, -1 for a root), parentName (empty for a root), posed,
    //       position [x,y,z], rotation [x,y,z,w] (a quaternion), scale [x,y,z]: the LOCAL transform to the parent, the
    //       clip's root travel held (the rest pose while posed is false); model: 16 floats, a column-major matrix in
    //       the skin root's space; palette: 12 floats, three rows of four (the translation last) of model x inverse
    //       bind, the matrix the skinning shaders read (zeros while posed is false)
    //   animationStats                    -> anim.active, anim.sampled, anim.posed, anim.controllers (what the
    //       animation systems did in the last frame of the world: zero while it is paused or no animator plays;
    //       controllers is the number of state machines whose transitions were looked at), skin.instances,
    //       skin.vertices (the skinned renderables holding a joint palette on the GPU, and their vertices),
    //       skin.cacheBytes (always 0: palette skinning in the shaders, no skinned-vertex cache), animators, skeletons,
    //       advancing (whether the world's animators' playheads move: false for the edit world until previewAnimation
    //       turns it on)
    //   playAnimation id [clip] [fade] [time] -> id, clip, playing, faded, previous {clip, time, playing}
    //       starts or resumes the entity's player. With a clip the player names it (an entity with no player gets one;
    //       the clip must be baked for its skeleton) and restarts at `time` (default 0); a fade over seconds crossfades
    //       from the clip it plays (a hard cut when that one is not bound). Without a clip it resumes the current one
    //       at its playhead (at `time` when given; a once-clip that ran to its end starts over).
    //       inverse: set of the changed fields, or removeComponent for a player it added
    //   pauseAnimation id                 -> id, playing, previous; inverse: set playing
    //   stopAnimation id                  -> id, playing, time, previous {playing, time}: stops and rewinds to the
    //   clip's
    //       start pose (shown at once, also in a paused world); inverse: set playing and seekAnimation back
    //   setAnimationSpeed id speed        -> id, speed, previous; inverse: set speed (negative plays backwards); merge
    //       "key" folds a run of calls on one entity into the newest step as seekAnimation's does
    //   seekAnimation id time|normalized  -> id, clip, time (where the playhead is: a loop wraps, a once-clip clamps),
    //       normalizedTime, duration, previous: puts the pose at `time` seconds or `normalized` 0..1 of the clip at
    //       once, also in a paused world (AnimationWorld::evaluateAt); refused while the clip is not bound (a new
    //       player or clip binds at the next frame); inverse: seekAnimation to the previous time. merge "key": a
    //       seek of the same entity with the newest undo step's key folds into it (the first seek's inverse stays), so
    //       a scrubber makes one step
    //   setAnimationParameter id name value -> id, name, type, value, previous: sets a parameter of the entity's bound
    //       controller (an AnimationControllerComponent): a number for a float, true or false for a bool, true for a
    //       trigger (false clears it). Runtime state of the animation world, not saved: the controller's transitions
    //       act on it while the world advances (the play world, or the edit world after previewAnimation). Refused for
    //       an entity with no bound controller, an unknown name (the reply lists the names), or a value of another
    //       kind. inverse: setAnimationParameter with the previous value; merge "key" folds a drag into one step as
    //       setAnimationSpeed's does
    //   attachments [target] [limit]      -> attachments: [attachment row], total: the bone attachments
    //       (BoneAttachmentComponent) of the world by entity id, only those that follow `target` when given; limit
    //       0..10000, default 100, 0 counts only
    //       row: entity (the attached child), name, target, targetName, joint (the name), position [x,y,z], rotation
    //       [x,y,z,w], scale [x,y,z] (the offset in the joint's frame), binding (unbound: the target's skin is not
    //       bound yet | bound | rejected), reason (none | noTarget | noSkin | unknownJoint | notChild), jointIndex (the
    //       joint the name resolved to, -1 while not bound)
    //   attachToBone id joint [target] [position] [rotation] [scale] -> id, target, joint, added, changed: makes the
    //       entity follow a joint of a skinned entity. It adds a BoneAttachmentComponent, or edits the one the entity
    //       has (what the call leaves out stays, identity for a new one); the animation systems then write the entity's
    //       local TransformComponent as joint model matrix x offset, whenever the pose changes and once when the
    //       attachment binds, so the entity's world transform is the target's x the joint's x the offset. The entity
    //       must be a direct child of target (default: its parent; reparent first otherwise); its transform is in the
    //       target's space. Refused, naming the entity and the joint: an entity that is not live or has no transform,
    //       a target with no SkinComponent or no bound skeleton, a joint the skeleton lacks (the reply lists the
    //       joints), an entity that is not a child of the target, a dynamic rigidbody on the entity (a dynamic body is
    //       a world-space root; kinematic and static bodies follow the bone), a position or scale that is not three
    //       finite numbers, a rotation that is not a non-zero quaternion. inverse: removeComponent for an attachment it
    //       added, else a set of each changed field; a call that changes nothing records no step. merge "key": calls
    //       on the same entity with the newest undo step's key fold into it (the first call's inverse stays), so an
    //       offset drag makes one step
    //   removeAnimationPlayer id          -> id, removed: removes the entity's AnimationPlayerComponent, and does
    //   nothing
    //       when it has none. It is the inverse an addComponent of an AnimationControllerComponent records for an
    //       entity that had no player (AnimationBind gives the controller one), so undoing the add leaves the entity as
    //       it was; inverse: addComponent with the removed player
    //   previewAnimation [on] [world]     -> advancing, previous: the edit world is scrub-only (its animators'
    //   playheads
    //       stay where seekAnimation and the transport commands put them, so a saved scene never holds a drift) until
    //       this turns the advance on; turning it off puts every controller back in its initial state (the initial
    //       state's clip at time 0, parameters at their defaults, no transition); a view command (not undoable, not
    //       saved); refused for the play world, which always advances
    // IK nodes need no command of their own: an entity with an AnimationIkComponent (animation/AnimationComponents.hpp:
    // skin, kind twoBone | aim, the joint names, target or targetPosition, poleVector, weight, followGround) is made,
    // edited and removed with the generic addComponent, set and removeComponent, which are undoable; find with
    // AnimationIkComponent lists them and AnimationIkStateComponent (binding, reason, reached) says how each one
    // stands (Rejected, NotEnabled: the skin's model was not imported with the usesIk setting, see importSettings). A
    // node acts in simulated frames (a paused world applies an edit when it steps). The five transport commands change
    // AnimationPlayerComponent fields a scene file holds, so they are ordinary undoable scene edits (not view
    // commands); one that changes nothing records no step. setAnimationParameter is undoable too, though its value is
    // never saved.
    SPITE_ENGINE_TOOLS_API void registerAnimationCommands(ControlServer& control, AssetRuntime& assets);

    // Adds the `animators`, `joints` and `attachments` tables to the AnimationWorld service of world (which must be
    // registered there): `snapshot` writes them as AnimationWorld_animators (one row per animator, the animator row
    // above), AnimationWorld_joints (one row per joint of every bound animator, the joint row above) and
    // AnimationWorld_attachments (one row per bone attachment, the attachment row above), so SQL joins them with the
    // component tables on entity. Rows are built when a snapshot asks, never per frame. Dropped with the service.
    SPITE_ENGINE_TOOLS_API void registerAnimationTables(const World& world);

    // One bone of an animator's current pose, in world space: the segment from a joint's parent to the joint (a root
    // joint's starts at the animator's own origin).
    struct AnimatorBone
    {
        Vec3 from;
        Vec3 to;
        u32 joint = 0;
    };
    // The AnimationAssetLoader::loadController of a host: reads the controller file a name gives (a path under root) as
    // text. The name is whitelisted: folders and file of letters, digits, _ - and. joined by "/", no leading
    // or trailing dot, no Windows device name; any other name is refused with an error that names it, a missing file
    // with an error that names the file.
    [[nodiscard]] SPITE_ENGINE_TOOLS_API std::function<Expected<std::string>(InternedString)> animationControllerReader(
        std::filesystem::path root);
    // The AnimationAssetLoader::controllerVersion of a host: a counter that grows when something under root changed
    // (a directory change notification, recursive; no disk read, cheap to call every frame; frame thread only).
    // A root that is no directory yet is looked for again now and then (the counter moves when it is found).
    [[nodiscard]] SPITE_ENGINE_TOOLS_API std::function<u64()> animationControllerVersion(
        const std::filesystem::path& root);
    // Whether the world's animators' playheads move (animationStats advancing); true for a world with no animation.
    [[nodiscard]] SPITE_ENGINE_TOOLS_API bool animatorsAdvance(const World& world);
    // Appends the bones of the animator (a SkinComponent entity of world) in skeleton order; none while it has no pose.
    SPITE_ENGINE_TOOLS_API void animatorBones(const World& world, Entity animator, std::vector<AnimatorBone>& out);
} // namespace spite
