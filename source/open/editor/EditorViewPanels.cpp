#include "EditorPanelRegistry.hpp"

#include "editor/EditorShell.hpp"

// The Scene and Game panels: their content is the image of a render view's target, which the shell owns with the view
// sizing and the Game panel's input focus (EditorShell, EditorSceneView).
namespace spite
{
    namespace
    {
        // no dock splitter squeezes a view below this: 320 wide and two bars and five rows high, which the
        // 36 px tab bars leave a 1024x600 window beside the Project panel's minimum
        constexpr Vec2 SCENE_MIN_SIZE{320.0f, 0.0f};
        constexpr Vec2 SCENE_MIN_BARS_ROWS{2.0f, 5.0f};

        const EditorPanelRegistrar SCENE_REGISTRATION{{.name = "scene",
                                                       .order = 20,
                                                       .dock = {.activate = true},
                                                       .minSize = SCENE_MIN_SIZE,
                                                       .minBarsRows = SCENE_MIN_BARS_ROWS,
                                                       .view = EditorShell::SCENE_VIEW}};
        const EditorPanelRegistrar GAME_REGISTRATION{{.name = "game",
                                                      .order = 30,
                                                      .dock = {.target = "scene"},
                                                      .minSize = SCENE_MIN_SIZE,
                                                      .minBarsRows = SCENE_MIN_BARS_ROWS,
                                                      .view = EditorShell::GAME_VIEW}};
    } // namespace
} // namespace spite
