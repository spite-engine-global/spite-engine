#pragma once

#include <memory>

#include "base/Platform.hpp"

namespace spite
{
    class Engine;
    class EditorModel;
    class EditorSettingsService;

    // The Scene view's grid and background as the SceneViewOverlay carries them to the grid pass: the owner
    // of every write to SceneViewOverlay::grid. A value comes from three places: the editor settings (the lines'
    // visibility, levels, plane and fade; applied before an editorSettings reply), the gizmo settings of the editor
    // model (the translate snap step when gridSpacingMode is snap) and the active theme (the clear and line colours:
    // the classes scene.grid.clear, .minor, .major, .axisX, .axisY and .axisZ of assets/ui/engine.json take their
    // background colour from the theme's tokens scene-clear, grid-minor, grid-major, grid-axis-x, -y and -z). Only view
    // 0, the Scene view, draws a grid, so its settings are the view's. update compares the stylesheet generation and
    // the gizmo's snap and writes nothing on a quiet editor.
    class EditorSceneGrid
    {
    public:
        EditorSceneGrid(Engine& engine, EditorSettingsService& settings, const EditorModel& editorModel);
        ~EditorSceneGrid();
        EditorSceneGrid(const EditorSceneGrid&) = delete;
        EditorSceneGrid& operator=(const EditorSceneGrid&) = delete;

        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
