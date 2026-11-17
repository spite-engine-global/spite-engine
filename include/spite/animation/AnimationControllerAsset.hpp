#pragma once

#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "animation/AnimationAuthoringExport.hpp"
#include "animation/AnimationController.hpp"
#include "animation/AnimationExport.hpp"
#include "base/Expected.hpp"
#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"

namespace spite
{
    // The graph a controller asset (JSON, version 1) describes, as plain data. The file:
    //   { "version": 1,
    //     "parameters": [ { "name": "speed", "type": "float", "default": 0 },
    //                     { "name": "grounded", "type": "bool", "default": true },
    //                     { "name": "jump", "type": "trigger" } ],
    //     "states": [ { "name": "idle", "clip": "hero.Idle", "speed": 1, "loop": true }, ... ],
    //     "initial": "idle",
    //     "transitions": [ { "from": "idle", "to": "walk", "duration": 0.2, "exitTime": 0.9,
    //                        "conditions": [ { "parameter": "speed", "op": ">", "value": 0.1 },
    //                                        { "parameter": "grounded", "value": true },
    //                                        { "parameter": "jump" } ] } ] }
    // Parameter types are float, bool and trigger (a trigger has no default); initial defaults to the first state; from
    // may be "*" (any state); exitTime is a normalized time of the source's clip; a float condition's op is one of
    // > >= < <= == !=, a bool's is == (the default) or != with a true or false value, a trigger's takes neither. A
    // transition is taken when every condition holds (and the source state's normalized time has reached exitTime
    // when it has one); the first one in file order wins. Unknown keys, states, parameters and clips are errors that
    // name them.
    //
    // A state plays one clip, or a blend tree in place of the clip (give exactly one of "clip" and "blendTree"):
    //   { "name": "move", "blendTree": { "type": "1D", "parameter": "speed",
    //       "children": [ { "clip": "hero.Idle", "threshold": 0 }, { "clip": "hero.Walk", "threshold": 1 },
    //                     { "clip": "hero.Run", "threshold": 3 } ] } }
    //   { "name": "strafe", "blendTree": { "type": "2D", "parameters": [ "x", "y" ], "children": [
    //       { "clip": "hero.Idle", "position": [0, 0] }, { "clip": "hero.Left", "position": [-1, 0] } ] } }
    // The parameters are floats. A 1D tree mixes the two children around the value (thresholds strictly ascend; below
    // the first or above the last the end child plays alone); a 2D tree weights the three children nearest to the point
    // by 1/d - 1/d4 (d4 the distance of the fourth nearest, none for three children or fewer), normalized, so a child
    // plays alone at its own position and the weights move continuously, except where four or more children
    // are equally near (a ring of children around the point): the nearest three then share it equally. The children
    // play in step: the playhead is a phase shared by all of them (a normalized time of the first child's clip, which
    // is the state's clip for its exitTime and the animator report), and it runs at the weighted mean of the children's
    // durations. At most MAX_BLEND_TREE_CHILDREN children.
    //
    // Layers play over the base layer (the top-level states and transitions), in file order, each with a state machine
    // of its own (states, initial and transitions as above, scoped to the layer), a joint mask and a weight:
    //   "layers": [ { "name": "upper", "additive": false, "weight": 1,   (or "weightParameter": "waveWeight")
    //                 "mask": { "root": "Spine", "weight": 1, "joints": [ { "joint": "Head", "weight": 0.5 } ] },
    //                 "states": [ ... ], "initial": "...", "transitions": [ ... ] } ]
    // The mask gives each joint a weight in 0..1: the joints of root's subtree (root included) get its weight (default
    // 1), the others 0, and `joints` then sets single joints (inside the subtree or not); no mask is every joint at 1.
    // An override layer blends over what is below it (the base and the layers before it) by weight x joint weight, so
    // at 1 its joint is the layer's pose and at 0 the pose below it; an additive layer adds its clip's pose relative to
    // the clip's first frame, scaled the same way. A weight is a finite number from 0 (a layer with no weight costs no
    // pose work and is paused); "weightParameter" names a float parameter that sets it (value clamped like the
    // constant). The runtime clamps an override weight to 1 and an additive one to ADDITIVE_WEIGHT_MAX, and uses only
    // the first MAX_ADDITIVE_LAYERS additive layers (the rest are disabled and the log says so once): what the baked
    // bounds of a model imported with usesLayers cover. At most MAX_CONTROLLER_LAYERS layers. A joint name that the
    // skeleton lacks is found when the controller binds: the controller is then Rejected (UnknownJoint). A layer plays
    // only on a model imported with usesLayers (otherwise Rejected, NotEnabled). Layers follow the player's transport
    // (they pause with it and run on after the base's once-clip ended) and, like the base, do not act in a world that
    // does not advance. A trigger is consumed by the first state machine that takes it, the base first, then the layers
    // in order. A layer with no weight keeps its playhead and any fade it began until it has weight again. Root motion
    // is the player's rootMotion flag (AnimationPlayerComponent), not a key of the file: it moves the entity by the
    // base motion's travel (the blend tree's and a transition's at their weights) and layers add none; a key not
    // listed here is refused.
    struct ControllerParameter
    {
        InternedString name;
        AnimationParameterKind kind = AnimationParameterKind::Float;
        f32 initial = 0.0f; // bool: 0 or 1
    };

    enum class ControllerCompare : u8
    {
        Greater,
        GreaterEqual,
        Less,
        LessEqual,
        Equal,
        NotEqual,
        Triggered
    };

    struct ControllerCondition
    {
        u32 parameter = 0; // index into parameters
        ControllerCompare compare = ControllerCompare::Equal;
        f32 value = 0.0f; // bool: 0 or 1
    };

    constexpr u32 MAX_BLEND_TREE_CHILDREN = 16;
    // children with a weight at once: the bake's bound of a blend (SpiteAsset's SkinnedBounds) counts on it
    constexpr u32 MAX_BLEND_TREE_ACTIVE = 3;

    enum class BlendTreeKind : u8
    {
        OneD,
        TwoD
    };

    struct BlendTreeChild
    {
        InternedString clip;
        f32 x = 0.0f; // 1D: the threshold
        f32 y = 0.0f;
    };

    struct ControllerBlendTree
    {
        BlendTreeKind kind = BlendTreeKind::OneD;
        u32 parameters[2] = {0, 0}; // indices into parameters (floats); 1D reads only the first
        std::vector<BlendTreeChild> children; // empty: the state plays its clip
    };

    struct ControllerState
    {
        InternedString name;
        InternedString clip; // a blend tree's state: the first child's clip
        f32 speed = 1.0f; // the player's speed in this state
        bool loop = true;
        ControllerBlendTree tree;

        [[nodiscard]] bool hasTree() const { return !tree.children.empty(); }
    };

    struct ControllerTransition
    {
        i32 from = -1; // index into states, -1 from any state
        u32 to = 0;
        f32 duration = 0.0f; // crossfade seconds, 0 cuts
        f32 exitTime = -1.0f; // normalized time of the source's clip the transition waits for, negative none
        std::vector<ControllerCondition> conditions;
    };

    struct ControllerStateMachine
    {
        std::vector<ControllerState> states;
        std::vector<ControllerTransition> transitions;
        u32 initial = 0;
    };

    constexpr u32 MAX_CONTROLLER_LAYERS = 4;
    // What the baked bounds of a usesLayers model cover (LAYER_ADDITIVE_MAX and LAYER_ADDITIVE_WEIGHT_MAX in
    // asset/BakedAnimation.hpp; SpiteAnimation may not include asset headers, a test holds the two equal).
    constexpr u32 MAX_ADDITIVE_LAYERS = 2;
    constexpr f32 ADDITIVE_WEIGHT_MAX = 1.0f;

    struct ControllerMaskJoint
    {
        InternedString joint;
        f32 weight = 1.0f;
    };

    struct ControllerJointMask
    {
        InternedString root; // invalid: no subtree
        f32 weight = 1.0f; // of the subtree
        std::vector<ControllerMaskJoint> joints;

        [[nodiscard]] bool any() const { return root.isValid() || !joints.empty(); }
    };

    struct ControllerLayer : ControllerStateMachine
    {
        InternedString name;
        ControllerJointMask mask;
        bool additive = false;
        f32 weight = 1.0f;
        i32 weightParameter = -1; // index into parameters, -1 none
    };

    struct AnimationControllerDef : ControllerStateMachine
    {
        InternedString name;
        std::vector<ControllerParameter> parameters;
        std::vector<ControllerLayer> layers;
    };

    // The weights of the tree's children at the parameter values (x the first parameter's, y the second's, ignored by a
    // 1D tree): weights.size() must be the child count. At most MAX_BLEND_TREE_ACTIVE are not zero and they sum to 1.
    SPITE_ANIMATION_API void blendTreeWeights(const ControllerBlendTree& tree, f32 x, f32 y, std::span<f32> weights);

    // Parses and validates the file text. name labels every error ("controller 'name': ..."). clipExists, when given,
    // checks each state's clip (a clip it refuses is an error naming the state and the clip).
    SPITE_ANIMATION_AUTHORING_API Expected<AnimationControllerDef> parseAnimationController(
        std::string_view text, InternedString name, const std::function<bool(InternedString)>& clipExists = {});
} // namespace spite
