#pragma once

#include <optional>
#include <vector>

#include "ecs/EcsTypes.hpp"
#include "rendering/RenderingSingletons.hpp"
#include "rendering/RenderPassModule.hpp"
#include "renderruntime/RenderingComponents.hpp"
#include "renderruntime/RenderRuntimeExport.hpp"
#include "world/TransformComponents.hpp"

namespace spite
{
    // A camera that is not an entity of the drawn world (the editor camera).
    struct CameraPose
    {
        TransformComponent transform;
        CameraComponent camera;
        // > 0: an orthographic projection showing this much world height at every depth (the width follows the
        // aspect); camera.fov is then unused. nearPlane and farPlane bound the depth in front of the transform.
        float orthographicHeight = 0.0f;
    };

    // What the render runtime draws: a camera entity of the world of
    // scene (0: the runtime's world, else RenderRuntime::attachWorld), rendered
    // at extent (zero follows the window).
    // Consumers (gameplay, editor tooling) write camera and extent; the upload
    // system computes matrices every frame; a view with no valid camera and no
    // pose draws a fallback pose (it never creates an entity).
    struct SPITE_RENDER_RUNTIME_API RenderView
    {
        u32 scene = 0;
        Entity camera = Entity::undefined();
        // Set: the view draws from this pose and neither reads nor spawns its camera entity, whose
        // binding stays for whoever follows it (the editor's game view follows the scene camera).
        std::optional<CameraPose> pose;
        // The view's output extent (TextureExtent::DISPLAY): view targets, Composite, overlays, UI and what a pointer
        // is read in. The scene renders at renderExtentFor(extent, renderScale); Composite upscales it.
        Extent2D extent{};
        // The render extent's share of extent on each axis (1: the view renders at its output size, the default;
        // clampRenderScale bounds it). The editor's Scene view stays at 1.
        float renderScale = 1.0f;
        // false: renderScale stays 1 and nothing sets it (a tool's view: the editor's Scene view, which the host
        // marks)
        bool scalable = true;
        // Where the host shows the view's target, in window framebuffer pixels
        // (the editor's Scene and Game panels); zero size: the whole window.
        Rect2D windowRect{};
        // false: the view renders nothing (its per-view passes are skipped; global passes still run)
        bool active = true;
        // last computed, for host tooling (picking rays)
        CameraMatricesData matrices{};
    };

    // A window position (framebuffer pixels) in the view's own pixels: relative
    // to windowRect and scaled to the view's extent, which lags a resizing panel
    // until its size settles. Unclipped: outside windowRect it lies outside the view.
    inline Vec2 viewPixels(const RenderView& view, const Vec2 windowPosition)
    {
        const Rect2D& rect = view.windowRect;
        if (rect.extent.width == 0 || rect.extent.height == 0)
        {
            return windowPosition;
        }
        const Vec2 local{windowPosition.x - static_cast<float>(rect.offset.x),
                         windowPosition.y - static_cast<float>(rect.offset.y)};
        if (view.extent.width == 0 || view.extent.height == 0)
        {
            return local;
        }
        return Vec2{local.x * static_cast<float>(view.extent.width) / static_cast<float>(rect.extent.width),
                    local.y * static_cast<float>(view.extent.height) / static_cast<float>(rect.extent.height)};
    }

    // A window pointer position in the view's own pixels (viewPixels); empty
    // outside windowRect.
    inline std::optional<Vec2> viewPointer(const RenderView& view, const Vec2 windowPosition)
    {
        const Rect2D& rect = view.windowRect;
        const float x = windowPosition.x - static_cast<float>(rect.offset.x);
        const float y = windowPosition.y - static_cast<float>(rect.offset.y);
        if (rect.extent.width != 0 && rect.extent.height != 0 &&
            (x < 0.0f || y < 0.0f || x >= static_cast<float>(rect.extent.width) ||
             y >= static_cast<float>(rect.extent.height)))
        {
            return std::nullopt;
        }
        return viewPixels(view, windowPosition);
    }

    // The views drawn each frame, one per GraphicsConfig::renderViewCount
    // swapchainView onto the swapchain, every other view k into the
    // graph texture viewTargetName(k). Each runs its own pass chain with its
    // own occlusion history. NO_SWAPCHAIN_VIEW renders every view offscreen and
    // leaves the swapchain to the UI (editor panels show the targets as images).
    struct SPITE_RENDER_RUNTIME_API RenderViews
    {
        std::vector<RenderView> views;
        u32 swapchainView = 0;
        // The view the game's camera draws: what gameplay casts pointer rays through. Other views of the same
        // scene may be tooling's (the editor's Scene view draws the play world from the editor camera).
        u32 gameView = 0;
    };

    inline std::optional<CameraMatricesData> viewCamera(const RenderViews& views, u32 view)
    {
        if (view >= views.views.size())
            return std::nullopt;
        return views.views[view].matrices;
    }

    // Copy the executing view's current camera before releasing the service lock.
    // Empty when the render runtime or view is unavailable.
    inline std::optional<CameraMatricesData> passViewCamera(RenderPassExecutionContext& context)
    {
        auto& services = context.services();
        if (!services.contains<RenderViews>())
            return std::nullopt;
        const u32 view = context.view();
        const auto views = services.read<RenderViews>();
        return viewCamera(*views, view);
    }
} // namespace spite
