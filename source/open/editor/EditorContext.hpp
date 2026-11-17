#pragma once

#include <string_view>
#include <vector>

#include "EditorHierarchyFilter.hpp"
#include "base/Assert.hpp"
#include "editor/EditorExport.hpp"
#include "editor/EditorShell.hpp"
#include "engine/Engine.hpp"
#include "engine/UiWorld.hpp"

namespace spite
{
    class EditorModel;
    class EditorCaptureFiles;
    class EditorSettingsService;
    class EditorAnimation;
    class EditorBuildState;
    class EditorDialogRequests;
    class EditorFeedback;
    class EditorSceneFlows;
    struct GizmoSettings;
    struct GizmoView;

    // The editor model's parts that stay inside SpiteEditor: the public EditorModel.hpp is the selection and its play
    // carry; the settings service it hosts and the gizmo settings are internal.
    struct SPITE_EDITOR_API EditorModelAccess
    {
        static EditorCaptureFiles& captureFiles(const EditorModel& model);
        static EditorSettingsService& settings(EditorModel& model);
        // The gizmo settings gizmoMode sets (EditorGizmoSettings.hpp); the reference stays valid for the model's life.
        static const GizmoSettings& gizmo(const EditorModel& model);
        static GizmoSettings& mutableGizmo(EditorModel& model);
        // The gizmo that draws and drags; one per model (setting over a set one asserts), cleared by passing {}.
        static void setGizmoView(EditorModel& model, GizmoView view);
        // The hierarchy's search and scope the model owns (EditorHierarchyFilter.hpp); the reference stays valid for
        // the model's life.
        static const HierarchyFilter& hierarchyFilter(const EditorModel& model);
        // The hierarchy panel that settles its rows when the filter changes; one per model, cleared by passing {}. A
        // hook, not model state, so a view holding a const EditorModel& sets it.
        static void setHierarchyView(const EditorModel& model, HierarchyFilterView view);
        // The dialog requests (EditorDialogRequests.hpp), alive as long as the model; EditorDialogs shows them.
        static EditorDialogRequests& dialogs(EditorModel& model);
        // The toasts and jobs the editor reports (EditorFeedback.hpp); the flows post to them, the status bar shows
        // them.
        static EditorFeedback& feedback(EditorModel& model);
        // The build state (EditorBuildState.hpp): the last build of each project and the command that reloads rebuilt
        // modules. A service the const model hands to its views: the Modules panel drives its update.
        static EditorBuildState& build(const EditorModel& model);
        // The animation tools (EditorAnimation.hpp): the selection's animator, its clips and the Scene view's bone
        // overlay flag. A service the const model hands to its views: the Animation panel and the gizmo read it.
        static EditorAnimation& animation(const EditorModel& model);
        // The scene file flows (EditorSceneFlows.hpp) the File menu, its hotkeys and the Scenes panel request.
        static EditorSceneFlows& flows(const EditorModel& model);
        // The selected entities no other selected entity contains, live in the selection's world: a subtree command
        // on each acts once per entity (duplicate, delete, copy). Empty when the world is gone.
        static std::vector<Entity> topmostSelection(const EditorModel& model);
        // Whether copy has put a scene document on the clipboard that paste would instantiate.
        static bool hasClipboard(const EditorModel& model);
    };

    // The UI world EditorShell adds to the engine for the editor's UI; control serves it as "editor".
    inline constexpr std::string_view EDITOR_UI_WORLD = "editor";
    inline constexpr u32 SCENE_VIEW = EditorShell::SCENE_VIEW;
    inline constexpr u32 GAME_VIEW = EditorShell::GAME_VIEW;

    inline UiWorld& editorUi(Engine& engine)
    {
        UiWorld* world = engine.uiWorld(EDITOR_UI_WORLD);
        SASSERTM(world, "The editor's UI world is gone: an editor part outlived its EditorShell.");
        return *world;
    }
} // namespace spite
