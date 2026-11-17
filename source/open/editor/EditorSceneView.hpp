#pragma once

#include <array>
#include <memory>
#include <string_view>

#include "base/MathFunctions.hpp"
#include "base/Schema.hpp"

namespace spite
{
    class Engine;
    class EditorGizmo;
    class EditorModel;
    struct Image;
    struct EditorSettings;

    // The views of the editorCamera `view` field: the free perspective camera, the same camera orthographic, and
    // the orthographic axis views (looking down -Y, +Y, -Z, +Z, -X, +X).
    inline constexpr std::array<std::string_view, 8> SCENE_VIEW_MODES{"perspective", "orthographic", "top",   "bottom",
                                                                      "front",       "back",         "right", "left"};

    // The UiDragDrop payload type of a dragged Project model: the Project panel starts it, the Scene view's ghost
    // follows it.
    inline constexpr std::string_view MODEL_DRAG_PAYLOAD = "asset.model";

    // The gestures the Scene view answers, in the order the toolbar's controls hint lists them: its text for each is
    // the message editor-scene.gesture.<id> ("frame" carries the Frame hotkey as {key}).
    inline constexpr std::array<std::string_view, 11> SCENE_VIEW_GESTURES{
        "pick", "pickAdd", "pickToggle", "orbit", "look", "fly", "boost", "pan", "dolly", "gizmo", "frame"};

    // The editor camera: an orbit camera around pivot, which flying moves with it. Angles in degrees; yaw 0
    // looks down -Z, positive pitch looks up (an axis view looks along +-Y: pitch +-90). Orthographic: the view is
    // 2 * distance * tan(fov / 2) tall at every depth, so the framing at the pivot is the perspective's.
    struct EditorCamera
    {
        Vec3 pivot{0.0f, 0.0f, 0.0f};
        float yaw = 0.0f;
        float pitch = -20.0f;
        float distance = 15.0f;
        float fov = 60.0f;
        float nearPlane = 0.1f;
        float farPlane = 1000.0f;
        bool orthographic = false;
        SPITE_OBSERVED_SCHEMA(EditorCamera, pivot, yaw, pitch, distance, fov, nearPlane, farPlane, orthographic)

        [[nodiscard]] Quat rotation() const;
        [[nodiscard]] Vec3 forward() const;
        [[nodiscard]] Vec3 position() const { return pivot - forward() * distance; }
        // The deltas the editorCamera command takes. Orbit turns around the pivot, look turns in place (the
        // pivot moves), pan and fly move both (camera-relative: right, up, forward), dolly moves toward the pivot.
        void orbit(float yawDegrees, float pitchDegrees);
        void look(float yawDegrees, float pitchDegrees);
        void pan(float right, float up);
        void fly(const Vec3& rightUpForward);
        void dolly(float amount);
    };

    // The Scene panel's interaction : the editor camera the scene view (view 0) draws
    // through (RenderView::pose; the game view keeps the scene camera), click picking from the visibility buffer
    // and the selection outline, all on the world the scene view draws (Engine::gameWorld: the play world
    // while playing). Every action is a control command under owner "editorSceneView":
    //   editorCamera: sets the camera (pivot, position, yaw, pitch, distance, fov, near, far) and/or applies the
    //     orbit, look, pan, fly and dolly deltas; frame=selection|all then moves the pivot to the middle of the
    //     selection's (or every) bounds and the distance to fit them in the view, keeping the direction; replies
    //     with the camera. view=perspective|orthographic|top|bottom|front|back|right|left picks the projection and
    //     an axis direction first: leaving perspective keeps the camera as it was and view=perspective restores it
    //     whole; an axis view turned by yaw, pitch, orbit or look is the orthographic view. A value the camera
    //     cannot hold (distance < 0.05, fov outside (1, 179), near <= 0, far <= near) and a pose that would not be
    //     finite (a sum or eye past the float range) refuse the whole command.
    //   pick x y [mode=replace|add|toggle]: what the scene view shows at that view pixel, read back from the GPU
    //     visibility buffer frames later, becomes the selection through the select command (the shown world);
    //     replies once read back (over a pipe or stdio), with the entity hit (none: the background); refused when
    //     Play or Stop rebinds the view before the read-back.
    // query dropPoint x y [snap]: where a model dropped at that view pixel lands: the nearest of the
    //     first scene bounds (WorldBoundingBoxComponent, the entity named) the pixel's ray enters and the ground
    //     plane y=0 in front of the camera and short of its far plane, for a perspective and an orthographic view;
    //     snapped to the gizmo's translate step when snap is on (default the gizmo's setting). Replies point,
    //     source (surface|ground), entity and snapped; refused without a scene view, outside it, for a ray along
    //     the ground and one that meets nothing. The Project panel's drop sends it, then instantiateModel with
    //     the point as the transform. While a model is dragged over the view a ghost (the model's bounds and a
    //     cross) shows that point (EditorGizmo::setGhost).
    //   query sceneView: the shown world and render scene, the camera, the picks pending, the last pick and the
    //     outlined instance slots.
    // Gestures on the Scene image queue them: a click picks (Shift adds, Ctrl toggles), Alt+drag orbits, the wheel
    // dollies; with an InputSystem, a right drag looks while WASD/QE fly (held Shift, either, ramps the speed:
    // FlyBoost) and a middle drag pans. A primary press on a transform gizmo handle drags it instead (EditorGizmo:
    // gizmoDrag begin, move, end). The scene selection's renderables and their descendants' are outlined
    // (SceneViewOverlay).
    //
    // Idle cost: nothing without a gesture, a pick in flight or a scene selection; with a selection, its
    // renderables' slots are re-read per frame (a slot can be assigned or reused without a command).
    class EditorSceneView
    {
    public:
        EditorSceneView(Engine& engine, const EditorModel& editorModel, EditorGizmo& gizmo, const Image& sceneImage,
                        const EditorSettings& settings);
        ~EditorSceneView();
        EditorSceneView(const EditorSceneView&) = delete;
        EditorSceneView& operator=(const EditorSceneView&) = delete;

        // a right drag begun on the Scene image looks and flies (WASDQE): from the press until update sees it end
        [[nodiscard]] bool flying() const;
        // The camera as the last editorCamera left it, and a count that moves with every change of it (a toolbar
        // refreshes from the count, never per frame).
        [[nodiscard]] const EditorCamera& camera() const;
        [[nodiscard]] u64 cameraRevision() const;
        // The camera's SCENE_VIEW_MODES entry.
        [[nodiscard]] std::string_view viewMode() const;
        // Whether the selection's outline is drawn (the toolbar's sceneOverlay outline); editor state, not saved.
        [[nodiscard]] bool outlineShown() const;
        void showOutline(bool shown);
        // What Frame acts on: the selection when the shown world has one, else everything.
        [[nodiscard]] static std::string_view frameTarget(const Engine& engine, const EditorModel& editorModel);
        // Once per frame between frames, after UI input routed.
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
