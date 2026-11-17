#pragma once

#include <memory>

#include "base/MathTypes.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class Engine;
    class EditorAgentLayer;
    class EditorModel;
    class EditorSceneView;
    class UiScope;

    // What the Scene view draws over its image besides the toolbar: three elements of the
    // panel's host that take no input, so the camera and the picks still reach the image under them.
    //   Ruler: a strip along the bottom edge (a canvas) with ticks and the world coordinate along the view's
    //     horizontal axis (the camera's right vector) at the camera's pivot depth: in an orthographic view the
    //     coordinate of every point, in a perspective one the scale at the pivot's depth. The labelled ticks are
    //     1, 2 or 5 powers of ten apart, about SPACING_ROWS rows of room; the minor ticks a fifth of that.
    //   HUD: a plate at the bottom left, over the ruler, with "SEL <name> . #<id>" (and "+N" for a larger selection)
    //     over "X <x>  Y <y>  Z <z>", the world position of the first selected entity of the shown world; hidden
    //     with nothing selected.
    //   Callout: a plate at the entity an agent is editing (EditorAgentLayer::callout: the entity of the EDITED window
    //     of the agent layer), "client . set field was -> now", beside the entity's screen position and kept inside
    //     the view; gone with the window and with agentLayer=off.
    // The ruler and the HUD are editor state, not saved: the toolbar's sceneOverlay names them ruler and hud. The
    // sceneHud query tells all three. Idle cost: a frame compares the camera's revision, the view's size, the
    // density, the first selected entity and its modified bits, and the agent layer's callout revision, and writes
    // nothing while none moved.
    class EditorSceneHud
    {
    public:
        EditorSceneHud(Engine& engine, UiScope& scope, const Widget& host, const EditorSceneView& sceneView,
                       const EditorModel& model, EditorAgentLayer& agent);
        ~EditorSceneHud();
        EditorSceneHud(const EditorSceneHud&) = delete;
        EditorSceneHud& operator=(const EditorSceneHud&) = delete;

        // The room over the Scene image: the panel host's size, in layout units.
        void setSize(Vec2 size);
        [[nodiscard]] bool rulerShown() const;
        void showRuler(bool shown);
        [[nodiscard]] bool hudShown() const;
        void showHud(bool shown);
        // Once per frame between frames, after the selection and the agent layer followed.
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
