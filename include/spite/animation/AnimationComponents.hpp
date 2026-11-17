#pragma once

#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    // The rig of an entity: names the skeleton asset (a rigged model's name) that the entity's joints, inverse binds
    // and clips belong to. Immutable like a mesh binding: a change is remove + add, and AnimationWorld binds the
    // skeleton when the component lands. A model with a skinned mesh instantiates one root with this component and one
    // renderable entity per skinned submesh below it; the joints are data in AnimationWorld, never entities.
    struct SkinComponent : IEngineComponent
    {
        InternedString skeleton;
        SPITE_SCHEMA(SkinComponent, SPITE_FIELD(skeleton, READONLY | fieldAssetType("SKELETON")))
    };

    // One clip played on the entity's skin (the same entity holds a SkinComponent). A clip baked for another skeleton
    // is refused: AnimationStateComponent says so and the log names the clip and both skeletons.
    //
    // time is the playhead in seconds: the animation systems advance it by the frame time times speed while playing,
    // wrapping a loop and stopping a once-clip at its end (playing goes false there, time stays on the last pose); an
    // edit of time, speed, weight, playing or loop takes effect on the next simulated frame, and a clip edit is bound
    // by the next AnimationBind, which runs in a paused world too. weight is the player's share against the skeleton's
    // rest pose: 1 plays the clip, 0 holds the rest pose, between them blends the two. The skinned bounds bake covers
    // these blends and the crossfade's (padded for the mix of rotations), so the static culling bounds hold
    // at every weight.
    //
    // The pose never carries the clip's ground-plane travel of a root joint (a joint with no parent): the bake bounds
    // the mesh with it held at the clip's first pose, so a pose that moved with it would leave its bounds and be
    // culled. rootMotion applies it to the entity instead: each simulated frame the entity's TransformComponent
    // position moves by the travel of the clip's first root joint (joint 0) on the ground plane between the playhead
    // before and after the step, in the parent's space (the entity's rotation and scale carry the skin's), once per
    // loop whatever the speed, frame time or direction; a blend (controller blend tree, crossfade) mixes the travel at
    // the weights of its clips, and the player's weight scales it. With a RootMotionDeltaComponent on the entity the
    // displacement goes there instead, for a character controller to turn into a desired velocity. A controller's
    // layers add none, and it works under a controller (which owns the other player fields, not this flag). A
    // scrub or a paused or non-advancing world moves nothing, and the editor's preview takes back what it moved when
    // it ends. Off by default: the clip then plays in place.
    struct AnimationPlayerComponent : IEngineComponent
    {
        InternedString clip;
        f32 time = 0.0f;
        f32 speed = 1.0f;
        f32 weight = 1.0f;
        bool playing = true;
        bool loop = true;
        bool rootMotion = false;
        SPITE_SCHEMA(AnimationPlayerComponent, clip, SPITE_FIELD(time, fieldUnit("s")),
                     SPITE_FIELD(speed, fieldStep(0.05)), SPITE_FIELD(weight, fieldRange(0.0, 1.0) | fieldStep(0.01)),
                     playing, loop, rootMotion)
    };

    // AnimationWorld's answer for the entity's player: added with the player, removed with it.
    struct AnimationStateComponent : IDerivedComponent
    {
        enum class Binding : u8
        {
            Unbound = 0, // no clip named, or the entity has no bound skin yet
            Bound = 1, // the clip is loaded and fits the skeleton
            Rejected = 2 // the clip failed to load or was baked for another skeleton; the log says which
        };
        SPITE_ENUM_NAMES_IN_CLASS(Binding, Unbound, Bound, Rejected)

        Binding binding = Binding::Unbound;
        f32 duration = 0.0f; // seconds, of the bound clip
        f32 normalizedTime = 0.0f; // the pose's time in the clip over its duration, 0 while no clip plays
        f32 blend = 0.0f; // progress of a running crossfade in (0, 1), 0 when none runs
        SPITE_OBSERVED_SCHEMA(AnimationStateComponent, SPITE_FIELD(binding, OBSERVED | READONLY),
                              SPITE_FIELD(duration, OBSERVED | READONLY | fieldUnit("s")),
                              SPITE_FIELD(normalizedTime, OBSERVED | READONLY), SPITE_FIELD(blend, OBSERVED | READONLY))
    };

    // A controller asset drives the entity's player: the file (a path under the animation root,
    // EngineDesc::animationRoot, for example "hero.controller.json") names parameters, states that each play a clip or
    // a blend tree of clips and the transitions between them (AnimationControllerAsset.hpp); a tree state's clip is its
    // first child's. The entity holds a SkinComponent; AnimationBind
    // gives it an AnimationPlayerComponent playing the initial state's clip when it has none, and from then on the
    // controller owns that player's clip, time, speed, loop and playing: a state change is a crossfade of the player
    // (AnimationWorld::crossfade). Immutable like SkinComponent (a change is remove + add). Parameter values are
    // runtime state, not saved: they start at the file's defaults and a game sets them (AnimationController). A world
    // that does not advance (the edit scene) stays in the initial state, so a saved scene holds no drift.
    struct AnimationControllerComponent : IEngineComponent
    {
        InternedString controller;
        SPITE_SCHEMA(AnimationControllerComponent, SPITE_FIELD(controller, READONLY))
    };

    // AnimationWorld's answer for the entity's controller: added with it, removed with it. The progress of the running
    // transition is AnimationStateComponent::blend, the state's normalized time
    // AnimationStateComponent::normalizedTime.
    //
    // The controller asset may declare layers (AnimationControllerAsset.hpp): they play over the base state machine
    // with joint masks and weights, and only on a model imported with the usesLayers setting (culling is static and
    // only such a model has bounds that hold a layered pose). Without it the controller is Rejected, NotEnabled, and
    // never runs; so is one whose layer names a joint the skeleton lacks (UnknownJoint). A clip, state or asset that
    // did not load is Rejected, LoadFailed (the log names it). AnimationWorld::layers reports each layer's state and
    // weight.
    struct AnimationControllerStateComponent : IDerivedComponent
    {
        enum class Reason : u8
        {
            None = 0,
            LoadFailed = 1, // the asset or a clip it names did not load, or the component names none
            NotEnabled = 2, // the asset has layers and the skin's model was not imported with usesLayers
            UnknownJoint = 3 // a layer's mask names a joint the skeleton lacks
        };
        SPITE_ENUM_NAMES_IN_CLASS(Reason, None, LoadFailed, NotEnabled, UnknownJoint)

        AnimationStateComponent::Binding binding = AnimationStateComponent::Binding::Unbound;
        InternedString state; // the state in force (the source while a transition runs); empty while not bound
        InternedString nextState; // the state a running transition leads to; empty when none runs
        Reason reason = Reason::None; // why Rejected
        SPITE_OBSERVED_SCHEMA(AnimationControllerStateComponent, SPITE_FIELD(binding, OBSERVED | READONLY),
                              SPITE_FIELD(state, OBSERVED | READONLY), SPITE_FIELD(nextState, OBSERVED | READONLY),
                              SPITE_FIELD(reason, OBSERVED | READONLY))
    };

    // Makes the entity follow a joint of a skinned entity's pose: the entity must be a direct child of target (a
    // SkinComponent entity) and is placed at the joint's model matrix (the target's skin-root space) times the offset,
    // so its world transform is the target's world transform x joint x offset. The offset is the child's pose in the
    // joint's frame, composed as position, then rotation, then scale (a TRS matrix); the joint's own rotation and scale
    // carry the child along, as they would a child bone.
    //
    // AnimationWorld writes the child's TransformComponent when a pose changes (poseVersion grows) and when the
    // attachment binds; a write that would leave the transform as it is writes nothing, so a still pose and an
    // unchanged attachment cost no component write. The joint is found by name when the attachment binds, and again
    // when the target's skeleton rebinds, never per frame. A TransformComponent has no shear: a joint with a
    // non-uniform scale and an offset that rotates it shear the child, which the written TRS cannot hold (it keeps the
    // nearest). A physics body on the child follows the physics hierarchy rule: kinematic and static bodies may have
    // parents, a dynamic body is a world-space root, and attachToBone refuses it.
    struct BoneAttachmentComponent : IEngineComponent
    {
        Entity target = Entity::undefined();
        InternedString joint;
        Vec3 position = {0.0f, 0.0f, 0.0f};
        Quat rotation = Quat::identity();
        Vec3 scale = {1.0f, 1.0f, 1.0f};
        SPITE_SCHEMA(BoneAttachmentComponent, target, joint, position, rotation, scale)
    };

    // AnimationWorld's answer for an attachment: added with it, removed with it. Unbound waits for something that may
    // still come (the target's skin binds later); Rejected needs an edit (a joint the skeleton lacks).
    struct BoneAttachmentStateComponent : IDerivedComponent
    {
        using Binding = AnimationStateComponent::Binding;

        enum class Reason : u8
        {
            None = 0,
            NoTarget = 1, // target is not set, or the entity it names is gone
            NoSkin = 2, // the target has no SkinComponent, or its skeleton is not bound (yet)
            UnknownJoint = 3, // the target's skeleton has no joint of that name
            NotChild = 4 // the entity is not a direct child of its target
        };
        SPITE_ENUM_NAMES_IN_CLASS(Reason, None, NoTarget, NoSkin, UnknownJoint, NotChild)

        Binding binding = Binding::Unbound;
        Reason reason = Reason::NoTarget;
        i32 jointIndex = -1; // the joint the name resolved to, -1 while not bound
        SPITE_OBSERVED_SCHEMA(BoneAttachmentStateComponent, SPITE_FIELD(binding, OBSERVED | READONLY),
                              SPITE_FIELD(reason, OBSERVED | READONLY), SPITE_FIELD(jointIndex, OBSERVED | READONLY))
    };

    // One inverse-kinematics node on a skinned entity's pose, an entity of its own (any entity: one node per entity, a
    // character with two legs and a head has three). It runs after the clips are sampled and blended and before the
    // palette and the bone attachments are built, so a skin, a shadow and an attachment on the joint all see it.
    // Nodes of one skin run in entity order: a node whose joints sit below another's chain goes after it.
    //
    // TwoBone (ozz IKTwoBoneJob) rotates start and mid so that end reaches the target; the three joints are ancestors
    // in order (the joints between them stay as they are). A target out of reach stretches the chain straight at it
    // (reached says so), a soften below 1 starts easing the last part of the reach early. midAxis is the bend axis in
    // mid's local space (a positive turn around it opens the angle between the bones, which decides the side a knee
    // bends to), poleVector the world direction the mid joint points to and twistAngle turns the chain around the
    // start-to-target line. Aim (IKAimJob) rotates the joint named by `end` so that `forward` (its local axis) points
    // at the target and `up` (its local axis) points along poleVector; start and mid are unused.
    //
    // The target is a world position: targetPosition itself, or in the space of the target entity when one is set (a
    // hand following a prop). The pole vector is a world direction too. With followGround (TwoBone) the target is not
    // authored: it is where the animated end joint is, lifted by the ground height there above the skin's own height
    // (the character stands on the ground at its origin) from the world's ground query
    // (AnimationController::setGroundQuery), so a foot keeps the height the clip gives it above the ground it meets;
    // no ground there leaves the pose as it is.
    //
    // A target entity that is gone reads as the identity matrix (targetPosition is then a world position), and the walk
    // up the transform hierarchy for the skin's and target's world matrices stops after 64 levels. A value that is not
    // finite or beyond 1e9, or a target the skin's space or the ground query makes that large, is refused (ozz asserts
    // on what it cannot normalize): AnimationIkStateComponent says Rejected, InvalidValue, and the pose is left as it
    // is until the input is sane again.
    //
    // A model must be imported with the usesIk setting (ModelImportSettings): culling is static and only such a model
    // has bounds that hold an IK pose, so a node on a skin of any other model is Rejected, NotEnabled, and never runs.
    //
    // weight blends from the pose (0) to the IK result (1). A node costs nothing while its target, the skin's
    // transform and the pose it reads stay as they were: the pose is rebuilt, and poseVersion grows, only when the
    // result differs from the one before. Edits take effect on the next simulated frame (a paused world runs no pose
    // system).
    struct AnimationIkComponent : IEngineComponent
    {
        enum class Kind : u8
        {
            TwoBone = 0,
            Aim = 1
        };
        SPITE_ENUM_NAMES_IN_CLASS(Kind, TwoBone, Aim)

        Entity skin = Entity::undefined(); // the entity with the SkinComponent whose pose this node bends
        Kind kind = Kind::TwoBone;
        InternedString start; // TwoBone: the first joint of the chain (the thigh)
        InternedString mid; // TwoBone: the joint between (the knee)
        InternedString end; // TwoBone: the joint that reaches (the ankle); Aim: the joint that aims
        Entity target = Entity::undefined(); // optional: targetPosition is in this entity's space
        Vec3 targetPosition = {0.0f, 0.0f, 0.0f};
        bool followGround = false;
        Vec3 poleVector = {0.0f, 1.0f, 0.0f};
        Vec3 midAxis = {0.0f, 0.0f, 1.0f}; // TwoBone
        Vec3 forward = {1.0f, 0.0f, 0.0f}; // Aim
        Vec3 up = {0.0f, 1.0f, 0.0f}; // Aim
        f32 twistAngle = 0.0f;
        f32 soften = 1.0f; // TwoBone
        f32 weight = 1.0f;
        SPITE_SCHEMA(AnimationIkComponent, skin, kind, start, mid, end, target, targetPosition, followGround,
                     poleVector, midAxis, forward, up, SPITE_FIELD(twistAngle, fieldUnit("rad")),
                     SPITE_FIELD(soften, fieldRange(0.0, 1.0) | fieldStep(0.05)),
                     SPITE_FIELD(weight, fieldRange(0.0, 1.0) | fieldStep(0.05)))
    };

    // AnimationWorld's answer for an IK node: added with it, removed with it. Unbound waits for what may still come
    // (the skin binds later); Rejected needs an edit (a joint the skeleton lacks, a value that is not finite).
    struct AnimationIkStateComponent : IDerivedComponent
    {
        using Binding = AnimationStateComponent::Binding;

        enum class Reason : u8
        {
            None = 0,
            NoSkin = 1, // skin is not set, or the entity has no bound skeleton (yet)
            UnknownJoint = 2, // the skeleton has no joint of a named name
            InvalidValue = 3, // an authored value is not finite
            NotEnabled = 4 // the skin's model was not imported with usesIk: its baked bounds hold no IK pose
        };
        SPITE_ENUM_NAMES_IN_CLASS(Reason, None, NoSkin, UnknownJoint, InvalidValue, NotEnabled)

        Binding binding = Binding::Unbound;
        Reason reason = Reason::NoSkin;
        bool reached =
            false; // the last run put the joint on its target (aim: the target was aimable); false at weight 0
        SPITE_OBSERVED_SCHEMA(AnimationIkStateComponent, SPITE_FIELD(binding, OBSERVED | READONLY),
                              SPITE_FIELD(reason, OBSERVED | READONLY), SPITE_FIELD(reached, OBSERVED | READONLY))
    };

    constexpr u32 INVALID_PALETTE_SLOT = ~0u;

    // The skin's derived state, added with the bound skeleton and read by the render runtime: jointCount is 0 when the
    // skeleton could not be bound (the log says why). paletteSlot is the render runtime's to assign; poseVersion grows
    // by one on each frame whose pose differs from the frame before (a paused, stopped or faded-out animator leaves it
    // alone).
    struct SkinPaletteComponent : IDerivedComponent
    {
        u32 paletteSlot = INVALID_PALETTE_SLOT;
        u32 jointCount = 0;
        u32 poseVersion = 0;
        SPITE_OBSERVED_SCHEMA(SkinPaletteComponent, SPITE_FIELD(paletteSlot, OBSERVED | READONLY),
                              SPITE_FIELD(jointCount, OBSERVED | READONLY),
                              SPITE_FIELD(poseVersion, OBSERVED | READONLY))
    };
} // namespace spite
