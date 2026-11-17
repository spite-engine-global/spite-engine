#pragma once

#include <memory>

#include "ui/UiWidgets.hpp"

namespace spite
{
    class Engine;
    class EditorModel;

    // The Animation panel, a view of EditorAnimation (the model holds the selection's animator, its clips
    // and the command lines; this panel keeps only presentation) as a UI document (assets/ui/editor/animation.json:
    // slots animation.toolbar's bar, animation.scrub, animation.rows and animation.status). Every action is a control
    // command, queued and sent between frames:
    //   a clip row   playAnimation clip (from its start); the row of the player's clip is selected
    //   Play         playAnimation (resumes the clip where it is)      Pause   pauseAnimation
    //   Stop         stopAnimation (rewinds to the first pose)
    //   Speed        setAnimationSpeed under the merge key animation.speed: a drag is one undo step
    //   the slider   seekAnimation under animation.scrub: a scrub is one undo step; the pose follows on screen
    //   Preview      previewAnimation (the edit scene is scrub-only until it is on; not shown in the play world)
    //   Bones        sceneOverlay bones: the Scene view draws the skeleton of the selected animator
    // The animationView query (owner editorModel) answers for the selection hidden or shown. Idle cost: a frame
    // compares the model's revision and the locale's and writes nothing when neither moved; a hidden panel only sends
    // what was queued.
    class EditorAnimationPanel
    {
    public:
        EditorAnimationPanel(Engine& engine, const EditorModel& model);
        ~EditorAnimationPanel();
        EditorAnimationPanel(const EditorAnimationPanel&) = delete;
        EditorAnimationPanel& operator=(const EditorAnimationPanel&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
