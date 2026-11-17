#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "EditorGizmoSettings.hpp"
#include "editor/EditorExport.hpp"
#include "engine/ComponentGizmos.hpp"
#include "base/MathFunctions.hpp"
#include "engine/AnimationCommands.hpp"
#include "rendering/SceneViewOverlay.hpp"

namespace spite
{
    class Engine;
    class EditorModel;
    class World;
    class EntityTransformManager;
    struct RenderView;

    struct ComponentGizmoTransforms
    {
        Mat4 world;
        Mat4 rigid;
        Vec3 position;
    };
    SPITE_EDITOR_API ComponentGizmoTransforms componentGizmoTransforms(const World& world, Entity entity,
                                                                       const EntityTransformManager& transforms,
                                                                       std::vector<Entity>& chain);

    enum class GizmoHandle : u8
    {
        None,
        X,
        Y,
        Z,
        Uniform, // scale: the centre box
    };

    // The scene view's camera as the gizmo projects through it: pixels of the view's extent, y down.
    struct GizmoCamera
    {
        Vec3 eye;
        Quat rotation;
        f32 tanHalfFov = 1.0f; // vertical
        f32 orthographicHeight = 0.0f; // > 0: an orthographic view of this much world height (tanHalfFov unused)
        f32 nearPlane = 0.1f;
        f32 farPlane = 1000.0f;
        Vec2 extent;

        // the view's pose at its extent; empty without a pose or an extent
        static std::optional<GizmoCamera> of(const RenderView& view);
        [[nodiscard]] std::optional<Vec2> project(const Vec3& point) const;
        // the unit direction of the ray through a pixel (from the eye; the view direction when orthographic) and
        // where it starts
        [[nodiscard]] Vec3 ray(Vec2 pixel) const;
        [[nodiscard]] Vec3 rayOrigin(Vec2 pixel) const;
        // the world length one pixel spans at the point's depth; 0 at or behind the near plane
        [[nodiscard]] f32 worldPerPixel(const Vec3& point) const;
    };

    // Where the gizmo stands: its pivot, its three unit axes (the space's) and its world length per handle.
    struct GizmoFrame
    {
        Vec3 pivot;
        std::array<Vec3, 3> axes;
        f32 size = 1.0f;
    };

    // The drop ghost: where a dragged model would land (point) and its bounds there, drawn with the gizmo's segments
    // as the box and a cross at the point (surface: the point lies on scene bounds, else on the ground).
    struct GizmoGhost
    {
        Vec3 point;
        Vec3 boundsMin;
        Vec3 boundsMax;
        bool surface = false;
    };
    void ghostSegments(const GizmoCamera& camera, const GizmoGhost& ghost,
                       std::vector<SceneViewOverlay::GizmoSegment>& out);

    // The skeleton of an animator in view pixels: a line for each bone and a small cross where it ends, at
    // each joint. A bone with an end behind the eye is left out.
    void boneSegments(const GizmoCamera& camera, std::span<const AnimatorBone> bones,
                      std::vector<SceneViewOverlay::GizmoSegment>& out);

    // The handle under a pixel (None: none within reach), and the gizmo's segments in view pixels with the
    // active handle highlighted.
    GizmoHandle hitGizmo(const GizmoCamera& camera, const GizmoFrame& frame, GizmoMode mode, Vec2 pixel);
    void gizmoSegments(const GizmoCamera& camera, const GizmoFrame& frame, GizmoMode mode, GizmoHandle active,
                       std::vector<SceneViewOverlay::GizmoSegment>& out);
    // A pixel that grabs the handle (and no other): an axis' point near its tip, a ring's point farthest from
    // the other rings, the pivot for Uniform; empty when the handle is not shown.
    std::optional<Vec2> gizmoGrabPoint(const GizmoCamera& camera, const GizmoFrame& frame, GizmoMode mode,
                                       GizmoHandle handle);

    // The editor's transform gizmo  on the topmost selected entities of the world the scene
    // view draws (the play world while playing; Play or Stop ends a drag). It draws the handles and drags them; its
    // settings (mode, space, snap, enabled, steps) are the EditorModel's (GizmoSettings, the gizmoMode command and
    // the gizmo query's settings, owner editorModel). It registers under owner "editorGizmo":
    //   gizmoDrag phase=begin|move|end [x y]: a drag in scene view pixels. begin grabs the handle under (x, y)
    //     and the targets; each move writes the targets' TransformComponent fields through one set/batch under
    //     the drag's merge key, so the whole drag is one undo step and one command-log row; end finishes.
    // and gives the model its view of the gizmo: whether a drag runs (gizmoMode refuses a mode or visibility
    // change then) and the gizmo query's pivot, axes, size, handle pixels and drag.
    // Translate moves along an axis (snapped by translateStep, relative to the start); rotate turns about an
    // axis through the pivot (the targets' mean position; snapped by rotateStep degrees); scale multiplies each
    // target's local scale along that local axis, or all three (Uniform), by a factor snapped to scaleStep.
    // World space uses the world axes; local space and scale use the last selected target's.
    //
    // Idle cost: nothing without a scene selection; with one, the gizmo's segments (9 to 144) are recomputed each
    // frame and uploaded only when they change.
    class EditorGizmo
    {
    public:
        [[nodiscard]] bool componentCategoryEnabled(ComponentGizmoCategory category) const;
        void setComponentCategoryEnabled(ComponentGizmoCategory category, bool enabled);
        [[nodiscard]] bool selectedComponentGizmosEnabled() const;
        void setSelectedComponentGizmosEnabled(bool enabled);
        // editorModel is not const: the gizmo sets its view on it and clears it in its destructor
        EditorGizmo(Engine& engine, EditorModel& editorModel);
        ~EditorGizmo();
        EditorGizmo(const EditorGizmo&) = delete;
        EditorGizmo& operator=(const EditorGizmo&) = delete;

        // the handle the gizmo shows under a scene view pixel; None without one
        [[nodiscard]] GizmoHandle handleAt(Vec2 viewPixel) const;
        // The scene view pixel the pointer is over (none: it left the view); the handle under it is drawn highlighted
        // like a dragged one while no drag runs.
        void setHover(std::optional<Vec2> viewPixel);
        // The drop ghost the scene view shows while a model is dragged over it; empty ends it.
        void setGhost(const std::optional<GizmoGhost>& ghost);
        // Once per frame between frames, after the scene view ran its commands: the gizmo's segments.
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
