#include "EditorSceneGrid.hpp"

#include <algorithm>

#include "EditorContext.hpp"
#include "EditorGizmoSettings.hpp"
#include "EditorSettings.hpp"
#include "base/InternedString.hpp"
#include "engine/Engine.hpp"
#include "rendering/SceneViewOverlay.hpp"
#include "uiview/UiStyleSheet.hpp"
#include "uiview/UiViewContext.hpp"

namespace spite
{
    namespace
    {
        // the style classes of assets/ui/engine.json whose background colour is a theme token
        constexpr const char* CLEAR_CLASS = "scene.grid.clear";
        constexpr const char* MINOR_CLASS = "scene.grid.minor";
        constexpr const char* MAJOR_CLASS = "scene.grid.major";
        constexpr const char* AXIS_X_CLASS = "scene.grid.axisX";
        constexpr const char* AXIS_Y_CLASS = "scene.grid.axisY";
        constexpr const char* AXIS_Z_CLASS = "scene.grid.axisZ";

        u32 colorOf(const UiStyleSheet& sheet, const char* styleClass)
        {
            const InternedString name(styleClass);
            return sheet.resolve(name, UiWidgetKind::Panel, UiInteractionState::Normal, name).backgroundColor;
        }

        SceneViewOverlay::Grid::Plane planeOf(const std::string_view name)
        {
            const auto at = std::ranges::find(GRID_PLANES, name);
            return at == GRID_PLANES.end() ? SceneViewOverlay::Grid::Plane::Auto
                                           : static_cast<SceneViewOverlay::Grid::Plane>(at - GRID_PLANES.begin());
        }
    } // namespace

    struct EditorSceneGrid::State
    {
        Engine* engine;
        EditorSettingsService* settings;
        const GizmoSettings* gizmo;
        u32 applied = 0; // the settings listener, removed with the grid
        u64 sheetGeneration = ~0ull;
        bool snapOn = false;
        f32 snapStep = 0.0f;

        // Writes the grid the settings, the gizmo and the theme ask for.
        void write()
        {
            const EditorSettings& s = settings->settings();
            const UiStyleSheet& sheet = editorUi(*engine).uiView().stylesheet();
            sheetGeneration = sheet.generation();
            snapOn = gizmo->snap;
            snapStep = gizmo->step(GizmoMode::Translate);
            SceneViewOverlay::Grid grid;
            grid.enabled = s.gridEnabled;
            grid.opacity = s.gridOpacity;
            grid.spacing = effectiveGridSpacing(s, snapOn, snapStep);
            grid.majorEvery = s.gridMajorEvery;
            grid.fadeDistance = s.gridFadeDistance;
            grid.plane = planeOf(s.gridPlane);
            grid.axes = s.gridAxes;
            grid.clearColor = colorOf(sheet, CLEAR_CLASS);
            grid.minorColor = colorOf(sheet, MINOR_CLASS);
            grid.majorColor = colorOf(sheet, MAJOR_CLASS);
            grid.axisXColor = colorOf(sheet, AXIS_X_CLASS);
            grid.axisYColor = colorOf(sheet, AXIS_Y_CLASS);
            grid.axisZColor = colorOf(sheet, AXIS_Z_CLASS);
            auto overlay = engine->services().write<SceneViewOverlay>();
            overlay->grid = grid;
        }
    };

    EditorSceneGrid::EditorSceneGrid(Engine& engine, EditorSettingsService& settings, const EditorModel& editorModel) :
        m_state(std::make_unique<State>(&engine, &settings, &EditorModelAccess::gizmo(editorModel)))
    {
        m_state->write();
        State* state = m_state.get();
        state->applied = settings.onApplied([state](const EditorSettings&) { state->write(); });
    }

    EditorSceneGrid::~EditorSceneGrid() { m_state->settings->removeApplied(m_state->applied); }

    void EditorSceneGrid::update()
    {
        State& state = *m_state;
        if (editorUi(*state.engine).uiView().stylesheet().generation() != state.sheetGeneration ||
            state.gizmo->snap != state.snapOn || state.gizmo->step(GizmoMode::Translate) != state.snapStep)
        {
            state.write();
        }
    }
} // namespace spite
