#pragma once

#include <memory>
#include <array>

#include "base/MathTypes.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class Engine;
    class EditorActions;
    class EditorGizmo;
    class EditorModel;
    class EditorSceneHud;
    class EditorSceneView;
    class EditorSettingsService;
    class UiScope;

    // The Scene panel's viewport toolbar (SV1): a document view (assets/ui/editor/scene.json, slots
    // scene.toolbar, scene.tray and scene.hint) over the panel's host, drawn above the Scene image and taking no input
    // where it has no control. One row; Overlays and Camera open the tray under it on their controls (one at a time).
    // Every control is a client of a control command, queued and run between frames:
    //   Frame (hotkey F): editorCamera frame=selection, or frame=all with nothing selected in the shown world.
    //   View: a dropdown of SCENE_VIEW_MODES; a choice is editorCamera view=.
    //   Shading: a dropdown of the modes of the viewMode query, the ones the renderer cannot show now disabled; a
    //     choice runs setViewMode (an undo step) and the dropdown follows the render graph's passes.
    //   Overlays: opens the tray on seven toggles, Grid (editorSettings gridEnabled), Gizmo (gizmoMode enabled),
    //   Outline
    //     (the selection's outline), Ruler and HUD (EditorSceneHud; editor state), Bones (the skeleton of the selected
    // animator,; EditorAnimation, editor state) and Toolbar (editorSettings
    //     sceneToolbar), selected while on; sceneOverlay [name] [on] [open] does the same by command, and the
    //     sceneToolbar query reads the overlays and the shading dropdown.
    //   Camera: opens the tray on Speed (editorSettings flySpeed, the setting Editor Settings edits) and FOV, Near and
    //     Far (editorCamera fov, near, far); sceneCamera [open] does the same by command.
    //   Hide (<<): editorSettings sceneToolbar=false; the bar then shows only Show (>>), which sets it back.
    //   ?: the controls hint (slot scene.hint), a line for each of SCENE_VIEW_GESTURES; sceneHint [open] shows or
    //     hides it with the panel hidden too, and the sceneToolbar query tells whether it is open and its lines.
    // The fields show the editor camera and the settings; a refused value reverts to what they hold. Idle cost: a
    // frame compares the camera's revision, the settings' and the locale's, the render graph's view mode and the
    // overlays' flags, and writes nothing when none moved.
    class EditorSceneToolbar
    {
    public:
        EditorSceneToolbar(Engine& engine, UiScope& scope, const Widget& host, EditorSceneView& sceneView,
                           EditorSceneHud& hud, const EditorModel& editorModel, EditorSettingsService& settings,
                           const EditorActions& actions, EditorGizmo& gizmo, std::array<Widget, 7> gizmoControls);
        ~EditorSceneToolbar();
        EditorSceneToolbar(const EditorSceneToolbar&) = delete;
        EditorSceneToolbar& operator=(const EditorSceneToolbar&) = delete;

        // The room over the Scene image: the panel host's size, in layout units.
        void setSize(Vec2 size);
        // Once per frame between frames, after UI input routed.
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
