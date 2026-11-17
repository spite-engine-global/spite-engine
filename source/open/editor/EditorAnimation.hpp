#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "ecs/EcsTypes.hpp"
#include "editor/EditorExport.hpp"
#include "engine/AnimationCommands.hpp"

namespace spite
{
    class Engine;

    // One clip the selected animator can play: baked for its skeleton.
    struct AnimationClipRow
    {
        std::string name;
        f32 duration = 0.0f; // seconds
        bool operator==(const AnimationClipRow&) const = default;
    };

    // What the Animation panel shows: the animator of the selection, its player and its clips, read from
    // the shown world (the play world while playing).
    struct AnimationView
    {
        std::string world; // the control name of the world shown: scene or play
        Entity animator = Entity::undefined(); // the selection's animator; undefined when there is none
        bool selected = false; // the selection holds entities of the shown world, an animator or not
        std::string name; // its entity name
        std::string skeleton;
        u32 joints = 0; // 0: its skin is not bound
        bool hasPlayer = false;
        std::string clip; // the player's clip; empty when it names none
        std::string binding = "none"; // none (no player), unbound, bound or rejected
        f32 duration = 0.0f; // seconds, of the bound clip
        f32 time = 0.0f; // the playhead
        f32 speed = 1.0f;
        f32 blend = 0.0f; // progress of a running crossfade, 0 when none
        bool playing = false;
        bool loop = false;
        bool advancing = true; // the world moves its playheads (the edit world only while its preview is on)
        bool previewable = false; // the world is the edit world: preview applies
        bool bones = false; // the Scene view draws the skeleton
        std::vector<AnimationClipRow> clips;

        // The panel's controls, each the command that would act: Play and Pause need a player, a seek a bound clip.
        [[nodiscard]] bool hasAnimator() const { return animator.isValid(); }
        [[nodiscard]] bool canPlay() const { return hasPlayer; }
        [[nodiscard]] bool canPause() const { return hasPlayer && playing; }
        [[nodiscard]] bool canStop() const { return hasPlayer; }
        [[nodiscard]] bool canSeek() const { return hasPlayer && binding == "bound" && duration > 0.0f; }
        [[nodiscard]] f32 normalized() const { return duration > 0.0f ? time / duration : 0.0f; }
        bool operator==(const AnimationView&) const = default;
    };

    // The editor model's part of the animation tools (the Animation panel is a view of it): the animator
    // the selection means, what its player and clips hold, the Scene view's bone overlay flag and the control lines
    // the panel's actions send. Every action is a control command (playAnimation, pauseAnimation,
    // stopAnimation, setAnimationSpeed, seekAnimation, previewAnimation) or the Scene overlay's (sceneOverlay bones):
    // the model sends none itself, it names them, and the panel queues the lines like every panel action.
    //
    // The animator is the first selected entity of the shown world that carries a SkinComponent, or, walking up the
    // hierarchy, a descendant of one (picking a rigged model's mesh selects a submesh below the skin root). The view
    // is rebuilt only when what it shows moved: refresh() compares a handful of component values and reads nothing
    // else, so a quiescent editor allocates and writes nothing; revision() moves with every change of the view.
    //
    // Query animationView (owner editorModel; computed from the world, so a hidden panel answers it too): the view's
    // fields (world, entity, name, skeleton, joints, player, clip, binding, duration, time, normalizedTime, speed,
    // blend, playing, loop, advancing, previewable, bones), the controls' state (canPlay, canPause, canStop, canSeek)
    // and clips: the rows {name, duration}, plus revision.
    class SPITE_EDITOR_API EditorAnimation
    {
    public:
        // selectionWorld, selectionRevision and selection are the model's; all outlive this
        EditorAnimation(Engine& engine, const std::string& selectionWorld, const u64& selectionRevision,
                        const std::vector<Entity>& selection);
        ~EditorAnimation();
        EditorAnimation(const EditorAnimation&) = delete;
        EditorAnimation& operator=(const EditorAnimation&) = delete;

        void registerCommands(std::string_view owner);

        // The view as the shown world holds it now; const (a view holds a const model) and writes only its cache.
        [[nodiscard]] const AnimationView& view() const;
        // Moves with every change of view(); a panel compares it instead of the view.
        [[nodiscard]] u64 revision() const;

        // The Scene view's bone overlay: set by the sceneOverlay command, editor state (not saved).
        [[nodiscard]] bool bonesShown() const;
        void showBones(bool shown);
        // The bones of the selection's animator in world space, when the overlay is on; appends nothing otherwise.
        void bones(std::vector<AnimatorBone>& out) const;

        // The control request lines of the panel's actions for view().animator (empty when the view has none), each
        // naming the shown world.
        // playLine: a clip plays it from the start (a crossfade of `fade` seconds when over 0), no clip resumes.
        [[nodiscard]] std::string playLine(std::string_view clip = {}, f32 fade = 0.0f) const;
        [[nodiscard]] std::string pauseLine() const;
        [[nodiscard]] std::string stopLine() const;
        // under the merge key animation.speed: a drag of the speed field is one undo step
        [[nodiscard]] std::string speedLine(f32 speed) const;
        // The playhead to `fraction` of the clip (0..1) under the scrub's merge key: a run of seeks is one undo step.
        // A loop's end is the pose just short of it (the time would wrap to the start).
        [[nodiscard]] std::string seekLine(f32 fraction) const;
        // The edit world's preview on or off; the play world has none (the line is empty there).
        [[nodiscard]] std::string previewLine(bool on) const;
        [[nodiscard]] static std::string bonesLine(bool on);

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
