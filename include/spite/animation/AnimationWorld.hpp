#pragma once

#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "animation/AnimationComponents.hpp"
#include "animation/AnimationController.hpp"
#include "animation/AnimationExport.hpp"
#include "animation/AnimationLoader.hpp"
#include "base/MathTypes.hpp"
#include "ecs/EcsTypes.hpp"
#include "ecs/SystemContext.hpp"

namespace spite
{
    class World;

    // Registers the animation components (AnimationComponents.hpp) process-wide, so a world without an AnimationWorld
    // can author them.
    SPITE_ANIMATION_API void registerAnimationComponents();

    inline constexpr const char* ANIMATION_BIND_SYSTEM_NAME = "AnimationBind";
    inline constexpr const char* ANIMATION_ADVANCE_SYSTEM_NAME = "AnimationAdvance";
    inline constexpr const char* ANIMATION_SAMPLE_SYSTEM_NAME = "AnimationSample";
    inline constexpr const char* ANIMATION_POSE_SYSTEM_NAME = "AnimationPose";

    // One joint's skin matrix: the top three rows of (joint model matrix x inverse bind) as three rows of four floats,
    // the fourth float of a row being the translation. 48 bytes, the layout the skinning shaders read.
    struct PaletteJoint
    {
        f32 row[3][4];
    };
    static_assert(sizeof(PaletteJoint) == 48, "the palette is uploaded as raw bytes");

    // What the animation systems did in the last frame of the world. A frame that ran no simulation (a paused world
    // that was not stepped) and a world with no playing animator both read zeros.
    struct AnimationStats
    {
        u32 active = 0; // animators the advance system visited
        u32 sampled = 0; // poses recomputed (clip sampling and blending)
        u32 posed = 0; // poses whose joints and palette were rebuilt: the poseVersion bumps
        u32 controllers = 0; // controllers whose transitions were evaluated
        u32 deferred = 0; // playing animators the update rate skipped this frame (AnimationLodSettings)
        u32 sharedSampled = 0; // shared poses sampled and built this frame
        u32 sharedGroups = 0; // shared poses alive, each serving one clip at one phase
    };

    // The slowest update rate: an animator beyond the last band or outside the viewer's frustum updates every this
    // many frames.
    inline constexpr u32 ANIMATION_LOD_SLOWEST_INTERVAL = 8;

    // How often a playing animator updates by its distance to the world's viewer (AnimationWorld::setViewer), and
    // which animators share one pose. Off by default: every animator updates every frame. A skipped frame's time is
    // owed to the animator, so its playhead after an update is where updating every frame would have put it. The rate
    // is decided when the animator updates (one transform read), so a skipped animator costs a counter test.
    struct AnimationLodSettings
    {
        bool enabled = false;
        // An animator nearer the viewer than bands[i] metres updates every 2^i frames (1, 2, 4); beyond the last band,
        // or outside the viewer's frustum, every ANIMATION_LOD_SLOWEST_INTERVAL frames. Ascending.
        f32 bands[3] = {15.0f, 30.0f, 60.0f};
        // metres around the skin root's position that the frustum test keeps visible
        f32 radius = 2.0f;
        // An animator updating every shareFrom frames or slower, playing one clip at full weight (no fade, no blend,
        // no IK node), shows the clip at the middle of one of sharePhases equal phases of its duration: one shared
        // pose per clip and phase is sampled once, and every animator at that phase shows it. 0 in either shares
        // none.
        u32 shareFrom = 4;
        u32 sharePhases = 32;
        bool operator==(const AnimationLodSettings&) const = default;
    };

    // Where the world is seen from, in world space: the engine sets it each frame from the camera of the view that
    // draws the world. planes (with hasFrustum) are the view's frustum, inward normals: a point p is inside a plane
    // when dot(xyz, p) + w >= 0.
    struct AnimationViewer
    {
        Vec3 position{0.0f};
        bool hasFrustum = false;
        Vec4 planes[6] = {};
    };

    // An animator's update rate and the shared pose it shows. sharedGroup 0: its own pose.
    struct AnimatorLod
    {
        u32 interval = 1; // frames between updates
        u32 sharedGroup = 0; // the shared pose's id, unique while it lives
        u32 sharedPhase = 0; // the phase of the clip the shared pose shows (0..sharePhases-1)
    };

    // One parameter of a bound controller and the value it holds (a bool or trigger: 0 or 1).
    struct AnimationParameterInfo
    {
        InternedString name;
        AnimationParameterKind kind = AnimationParameterKind::Float;
        f32 value = 0.0f;
    };

    // One layer of a bound controller as it plays now (AnimationWorld::layers).
    struct AnimationLayerInfo
    {
        InternedString name;
        bool additive = false;
        bool enabled = true; // false: an additive layer past the limit the baked bounds hold
        f32 weight = 0.0f; // clamped, the one the pose uses
        InternedString state; // the layer's state in force (the source while a transition runs)
        InternedString nextState; // the state a running transition leads to; empty when none runs
        f32 normalizedTime = 0.0f; // of the state's clip
        f32 blend = 0.0f; // progress of a running transition, 0 when none runs
    };

    // One World's animation state: publishes itself as a service of the world and registers the animation systems
    // there. Joints are data held here, keyed by entity, never entities.
    //
    // Binding. The lifecycle events of SkinComponent and AnimationPlayerComponent queue the entity; each BEGIN_FRAME
    // (it runs while the world is paused too) AnimationBind reconciles the queued entities and does nothing when the
    // queue is empty: it loads the skeleton of a SkinComponent through the AnimationAssetLoader service (shared by
    // every entity of that skeleton, dropped with the last), adds SkinPaletteComponent, loads the clip of an
    // AnimationPlayerComponent for that skeleton and adds AnimationStateComponent; a component that went takes its
    // derived one and its data with it. A load that fails is logged once per name and leaves its entities unbound
    // (SkinPaletteComponent::jointCount 0, Binding::Rejected); the loader is not asked for that name again until rebind
    // clears what failed.
    //
    // Assets. AnimationBind asks the loader's changesSince each frame: the names an import or reimport committed drop
    // the cached skeleton and clip of that name and what failed under it, rebind the animators bound to it (a skeleton
    // that changed builds the animator anew, a clip loads again and cuts at the playhead) and retry the skins whose
    // skeleton did not bind. A change tick scan of the players queues an edited clip likewise; both cost a comparison
    // while nothing changed.
    //
    // Pose. Three systems follow the world's time and pause rules, so a paused world runs none of them:
    // AnimationAdvance (UPDATE) steps the playheads of the animators in a dense active list, AnimationSample (UPDATE,
    // after it, parallel over the animators whose evaluation changed) samples the clips with ozz and blends them,
    // AnimationPose (UPDATE, after AnimationSample, before the world's TransformMatrices; parallel) turns a changed
    // local pose into model matrices and the skin palette, adds one to SkinPaletteComponent::poseVersion and places the
    // animator's bone attachments. An animator that is stopped, faded out or paused is not in the list and costs
    // nothing; a pose that comes out bit for bit as before bumps nothing. A skin with no clip shows the skeleton's rest
    // pose.
    //
    // Bone attachments (BoneAttachmentComponent). AnimationBind reads the component of each queued child (a change
    // tick scan finds an edit; lifecycle events find an add or remove; a change of the transform hierarchy checks them
    // all again), looks the joint up by name in the target's skeleton once and stores its index, and publishes the
    // outcome in BoneAttachmentStateComponent: an unknown joint or a child that is not a direct child of the target is
    // logged once, naming the joint or the entities. The target's binding or rebinding resolves its attachments again.
    // AnimationPose then writes the child's local TransformComponent for the animators whose pose changed, and
    // writes nothing when the result equals what the child holds; a destroyed target leaves the child alone. Pose and
    // sample write nothing while the world is paused; AnimationBind, which runs paused too, places an attachment when
    // it binds or is edited (the target already has a pose) and publishes BoneAttachmentStateComponent.
    //
    // IK nodes (AnimationIkComponent). AnimationPose, after the poses of the frame are sampled, reads each node's
    // component (a field comparison), finds the skin's instance and the node's joints by name (again only when the
    // component or the instance changed), and puts the instance in the pose list when a node was added, edited,
    // removed, or its target or the skin's transform (composed from TransformComponent up the hierarchy, so this
    // frame's) differs from the last run's. The pose job then runs the nodes of an instance in entity order over the
    // sampled pose, ozz IKTwoBoneJob and IKAimJob, corrects the local rotations and rebuilds the model matrices below
    // the chain; the palette it ends in is compared with the one before and poseVersion grows (attachments follow) only
    // when it differs. A world without nodes pays one flag test per frame. The skinned bounds bake does not cover a
    // pose with IK: a joint can leave the baked bounds (inbox).
    //
    // Controllers. An entity with an AnimationControllerComponent and a bound skin gets its controller at AnimationBind
    // (the asset text comes from AnimationAssetLoader::loadController, parsed and checked against the skeleton's clips:
    // a failure is logged once per name and AnimationControllerStateComponent says Rejected). When the loader's
    // controllerVersion moves (a file under the animation root was written) the world reads the controllers its
    // entities name again and rebinds the entities of one whose text changed, in a paused world too: a world that
    // advances keeps each entity's state by name, its parameters by name and kind and its playhead (a state that plays
    // another clip starts it over, a running transition counts as done, layers start over), a world that does not
    // advance (the edit scene) shows the new initial state, and a text that does not load or bind leaves the entity on
    // the controller it has (logged once). The controller owns the entity's player (see AnimationControllerComponent)
    // and acts only inside AnimationAdvance, only while the world advances and only for an animator in the active list,
    // so it adds no system of its own: a parameter change wakes the animator once, a state that waits for a normalized
    // time (exitTime) is visited while its clip plays, and a paused or stopped controller costs nothing. A transition
    // (conditions true, source time reached) is taken at most once in a frame and never while another runs: the player
    // names the target's clip, AnimationBind loads it at the next BEGIN_FRAME and crossfades from the old one over the
    // transition's seconds (a cut for 0), and the state changes when the fade ends. A state may play a blend tree
    // instead of one clip: the player names the first child's clip, the pose mixes all the children by weights that
    // follow the tree's float parameters (a change wakes the animator once; the weights are recomputed only when a
    // value changed) and the playhead is a phase every child plays at its own length. The motion that fades out of a
    // tree keeps the weights it had; a transition between two states with the same first clip, one of them a tree,
    // starts its fade in AnimationAdvance itself, there being no clip change for AnimationBind to see. One blend mixes
    // at most MAX_BLEND_CLIP_LAYERS clips (the two motions of a fade, three children each), which the skinned bounds
    // bake covers.
    //
    // Layers. A controller asset's layers (AnimationControllerAsset.hpp) each run a state machine of their own over the
    // controller's parameters, advanced by AnimationAdvance on the transport of the player (a layer runs while the
    // player plays, and on after the base's once-clip ended); a layer's weight (a constant, or a float parameter) and
    // state decide what it adds to the pose: AnimationSample samples the clips of the base and of every layer with a
    // weight and blends them with one ozz BlendingJob, the override layers over what is below them by weight x joint
    // weight (the base and each layer get joint weights so that the shares sum to 1), the additive layers after it as
    // the clip's pose relative to its first frame. A layer with no weight is not in the pose and is paused; layers
    // whose weights and states stand still cost no system work and poseVersion grows only when the pose differs. Layers
    // play only on a model imported with usesLayers (the controller is otherwise Rejected, NotEnabled) and at most
    // MAX_ADDITIVE_LAYERS additive layers count, weights clamped to ADDITIVE_WEIGHT_MAX: what the baked bounds hold.
    //
    // The sampled pose holds the ground-plane travel of every root joint (one with no parent) at the clip's first pose:
    // the bake's bounds assume it, so the travel belongs to the entity, not the pose (see AnimationPlayerComponent).
    //
    // Root motion. An animator whose player has rootMotion set moves its entity by the travel of the first root joint
    // (joint 0) on the ground plane, as AnimationAdvance steps the playhead: each frame covers the travel between the
    // playhead before and after, loops included (a clip's travel over one loop, applied once per loop whatever the
    // frame time, speed or its sign), the clips of a blend (blend tree children, the two motions of a crossfade) at the
    // weights the pose gives them and the player's weight. Only the base motion counts: a controller layer adds
    // none, and a frame that does not step the playhead (paused, a world that does not advance, evaluateAt, the frame
    // after a clip change) moves nothing. The travel is in the skin's space; its rotation and scale carry it into the
    // parent's, where it is added to the entity's TransformComponent position, or, when the entity has a
    // RootMotionDeltaComponent, to that component's delta for a consumer (a character controller) to take as a desired
    // velocity. Vertical travel and the root's rotation stay in the pose. A world that stops advancing (the editor's
    // preview ends) takes the position back by what root motion added, so a saved scene holds no drift.
    //
    // Threads. The systems touch the pose store without the mutex that guards binding: they never overlap
    // AnimationBind, evaluateAt, crossfade or a reader of the pose, which all run outside the update stages on the
    // frame thread or in later stages. A system that reads poses orders itself after AnimationPose. Destroy it before
    // its World.
    class SPITE_ANIMATION_API AnimationWorld
    {
    public:
        explicit AnimationWorld(World& world);
        ~AnimationWorld();
        AnimationWorld(const AnimationWorld&) = delete;
        AnimationWorld& operator=(const AnimationWorld&) = delete;

        // The entity's joints in the skeleton's depth-first order (a parent before its children); empty while its skin
        // is not bound.
        [[nodiscard]] std::span<const JointInfo> joints(Entity entity) const;

        // The joint's model-space matrix in the entity's current pose (the skin root's space; the entity's own
        // transform is not in it). False while the skin has no pose yet or the joint does not exist.
        [[nodiscard]] bool jointModelTransform(Entity entity, u32 joint, Mat4& out) const;
        [[nodiscard]] bool jointModelTransform(Entity entity, InternedString jointName, Mat4& out) const;
        // The joint's local transform (relative to its parent) in the current pose, the clip's root travel held.
        [[nodiscard]] bool jointLocalTransform(Entity entity, u32 joint, Vec3& position, Quat& rotation,
                                               Vec3& scale) const;
        // The skin palette of the current pose, one entry per joint: valid until the next animation system run,
        // evaluateAt or rebind of that entity. Empty while there is no pose.
        [[nodiscard]] std::span<const PaletteJoint> palette(Entity entity) const;

        // Puts the entity's pose at time seconds of its clip right now, with no frame needed: for a scrubber while the
        // world is paused. The time wraps for a looping player and clamps for a once-player; the playhead (time of
        // AnimationPlayerComponent) is set to it, and poseVersion grows when the pose differs. A running crossfade is
        // left out of the evaluated pose (the clip alone, at the player's weight; a weight below 1 blends with the rest
        // pose, covered by the bounds as crossfade is). False when the entity has no bound clip. Call it on the
        // frame thread outside the update stages.
        bool evaluateAt(Entity entity, f32 time);

        // Starts a crossfade from the clip that plays to clip: the player names the new clip from startTime seconds on,
        // the old one keeps advancing at the same speed and loops as it does, and over the seconds that follow the
        // weights move linearly from the old to the new (the sum stays at the player's weight, any rest is the rest
        // pose). The new clip binds at the next BEGIN_FRAME and the fade counts from the simulated frame after that;
        // the poses stay as they are until then. A clip that fails to load leaves the player rejected with no fade.
        // seconds 0 swaps at once. False when the entity has no bound clip, no player, or already plays that clip. Call
        // it on the frame thread outside the update stages.
        // Culling: the skinned bounds bake also covers every blend of two clips' poses and the rest pose (a crossfade,
        // or a weight below 1, which uses the same blend), padded for the nlerp of rotations; it relies on
        // the two clips of a crossfade being two clips, which this refuses to violate.
        bool crossfade(Entity entity, InternedString clip, f32 seconds, f32 startTime = 0.0f);

        // Forgets a crossfade that crossfade asked for and the next AnimationBind has not started: the clip change that
        // follows (the player's clip written by anything else) is a plain one and cuts. A fade that is not pending, or
        // an entity with none bound, is left alone.
        void cancelCrossfade(Entity entity);

        // Controller parameters (AnimationControllerComponent). The AnimationController facade and the control commands
        // call these; both are safe from any thread outside AnimationBind. A value is applied to the controller at once
        // (parameters shows it) and acted on by the next AnimationAdvance; the same value again changes nothing and
        // wakes nothing. kind must be the parameter's own (Trigger: value 1 sets it, 0 clears it).
        AnimationParameterResult setParameter(Entity entity, std::string_view name, AnimationParameterKind kind,
                                              f32 value) const;
        // The entity's controller parameters in the asset's order with their values; empty while it has no bound
        // controller.
        [[nodiscard]] std::vector<AnimationParameterInfo> parameters(Entity entity) const;
        // The entity's controller layers in the asset's order; empty while it has no bound controller or the asset
        // declares none.
        [[nodiscard]] std::vector<AnimationLayerInfo> layers(Entity entity) const;

        // Queues the entity for the next AnimationBind: for a retry after a load failed (it forgets every name that
        // failed, so the loader is asked again). A queued entity whose bindings already match costs a comparison. No
        // call is needed for an edit of AnimationPlayerComponent::clip (the next AnimationBind finds it by change
        // ticks, in a paused world too) nor for an import of the skeleton or clip (see Assets below).
        // A controller that was rejected is tried again too (a fixed file, a clip imported since); one an instance
        // already holds is not re-read: remove and add its AnimationControllerComponent to
        // take an edited asset.
        void rebind(Entity entity);
        // Loads and validates a skeleton before an authoring command changes a skin binding.
        Expected<void> prepareSkeleton(InternedString name, std::span<const InternedString> clips = {});
        Expected<void> commitPreparedSkeleton(Entity entity, InternedString expected, InternedString name);
        void cancelPreparedSkeleton(InternedString name);

        // Whether AnimationAdvance moves the playheads of playing animators (default true). A world that does not
        // advance is scrub-only: its players keep their time (seekAnimation and evaluateAt still pose them, and a
        // pending crossfade completes at once), so a scene authored there stores the time its author set, never a
        // drift. Turning it on wakes every animator once, so the playing ones move from the next simulated frame.
        // The engine switches it off for the edit scene (the editor's preview turns it on); a play world advances.
        void setAdvancing(bool advancing);
        [[nodiscard]] bool advancing() const;

        // The update rate by distance and the shared poses (AnimationLodSettings). They act only while the world
        // advances and has a viewer; a change reaches each animator at its next update (every playing one updates
        // within ANIMATION_LOD_SLOWEST_INTERVAL frames). Frame thread, outside the update stages.
        void setLod(const AnimationLodSettings& settings);
        [[nodiscard]] AnimationLodSettings lod() const;
        void setViewer(const AnimationViewer& viewer);
        // no viewer: every animator updates every frame, none shares a pose
        void clearViewer();
        [[nodiscard]] bool hasViewer() const;
        // the entity's update rate and shared pose; the defaults for an entity with no animator
        [[nodiscard]] AnimatorLod animatorLod(Entity entity) const;

        // distinct skeletons held by bound entities
        [[nodiscard]] u32 skeletonCount() const;
        // AnimationBind runs that had entities to reconcile: a world that changed no skin or player adds none
        [[nodiscard]] u64 bindRuns() const;
        // child transforms the world wrote for bone attachments, in all: a still pose and an unchanged attachment add
        // none
        [[nodiscard]] u64 attachmentWrites() const;
        // targets that bone attachments name and that are alive: a destroyed target's entry is dropped when its
        // attachments look again, so a world that churns skinned entities does not grow the table
        [[nodiscard]] u32 attachmentTargets() const;
        // The ground query IK nodes with followGround ask (AnimationController::setGroundQuery); frame thread only.
        void setGroundQuery(GroundQuery query);
        // IK node runs in all (a node whose inputs did not change adds none)
        [[nodiscard]] u64 ikRuns() const;
        // transforms and RootMotionDeltaComponents root motion wrote, in all: an animator with no travel adds none
        [[nodiscard]] u64 rootMotionWrites() const;
        // what root motion added to the entity's TransformComponent position in all (parent space), zero for an entity
        // that hands its root motion to a RootMotionDeltaComponent or has none
        [[nodiscard]] Vec3 rootMotionApplied(Entity entity) const;
        // Between the two calls every entity's position is without what root motion added to it, so a scene collected
        // meanwhile (a save, an inverse, the snapshot a play session starts from) holds the author's positions, not a
        // preview's drift; release puts the positions back exactly. Not nested; call from the thread that owns the
        // world between frames.
        void holdRootMotion();
        void releaseRootMotion();
        // what the systems did in the last frame (anim.active, anim.sampled, anim.posed)
        [[nodiscard]] AnimationStats stats() const;
        // ozz blocks live in this module's copy of the library, all served by the process allocator (SpiteBase)
        [[nodiscard]] static u64 ozzAllocations();
        // ozz blocks ever allocated by this module's copy: a frame that allocates nothing leaves it where it was
        [[nodiscard]] static u64 ozzAllocationsTotal();

    private:
        friend SPITE_ANIMATION_API void bindAnimation(SystemContext& ctx);
        friend SPITE_ANIMATION_API void advanceAnimation(SystemContext& ctx);
        friend SPITE_ANIMATION_API void sampleAnimation(SystemContext& ctx);
        friend SPITE_ANIMATION_API void poseAnimation(SystemContext& ctx);
        void bind(SystemContext& ctx);
        void advance(SystemContext& ctx);
        void sample(SystemContext& ctx);
        void pose(SystemContext& ctx);

        struct State;
        std::unique_ptr<State> m_state;
    };

    SPITE_ANIMATION_API void bindAnimation(SystemContext& ctx);
    SPITE_ANIMATION_API void advanceAnimation(SystemContext& ctx);
    SPITE_ANIMATION_API void sampleAnimation(SystemContext& ctx);
    SPITE_ANIMATION_API void poseAnimation(SystemContext& ctx);
} // namespace spite
