#include "EditorGizmo.hpp"
#include "engine/ComponentGizmos.hpp"
#include "EditorAnimation.hpp"
#include "EditorContext.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "editor/EditorModel.hpp"
#include "base/Logging.hpp"
#include "control/ControlServer.hpp"
#include "ecs/Query.hpp"
#include "ecs/QueryBuilder.hpp"
#include "engine/Engine.hpp"
#include "renderruntime/LightComponents.hpp"
#include "renderruntime/RenderRuntime.hpp"
#include "renderruntime/RenderView.hpp"
#include "serialization/FormatWalk.hpp"
#include "world/EntityTransformManager.hpp"
#include "world/TransformComponents.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using Segment = SceneViewOverlay::GizmoSegment;
        constexpr std::string_view OWNER = "editorGizmo";
        constexpr std::array<std::string_view, 3> DRAG_PHASES{"begin", "move", "end"};
        constexpr std::array<std::string_view, 5> HANDLES{"none", "x", "y", "z", "uniform"};

        constexpr f32 GIZMO_PIXELS = 90.0f; // a handle's on-screen length
        constexpr f32 HIT_PIXELS = 8.0f;
        constexpr f32 UNIFORM_PIXELS = 10.0f; // the scale centre box's reach
        constexpr f32 MIN_AXIS_PIXELS = 6.0f; // an axis pointing at the eye shows no handle
        constexpr u32 RING_SEGMENTS = 48;
        constexpr f32 MIN_SCALE_FACTOR = 0.01f;

        constexpr u32 rgba(const u32 r, const u32 g, const u32 b, const u32 a = 255)
        {
            return r | g << 8 | b << 16 | a << 24;
        }
        constexpr std::array<u32, 3> AXIS_COLORS{rgba(230, 60, 60), rgba(90, 200, 70), rgba(60, 110, 235)};
        constexpr u32 UNIFORM_COLOR = rgba(220, 220, 220);
        constexpr u32 ACTIVE_COLOR = rgba(255, 210, 40);
        constexpr u32 GHOST_GROUND_COLOR = rgba(80, 200, 255);
        constexpr f32 GHOST_CROSS_PIXELS = 8.0f;
        constexpr u32 BONE_COLOR = rgba(120, 230, 255);
        constexpr f32 BONE_JOINT_PIXELS = 3.0f;

        u32 axisIndex(const GizmoHandle handle) { return static_cast<u32>(handle) - static_cast<u32>(GizmoHandle::X); }
        bool isAxis(const GizmoHandle handle) { return handle >= GizmoHandle::X && handle <= GizmoHandle::Z; }

        f32 distanceToSegment(const Vec2 point, const Vec2 a, const Vec2 b)
        {
            const Vec2 ab = b - a;
            const f32 lengthSq = dot(ab, ab);
            const f32 t = lengthSq > 0.0f ? clamp(dot(point - a, ab) / lengthSq, 0.0f, 1.0f) : 0.0f;
            return length(point - (a + ab * t));
        }

        // the ring of axis i: the circle of radius size about the pivot in the plane of the other two axes
        Vec3 ringPoint(const GizmoFrame& frame, const u32 axis, const f32 angle)
        {
            return frame.pivot +
                (frame.axes[(axis + 1) % 3] * std::cos(angle) + frame.axes[(axis + 2) % 3] * std::sin(angle)) *
                frame.size;
        }

        // the ring's projected polyline; points behind the eye break it
        using Ring = std::array<std::optional<Vec2>, RING_SEGMENTS + 1>;
        Ring ringPixels(const GizmoCamera& camera, const GizmoFrame& frame, const u32 axis)
        {
            Ring points;
            for (u32 i = 0; i <= RING_SEGMENTS; ++i)
            {
                points[i] = camera.project(ringPoint(frame, axis, 6.2831853f * static_cast<f32>(i) / RING_SEGMENTS));
            }
            return points;
        }

        f32 distanceToPolyline(const Vec2 pixel, const Ring& points)
        {
            f32 best = 1e30f;
            for (sizet i = 1; i < points.size(); ++i)
            {
                if (points[i - 1] && points[i])
                {
                    best = std::min(best, distanceToSegment(pixel, *points[i - 1], *points[i]));
                }
            }
            return best;
        }

        // the closest point's parameter along the line pivot + t * axis to the ray origin + s * direction (unit
        // vectors); empty when they are nearly parallel
        std::optional<f32> axisParameter(const Vec3& pivot, const Vec3& axis, const Vec3& origin, const Vec3& direction)
        {
            const f32 b = dot(axis, direction);
            const f32 denominator = 1.0f - b * b;
            if (denominator < 1e-4f)
            {
                return std::nullopt;
            }
            const Vec3 w = pivot - origin;
            return (b * dot(direction, w) - dot(axis, w)) / denominator;
        }

        // where the ray meets the plane through pivot with the normal, as a unit vector from the pivot
        std::optional<Vec3> planeDirection(const Vec3& pivot, const Vec3& normal, const Vec3& origin,
                                           const Vec3& direction)
        {
            const f32 denominator = dot(direction, normal);
            if (std::abs(denominator) < 1e-4f)
            {
                return std::nullopt;
            }
            const Vec3 hit = origin + direction * (dot(pivot - origin, normal) / denominator);
            const Vec3 offset = hit - pivot;
            const f32 offsetLength = length(offset);
            return offsetLength > 1e-6f ? std::optional(offset / offsetLength) : std::nullopt;
        }

        f32 snapped(const f32 value, const f32 step, const bool snap)
        {
            return snap && step > 0.0f ? std::round(value / step) * step : value;
        }

        Vec3 column(const Mat4& matrix, const u32 index)
        {
            return Vec3(matrix[index][0], matrix[index][1], matrix[index][2]);
        }

        Json floats(const f32* values, const u32 count)
        {
            Json array = Json::array();
            for (u32 i = 0; i < count; ++i)
            {
                array.push_back(values[i]);
            }
            return array;
        }
    } // namespace

    std::optional<GizmoCamera> GizmoCamera::of(const RenderView& view)
    {
        if (!view.pose || view.extent.width == 0 || view.extent.height == 0)
        {
            return std::nullopt;
        }
        return GizmoCamera{.eye = view.pose->transform.position,
                           .rotation = view.pose->transform.rotation,
                           .tanHalfFov = std::tan(view.pose->camera.fov * 0.5f),
                           .orthographicHeight = view.pose->orthographicHeight,
                           .nearPlane = view.pose->camera.nearPlane,
                           .farPlane = view.pose->camera.farPlane,
                           .extent = Vec2(static_cast<f32>(view.extent.width), static_cast<f32>(view.extent.height))};
    }

    std::optional<Vec2> GizmoCamera::project(const Vec3& point) const
    {
        const Vec3 local = spite::rotate(inverse(rotation), point - eye);
        if (-local.z < nearPlane)
        {
            return std::nullopt;
        }
        const f32 aspect = extent.x / extent.y;
        const bool orthographic = orthographicHeight > 0.0f;
        const f32 x =
            orthographic ? local.x / (0.5f * orthographicHeight * aspect) : local.x / (-local.z * tanHalfFov * aspect);
        const f32 y = orthographic ? local.y / (0.5f * orthographicHeight) : local.y / (-local.z * tanHalfFov);
        return Vec2((x + 1.0f) * 0.5f * extent.x, (1.0f - y) * 0.5f * extent.y);
    }

    Vec3 GizmoCamera::ray(const Vec2 pixel) const
    {
        if (orthographicHeight > 0.0f)
        {
            return spite::rotate(rotation, Vec3(0.0f, 0.0f, -1.0f));
        }
        const f32 aspect = extent.x / extent.y;
        const f32 x = pixel.x / extent.x * 2.0f - 1.0f;
        const f32 y = 1.0f - pixel.y / extent.y * 2.0f;
        return normalize(spite::rotate(rotation, Vec3(x * tanHalfFov * aspect, y * tanHalfFov, -1.0f)));
    }

    Vec3 GizmoCamera::rayOrigin(const Vec2 pixel) const
    {
        if (orthographicHeight <= 0.0f)
        {
            return eye;
        }
        const f32 x = pixel.x / extent.x * 2.0f - 1.0f;
        const f32 y = 1.0f - pixel.y / extent.y * 2.0f;
        const f32 halfHeight = 0.5f * orthographicHeight;
        return eye + spite::rotate(rotation, Vec3(x * halfHeight * extent.x / extent.y, y * halfHeight, 0.0f));
    }

    f32 GizmoCamera::worldPerPixel(const Vec3& point) const
    {
        const f32 depth = -spite::rotate(inverse(rotation), point - eye).z;
        if (depth < nearPlane)
        {
            return 0.0f;
        }
        return orthographicHeight > 0.0f ? orthographicHeight / extent.y : 2.0f * tanHalfFov * depth / extent.y;
    }

    GizmoHandle hitGizmo(const GizmoCamera& camera, const GizmoFrame& frame, const GizmoMode mode, const Vec2 pixel)
    {
        const std::optional<Vec2> pivot = camera.project(frame.pivot);
        if (!pivot)
        {
            return GizmoHandle::None;
        }
        if (mode == GizmoMode::Scale && length(pixel - *pivot) <= UNIFORM_PIXELS)
        {
            return GizmoHandle::Uniform;
        }
        GizmoHandle best = GizmoHandle::None;
        f32 bestDistance = HIT_PIXELS;
        for (u32 axis = 0; axis < 3; ++axis)
        {
            f32 distance = 1e30f;
            if (mode == GizmoMode::Rotate)
            {
                distance = distanceToPolyline(pixel, ringPixels(camera, frame, axis));
            }
            else if (const std::optional<Vec2> tip = camera.project(frame.pivot + frame.axes[axis] * frame.size);
                     tip && length(*tip - *pivot) >= MIN_AXIS_PIXELS)
            {
                distance = distanceToSegment(pixel, *pivot, *tip);
            }
            if (distance <= bestDistance)
            {
                bestDistance = distance;
                best = static_cast<GizmoHandle>(static_cast<u32>(GizmoHandle::X) + axis);
            }
        }
        return best;
    }

    void gizmoSegments(const GizmoCamera& camera, const GizmoFrame& frame, const GizmoMode mode,
                       const GizmoHandle active, std::vector<Segment>& out)
    {
        out.clear();
        const std::optional<Vec2> pivot = camera.project(frame.pivot);
        if (!pivot)
        {
            return;
        }
        const auto box = [&out](const Vec2 center, const f32 half, const u32 color, const f32 halfWidth)
        {
            const Vec2 corners[4]{center + Vec2(-half, -half), center + Vec2(half, -half), center + Vec2(half, half),
                                  center + Vec2(-half, half)};
            for (u32 i = 0; i < 4; ++i)
            {
                out.push_back({corners[i], corners[(i + 1) % 4], halfWidth, color});
            }
        };
        for (u32 axis = 0; axis < 3; ++axis)
        {
            const bool highlighted = isAxis(active) && axisIndex(active) == axis;
            const u32 color = highlighted ? ACTIVE_COLOR : AXIS_COLORS[axis];
            const f32 halfWidth = highlighted ? 2.5f : 1.5f;
            if (mode == GizmoMode::Rotate)
            {
                const Ring ring = ringPixels(camera, frame, axis);
                for (sizet i = 1; i < ring.size(); ++i)
                {
                    if (ring[i - 1] && ring[i])
                    {
                        out.push_back({*ring[i - 1], *ring[i], halfWidth, color});
                    }
                }
                continue;
            }
            const std::optional<Vec2> tip = camera.project(frame.pivot + frame.axes[axis] * frame.size);
            if (!tip || length(*tip - *pivot) < MIN_AXIS_PIXELS)
            {
                continue;
            }
            out.push_back({*pivot, *tip, halfWidth, color});
            if (mode == GizmoMode::Translate)
            {
                const Vec2 direction = normalize(*tip - *pivot);
                const Vec2 side(-direction.y, direction.x);
                out.push_back({*tip, *tip - direction * 12.0f + side * 5.0f, halfWidth, color});
                out.push_back({*tip, *tip - direction * 12.0f - side * 5.0f, halfWidth, color});
            }
            else
            {
                box(*tip, 5.0f, color, halfWidth);
            }
        }
        if (mode == GizmoMode::Scale)
        {
            const bool highlighted = active == GizmoHandle::Uniform;
            box(*pivot, 6.0f, highlighted ? ACTIVE_COLOR : UNIFORM_COLOR, highlighted ? 2.5f : 1.5f);
        }
    }

    void ghostSegments(const GizmoCamera& camera, const GizmoGhost& ghost, std::vector<Segment>& out)
    {
        const u32 color = ghost.surface ? ACTIVE_COLOR : GHOST_GROUND_COLOR;
        if (const std::optional<Vec2> at = camera.project(ghost.point))
        {
            for (const Vec2 arm : {Vec2(GHOST_CROSS_PIXELS, 0.0f), Vec2(0.0f, GHOST_CROSS_PIXELS)})
            {
                out.push_back({*at - arm, *at + arm, 1.5f, color});
            }
        }
        const Vec3 low = ghost.point + ghost.boundsMin;
        const Vec3 high = ghost.point + ghost.boundsMax;
        if (!std::isfinite(low.x + low.y + low.z + high.x + high.y + high.z) || low.x > high.x || low.y > high.y ||
            low.z > high.z)
        {
            return;
        }
        std::array<std::optional<Vec2>, 8> corners;
        for (u32 i = 0; i < 8; ++i)
        {
            corners[i] = camera.project(
                Vec3((i & 1) != 0 ? high.x : low.x, (i & 2) != 0 ? high.y : low.y, (i & 4) != 0 ? high.z : low.z));
        }
        for (u32 i = 0; i < 8; ++i)
        {
            for (const u32 bit : {1u, 2u, 4u})
            {
                if ((i & bit) == 0 && corners[i] && corners[i | bit])
                {
                    out.push_back({*corners[i], *corners[i | bit], 1.0f, color});
                }
            }
        }
    }

    void boneSegments(const GizmoCamera& camera, const std::span<const AnimatorBone> bones, std::vector<Segment>& out)
    {
        for (const AnimatorBone& bone : bones)
        {
            const std::optional<Vec2> to = camera.project(bone.to);
            if (!to)
            {
                continue;
            }
            for (const Vec2 arm : {Vec2(BONE_JOINT_PIXELS, 0.0f), Vec2(0.0f, BONE_JOINT_PIXELS)})
            {
                out.push_back({*to - arm, *to + arm, 1.0f, BONE_COLOR});
            }
            if (const std::optional<Vec2> from = camera.project(bone.from); from && bone.from != bone.to)
            {
                out.push_back({*from, *to, 1.5f, BONE_COLOR});
            }
        }
    }

    std::optional<Vec2> gizmoGrabPoint(const GizmoCamera& camera, const GizmoFrame& frame, const GizmoMode mode,
                                       const GizmoHandle handle)
    {
        const std::optional<Vec2> pivot = camera.project(frame.pivot);
        if (!pivot || handle == GizmoHandle::None || (handle == GizmoHandle::Uniform && mode != GizmoMode::Scale))
        {
            return std::nullopt;
        }
        if (handle == GizmoHandle::Uniform)
        {
            return pivot;
        }
        const u32 axis = axisIndex(handle);
        std::optional<Vec2> grab;
        if (mode == GizmoMode::Rotate)
        {
            const Ring ring = ringPixels(camera, frame, axis);
            const Ring others[2]{ringPixels(camera, frame, (axis + 1) % 3), ringPixels(camera, frame, (axis + 2) % 3)};
            f32 clearest = HIT_PIXELS;
            for (const std::optional<Vec2>& point : ring)
            {
                if (!point)
                {
                    continue;
                }
                const f32 clearance =
                    std::min(distanceToPolyline(*point, others[0]), distanceToPolyline(*point, others[1]));
                if (clearance > clearest)
                {
                    clearest = clearance;
                    grab = point;
                }
            }
        }
        else if (const std::optional<Vec2> tip = camera.project(frame.pivot + frame.axes[axis] * frame.size);
                 tip && length(*tip - *pivot) >= MIN_AXIS_PIXELS)
        {
            grab = *pivot + (*tip - *pivot) * 0.8f;
        }
        if (grab && hitGizmo(camera, frame, mode, *grab) != handle)
        {
            return std::nullopt; // another handle overlaps it
        }
        return grab;
    }

    ComponentGizmoTransforms componentGizmoTransforms(const World& world, const Entity entity,
                                                      const EntityTransformManager& transforms,
                                                      std::vector<Entity>& chain)
    {
        chain.clear();
        for (Entity at = entity; at.isValid() && world.isEntityAlive(at); at = transforms.attachedParentOf(at))
            chain.push_back(at);
        Mat4 matrix(1.0f);
        Vec3 position{0.0f}, scale{1.0f};
        Quat rotation = Quat::identity();
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            const auto* local = world.tryGetConstComponent<TransformComponent>(*it);
            if (!local)
                continue;
            matrix = multiply(matrix, trs(local->position, local->rotation, local->scale));
            position = position + rotate(rotation, scale * local->position);
            rotation = normalize(rotation * local->rotation);
            scale = scale * local->scale;
        }
        return {matrix, trs(position, rotation, Vec3{1.0f}), position};
    }

    struct EditorGizmo::State
    {
        struct Target
        {
            Entity entity;
            TransformComponent start;
            Mat4 parent{1.0f}; // the parent's world matrix at begin (identity at the root)
            Mat4 parentInverse{1.0f};
            Vec3 worldPosition;
        };

        struct Drag
        {
            GizmoHandle handle = GizmoHandle::None;
            GizmoMode mode = GizmoMode::Translate;
            GizmoFrame frame;
            GizmoCamera camera;
            Vec2 startPixel;
            f32 startParameter = 0.0f; // translate, scale: along the axis
            Vec3 startDirection; // rotate: from the pivot in the ring's plane
            std::vector<Target> targets;
            std::string_view world; // the scene view's world at begin: Play or Stop ends the drag
            std::string merge;
            Json sent; // the last move's set commands
            u32 steps = 0; // moves that wrote
        };

        State(Engine& engine, EditorModel& editorModel) :
            engine(&engine), control(&engine.control()), editorModel(&editorModel),
            settings(&EditorModelAccess::gizmo(editorModel))
        {
        }

        Engine* engine;
        ControlServer* control;
        EditorModel* editorModel;
        const GizmoSettings* settings; // the model's; valid as long as the model
        std::optional<Drag> drag;
        u64 dragSeed = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
        u64 drags = 0;

        // the targets as of the last selection or hierarchy change
        std::vector<Entity> selection;
        u64 structure = ~0ull;
        const EntityTransformManager* structureOf = nullptr; // whose structure generation (the world changes on Play)
        std::vector<Entity> targets;
        std::vector<ComponentGizmoLine> componentLines = std::vector<ComponentGizmoLine>(16384);
        std::vector<Entity> componentChain;
        u32 queryCategories = ~0u;
        u64 queryRevision = ~0ull;
        std::vector<ComponentID> componentTypes;
        std::unordered_set<Entity> componentEntities;
        std::vector<Segment> segments; // per-frame scratch
        std::vector<AnimatorBone> bones; // per-frame scratch: the selected animator's skeleton while the overlay is on
        std::vector<Entity> dragEntities; // the drag's targets' entities
        bool showing = false; // the overlay holds segments this update wrote
        std::optional<GizmoGhost> ghost;
        std::optional<Vec2> hoverPixel;

        // -- the gizmo ------------------------------------------------------------------

        std::optional<GizmoCamera> camera() const { return GizmoCamera::of(engine->render().view(SCENE_VIEW)); }

        // The live topmost entities with a transform of the selection in the scene view's world, in selection
        // order; cached per selection, hierarchy and world change.
        const std::vector<Entity>& currentTargets()
        {
            const std::span<const Entity> selected = editorModel->selection();
            if (editorModel->selectionWorld() != engine->gameWorldName() || selected.empty())
            {
                targets.clear();
                selection.clear();
                return targets;
            }
            World& world = engine->gameWorld();
            EntityTransformManager& transforms = engine->gameTransforms();
            if (!std::ranges::equal(selected, selection) || &transforms != structureOf ||
                transforms.structureGeneration() != structure)
            {
                transforms.compile();
                selection.assign(selected.begin(), selected.end());
                structure = transforms.structureGeneration();
                structureOf = &transforms;
                targets.clear();
                for (const Entity entity : selection)
                {
                    bool nested = false;
                    for (Entity parent = transforms.parentOf(entity); parent.isValid() && !nested;
                         parent = transforms.parentOf(parent))
                    {
                        nested = std::ranges::find(selection, parent) != selection.end();
                    }
                    if (!nested)
                    {
                        targets.push_back(entity);
                    }
                }
            }
            std::erase_if(targets, [&world](const Entity entity)
                          { return !world.isEntityAlive(entity) || !world.hasComponent<TransformComponent>(entity); });
            return targets;
        }

        Mat4 parentMatrix(const Entity entity) const
        {
            const Entity parent = engine->gameTransforms().parentOf(entity);
            World& world = engine->gameWorld();
            if (parent.isValid() && world.isEntityAlive(parent) && world.hasComponent<TransformMatrixComponent>(parent))
            {
                return world.getConstComponent<TransformMatrixComponent>(parent).matrix;
            }
            return Mat4(1.0f);
        }

        // the entity's world matrix from its current local transform (its own matrix lags a set by a frame)
        Mat4 worldMatrix(const Entity entity) const
        {
            const TransformComponent& local = engine->gameWorld().getConstComponent<TransformComponent>(entity);
            return multiply(parentMatrix(entity), trs(local.position, local.rotation, local.scale));
        }

        std::optional<GizmoFrame> frameOf(const std::vector<Entity>& entities, const GizmoCamera& view) const
        {
            if (entities.empty())
            {
                return std::nullopt;
            }
            GizmoFrame frame{.pivot = Vec3(0.0f, 0.0f, 0.0f),
                             .axes = {Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f)}};
            Mat4 active(1.0f);
            for (const Entity entity : entities)
            {
                active = worldMatrix(entity);
                frame.pivot = frame.pivot + column(active, 3);
            }
            frame.pivot = frame.pivot / static_cast<f32>(entities.size());
            if (settings->space == GizmoSpace::Local || settings->mode == GizmoMode::Scale)
            {
                for (u32 axis = 0; axis < 3; ++axis)
                {
                    const Vec3 direction = column(active, axis);
                    if (lengthSquared(direction) > 1e-12f)
                    {
                        frame.axes[axis] = normalize(direction);
                    }
                }
            }
            frame.size = GIZMO_PIXELS * view.worldPerPixel(frame.pivot);
            if (frame.size <= 0.0f)
            {
                return std::nullopt; // behind the eye
            }
            return frame;
        }

        void publish(const std::vector<Segment>& next)
        {
            auto overlay = engine->services().write<SceneViewOverlay>();
            if (overlay->gizmo.size() == next.size() &&
                std::memcmp(overlay->gizmo.data(), next.data(), next.size() * sizeof(Segment)) == 0)
            {
                return;
            }
            overlay->gizmo.assign(next.begin(), next.end());
            ++overlay->gizmoGeneration;
        }

        void update()
        {
            const std::optional<GizmoCamera> view = camera();
            if (drag && drag->world != engine->gameWorldName())
            {
                drag.reset(); // Play or Stop: its targets belong to the world the scene view left
                dragEntities.clear();
            }
            const bool hasSelection =
                !editorModel->selection().empty() && editorModel->selectionWorld() == engine->gameWorldName();
            const bool selected = settings->enabled && hasSelection;
            bones.clear();
            EditorModelAccess::animation(*editorModel).bones(bones);
            const bool shown = view &&
                (selected || settings->componentCategories || (hasSelection && settings->selectedComponentGizmos) ||
                 ghost || !bones.empty());
            if (!shown)
            {
                if (showing)
                {
                    showing = false;
                    publish({});
                }
                return;
            }
            showing = true;
            if (drag &&
                std::ranges::any_of(drag->targets, [this](const Target& target)
                                    { return !engine->gameWorld().isEntityAlive(target.entity); }))
            {
                drag.reset(); // a target died (undo, delete): the drag is over
                dragEntities.clear();
            }
            const std::optional<GizmoFrame> frame = frameOf(drag ? dragEntities : currentTargets(), *view);
            segments.clear();
            if (frame && selected)
            {
                const GizmoMode mode = drag ? drag->mode : settings->mode;
                const GizmoHandle lit = drag ? drag->handle
                    : hoverPixel             ? hitGizmo(*view, *frame, mode, *hoverPixel)
                                             : GizmoHandle::None;
                gizmoSegments(*view, *frame, mode, lit, segments);
            }
            if ((settings->componentCategories || (hasSelection && settings->selectedComponentGizmos)) &&
                engine->services().contains<ComponentGizmos>())
            {
                World& world = engine->gameWorld();
                auto registry = engine->services().read<ComponentGizmos>();
                if (queryRevision != registry->revision() || queryCategories != settings->componentCategories)
                {
                    queryRevision = registry->revision();
                    queryCategories = settings->componentCategories;
                    componentTypes = registry->components(settings->componentCategories);
                }
                ComponentGizmoWriter writer(componentLines);
                componentEntities.clear();
                constexpr sizet MAX_COMPONENT_ENTITIES = 4096;
                const auto drawEntity = [&](const Entity entity, const u32 categories)
                {
                    if (writer.full() || componentEntities.size() >= MAX_COMPONENT_ENTITIES ||
                        !world.isEntityAlive(entity) || !world.hasComponent<TransformComponent>(entity) ||
                        !componentEntities.insert(entity).second)
                        return;
                    const auto transform =
                        componentGizmoTransforms(world, entity, engine->gameTransforms(), componentChain);
                    registry->draw({entity, world, transform.world, transform.rigid, view->extent.x / view->extent.y,
                                    view->worldPerPixel(transform.position)},
                                   writer, categories);
                };
                if (hasSelection && settings->selectedComponentGizmos)
                {
                    constexpr u32 ALL_CATEGORIES = componentGizmoBit(ComponentGizmoCategory::Light) |
                        componentGizmoBit(ComponentGizmoCategory::Collider) |
                        componentGizmoBit(ComponentGizmoCategory::Camera) |
                        componentGizmoBit(ComponentGizmoCategory::Custom);
                    for (const Entity entity : editorModel->selection())
                        drawEntity(entity, ALL_CATEGORIES);
                }
                const ComponentID transform = getComponentId<TransformComponent>();
                for (const ComponentID component : componentTypes)
                {
                    const ComponentID ids[] = {transform, component};
                    auto builder = world.createQueryBuilder();
                    builder.with_read(std::span(ids, component == transform ? 1 : 2));
                    const auto query = builder.buildQuery();
                    for (const Entity entity : query->view<Entity>())
                    {
                        if (writer.full() || componentEntities.size() >= MAX_COMPONENT_ENTITIES)
                            break;
                        drawEntity(entity, settings->componentCategories);
                    }
                    if (writer.full() || componentEntities.size() >= MAX_COMPONENT_ENTITIES)
                        break;
                }
                for (const ComponentGizmoLine& line : writer.lines())
                {
                    Vec3 a = line.a, b = line.b;
                    const float da = -rotate(inverse(view->rotation), a - view->eye).z;
                    const float db = -rotate(inverse(view->rotation), b - view->eye).z;
                    const float near = view->nearPlane + 1.0e-4f;
                    if (da < near && db < near)
                        continue;
                    if (da < near)
                        a = a + (b - a) * ((near - da) / (db - da));
                    else if (db < near)
                        b = b + (a - b) * ((near - db) / (da - db));
                    const auto pa = view->project(a), pb = view->project(b);
                    if (pa && pb)
                        segments.push_back({*pa, *pb, line.halfWidth, line.color});
                }
            }
            boneSegments(*view, bones, segments);
            if (ghost)
            {
                ghostSegments(*view, *ghost, segments);
            }
            publish(segments);
        }

        // -- dragging -------------------------------------------------------------------

        std::string begin(const Vec2 pixel, IFormatWriter& out)
        {
            if (drag)
            {
                return std::string("a gizmo drag is in progress; end it first");
            }
            const std::optional<GizmoCamera> view = camera();
            if (!view)
            {
                return std::string("no scene view: the editor's views are off");
            }
            if (!settings->enabled)
            {
                return std::string("the gizmo is hidden: gizmoMode enabled=true shows it");
            }
            const std::vector<Entity> entities = currentTargets();
            const std::optional<GizmoFrame> frame = frameOf(entities, *view);
            if (!frame)
            {
                return std::string("no gizmo: select scene entities with a transform in front of the camera");
            }
            const GizmoHandle handle = hitGizmo(*view, *frame, settings->mode, pixel);
            if (handle == GizmoHandle::None)
            {
                return std::format("no gizmo handle at ({}, {}); the gizmo query names each handle's pixel", pixel.x,
                                   pixel.y);
            }
            Drag next{.handle = handle, .mode = settings->mode, .frame = *frame, .camera = *view, .startPixel = pixel};
            const Vec3 direction = view->ray(pixel);
            if (settings->mode == GizmoMode::Rotate)
            {
                const std::optional<Vec3> start =
                    planeDirection(frame->pivot, frame->axes[axisIndex(handle)], view->rayOrigin(pixel), direction);
                if (!start)
                {
                    return std::string("the ring is edge-on: orbit the camera to grab it");
                }
                next.startDirection = *start;
            }
            else if (isAxis(handle))
            {
                const std::optional<f32> start =
                    axisParameter(frame->pivot, frame->axes[axisIndex(handle)], view->rayOrigin(pixel), direction);
                if (!start)
                {
                    return std::string("the axis points at the camera: orbit the camera to grab it");
                }
                next.startParameter = *start;
            }
            next.world = engine->gameWorldName();
            World& world = engine->gameWorld();
            for (const Entity entity : entities)
            {
                Target target{.entity = entity, .start = world.getConstComponent<TransformComponent>(entity)};
                target.parent = parentMatrix(entity);
                target.parentInverse = inverse(target.parent);
                target.worldPosition = column(
                    multiply(target.parent, trs(target.start.position, target.start.rotation, target.start.scale)), 3);
                next.targets.push_back(target);
            }
            next.merge = std::format("gizmo.{}.{}", dragSeed, ++drags);
            drag = std::move(next);
            dragEntities.assign(entities.begin(), entities.end());
            // what a move that changes nothing computes: no step until a value differs
            drag->sent =
                commandsFor(settings->mode == GizmoMode::Scale ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 0.0f, 0.0f));
            out.writeString("handle", HANDLES[static_cast<u8>(handle)]);
            out.writeString("mode", GIZMO_MODE_NAMES[static_cast<u8>(settings->mode)]);
            out.beginArray("entities", static_cast<u32>(drag->targets.size()));
            for (const Target& target : drag->targets)
            {
                out.writeU64("", target.entity.id());
            }
            out.endArray();
            return std::string{};
        }

        // The drag's value at a pixel: translate a world offset along the axis, rotate degrees about it, scale a
        // factor; empty where the pointer's ray says nothing (parallel to the axis or the ring's plane).
        std::optional<Vec3> value(const Vec2 pixel) const
        {
            const Drag& d = *drag;
            const Vec3 direction = d.camera.ray(pixel);
            if (d.handle == GizmoHandle::Uniform)
            {
                const f32 factor = 1.0f + ((pixel.x - d.startPixel.x) - (pixel.y - d.startPixel.y)) / GIZMO_PIXELS;
                return Vec3(
                    std::max(MIN_SCALE_FACTOR, 1.0f + snapped(factor - 1.0f, settings->scaleStep, settings->snap)),
                    0.0f, 0.0f);
            }
            const Vec3& axis = d.frame.axes[axisIndex(d.handle)];
            if (d.mode == GizmoMode::Rotate)
            {
                const std::optional<Vec3> now =
                    planeDirection(d.frame.pivot, axis, d.camera.rayOrigin(pixel), direction);
                if (!now)
                {
                    return std::nullopt;
                }
                const f32 degrees =
                    toDegrees(std::atan2(dot(axis, cross(d.startDirection, *now)), dot(d.startDirection, *now)));
                return Vec3(snapped(degrees, settings->rotateStep, settings->snap), 0.0f, 0.0f);
            }
            const std::optional<f32> t = axisParameter(d.frame.pivot, axis, d.camera.rayOrigin(pixel), direction);
            if (!t)
            {
                return std::nullopt;
            }
            if (d.mode == GizmoMode::Translate)
            {
                return axis * snapped(*t - d.startParameter, settings->translateStep, settings->snap);
            }
            const f32 factor = 1.0f + (*t - d.startParameter) / d.frame.size;
            return Vec3(std::max(MIN_SCALE_FACTOR, 1.0f + snapped(factor - 1.0f, settings->scaleStep, settings->snap)),
                        0.0f, 0.0f);
        }

        // each target's set commands for a drag value, computed from its start transform
        Json commandsFor(const Vec3& amount) const
        {
            const Drag& d = *drag;
            Json commands = Json::array();
            const auto set = [&commands](const Entity entity, const char* field, const f32* values, const u32 count)
            {
                commands.push_back({{"command", "set"},
                                    {"args",
                                     {{"id", entity.id()},
                                      {"path", std::string("TransformComponent.") + field},
                                      {"value", floats(values, count)}}}});
            };
            for (const Target& target : d.targets)
            {
                TransformComponent next = target.start;
                if (d.mode == GizmoMode::Translate)
                {
                    next.position = target.start.position + multiplyVector(target.parentInverse, amount);
                    set(target.entity, "position", &next.position.x, 3);
                }
                else if (d.mode == GizmoMode::Rotate)
                {
                    const Vec3& axis = d.frame.axes[axisIndex(d.handle)];
                    const f32 radians = toRadians(amount.x);
                    // the start values at 0 degrees: a round trip through the parent inverse drifts by rounding
                    const Vec3 localAxis = multiplyVector(target.parentInverse, axis);
                    if (radians != 0.0f)
                    {
                        const Vec3 world = d.frame.pivot +
                            spite::rotate(fromAxisAngle(axis, radians), target.worldPosition - d.frame.pivot);
                        next.position = multiplyPoint(target.parentInverse, world);
                        if (lengthSquared(localAxis) > 1e-12f)
                        {
                            next.rotation = normalize(
                                multiply(fromAxisAngle(normalize(localAxis), radians), target.start.rotation));
                        }
                    }
                    set(target.entity, "position", &next.position.x, 3);
                    set(target.entity, "rotation", &next.rotation.x, 4);
                }
                else
                {
                    for (u32 axis = 0; axis < 3; ++axis)
                    {
                        if (d.handle == GizmoHandle::Uniform || axisIndex(d.handle) == axis)
                        {
                            next.scale[axis] = target.start.scale[axis] * amount.x;
                        }
                    }
                    set(target.entity, "scale", &next.scale.x, 3);
                }
            }
            return commands;
        }

        std::string move(const Vec2 pixel, IFormatWriter& out)
        {
            if (!drag)
            {
                return std::string("no gizmo drag: gizmoDrag phase=begin first");
            }
            if (drag->world != engine->gameWorldName())
            {
                drag.reset();
                dragEntities.clear();
                return std::string("Play or Stop changed the scene view's world: the drag ended");
            }
            const std::optional<Vec3> amount = value(pixel);
            out.writeBool("applied", false);
            if (!amount)
            {
                return std::string{};
            }
            Json commands = commandsFor(*amount);
            if (commands == drag->sent)
            {
                return std::string{};
            }
            Json request;
            if (commands.size() == 1)
            {
                request = {{"command", "set"}, {"args", commands[0]["args"]}};
                request["args"]["world"] = drag->world;
                request["args"]["merge"] = drag->merge;
            }
            else
            {
                request = {{"command", "batch"},
                           {"args", {{"world", drag->world}, {"merge", drag->merge}, {"commands", commands}}}};
            }
            const Json reply = Json::parse(control->handle(request.dump()), nullptr, false);
            if (!reply.is_object() || !reply.value("ok", false))
            {
                drag.reset();
                dragEntities.clear();
                return "the drag's edit failed, the drag ended: " + reply.dump();
            }
            drag->sent = std::move(commands);
            ++drag->steps;
            out.writeBool("applied", true);
            if (drag->mode == GizmoMode::Translate)
            {
                out.writeFloats("offset", std::span<const f32>(&amount->x, 3));
            }
            else
            {
                out.writeF32(drag->mode == GizmoMode::Rotate ? "degrees" : "factor", amount->x);
            }
            return std::string{};
        }

        std::string end(IFormatWriter& out)
        {
            if (!drag)
            {
                return std::string("no gizmo drag to end");
            }
            out.writeBool("edited", drag->steps > 0);
            drag.reset();
            dragEntities.clear();
            return std::string{};
        }

        // -- control --------------------------------------------------------------------

        // the gizmo query after the settings the model wrote
        void writeView(IFormatWriter& out)
        {
            const std::optional<GizmoCamera> view = camera();
            const std::optional<GizmoFrame> frame =
                view && settings->enabled ? frameOf(drag ? dragEntities : currentTargets(), *view) : std::nullopt;
            out.writeBool("visible", frame.has_value());
            if (frame)
            {
                out.writeFloats("pivot", std::span<const f32>(&frame->pivot.x, 3));
                out.beginArray("axes", 3);
                for (const Vec3& axis : frame->axes)
                {
                    out.writeFloats("", std::span<const f32>(&axis.x, 3));
                }
                out.endArray();
                out.writeF32("size", frame->size);
                out.beginObject("handles");
                for (u8 handle = static_cast<u8>(GizmoHandle::X); handle < HANDLES.size(); ++handle)
                {
                    if (const std::optional<Vec2> grab =
                            gizmoGrabPoint(*view, *frame, settings->mode, static_cast<GizmoHandle>(handle)))
                    {
                        out.writeFloats(HANDLES[handle], std::span<const f32>(&grab->x, 2));
                    }
                }
                out.endObject();
            }
            out.beginObject("drag");
            out.writeBool("active", drag.has_value());
            if (drag)
            {
                out.writeString("handle", HANDLES[static_cast<u8>(drag->handle)]);
                out.writeString("merge", drag->merge);
                out.writeU32("moves", drag->steps);
            }
            out.endObject();
        }

        void registerCommands()
        {
            control->registerViewCommand(
                "gizmoDrag", OWNER,
                "drags a transform gizmo handle in the scene view: begin grabs the handle at (x, y), each move "
                "edits the selection's transforms (one undo step per drag), end finishes",
                {requiredArg("phase", ArgKind::String, "begin, move or end", DRAG_PHASES),
                 optionalArg("x", ArgKind::Number, "scene view pixel column (begin, move)"),
                 optionalArg("y", ArgKind::Number, "scene view pixel row (begin, move)")},
                [this](const ControlArgs& args, IFormatWriter& out)
                {
                    const std::string phase = args.string("phase").value_or(std::string{});
                    if (phase == "end")
                    {
                        return end(out);
                    }
                    const auto number = [&args](const std::string_view key) -> std::optional<f32>
                    {
                        const Json value = Json::parse(args.json(key).value_or("null"), nullptr, false);
                        return value.is_number() ? std::optional(value.get<f32>()) : std::nullopt;
                    };
                    const std::optional<f32> x = number("x");
                    const std::optional<f32> y = number("y");
                    if (!x || !y)
                    {
                        return std::string("gizmoDrag begin and move need x and y: a scene view pixel");
                    }
                    return phase == "begin" ? begin(Vec2(*x, *y), out) : move(Vec2(*x, *y), out);
                });
        }
    };

    EditorGizmo::EditorGizmo(Engine& engine, EditorModel& editorModel) :
        m_state(std::make_unique<State>(engine, editorModel))
    {
        State& state = *m_state;
        state.registerCommands();
        EditorModelAccess::setGizmoView(
            editorModel,
            {[&state] { return state.drag.has_value(); }, [&state](IFormatWriter& out) { state.writeView(out); }});
    }

    EditorGizmo::~EditorGizmo()
    {
        EditorModelAccess::setGizmoView(*m_state->editorModel, {});
        m_state->control->unregisterOwner(OWNER);
        auto overlay = m_state->engine->services().write<SceneViewOverlay>();
        overlay->gizmo.clear();
        ++overlay->gizmoGeneration;
    }

    GizmoHandle EditorGizmo::handleAt(const Vec2 viewPixel) const
    {
        State& state = *m_state;
        const std::optional<GizmoCamera> view = state.camera();
        if (!view || state.drag || !state.settings->enabled)
        {
            return GizmoHandle::None;
        }
        const std::optional<GizmoFrame> frame = state.frameOf(state.currentTargets(), *view);
        return frame ? hitGizmo(*view, *frame, state.settings->mode, viewPixel) : GizmoHandle::None;
    }

    void EditorGizmo::setHover(const std::optional<Vec2> viewPixel) { m_state->hoverPixel = viewPixel; }

    void EditorGizmo::setGhost(const std::optional<GizmoGhost>& ghost) { m_state->ghost = ghost; }

    bool EditorGizmo::componentCategoryEnabled(const ComponentGizmoCategory category) const
    {
        return (m_state->settings->componentCategories & componentGizmoBit(category)) != 0;
    }

    void EditorGizmo::setComponentCategoryEnabled(const ComponentGizmoCategory category, const bool enabled)
    {
        const u32 bit = componentGizmoBit(category);
        auto& categories = EditorModelAccess::mutableGizmo(*m_state->editorModel).componentCategories;
        categories = enabled ? categories | bit : categories & ~bit;
    }

    bool EditorGizmo::selectedComponentGizmosEnabled() const { return m_state->settings->selectedComponentGizmos; }

    void EditorGizmo::setSelectedComponentGizmosEnabled(const bool enabled)
    {
        EditorModelAccess::mutableGizmo(*m_state->editorModel).selectedComponentGizmos = enabled;
    }

    void EditorGizmo::update() { m_state->update(); }
} // namespace spite
