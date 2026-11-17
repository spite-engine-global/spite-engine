#include "EditorSceneView.hpp"
#include "EditorContext.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "EditorGizmo.hpp"
#include "editor/EditorModel.hpp"
#include "EditorSettings.hpp"
#include "base/FlyBoost.hpp"
#include "base/ContentRoot.hpp"
#include "base/io/FileSystem.hpp"
#include "application/input/InputSystem.hpp"
#include "asset/LoadBakedFunctions.hpp"
#include "assetruntime/AssetRuntime.hpp"
#include "assetruntime/RenderStateComponents.hpp"
#include "base/Logging.hpp"
#include "control/ControlServer.hpp"
#include "ecs/Chunk.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "engine/Engine.hpp"
#include "rendering/IRenderer.hpp"
#include "rendering/RenderDeviceCapabilities.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/SceneViewOverlay.hpp"
#include "renderruntime/RenderRuntime.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiDragDrop.hpp"
#include "ui/UiEvents.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiWidgets.hpp"
#include "world/EntityTransformManager.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        constexpr std::string_view OWNER = "editorSceneView";
        constexpr float MAX_PITCH = 89.0f;
        constexpr float MIN_DISTANCE = 0.05f;
        // a pick whose readback never came back (a renderpipeline reload dropped it) fails after this many frames
        constexpr u64 PICK_TIMEOUT_FRAMES = 120;
        // gesture scales: degrees per UI pixel, the camera distance per pixel or wheel notch, distance per second
        constexpr float DEGREES_PER_PIXEL = 0.25f;
        constexpr float PAN_PER_PIXEL = 0.0015f;
        constexpr float DOLLY_PER_NOTCH = 0.15f;
        // Frame: a bound smaller than this (a lone point) frames as a sphere of it, and the fit leaves a margin
        constexpr float MIN_FRAME_RADIUS = 0.5f;
        constexpr float FRAME_MARGIN = 1.1f;
        constexpr std::array<std::string_view, 2> FRAME_TARGETS{"selection", "all"};
        // the views of SCENE_VIEW_MODES: the first two are free cameras, the others axis views with their yaw and pitch
        constexpr u32 ORTHOGRAPHIC_VIEW = 1;
        constexpr u32 FIRST_AXIS_VIEW = 2;
        constexpr std::array<Vec2, 8> VIEW_DIRECTIONS{Vec2{},
                                                      Vec2{},
                                                      Vec2{0.0f, -90.0f},
                                                      Vec2{0.0f, 90.0f},
                                                      Vec2{0.0f, 0.0f},
                                                      Vec2{180.0f, 0.0f},
                                                      Vec2{90.0f, 0.0f},
                                                      Vec2{-90.0f, 0.0f}};
        // an orthographic eye stands near + the reach behind the pivot (the reach is half the clip depth when that is
        // shorter), whatever the zoom: content up to the reach nearer than the pivot is drawn, the pivot is never in
        // the near clip, and with the default near it stays within the sun shadows' reach (shadows.maxDistance, 100)
        constexpr float ORTHOGRAPHIC_REACH = 80.0f;

        enum class PickMode : u8
        {
            Replace,
            Add,
            Toggle,
        };
        constexpr std::array<std::string_view, 3> PICK_MODES{"replace", "add", "toggle"};
        // a ray whose y is smaller runs along the ground (y = 0): no point to drop on
        constexpr float GROUND_PARALLEL = 1.0e-4f;

        // where the ray (a unit direction) enters the box, a distance of at least minDistance; a miss, a box the ray
        // starts inside or has passed, and a non-finite box are empty
        std::optional<float> rayBoxDistance(const Vec3& origin, const Vec3& direction, const Vec3& low,
                                            const Vec3& high, const float minDistance)
        {
            if (!std::isfinite(low.x + low.y + low.z + high.x + high.y + high.z))
            {
                return std::nullopt;
            }
            const float o[3]{origin.x, origin.y, origin.z};
            const float d[3]{direction.x, direction.y, direction.z};
            const float lo[3]{low.x, low.y, low.z};
            const float hi[3]{high.x, high.y, high.z};
            float enter = -1.0e30f;
            float leave = 1.0e30f;
            for (u32 axis = 0; axis < 3; ++axis)
            {
                if (std::abs(d[axis]) < 1.0e-8f)
                {
                    if (o[axis] < lo[axis] || o[axis] > hi[axis])
                    {
                        return std::nullopt; // parallel to this slab and outside it
                    }
                    continue;
                }
                const float t0 = (lo[axis] - o[axis]) / d[axis];
                const float t1 = (hi[axis] - o[axis]) / d[axis];
                enter = std::max(enter, std::min(t0, t1));
                leave = std::min(leave, std::max(t0, t1));
                if (enter > leave)
                {
                    return std::nullopt;
                }
            }
            return enter >= minDistance ? std::optional(enter) : std::nullopt;
        }

        // where the ray meets y = 0 within [minDistance, maxDistance]; empty with the reason otherwise
        std::optional<float> rayGroundDistance(const Vec3& origin, const Vec3& direction, const float minDistance,
                                               const float maxDistance, std::string& why)
        {
            if (std::abs(direction.y) < GROUND_PARALLEL)
            {
                why = "the view looks along the ground (a ray parallel to y = 0)";
                return std::nullopt;
            }
            const float t = -origin.y / direction.y;
            if (!std::isfinite(t) || t < minDistance)
            {
                why = "the ground (y = 0) lies behind the camera";
                return std::nullopt;
            }
            if (t > maxDistance)
            {
                why = "the ground (y = 0) lies beyond the far plane";
                return std::nullopt;
            }
            return t;
        }

        // every stored field and the eye the fields place: finite inputs can still overflow (a pivot at -3e38 with a
        // position at 3e38, a distance added to a far pivot)
        float orthographicHeightOf(const EditorCamera& pose)
        {
            return 2.0f * pose.distance * std::tan(toRadians(pose.fov) * 0.5f);
        }

        Vec3 orthographicEyeOf(const EditorCamera& pose)
        {
            const float reach = std::min(ORTHOGRAPHIC_REACH, 0.5f * (pose.farPlane - pose.nearPlane));
            return pose.pivot - pose.forward() * (pose.nearPlane + reach);
        }

        // an orthographic view also derives its height and the eye pulled back from the pivot (distance 1e38 at fov 179
        // overflows the height with every stored field finite)
        bool poseFinite(const EditorCamera& pose, const bool orthographic)
        {
            const Vec3 eye = pose.position();
            const Vec3 orthoEye = orthographic ? orthographicEyeOf(pose) : eye;
            const float height = orthographic ? orthographicHeightOf(pose) : 1.0f;
            for (const float value :
                 {pose.pivot.x, pose.pivot.y, pose.pivot.z, pose.yaw, pose.pitch, pose.distance, pose.fov,
                  pose.nearPlane, pose.farPlane, eye.x, eye.y, eye.z, orthoEye.x, orthoEye.y, orthoEye.z, height})
            {
                if (!std::isfinite(value))
                {
                    return false;
                }
            }
            return true;
        }

        // a JSON number that is a finite float (1e39 is a finite double and an infinite float)
        bool finiteNumber(const Json& value) { return value.is_number() && std::isfinite(value.get<float>()); }

        std::optional<Vec3> vec3(const ControlArgs& args, const std::string_view key, std::string& error)
        {
            const std::optional<std::string> text = args.json(key);
            if (!text)
            {
                return std::nullopt;
            }
            const Json value = Json::parse(*text, nullptr, false);
            if (!value.is_array() || value.size() != 3 || !std::ranges::all_of(value, finiteNumber))
            {
                error = std::format("{} is an array of 3 finite numbers", key);
                return std::nullopt;
            }
            return Vec3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
        }

        std::optional<Vec2> vec2(const ControlArgs& args, const std::string_view key, std::string& error)
        {
            const std::optional<std::string> text = args.json(key);
            if (!text)
            {
                return std::nullopt;
            }
            const Json value = Json::parse(*text, nullptr, false);
            if (!value.is_array() || value.size() != 2 || !std::ranges::all_of(value, finiteNumber))
            {
                error = std::format("{} is an array of 2 finite numbers", key);
                return std::nullopt;
            }
            return Vec2(value[0].get<float>(), value[1].get<float>());
        }

        std::optional<float> number(const ControlArgs& args, const std::string_view key)
        {
            const std::optional<std::string> text = args.json(key);
            if (!text)
            {
                return std::nullopt;
            }
            const Json value = Json::parse(*text, nullptr, false);
            return finiteNumber(value) ? std::optional(value.get<float>()) : std::nullopt;
        }
    } // namespace

    Quat EditorCamera::rotation() const
    {
        return normalize(fromAxisAngle(Vec3(0.0f, 1.0f, 0.0f), toRadians(yaw)) *
                         fromAxisAngle(Vec3(1.0f, 0.0f, 0.0f), toRadians(pitch)));
    }

    Vec3 EditorCamera::forward() const { return rotate(rotation(), Vec3(0.0f, 0.0f, -1.0f)); }

    void EditorCamera::orbit(const float yawDegrees, const float pitchDegrees)
    {
        yaw = std::remainder(yaw + yawDegrees, 360.0f);
        pitch = clamp(pitch + pitchDegrees, -MAX_PITCH, MAX_PITCH);
    }

    void EditorCamera::look(const float yawDegrees, const float pitchDegrees)
    {
        const Vec3 eye = position();
        orbit(yawDegrees, pitchDegrees);
        pivot = eye + forward() * distance;
    }

    void EditorCamera::pan(const float right, const float up)
    {
        const Quat turn = rotation();
        pivot = pivot + rotate(turn, Vec3(1.0f, 0.0f, 0.0f)) * right + rotate(turn, Vec3(0.0f, 1.0f, 0.0f)) * up;
    }

    void EditorCamera::fly(const Vec3& rightUpForward)
    {
        pan(rightUpForward.x, rightUpForward.y);
        pivot = pivot + forward() * rightUpForward.z;
    }

    void EditorCamera::dolly(const float amount) { distance = std::max(MIN_DISTANCE, distance - amount); }

    struct EditorSceneView::State
    {
        struct PendingPick
        {
            u64 id = 0;
            PickMode mode = PickMode::Replace;
            u32 x = 0;
            u32 y = 0;
            u64 frame = 0; // when it was requested
            u64 slotEpoch = 0; // InstanceSlots::epoch of the frame the click saw
            u32 scene = 0; // the render scene the scene view drew at the click
            u32 x2 = 0;
            u32 y2 = 0;
            bool rectangle = false;
            u64 cameraRevision = 0;
            u64 gestureRevision = 0;
            Extent2D extent{};
            std::vector<Entity> selection;
            std::string selectionWorld;
            DeferredReply reply;
        };

        struct LastPick
        {
            u64 id = 0;
            u32 x = 0;
            u32 y = 0;
            u32 object = SceneViewOverlay::NO_OBJECT;
            Entity entity = Entity::undefined();
            bool rectangle = false;
            u32 x2 = 0;
            u32 y2 = 0;
            u32 hitCount = 0;
        };

        State(Engine& engine, const EditorModel& editorModel, EditorGizmo& gizmo, const Image& image,
              const EditorSettings& editorSettings) :
            engine(&engine), control(&engine.control()), editorModel(&editorModel), gizmo(&gizmo), image(image),
            settings(editorSettings)
        {
        }

        Engine* engine;
        ControlServer* control;
        const EditorModel* editorModel;
        EditorGizmo* gizmo;
        Image image;
        InputSystem* input = nullptr;
        EditorCamera camera;
        u64 revision = 0; // moves with every change of camera
        u32 mode = 0; // the camera's SCENE_VIEW_MODES entry
        std::optional<EditorCamera> perspectiveHome; // the perspective camera while the view is anything else
        bool poseDirty = true;

        std::vector<std::string> pending; // request lines update() dispatches
        u64 frame = 0;
        u64 nextPick = 0;
        std::vector<PendingPick> picks;
        LastPick lastPick;

        // gestures: modifiers of the primary press on the image, raw-input drags begun on it
        u8 pressModifiers = 0;
        bool gizmoPress = false; // the last primary press grabbed a gizmo handle (its click is not a pick)
        bool gizmoDragging = false; // its end is not queued yet
        bool marqueeDragging = false;
        bool marqueeReleased = false;
        bool marqueePressValid = false;
        Vec2 marqueeStart{};
        Vec2 marqueeEnd{};
        Extent2D marqueeExtent{};
        u64 gestureRevision = 0;
        bool looking = false;
        bool panning = false;
        std::chrono::steady_clock::time_point lastFly{};
        FlyBoost flyBoost;
        const EditorSettings& settings;

        // the outline: the shown world's selected renderables (with descendants) as of a selection, structure or
        // world change
        std::vector<Entity> outlineSelection;
        const EntityTransformManager* outlineTransforms = nullptr;
        u64 outlineStructure = ~0ull;
        std::vector<Entity> renderables;
        std::vector<u32> slots; // per-frame scratch
        bool outlining = false;
        bool outlineEnabled = true; // the Scene toolbar's Overlays: false draws no outline
        u64 highlightBatchRevision = 0;
        std::chrono::steady_clock::time_point highlightUntil{};
        std::vector<Entity> highlightedEntities;
        bool highlightDrawn = false;
        static constexpr u32 CAPTURE_VIEW = 6;
        std::shared_ptr<bool> captureAlive = std::make_shared<bool>(true);
        bool captureBusy = false;
        DeferredReply captureReply;
        u64 captureTicket = 0;

        // -- camera --------------------------------------------------------------------

        void applyPose()
        {
            if (!poseDirty)
            {
                return;
            }
            poseDirty = false;
            CameraPose pose;
            pose.transform.position = camera.position();
            pose.transform.rotation = camera.rotation();
            pose.camera.fov = toRadians(camera.fov);
            pose.camera.nearPlane = camera.nearPlane;
            pose.camera.farPlane = camera.farPlane;
            if (camera.orthographic)
            {
                pose.orthographicHeight = orthographicHeightOf(camera);
                pose.transform.position = orthographicEyeOf(camera);
            }
            engine->render().view(SCENE_VIEW).pose = pose;
        }

        void writeCamera(IFormatWriter& out) const
        {
            out.beginObject("camera");
            writeValue(EditorCamera::spiteSchema(), &camera, out, FieldSet::All);
            out.writeString("view", SCENE_VIEW_MODES[mode]);
            const Vec3 position = camera.position();
            out.writeFloats("position", std::span<const f32>(&position.x, 3));
            out.endObject();
        }

        // the box Frame fits: bounds of renderables, the position of anything else with a transform
        struct FrameBounds
        {
            Vec3 lo{1.0e30f};
            Vec3 hi{-1.0e30f};
            bool any = false;

            void add(const Vec3& low, const Vec3& high)
            {
                lo = min(lo, low);
                hi = max(hi, high);
                any = true;
            }

            void add(World& world, const Entity entity)
            {
                if (!world.isEntityAlive(entity))
                {
                    return;
                }
                if (world.hasComponent<WorldBoundingBoxComponent>(entity))
                {
                    const auto& box = world.getConstComponent<WorldBoundingBoxComponent>(entity);
                    add(box.min, box.max);
                }
                else if (world.hasComponent<TransformMatrixComponent>(entity))
                {
                    const Mat4& matrix = world.getConstComponent<TransformMatrixComponent>(entity).matrix;
                    add(Vec3(matrix[3][0], matrix[3][1], matrix[3][2]), Vec3(matrix[3][0], matrix[3][1], matrix[3][2]));
                }
            }
        };

        // Points `next` at what `target` names: the pivot to the middle of its bounds and the distance to fit them in
        // the view's narrower angle, the direction kept. An empty message on success.
        std::string frameCamera(const std::string_view target, EditorCamera& next, std::optional<Extent2D> size = {})
        {
            World& world = engine->gameWorld();
            FrameBounds bounds;
            if (target == FRAME_TARGETS[0])
            {
                const std::span<const Entity> selected = editorModel->selection();
                if (editorModel->selectionWorld() != engine->gameWorldName() || selected.empty())
                {
                    return std::format("nothing is selected in the {} world; frame=all frames everything",
                                       engine->gameWorldName());
                }
                EntityTransformManager& transforms = engine->gameTransforms();
                transforms.compile();
                for (const Entity entity : selected)
                {
                    bounds.add(world, entity);
                    for (const Entity descendant : transforms.subtreeOf(entity))
                    {
                        bounds.add(world, descendant);
                    }
                }
            }
            else
            {
                for (const ComponentID id :
                     {getComponentId<WorldBoundingBoxComponent>(), getComponentId<TransformMatrixComponent>()})
                {
                    for (const Chunk* chunk : world.getChunksContaining(Aspect(id)))
                    {
                        for (const Entity entity : chunk->entities())
                        {
                            bounds.add(world, entity);
                        }
                    }
                    if (bounds.any)
                    {
                        break;
                    }
                }
            }
            if (!bounds.any)
            {
                return std::format("the {} world has nothing to frame", engine->gameWorldName());
            }
            const Extent2D extent = size.value_or(engine->render().view(SCENE_VIEW).extent);
            const float aspect =
                extent.width > 0 && extent.height > 0 ? static_cast<float>(extent.width) / extent.height : 16.0f / 9.0f;
            const float halfVertical = toRadians(next.fov) * 0.5f;
            const float halfAngle = std::min(halfVertical, std::atan(std::tan(halfVertical) * aspect));
            const float radius = std::max(length(bounds.hi - bounds.lo) * 0.5f, MIN_FRAME_RADIUS);
            next.pivot = (bounds.lo + bounds.hi) * 0.5f;
            next.distance = std::max(MIN_DISTANCE, radius / std::sin(halfAngle) * FRAME_MARGIN);
            return std::string{};
        }

        std::string setCamera(const ControlArgs& args, IFormatWriter& out)
        {
            std::string error;
            for (const char* key : {"yaw", "pitch", "distance", "fov", "near", "far", "dolly"})
            {
                if (args.json(key) && !number(args, key))
                {
                    return std::format("{} is a finite number", key);
                }
            }
            EditorCamera next = camera;
            u32 nextMode = mode;
            std::optional<EditorCamera> home = perspectiveHome;
            const std::optional<Vec3> pivot = vec3(args, "pivot", error);
            const std::optional<Vec3> position = vec3(args, "position", error);
            const std::optional<Vec2> orbit = vec2(args, "orbit", error);
            const std::optional<Vec2> look = vec2(args, "look", error);
            const std::optional<Vec2> pan = vec2(args, "pan", error);
            const std::optional<Vec3> fly = vec3(args, "fly", error);
            if (!error.empty())
            {
                return error;
            }
            if (const std::optional<std::string> view = args.string("view"))
            {
                const u32 target =
                    static_cast<u32>(std::ranges::find(SCENE_VIEW_MODES, *view) - SCENE_VIEW_MODES.begin());
                if (target == 0 && mode != 0)
                {
                    next = *home;
                    home.reset();
                }
                else if (target != 0 && mode == 0)
                {
                    home = camera;
                }
                if (target >= FIRST_AXIS_VIEW)
                {
                    next.yaw = VIEW_DIRECTIONS[target].x;
                    next.pitch = VIEW_DIRECTIONS[target].y;
                }
                nextMode = target;
            }
            for (const auto& [key, field] : {std::pair{"yaw", &next.yaw},
                                             {"pitch", &next.pitch},
                                             {"distance", &next.distance},
                                             {"fov", &next.fov},
                                             {"near", &next.nearPlane},
                                             {"far", &next.farPlane}})
            {
                if (const std::optional<float> value = number(args, key))
                {
                    *field = *value;
                }
            }
            if (number(args, "pitch"))
            {
                next.pitch = clamp(next.pitch, -MAX_PITCH, MAX_PITCH);
            }
            if (pivot)
            {
                next.pivot = *pivot;
            }
            if (position && pivot)
            {
                // look from position at pivot
                const Vec3 toPivot = *pivot - *position;
                next.distance = length(toPivot);
                if (next.distance < MIN_DISTANCE)
                {
                    return std::string("position and pivot coincide");
                }
                const Vec3 direction = toPivot / next.distance;
                next.yaw = toDegrees(std::atan2(-direction.x, -direction.z));
                next.pitch = clamp(toDegrees(std::asin(clamp(direction.y, -1.0f, 1.0f))), -MAX_PITCH, MAX_PITCH);
            }
            else if (position)
            {
                next.pivot = *position + next.forward() * next.distance;
            }
            if (orbit)
            {
                next.orbit(orbit->x, orbit->y);
            }
            if (look)
            {
                next.look(look->x, look->y);
            }
            if (pan)
            {
                next.pan(pan->x, pan->y);
            }
            if (fly)
            {
                next.fly(*fly);
            }
            if (const std::optional<float> dolly = number(args, "dolly"))
            {
                next.dolly(*dolly);
            }
            if (const std::optional<std::string> target = args.string("frame"))
            {
                if (std::string failed = frameCamera(*target, next); !failed.empty())
                {
                    return failed;
                }
            }
            if (!poseFinite(next, nextMode != 0))
            {
                return std::string("the camera pose would not be finite (a position, distance or delta is too large, "
                                   "or an orthographic height)");
            }
            if (!(next.distance >= MIN_DISTANCE) || !(next.fov >= 1.0f && next.fov <= 179.0f) ||
                !(next.nearPlane > 0.0f && next.farPlane > next.nearPlane))
            {
                return std::string("distance >= 0.05, 1 <= fov <= 179 and 0 < near < far");
            }
            if (nextMode >= FIRST_AXIS_VIEW &&
                (std::abs(std::remainder(next.yaw - VIEW_DIRECTIONS[nextMode].x, 360.0f)) > 1.0e-3f ||
                 std::abs(next.pitch - VIEW_DIRECTIONS[nextMode].y) > 1.0e-3f))
            {
                nextMode = ORTHOGRAPHIC_VIEW; // an axis view turned is the free orthographic one
            }
            next.orthographic = nextMode != 0;
            camera = next;
            mode = nextMode;
            perspectiveHome = home;
            ++revision;
            poseDirty = true;
            applyPose();
            writeCamera(out);
            return std::string{};
        }

        // -- picking -------------------------------------------------------------------

        std::string pick(const ControlArgs& args, IFormatWriter& out)
        {
            const std::optional<float> x = number(args, "x");
            const std::optional<float> y = number(args, "y");
            if (!x || !y || *x < 0.0f || *y < 0.0f)
            {
                return std::string("pick needs x and y: a scene view pixel");
            }
            PickMode mode = PickMode::Replace;
            if (const std::optional<std::string> name = args.string("mode"))
            {
                mode = static_cast<PickMode>(std::ranges::find(PICK_MODES, *name) - PICK_MODES.begin());
            }
            ServiceLocator& services = engine->services();
            const Extent2D extent = engine->render().view(SCENE_VIEW).extent;
            if (extent.width > 0 && (*x >= static_cast<float>(extent.width) || *y >= static_cast<float>(extent.height)))
            {
                return std::format("({}, {}) lies outside the {}x{} scene view", *x, *y, extent.width, extent.height);
            }
            PendingPick request{.id = ++nextPick,
                                .mode = mode,
                                .x = static_cast<u32>(*x),
                                .y = static_cast<u32>(*y),
                                .frame = frame,
                                .slotEpoch = engine->render().sceneGpuStream().instanceSlots().epoch(),
                                .scene = engine->render().view(SCENE_VIEW).scene};
            services.write<SceneViewOverlay>()->picks.push_back(
                {request.id, request.x, request.y, request.x, request.y});
            request.reply = control->deferReply();
            if (!request.reply.isValid())
            {
                // a GUI click: nobody waits for the result, the selection shows it
                out.writeU64("pick", request.id);
                out.writeBool("queued", true);
            }
            picks.push_back(request);
            return std::string{};
        }

        std::string pickRect(const ControlArgs& args, IFormatWriter& out)
        {
            const auto x = number(args, "x");
            const auto y = number(args, "y");
            const auto x2 = number(args, "x2");
            const auto y2 = number(args, "y2");
            const Extent2D extent = engine->render().view(SCENE_VIEW).extent;
            if (!x || !y || !x2 || !y2 || !extent.width || !extent.height || *x < 0 || *y < 0 || *x2 < 0 || *y2 < 0 ||
                *x >= extent.width || *x2 >= extent.width || *y >= extent.height || *y2 >= extent.height)
                return "pickRect needs two points inside the scene view";
            PickMode mode = PickMode::Replace;
            if (const auto name = args.string("mode"))
                mode = static_cast<PickMode>(std::ranges::find(PICK_MODES, *name) - PICK_MODES.begin());
            PendingPick request{
                .id = ++nextPick,
                .mode = mode,
                .x = static_cast<u32>(std::min(*x, *x2)),
                .y = static_cast<u32>(std::min(*y, *y2)),
                .frame = frame,
                .slotEpoch = engine->render().sceneGpuStream().instanceSlots().epoch(),
                .scene = engine->render().view(SCENE_VIEW).scene,
                .x2 = static_cast<u32>(std::max(*x, *x2)),
                .y2 = static_cast<u32>(std::max(*y, *y2)),
                .rectangle = true,
                .cameraRevision = revision,
                .gestureRevision = gestureRevision,
                .extent = extent,
                .selection = std::vector<Entity>(editorModel->selection().begin(), editorModel->selection().end()),
                .selectionWorld = std::string(editorModel->selectionWorld())};
            engine->services().write<SceneViewOverlay>()->picks.push_back(
                {request.id, request.x, request.y, request.x2, request.y2,
                 std::max(1u, engine->render().sceneGpuStream().instanceSlots().capacity())});
            request.reply = control->deferReply();
            if (!request.reply.isValid())
            {
                out.writeU64("pick", request.id);
                out.writeBool("queued", true);
            }
            picks.push_back(std::move(request));
            return {};
        }

        // The entity the visibility buffer's object id names: the slot's owner in the world the scene view draws,
        // unless it was granted the slot after the frame the click saw (the slot was freed and reused while the pick
        // was in flight).
        Entity resolve(const PendingPick& request, const u32 object) const
        {
            if (object == SceneViewOverlay::NO_OBJECT)
            {
                return Entity::undefined();
            }
            const InstanceSlots& slots = engine->render().sceneGpuStream().instanceSlots();
            const Entity owner = slots.ownerIn(object, request.scene);
            World& world = engine->gameWorld();
            if (!owner.isValid() || slots.grantEpoch(object) > request.slotEpoch || !world.isEntityAlive(owner) ||
                !world.hasComponent<GpuInstanceSlotComponent>(owner) ||
                world.getConstComponent<GpuInstanceSlotComponent>(owner).slot != object)
            {
                return Entity::undefined();
            }
            return owner;
        }

        // the selection the mode makes of the inspector's (its selection in the shown world; none from another)
        std::vector<Entity> selectionAfter(const PickMode mode, const Entity hit) const
        {
            std::vector<Entity> selection;
            if (editorModel->selectionWorld() == engine->gameWorldName())
            {
                selection.assign(editorModel->selection().begin(), editorModel->selection().end());
            }
            switch (mode)
            {
            case PickMode::Replace:
                selection.clear();
                if (hit.isValid())
                {
                    selection.push_back(hit);
                }
                break;
            case PickMode::Add:
                if (hit.isValid() && std::ranges::find(selection, hit) == selection.end())
                {
                    selection.push_back(hit);
                }
                break;
            case PickMode::Toggle:
                if (hit.isValid() && std::erase(selection, hit) == 0)
                {
                    selection.push_back(hit);
                }
                break;
            }
            return selection;
        }

        void finishPick(const PendingPick& request, const SceneViewOverlay::PickResult& result)
        {
            const u32 object = result.object;
            if (request.rectangle &&
                (request.id != nextPick || request.cameraRevision != revision ||
                 request.gestureRevision != gestureRevision ||
                 request.extent.width != engine->render().view(SCENE_VIEW).extent.width ||
                 request.extent.height != engine->render().view(SCENE_VIEW).extent.height ||
                 request.selectionWorld != editorModel->selectionWorld() ||
                 !std::ranges::equal(request.selection, editorModel->selection())))
            {
                if (request.reply.isValid())
                    control->completeReply(request.reply,
                                           [](IFormatWriter&) { return std::string("pick rectangle became stale"); });
                return;
            }
            if (engine->render().view(SCENE_VIEW).scene != request.scene)
            {
                // Play or Stop rebound the scene view since the click: the object id names another world's slot
                lastPick = {request.id, request.x, request.y, object, Entity::undefined()};
                if (request.reply.isValid())
                {
                    control->completeReply(request.reply,
                                           [](IFormatWriter&)
                                           {
                                               return std::string("Play or Stop changed the scene view's world "
                                                                  "before the pick read back");
                                           });
                }
                return;
            }
            const Entity hit = request.rectangle ? Entity::undefined() : resolve(request, object);
            lastPick = {request.id, request.x, request.y, object, hit};
            std::vector<Entity> selection = selectionAfter(request.mode, hit);
            std::vector<Entity> hits;
            if (request.rectangle)
            {
                for (const u32 slot : result.objects)
                {
                    const Entity entity = resolve(request, slot);
                    if (entity.isValid())
                        hits.push_back(entity);
                }
                std::ranges::sort(hits, {}, &Entity::id);
                hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
                if (request.mode == PickMode::Replace)
                    selection = hits;
                else if (request.mode == PickMode::Add)
                {
                    std::unordered_set<u64> existing;
                    for (const Entity entity : selection)
                        existing.insert(entity.id());
                    for (const Entity entity : hits)
                        if (existing.insert(entity.id()).second)
                            selection.push_back(entity);
                }
                else
                {
                    std::unordered_set<u64> existing;
                    for (const Entity entity : selection)
                        existing.insert(entity.id());
                    std::unordered_set<u64> hitIds;
                    for (const Entity entity : hits)
                        hitIds.insert(entity.id());
                    std::erase_if(selection, [&](const Entity entity) { return hitIds.contains(entity.id()); });
                    for (const Entity entity : hits)
                        if (!existing.contains(entity.id()))
                            selection.push_back(entity);
                }
            }
            lastPick.rectangle = request.rectangle;
            lastPick.x2 = request.x2;
            lastPick.y2 = request.y2;
            lastPick.hitCount = static_cast<u32>(hits.size());
            Json ids = Json::array();
            for (const Entity entity : selection)
            {
                ids.push_back(entity.id());
            }
            const std::string reply = control->handle(
                Json{{"command", "select"}, {"args", {{"world", engine->gameWorldName()}, {"ids", ids}}}}.dump());
            const bool selected = Json::parse(reply, nullptr, false).value("ok", false);
            if (!selected)
            {
                SWARNING_LOG("scene view: pick -> {}", reply);
            }
            if (request.reply.isValid())
            {
                control->completeReply(request.reply,
                                       [&](IFormatWriter& out)
                                       {
                                           if (!selected)
                                           {
                                               return "select failed: " + reply;
                                           }
                                           if (request.rectangle)
                                           {
                                               out.writeU32("hitCount", static_cast<u32>(hits.size()));
                                               out.beginArray("hits", static_cast<u32>(hits.size()));
                                               for (const Entity entity : hits)
                                                   out.writeU64("", entity.id());
                                               out.endArray();
                                           }
                                           else
                                           {
                                               out.writeBool("hit", hit.isValid());
                                               out.writeEntity("entity", hit);
                                               out.writeU32("slot", object);
                                           }
                                           out.beginArray("selection", static_cast<u32>(selection.size()));
                                           for (const Entity entity : selection)
                                           {
                                               out.writeU64("", entity.id());
                                           }
                                           out.endArray();
                                           return std::string{};
                                       });
            }
        }

        void followPicks()
        {
            // drained even with nothing pending: a result of a timed-out pick must not accumulate
            std::vector<SceneViewOverlay::PickResult> results;
            {
                auto overlay = engine->services().write<SceneViewOverlay>();
                results.swap(overlay->results);
            }
            if (picks.empty())
            {
                return;
            }
            for (const SceneViewOverlay::PickResult& result : results)
            {
                const auto it = std::ranges::find(picks, result.id, &PendingPick::id);
                if (it != picks.end())
                {
                    const PendingPick request = *it;
                    picks.erase(it);
                    finishPick(request, result);
                }
            }
            std::erase_if(
                picks,
                [this](const PendingPick& request)
                {
                    if (frame - request.frame <= PICK_TIMEOUT_FRAMES)
                    {
                        return false;
                    }
                    std::erase_if(engine->services().write<SceneViewOverlay>()->picks,
                                  [&](const SceneViewOverlay::Pick& queued) { return queued.id == request.id; });
                    if (request.reply.isValid())
                    {
                        control->completeReply(request.reply,
                                               [](IFormatWriter&)
                                               {
                                                   return std::string("the pick was never read back: no render graph "
                                                                      "(headless), or the render pipeline reloaded");
                                               });
                    }
                    return true;
                });
        }

        // -- drop point ----------------------------------------------------------------

        struct DropHit
        {
            Vec3 point;
            Entity entity = Entity::undefined(); // the scene bounds the point lies on; none: the ground
        };

        // the nearest of the first scene bounds the pixel's ray enters and the ground plane; an empty string with
        // `hit` filled, else the reason
        std::string dropPointAt(const Vec2 pixel, const bool snap, DropHit& hit) const
        {
            const std::optional<GizmoCamera> view = GizmoCamera::of(engine->render().view(SCENE_VIEW));
            if (!view)
            {
                return std::string("no scene view to drop into: the editor's views are off or it has no size");
            }
            if (!(pixel.x >= 0.0f && pixel.y >= 0.0f && pixel.x < view->extent.x && pixel.y < view->extent.y))
            {
                return std::format("({}, {}) lies outside the {}x{} scene view", pixel.x, pixel.y, view->extent.x,
                                   view->extent.y);
            }
            const Vec3 direction = view->ray(pixel);
            const Vec3 origin = view->rayOrigin(pixel);
            float best = 1.0e30f;
            hit.entity = Entity::undefined();
            for (const Chunk* chunk :
                 engine->gameWorld().getChunksContaining(Aspect(getComponentId<WorldBoundingBoxComponent>())))
            {
                const WorldBoundingBoxComponent* boxes = chunk->getComponents<WorldBoundingBoxComponent>();
                const std::span<const Entity> entities = chunk->entities();
                for (sizet i = 0; i < entities.size(); ++i)
                {
                    const std::optional<float> at =
                        rayBoxDistance(origin, direction, boxes[i].min, boxes[i].max, view->nearPlane);
                    if (at && *at < best && *at <= view->farPlane)
                    {
                        best = *at;
                        hit.entity = entities[i];
                    }
                }
            }
            std::string why;
            const std::optional<float> ground =
                rayGroundDistance(origin, direction, view->nearPlane, view->farPlane, why);
            if (ground && *ground < best)
            {
                best = *ground;
                hit.entity = Entity::undefined();
            }
            else if (!ground && !hit.entity.isValid())
            {
                return std::format("the ray through ({}, {}) meets no scene bounds and {}", pixel.x, pixel.y, why);
            }
            hit.point = origin + direction * best;
            if (const float step = EditorModelAccess::gizmo(*editorModel).step(GizmoMode::Translate);
                snap && step > 0.0f)
            {
                hit.point = Vec3(std::round(hit.point.x / step) * step, std::round(hit.point.y / step) * step,
                                 std::round(hit.point.z / step) * step);
            }
            return std::string{};
        }

        std::string dropPoint(const ControlArgs& args, IFormatWriter& out) const
        {
            const std::optional<float> x = number(args, "x");
            const std::optional<float> y = number(args, "y");
            if (!x || !y)
            {
                return std::string("dropPoint needs x and y: a scene view pixel");
            }
            const bool snap = args.boolean("snap").value_or(EditorModelAccess::gizmo(*editorModel).snap);
            DropHit hit;
            if (std::string error = dropPointAt(Vec2(*x, *y), snap, hit); !error.empty())
            {
                return error;
            }
            out.writeFloats("point", std::span<const f32>(&hit.point.x, 3));
            out.writeString("source", hit.entity.isValid() ? "surface" : "ground");
            if (hit.entity.isValid())
            {
                out.writeEntity("entity", hit.entity);
            }
            out.writeBool("snapped", snap);
            return std::string{};
        }

        // the ghost of a model dragged over the view: its bounds and a cross where it would land
        struct DropGhost
        {
            std::string model;
            Vec3 boundsMin{0.0f};
            Vec3 boundsMax{0.0f};
            Vec2 pixel{-1.0f, -1.0f};
            u64 cameraRevision = ~0ull;
            u64 stateGeneration = ~0ull;
            bool shown = false;
        } ghost;

        // the union of a model's submesh bounds (node transforms ignored: a hint of its size); empty when unreadable
        void readBounds(const std::string& model)
        {
            ghost.model = model;
            ghost.boundsMin = ghost.boundsMax = Vec3(0.0f);
            const auto descriptor =
                loadBakedModelDescriptor(InternedString(model.c_str()), engine->assets().manifest());
            if (!descriptor)
            {
                return;
            }
            const auto& submeshes = (*descriptor)->submeshes;
            for (sizet i = 0; i < submeshes.size(); ++i)
            {
                const BoundingBox& at = submeshes[i].localBounds;
                ghost.boundsMin = i == 0 ? at.min : min(ghost.boundsMin, at.min);
                ghost.boundsMax = i == 0 ? at.max : max(ghost.boundsMax, at.max);
            }
        }

        // the ghost goes; the model's bounds stay while its drag does (a pointer leaving and re-entering the view)
        void endGhost(const bool dragging)
        {
            if (ghost.shown || !ghost.model.empty())
            {
                if (dragging)
                {
                    ghost.shown = false;
                    ghost.pixel = Vec2(-1.0f, -1.0f);
                }
                else
                {
                    ghost = {};
                }
                gizmo->setGhost(std::nullopt);
            }
        }

        // Where a model dragged over the view would land, as the drop will place it. A null check without a drag.
        void followDrop()
        {
            const UiDragDrop::Drag* drag = editorUi(*engine).ui().dragDrop().current();
            std::optional<Vec2> pixel;
            if (drag && drag->accepting && drag->payload.type == InternedString(MODEL_DRAG_PAYLOAD))
            {
                pixel = viewPointer(engine->render().view(SCENE_VIEW), drag->position);
            }
            if (!pixel)
            {
                endGhost(drag != nullptr);
                return;
            }
            const std::string_view names = drag->payload.text;
            const std::string model(names.substr(0, names.find('\n')));
            if (model != ghost.model)
            {
                readBounds(model);
                ghost.shown = false;
            }
            if (ghost.shown && *pixel == ghost.pixel && ghost.cameraRevision == revision &&
                ghost.stateGeneration == control->stateGeneration())
            {
                return;
            }
            ghost.shown = true;
            ghost.pixel = *pixel;
            ghost.cameraRevision = revision;
            ghost.stateGeneration = control->stateGeneration();
            DropHit hit;
            if (!dropPointAt(*pixel, EditorModelAccess::gizmo(*editorModel).snap, hit).empty())
            {
                gizmo->setGhost(std::nullopt);
                return;
            }
            gizmo->setGhost(GizmoGhost{hit.point, ghost.boundsMin, ghost.boundsMax, hit.entity.isValid()});
        }

        // -- outline -------------------------------------------------------------------

        void followAgentHighlight()
        {
            const auto status = control->batchStatus();
            if (status.revision != highlightBatchRevision)
            {
                highlightBatchRevision = status.revision;
                highlightedEntities.clear();
                if (status.state == "completed" && (status.source == "pipe" || status.source == "stdio") &&
                    status.world == "scene" && status.step != 0)
                {
                    const Json reply =
                        Json::parse(control->handle(R"({"query":"history","args":{"world":"scene"}})"), nullptr, false);
                    if (reply.is_object() && reply.value("ok", false) && reply.contains("result") &&
                        reply["result"].contains("steps") && reply["result"]["steps"].is_array() &&
                        !reply["result"]["steps"].empty())
                    {
                        for (const auto& step : reply["result"]["steps"])
                        {
                            if (step.value("step", u64{0}) != status.step || !step.value("done", false) ||
                                !step.contains("entities"))
                                continue;
                            for (const auto& id : step["entities"])
                                if (id.is_number_unsigned())
                                    highlightedEntities.emplace_back(id.get<u64>());
                            break;
                        }
                    }
                    highlightUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                }
            }
            auto overlay = engine->services().write<SceneViewOverlay>();
            const auto view = GizmoCamera::of(engine->render().view(SCENE_VIEW));
            if (!outlineEnabled || highlightedEntities.empty() || !view || engine->gameWorldName() != "scene" ||
                std::chrono::steady_clock::now() >= highlightUntil)
            {
                if (highlightDrawn)
                {
                    overlay->agentHighlight.clear();
                    ++overlay->agentHighlightGeneration;
                    highlightDrawn = false;
                }
                return;
            }
            World& world = engine->gameWorld();
            auto& transforms = engine->gameTransforms();
            transforms.compile();
            std::vector<BoundingBox> boxes;
            for (const Entity root : highlightedEntities)
            {
                if (!world.isEntityAlive(root))
                    continue;
                std::vector<Entity> entities{root};
                const auto children = transforms.subtreeOf(root);
                entities.insert(entities.end(), children.begin(), children.end());
                for (const Entity entity : entities)
                {
                    if (!world.isEntityAlive(entity) || !world.hasComponent<WorldBoundingBoxComponent>(entity))
                        continue;
                    const auto& bounds = world.getConstComponent<WorldBoundingBoxComponent>(entity);
                    boxes.push_back({bounds.min, bounds.max});
                }
            }
            if (boxes.size() > 16)
            {
                BoundingBox group = boxes.front();
                for (const auto& box : boxes)
                {
                    group.min = {std::min(group.min.x, box.min.x), std::min(group.min.y, box.min.y),
                                 std::min(group.min.z, box.min.z)};
                    group.max = {std::max(group.max.x, box.max.x), std::max(group.max.y, box.max.y),
                                 std::max(group.max.z, box.max.z)};
                }
                boxes.assign(1, group);
            }
            overlay->agentHighlight.clear();
            for (const auto& box : boxes)
            {
                std::array<std::optional<Vec2>, 8> points;
                for (u32 i = 0; i < 8; ++i)
                    points[i] = view->project(Vec3{i & 1 ? box.max.x : box.min.x, i & 2 ? box.max.y : box.min.y,
                                                   i & 4 ? box.max.z : box.min.z});
                for (u32 i = 0; i < 8; ++i)
                    for (u32 bit : {1u, 2u, 4u})
                        if (!(i & bit) && points[i] && points[i | bit])
                            overlay->agentHighlight.push_back({*points[i], *points[i | bit], 1.5f, 0xffe070bfu});
            }
            ++overlay->agentHighlightGeneration;
            highlightDrawn = true;
        }

        void followOutline()
        {
            const std::span<const Entity> selection = editorModel->selection();
            const bool shown =
                outlineEnabled && editorModel->selectionWorld() == engine->gameWorldName() && !selection.empty();
            if (!shown)
            {
                if (outlining)
                {
                    outlining = false;
                    outlineSelection.clear();
                    renderables.clear();
                    auto overlay = engine->services().write<SceneViewOverlay>();
                    overlay->outlined.clear();
                    ++overlay->outlineGeneration;
                }
                return;
            }
            outlining = true;
            World& world = engine->gameWorld();
            EntityTransformManager& transforms = engine->gameTransforms();
            if (!std::ranges::equal(selection, outlineSelection) || &transforms != outlineTransforms ||
                transforms.structureGeneration() != outlineStructure)
            {
                transforms.compile();
                outlineTransforms = &transforms;
                outlineSelection.assign(selection.begin(), selection.end());
                outlineStructure = transforms.structureGeneration();
                renderables.clear();
                for (const Entity entity : selection)
                {
                    renderables.push_back(entity);
                    const std::span<const Entity> subtree = transforms.subtreeOf(entity);
                    renderables.insert(renderables.end(), subtree.begin(), subtree.end());
                }
                std::ranges::sort(renderables);
                const auto [first, last] = std::ranges::unique(renderables);
                renderables.erase(first, last);
            }
            slots.clear();
            for (const Entity entity : renderables)
            {
                if (world.isEntityAlive(entity) && world.hasComponent<GpuInstanceSlotComponent>(entity))
                {
                    if (const u32 slot = world.getConstComponent<GpuInstanceSlotComponent>(entity).slot;
                        slot != INVALID_INSTANCE_SLOT)
                    {
                        slots.push_back(slot);
                    }
                }
            }
            std::ranges::sort(slots);
            auto overlay = engine->services().write<SceneViewOverlay>();
            if (slots != overlay->outlined)
            {
                overlay->outlined.assign(slots.begin(), slots.end());
                ++overlay->outlineGeneration;
            }
        }

        // -- gestures ------------------------------------------------------------------

        void queueCamera(const std::string_view key, const std::string& value)
        {
            pending.push_back(std::format(R"({{"command":"editorCamera","args":{{"{}":{}}}}})", key, value));
        }

        void queueGizmo(const std::string_view phase, const Vec2 pixel)
        {
            pending.push_back(std::format(R"({{"command":"gizmoDrag","args":{{"phase":"{}","x":{},"y":{}}}}})", phase,
                                          pixel.x, pixel.y));
        }

        void subscribeGestures()
        {
            UiRouter& router = editorUi(*engine).ui().router();
            const Entity element = image.entity();
            router.subscribe(element, UiEventKind::PointerDown,
                             [this](const void* payload)
                             {
                                 const auto& event = *static_cast<const UiPointerDownEvent*>(payload);
                                 if (event.button == 0)
                                 {
                                     ++gestureRevision;
                                     marqueeReleased = false;
                                     marqueeDragging = false;
                                     pressModifiers = event.modifiers;
                                     // a press on a gizmo handle drags it; its click picks nothing
                                     gizmoPress = false;
                                     if (gizmoDragging) // its release went elsewhere
                                     {
                                         gizmoDragging = false;
                                         pending.emplace_back(R"({"command":"gizmoDrag","args":{"phase":"end"}})");
                                     }
                                     const std::optional<Vec2> pixel =
                                         viewPointer(engine->render().view(SCENE_VIEW), event.position);
                                     marqueePressValid = pixel.has_value();
                                     if (pixel)
                                         marqueeStart = *pixel;
                                     marqueeExtent = engine->render().view(SCENE_VIEW).extent;
                                     if ((event.modifiers & UiKeyModifier::ALT) == 0 && pixel &&
                                         gizmo->handleAt(*pixel) != GizmoHandle::None)
                                     {
                                         gizmoPress = true;
                                         gizmoDragging = true;
                                         queueGizmo("begin", *pixel);
                                     }
                                 }
                                 else if (input && event.button == 1)
                                 {
                                     looking = true;
                                     lastFly = std::chrono::steady_clock::now();
                                 }
                                 else if (input && event.button == 2)
                                 {
                                     panning = true;
                                 }
                             });
            router.subscribe(
                element, UiEventKind::Click,
                [this](const void* payload)
                {
                    const auto& event = *static_cast<const UiClickEvent*>(payload);
                    if (event.button != 0 || (pressModifiers & UiKeyModifier::ALT) != 0 || gizmoPress)
                    {
                        return;
                    }
                    const std::optional<Vec2> pixel = viewPointer(engine->render().view(SCENE_VIEW), event.position);
                    if (!pixel)
                    {
                        return;
                    }
                    const PickMode mode = (event.modifiers & UiKeyModifier::CTRL) != 0 ? PickMode::Toggle
                        : (event.modifiers & UiKeyModifier::SHIFT) != 0                ? PickMode::Add
                                                                                       : PickMode::Replace;
                    pending.push_back(std::format(R"({{"command":"pick","args":{{"x":{},"y":{},"mode":"{}"}}}})",
                                                  static_cast<u32>(pixel->x), static_cast<u32>(pixel->y),
                                                  PICK_MODES[static_cast<u8>(mode)]));
                });
            router.subscribe(element, UiEventKind::HoverEnter,
                             [this](const void* payload)
                             {
                                 const auto& event = *static_cast<const UiHoverEnterEvent*>(payload);
                                 gizmo->setHover(viewPointer(engine->render().view(SCENE_VIEW), event.position));
                             });
            router.subscribe(element, UiEventKind::HoverMove,
                             [this](const void* payload)
                             {
                                 const auto& event = *static_cast<const UiHoverMoveEvent*>(payload);
                                 gizmo->setHover(viewPointer(engine->render().view(SCENE_VIEW), event.position));
                             });
            router.subscribe(element, UiEventKind::HoverLeave, [this](const void*) { gizmo->setHover(std::nullopt); });
            const auto showMarquee = [this](const Vec2 position)
            {
                const auto& view = engine->render().view(SCENE_VIEW);
                if (!view.extent.width || !view.extent.height)
                    return;
                Vec2 end = viewPixels(view, position);
                end.x = std::clamp(end.x, 0.0f, static_cast<float>(view.extent.width - 1));
                end.y = std::clamp(end.y, 0.0f, static_cast<float>(view.extent.height - 1));
                const Vec2 a{std::min(marqueeStart.x, end.x), std::min(marqueeStart.y, end.y)};
                const Vec2 b{std::max(marqueeStart.x, end.x), std::max(marqueeStart.y, end.y)};
                auto overlay = engine->services().write<SceneViewOverlay>();
                const u32 color =
                    editorUi(*engine).ui().colorTokens().find(InternedString("accent")).value_or(0xffffffffu);
                overlay->marquee = {{a, {b.x, a.y}, 1.0f, color},
                                    {{b.x, a.y}, b, 1.0f, color},
                                    {b, {a.x, b.y}, 1.0f, color},
                                    {{a.x, b.y}, a, 1.0f, color}};
                ++overlay->marqueeGeneration;
            };
            router.subscribe(element, UiEventKind::DragStart,
                             [this, showMarquee](const void* payload)
                             {
                                 if ((pressModifiers & UiKeyModifier::ALT) || gizmoPress || !marqueePressValid)
                                     return;
                                 marqueeDragging = true;
                                 showMarquee(static_cast<const UiDragStartEvent*>(payload)->position);
                             });
            router.subscribe(element, UiEventKind::DragMove,
                             [this, showMarquee](const void* payload)
                             {
                                 const auto& event = *static_cast<const UiDragMoveEvent*>(payload);
                                 if ((pressModifiers & UiKeyModifier::ALT) != 0)
                                 {
                                     queueCamera("orbit",
                                                 std::format("[{},{}]", -event.delta.x * DEGREES_PER_PIXEL,
                                                             -event.delta.y * DEGREES_PER_PIXEL));
                                 }
                                 else if (gizmoDragging)
                                 {
                                     // unclipped: a drag goes on past the panel's edge
                                     queueGizmo("move", viewPixels(engine->render().view(SCENE_VIEW), event.position));
                                 }
                                 else if (marqueeDragging)
                                     showMarquee(event.position);
                             });
            const auto endGizmo = [this]
            {
                if (gizmoDragging)
                {
                    gizmoDragging = false;
                    pending.emplace_back(R"({"command":"gizmoDrag","args":{"phase":"end"}})");
                }
            };
            router.subscribe(element, UiEventKind::DragEnd,
                             [this, endGizmo](const void* payload)
                             {
                                 endGizmo();
                                 if (!marqueeDragging)
                                     return;
                                 marqueeDragging = false;
                                 auto overlay = engine->services().write<SceneViewOverlay>();
                                 overlay->marquee.clear();
                                 ++overlay->marqueeGeneration;
                                 const auto& view = engine->render().view(SCENE_VIEW);
                                 if (!view.extent.width || !view.extent.height ||
                                     view.extent.width != marqueeExtent.width ||
                                     view.extent.height != marqueeExtent.height)
                                     return;
                                 Vec2 end = viewPixels(view, static_cast<const UiDragEndEvent*>(payload)->position);
                                 end.x = std::clamp(end.x, 0.0f, static_cast<float>(view.extent.width - 1));
                                 end.y = std::clamp(end.y, 0.0f, static_cast<float>(view.extent.height - 1));
                                 marqueeEnd = end;
                                 marqueeReleased = true;
                             });
            router.subscribe(element, UiEventKind::PointerUp,
                             [this, endGizmo](const void* payload)
                             {
                                 if (static_cast<const UiPointerUpEvent*>(payload)->button != 0)
                                     return;
                                 endGizmo();
                                 if (!marqueeReleased)
                                     return;
                                 marqueeReleased = false;
                                 const PickMode mode = (pressModifiers & UiKeyModifier::CTRL) ? PickMode::Toggle
                                     : (pressModifiers & UiKeyModifier::SHIFT)                ? PickMode::Add
                                                                                              : PickMode::Replace;
                                 pending.push_back(Json{{"command", "pickRect"},
                                                        {"args",
                                                         {{"x", marqueeStart.x},
                                                          {"y", marqueeStart.y},
                                                          {"x2", marqueeEnd.x},
                                                          {"y2", marqueeEnd.y},
                                                          {"mode", PICK_MODES[static_cast<u8>(mode)]}}}}
                                                       .dump());
                             });
            image.onWheel([this](const Vec2 wheel)
                          { queueCamera("dolly", std::format("{}", wheel.y * DOLLY_PER_NOTCH * camera.distance)); });
        }

        // right drag: look, WASD/QE fly; middle drag: pan (raw input: the UI routes drags of the primary button only)
        void followRawDrags()
        {
            if (!looking && !panning)
            {
                return;
            }
            const MouseState mouse = input->getMouseState();
            if (looking && !input->getMouseButtonState(MouseButton::Right).isPressed)
            {
                looking = false;
                flyBoost.held = 0.0f;
            }
            if (panning && !input->getMouseButtonState(MouseButton::Middle).isPressed)
            {
                panning = false;
            }
            const bool moved = mouse.deltaX != 0.0f || mouse.deltaY != 0.0f;
            if (looking)
            {
                if (moved)
                {
                    queueCamera(
                        "look",
                        std::format("[{},{}]", -mouse.deltaX * DEGREES_PER_PIXEL, -mouse.deltaY * DEGREES_PER_PIXEL));
                }
                const auto now = std::chrono::steady_clock::now();
                const float seconds = std::min(std::chrono::duration<float>(now - lastFly).count(), 0.1f);
                lastFly = now;
                const auto axis = [this](const KeyCode positive, const KeyCode negative)
                {
                    return (input->getKeyState(positive).isPressed ? 1.0f : 0.0f) -
                        (input->getKeyState(negative).isPressed ? 1.0f : 0.0f);
                };
                const Vec3 direction(axis(KeyCode::D, KeyCode::A), axis(KeyCode::E, KeyCode::Q),
                                     axis(KeyCode::W, KeyCode::S));
                const bool shift =
                    input->getKeyState(KeyCode::LSHIFT).isPressed || input->getKeyState(KeyCode::RSHIFT).isPressed;
                const float boost = flyBoost.advance(shift, seconds, settings.flyBoostRate, settings.flyBoostMax);
                if (direction != Vec3(0.0f, 0.0f, 0.0f))
                {
                    const Vec3 step =
                        direction * (settings.flySpeed * std::max(camera.distance, 1.0f) * seconds * boost);
                    queueCamera("fly", std::format("[{},{},{}]", step.x, step.y, step.z));
                }
            }
            else if (panning && moved)
            {
                const float scale = PAN_PER_PIXEL * camera.distance;
                queueCamera("pan", std::format("[{},{}]", -mouse.deltaX * scale, mouse.deltaY * scale));
            }
        }

        void dispatch()
        {
            if (pending.empty())
            {
                return;
            }
            std::vector<std::string> requests;
            requests.swap(pending);
            std::optional<bool> dragLive; // whether the batch's last gizmoDrag request left a drag running
            for (const std::string& request : requests)
            {
                const std::string reply = control->handle(request);
                const bool failed = reply.find(R"("ok":false)") != std::string::npos;
                if (failed)
                {
                    SWARNING_LOG("scene view: {} -> {}", request, reply);
                }
                if (request.starts_with(R"({"command":"gizmoDrag")"))
                {
                    dragLive = !failed && request.find(R"("phase":"end")") == std::string::npos;
                }
            }
            if (dragLive)
            {
                gizmoDragging = *dragLive; // a failed begin or move: stop queueing the dead drag's moves
            }
        }

        // -- control -------------------------------------------------------------------

        void registerCommands()
        {
            // not undoable: the camera and picking are editor view state; the select a pick makes is the edit
            control->registerViewCommand(
                "editorCamera", OWNER,
                "sets the scene view's editor camera and/or moves it (angles in degrees, distances in world units)",
                {optionalArg("pivot", ArgKind::Json, "[x,y,z]: the point the camera orbits"),
                 optionalArg("position", ArgKind::Json,
                             "[x,y,z]: the eye; with pivot, looks from it at the pivot, alone keeps the direction"),
                 optionalArg("yaw", ArgKind::Number, "degrees about +Y; 0 looks down -Z"),
                 optionalArg("pitch", ArgKind::Number, "degrees, positive looks up; within +-89"),
                 optionalArg("distance", ArgKind::Number, "from the eye to the pivot"),
                 optionalArg("fov", ArgKind::Number, "vertical field of view, degrees"),
                 optionalArg("near", ArgKind::Number,
                             "near plane; orthographic: the pivot lies 80 (or half the clip depth) past it"),
                 optionalArg("far", ArgKind::Number, "far plane"),
                 optionalArg("orbit", ArgKind::Json, "[yaw,pitch] degrees turned around the pivot"),
                 optionalArg("look", ArgKind::Json, "[yaw,pitch] degrees turned in place"),
                 optionalArg("pan", ArgKind::Json, "[right,up]: moves eye and pivot"),
                 optionalArg("fly", ArgKind::Json, "[right,up,forward]: moves eye and pivot"),
                 optionalArg("dolly", ArgKind::Number, "moves the eye toward the pivot (negative: away)"),
                 optionalArg("view", ArgKind::String,
                             "first: the projection and direction; leaving perspective keeps the camera and "
                             "perspective restores it; an axis view looks along that axis; orthographic: the "
                             "camera's position stays pivot - forward * distance, not the drawn eye",
                             {}, SCENE_VIEW_MODES),
                 optionalArg("frame", ArgKind::String,
                             "after the other fields: pivot on the middle of the selection's (every entity's) bounds, "
                             "distance to fit them; refused with nothing to frame",
                             {}, FRAME_TARGETS)},
                [this](const ControlArgs& args, IFormatWriter& out) { return setCamera(args, out); });
            control->registerCommand(
                "pick", OWNER,
                "selects what the scene view shows at a pixel, read back from its GPU visibility buffer; a see-through "
                "(blended) surface in front of it is selected where its coverage reaches graphics.pickAlphaThreshold "
                "(the nearest such surface), and looked through below it; answers once read back",
                {requiredArg("x", ArgKind::Number, "scene view pixel column (the views query gives its extent)"),
                 requiredArg("y", ArgKind::Number, "scene view pixel row"),
                 optionalArg("mode", ArgKind::String,
                             "replace (the background clears), add, or toggle the entity in the selection", "replace",
                             PICK_MODES)},
                [this](const ControlArgs& args, IFormatWriter& out) { return pick(args, out); });
            control->registerCommand(
                "pickRect", OWNER,
                "selects visible entities in a rectangle of scene view pixels; its readback replies with hitCount, "
                "hits and selection, or queued for a local gesture; x2 and y2 are the opposite corner",
                {requiredArg("x", ArgKind::Number, "first column"), requiredArg("y", ArgKind::Number, "first row"),
                 requiredArg("x2", ArgKind::Number, "opposite column"),
                 requiredArg("y2", ArgKind::Number, "opposite row"),
                 optionalArg("mode", ArgKind::String, "replace, add or toggle", "replace", PICK_MODES)},
                [this](const ControlArgs& args, IFormatWriter& out) { return pickRect(args, out); });
            control->registerQuery("dropPoint", OWNER,
                                   "where a model dropped at a scene view pixel lands: the nearest scene bounds the "
                                   "pixel's ray enters, else the ground plane y=0; perspective and orthographic",
                                   {requiredArg("x", ArgKind::Number, "scene view pixel column"),
                                    requiredArg("y", ArgKind::Number, "scene view pixel row"),
                                    optionalArg("snap", ArgKind::Boolean,
                                                "snap the point to the gizmo's translate step (default the gizmo's "
                                                "snap setting)")},
                                   [this](const ControlArgs& args, IFormatWriter& out)
                                   { return dropPoint(args, out); });
            control->registerCommand(
                "captureScene", OWNER,
                "captures the scene without moving the editor camera; returns the PNG path and temporary view id; "
                "fails if the capture cannot be recorded within 10 seconds",
                {requiredArg("path", ArgKind::String, "PNG path relative to the captures root"),
                 optionalArg("frame", ArgKind::String, "frame all or selection using the temporary camera", {},
                             FRAME_TARGETS),
                 optionalArg("width", ArgKind::Integer,
                             "output width in pixels; supply with height, within the device 2D limit and 16777216 "
                             "total pixels"),
                 optionalArg("height", ArgKind::Integer,
                             "output height in pixels; supply with width (default: Scene view extent)"),
                 optionalArg("settle", ArgKind::Integer, "frames to settle temporal rendering", "0")},
                [this](const ControlArgs& args, IFormatWriter&) -> std::string
                {
                    if (captureBusy)
                        return "a scene capture is already in progress";
                    if (engine->render().viewCount() <= CAPTURE_VIEW ||
                        engine->config().graphics.isCaptureView(CAPTURE_VIEW))
                        return "the dedicated scene capture view is unavailable";
                    const u64 settle = args.integer("settle").value_or(0);
                    if (settle > 240)
                        return "settle must be between 0 and 240";
                    const auto width = args.integer("width");
                    const auto height = args.integer("height");
                    if (width.has_value() != height.has_value())
                        return "width and height must be supplied together";
                    Extent2D extent = engine->render().view(SCENE_VIEW).extent;
                    if (width)
                    {
                        const u32 limit = engine->services().read<IRenderer>()->getCapabilities().maxImageDimension2D;
                        constexpr u64 MAX_CAPTURE_PIXELS = 16ULL * 1024 * 1024;
                        if (*width == 0 || *height == 0 || *width > limit || *height > limit ||
                            *width > MAX_CAPTURE_PIXELS / *height)
                            return "width and height must be positive, within the device 2D limit and 16777216 pixels";
                        extent = {static_cast<u32>(*width), static_cast<u32>(*height)};
                    }
                    std::filesystem::path path;
                    if (const std::string error = control->resolveFile("captures", *args.string("path"), path);
                        !error.empty())
                        return error;
                    EditorCamera temporary = camera;
                    if (const auto frame = args.string("frame"))
                        if (const std::string error = frameCamera(*frame, temporary, extent); !error.empty())
                            return error;
                    const DeferredReply reply = control->deferReply();
                    if (!reply.isValid())
                        return "captureScene needs a control pipe or stdio reply";
                    captureReply = reply;
                    RenderView& target = engine->render().view(CAPTURE_VIEW);
                    const RenderView saved = target;
                    target.scene = engine->render().view(SCENE_VIEW).scene;
                    target.extent = extent;
                    target.renderScale = 1.0f;
                    target.scalable = false;
                    target.active = true;
                    CameraPose pose;
                    pose.transform.position = temporary.position();
                    pose.transform.rotation = temporary.rotation();
                    pose.camera.fov = toRadians(temporary.fov);
                    pose.camera.nearPlane = temporary.nearPlane;
                    pose.camera.farPlane = temporary.farPlane;
                    if (temporary.orthographic)
                    {
                        pose.orthographicHeight = orthographicHeightOf(temporary);
                        pose.transform.position = orthographicEyeOf(temporary);
                    }
                    target.pose = pose;
                    captureBusy = true;
                    const std::weak_ptr<bool> alive = captureAlive;
                    captureTicket = engine->captureViewTracked(
                        CAPTURE_VIEW, path, static_cast<u32>(settle),
                        [this, alive, reply, saved, path](std::string error)
                        {
                            if (alive.expired())
                                return;
                            engine->render().view(CAPTURE_VIEW) = saved;
                            captureBusy = false;
                            captureTicket = 0;
                            captureReply = {};
                            control->completeReply(reply,
                                                   [error = std::move(error), path](IFormatWriter& out)
                                                   {
                                                       if (!error.empty())
                                                           return error;
                                                       out.writeString("path", utf8Path(path));
                                                       out.writeU32("view", CAPTURE_VIEW);
                                                       return std::string{};
                                                   });
                        });
                    if (!captureTicket)
                    {
                        target = saved;
                        captureBusy = false;
                        captureReply = {};
                        control->completeReply(reply,
                                               [](IFormatWriter&) { return std::string("scene capture unavailable"); });
                    }
                    return {};
                });
            control->registerViewCommand(
                "historyHighlight", OWNER,
                "briefly outlines the affected entities of a history step independently of selection",
                {requiredArg("step", ArgKind::Integer, "history step id")},
                [this](const ControlArgs& args, IFormatWriter& out) -> std::string
                {
                    const Json reply =
                        Json::parse(control->handle(R"({"query":"history","args":{"world":"scene"}})"), nullptr, false);
                    if (!reply.is_object() || !reply.value("ok", false) || !reply.contains("result") ||
                        !reply["result"].contains("steps"))
                        return "cannot read scene history";
                    for (const auto& step : reply["result"]["steps"])
                    {
                        if (step.value("step", u64{0}) != args.integer("step").value_or(0))
                            continue;
                        highlightedEntities.clear();
                        for (const auto& id : step["entities"])
                            if (id.is_number_unsigned())
                                highlightedEntities.emplace_back(id.get<u64>());
                        highlightUntil = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                        out.writeU32("entities", static_cast<u32>(highlightedEntities.size()));
                        return {};
                    }
                    return "history step is no longer available";
                });
            control->registerQuery(
                "editorContext", OWNER,
                "reports the checkout content root, open scene path, dirty state and stable sceneView id", {},
                [this](const ControlArgs&, IFormatWriter& out) -> std::string
                {
                    const Json reply = Json::parse(control->handle(R"({"query":"sceneInfo"})"), nullptr, false);
                    if (!reply.is_object() || !reply.value("ok", false) || !reply.contains("result") ||
                        !reply["result"].is_object())
                        return "cannot read scene information";
                    const Json& scene = reply["result"];
                    out.writeString("checkout", utf8Path(contentRoot()));
                    out.writeString("scene", scene.value("path", std::string{}));
                    out.writeBool("dirty", scene.value("dirty", false));
                    out.writeU32("sceneView", SCENE_VIEW);
                    return {};
                });
            control->registerQuery("sceneView", OWNER,
                                   "the scene view's world (play while playing, else scene) and render scene, its "
                                   "editor camera, pending picks, last pick and outlined instance slots",
                                   {},
                                   [this](const ControlArgs&, IFormatWriter& out)
                                   {
                                       out.writeU32("view", SCENE_VIEW);
                                       out.writeString("world", engine->gameWorldName());
                                       out.writeU32("scene", engine->render().view(SCENE_VIEW).scene);
                                       writeCamera(out);
                                       out.beginArray("picksPending", static_cast<u32>(picks.size()));
                                       for (const PendingPick& request : picks)
                                       {
                                           out.beginObject({});
                                           out.writeU64("pick", request.id);
                                           out.writeU32("x", request.x);
                                           out.writeU32("y", request.y);
                                           out.writeBool("rectangle", request.rectangle);
                                           if (request.rectangle)
                                           {
                                               out.writeU32("x2", request.x2);
                                               out.writeU32("y2", request.y2);
                                           }
                                           out.writeString("mode", PICK_MODES[static_cast<u8>(request.mode)]);
                                           out.endObject();
                                       }
                                       out.endArray();
                                       out.beginObject("lastPick");
                                       out.writeU64("pick", lastPick.id);
                                       out.writeU32("x", lastPick.x);
                                       out.writeU32("y", lastPick.y);
                                       out.writeBool("rectangle", lastPick.rectangle);
                                       if (lastPick.rectangle)
                                       {
                                           out.writeU32("x2", lastPick.x2);
                                           out.writeU32("y2", lastPick.y2);
                                           out.writeU32("hitCount", lastPick.hitCount);
                                       }
                                       out.writeBool("hit", lastPick.entity.isValid());
                                       out.writeEntity("entity", lastPick.entity);
                                       out.writeU32("slot", lastPick.object);
                                       out.endObject();
                                       const auto overlay = engine->services().read<SceneViewOverlay>();
                                       out.beginArray("outlinedSlots", static_cast<u32>(overlay->outlined.size()));
                                       for (const u32 slot : overlay->outlined)
                                       {
                                           out.writeU32("", slot);
                                       }
                                       out.endArray();
                                       return std::string{};
                                   });
        }
    };

    EditorSceneView::EditorSceneView(Engine& engine, const EditorModel& editorModel, EditorGizmo& gizmo,
                                     const Image& sceneImage, const EditorSettings& settings) :
        m_state(std::make_unique<State>(engine, editorModel, gizmo, sceneImage, settings))
    {
        State& state = *m_state;
        editorUi(engine).world().getComponent<UiStateComponent>(sceneImage.entity()).dragThreshold = 4.0f;
        if (engine.services().contains<InputSystem>())
        {
            state.input = &engine.services().get<InputSystem>();
        }
        state.subscribeGestures();
        state.registerCommands();
        if (engine.render().viewCount() > State::CAPTURE_VIEW &&
            !engine.config().graphics.isCaptureView(State::CAPTURE_VIEW))
            engine.render().view(State::CAPTURE_VIEW).active = false;
        state.applyPose();
    }

    EditorSceneView::~EditorSceneView()
    {
        State& state = *m_state;
        state.captureAlive.reset();
        if (state.captureTicket)
            state.engine->cancelCapture(state.captureTicket);
        if (state.captureReply.isValid())
            state.control->completeReply(state.captureReply,
                                         [](IFormatWriter&) { return std::string("the scene view closed"); });
        if (state.engine->render().viewCount() > State::CAPTURE_VIEW &&
            !state.engine->config().graphics.isCaptureView(State::CAPTURE_VIEW))
            state.engine->render().view(State::CAPTURE_VIEW).active = false;
        state.control->unregisterOwner(OWNER);
        for (const State::PendingPick& request : state.picks)
        {
            if (request.reply.isValid())
            {
                state.control->completeReply(request.reply,
                                             [](IFormatWriter&) { return std::string("the scene view closed"); });
            }
        }
        {
            auto overlay = state.engine->services().write<SceneViewOverlay>();
            overlay->picks.clear();
            overlay->results.clear();
            overlay->outlined.clear();
            overlay->marquee.clear();
            overlay->agentHighlight.clear();
            ++overlay->agentHighlightGeneration;
            ++overlay->marqueeGeneration;
            ++overlay->outlineGeneration;
        }
        state.engine->render().view(SCENE_VIEW).pose.reset();
    }

    bool EditorSceneView::flying() const { return m_state->looking; }

    const EditorCamera& EditorSceneView::camera() const { return m_state->camera; }

    u64 EditorSceneView::cameraRevision() const { return m_state->revision; }

    bool EditorSceneView::outlineShown() const { return m_state->outlineEnabled; }

    void EditorSceneView::showOutline(const bool shown) { m_state->outlineEnabled = shown; }

    std::string_view EditorSceneView::viewMode() const { return SCENE_VIEW_MODES[m_state->mode]; }

    std::string_view EditorSceneView::frameTarget(const Engine& engine, const EditorModel& editorModel)
    {
        return editorModel.selectionWorld() == engine.gameWorldName() && !editorModel.selection().empty()
            ? FRAME_TARGETS[0]
            : FRAME_TARGETS[1];
    }

    void EditorSceneView::update()
    {
        State& state = *m_state;
        ++state.frame;
        state.marqueeReleased = false; // a cancelled capture has no accompanying PointerUp
        state.followRawDrags();
        state.dispatch();
        state.followPicks();
        state.followOutline();
        state.followAgentHighlight();
        state.followDrop();
    }
} // namespace spite
