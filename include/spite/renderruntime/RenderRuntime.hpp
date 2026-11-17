#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/ApplicationConfig.hpp"
#include "base/Expected.hpp"
#include "base/GraphicsApi.hpp"
#include "base/ServiceLocator.hpp"
#include "base/ShadowLibrary.hpp"
#include "base/Handle.hpp"
#include "ecs/World.hpp"
#include "rendering/GpuBudget.hpp"
#include "rendering/RenderingSingletons.hpp"
#include "rendering/RenderPassSetupService.hpp"
#include "renderruntime/RenderRuntimeExport.hpp"
#include "renderruntime/RenderView.hpp"
#include "renderruntime/SceneGpuStream.hpp"
#include "renderruntime/SunShadows.hpp"
#include "renderruntime/Upscale.hpp"

namespace spite
{
    class Window;
    class RenderingManager;
    struct RenderPipelineDescriptor;

    // The SpiteRenderPipeline build the runtime calls (the renderpipeline hot-reload tier), as the renderPipeline query
    // shows it.
    struct RenderPipelineStatus
    {
        std::filesystem::path source; // the DLL the running build was copied from
        std::filesystem::path shadow; // the mapped copy
        u32 reloads = 0;
        // why the last reload was refused; empty after a successful one
        std::string lastError;
        // previous builds left mapped because a profiler may still read names in them
        u32 pinnedImages = 0;
        bool watching = false; // a rebuilt default DLL reloads at the next frame boundary
    };

    struct RenderRuntimeDesc
    {
        GraphicsApi api = GraphicsApi::Vulkan;
        // must outlive the runtime
        const ApplicationConfig* config = nullptr;
        // null = headless: device, transfer, and bindless only; no renderer,
        // no pass graph, no presentation
        Window* window = nullptr;
        // Extra pass modules (test readback, tooling) registered after the
        // standard set, under the "host" owner, before the pass-setup finalize.
        std::function<void(std::vector<RenderPassModuleDefinition>&)> extraModules;
        // the RenderPassSetupService's pass enables and implementation pins
        RenderPassSetupRuntimeConfig passConfig;
        // false: no Vulkan instance or device (CPU-only tests): the views, sun shadows, scene stream and stats stay,
        // with the BoundingBoxes system per scene; no pipeline, renderer, transfers or GPU systems. Needs no window.
        bool gpu = true;
    };

    // The one rendering entry point: backend lifecycle (headless or windowed),
    // pass-pipeline bootstrap, the per-frame scene upload systems, transfer
    // pumping, and presentation. The host owns the application shell, the
    // world, and the entity transform manager; the runtime registers five
    // named systems into that world — BeginFrame, BoundingBoxes (POST_UPDATE,
    // after the world module's "TransformMatrices"), SceneGpuUpload (PRE_RENDER),
    // TransferSubmission, Render — and services RenderViews and
    // SceneGpuStream for bindings and streaming state. That world is scene 0;
    // attachWorld draws further worlds through the same GPU scene.
    class SPITE_RENDER_RUNTIME_API RenderRuntime
    {
    public:
        RenderRuntime(const RenderRuntimeDesc& desc, ecs::World& world, ServiceLocator& worldServices);
        ~RenderRuntime();
        // Called after GPU consumers detach and before destroying the runtime for a device restart.
        void prepareRestart();

        void initialize();
        // false when built with RenderRuntimeDesc::gpu false: no device exists
        [[nodiscard]] bool hasDevice() const { return m_rendering != nullptr; }
        // Blocks until all submitted device work completes. Hosts tearing down
        // GPU-owning consumers (asset runtimes, tools) call this first. Without a device: nothing to wait for.
        void waitIdle();
        // Device memory heaps with their current budget and usage; the input
        // to resolveGpuBudget. Empty without a device.
        [[nodiscard]] std::vector<GpuHeapMemoryInfo> gpuHeaps() const;

        RenderView& view(const u32 index = 0) { return m_views->views[index]; }
        [[nodiscard]] u32 viewCount() const { return static_cast<u32>(m_views->views.size()); }
        // the view drawn onto the swapchain (default 0); NO_SWAPCHAIN_VIEW draws
        // every view offscreen. A change rebuilds the graph before the next frame.
        // False, and nothing changes, for an index past the views or a probe capture view (it has no output).
        bool setSwapchainView(const u32 index);
        [[nodiscard]] u32 swapchainView() const { return m_views->swapchainView; }
        // the view the game's camera draws (RenderViews::gameView), set by the host
        void setGameView(const u32 index) { m_views->gameView = index; }
        [[nodiscard]] u32 gameView() const { return m_views->gameView; }
        SceneGpuStream& sceneGpuStream() { return *m_sceneGpuStream; }

        // Draws another world through this runtime's GPU scene: registers its
        // BoundingBoxes and SceneGpuUpload systems and slot hooks on it, and
        // returns its scene id. Views with RenderView::scene equal to the id
        // draw it, lit by its lights. The host updates attached worlds together
        // with the runtime's world (World::updateWorlds) and detaches a world
        // before destroying it.
        u32 attachWorld(ecs::World& world);
        void detachWorld(u32 scene);
        // A scene no view draws for a while (the edit scene during Play) gives its instance slots back at once and
        // writes no objects while suspended; resumed (between frames), its renderables take slots again at once and
        // the next upload writes their records, drawn that frame. Released slots are handed over in place, so a
        // suspend paired with another scene's spawn (or a resume with another's detach) needs no more slots than the
        // larger scene.
        void suspendScene(u32 scene);
        void resumeScene(u32 scene);
        // cumulative scene-upload telemetry (SceneGpuStatsSingleton is owned
        // here but lives on the world's service container)
        SceneGpuStatsSingleton& sceneGpuStats() { return *m_sceneGpuStats; }
        // every view's fitted sun cascades (the shadowCascades table)
        [[nodiscard]] const SunShadows& sunShadows() const { return *m_sunShadows; }
        // marks the view's cascades dirty for the next frame; the cascades marked (SunShadows::invalidate)
        u32 invalidateShadows(const u32 view) { return m_sunShadows->invalidate(view); }
        // the same for one light's shadow maps, or every light's for an undefined entity; the slots marked
        u32 invalidateShadowMaps(const u32 view, const Entity light)
        {
            return m_sunShadows->invalidateMaps(view, light);
        }
        // the view's row of the upscale table: its scale and extents (a view with no extent shows the window's), what
        // it asked for and what runs, the state of its jitter cycle and the cost of its Upscale_* passes
        [[nodiscard]] UpscaleRow upscaleRow(u32 view) const;

        // Swaps the running SpiteRenderPipeline for the build at path (empty: SpiteRenderPipeline.dll beside the
        // executable). Call between frames. The new build is mapped from a shadow copy and checked first: refused
        // (missing, unloadable, no entry point, other API stamp or toolset), the running build keeps rendering
        // and the reason is returned, logged and kept in the status. Otherwise its pass modules replace the old
        // ones in one step (drop, register, finalize: the graph recompiles next frame; buffers the old passes
        // created retire by frame slot; a pass of the same name keeps its on/off switch, so a view mode stays)
        // and the old image is freed, or kept mapped while a profiler may hold names
        // from it. Global resources stay as the first build registered them.
        Expected<RenderPipelineStatus> reloadPipeline(const std::filesystem::path& path = {});
        // What a build's descriptor must hold for reloadPipeline to accept it: the host's API stamp and toolset (a
        // descriptor left at its zero defaults is refused) and both entry functions.
        [[nodiscard]] static Expected<void> checkPipelineDescriptor(const RenderPipelineDescriptor& descriptor);
        // Once per frame, at the frame boundary: reloads the default DLL once it was rewritten and its directory
        // has been quiet for PIPELINE_SETTLE. One non-blocking wait on a change notification otherwise.
        void pollPipelineReload();
        [[nodiscard]] const RenderPipelineStatus& pipelineStatus() const { return m_pipelineStatus; }

    private:
        void registerSystems();
        // the world's scene: its per-world stream on its services, slot hooks
        // and the BoundingBoxes and SceneGpuUpload systems
        u32 addScene(ecs::World& world, const char* moduleName);

        // a linker writes the DLL in several steps, then its PDB
        static constexpr std::chrono::milliseconds PIPELINE_SETTLE{500};

        RenderRuntimeDesc m_desc;
        ecs::World& m_world;
        ServiceLocator& m_services;

        // the running SpiteRenderPipeline; freed after m_rendering (the graph's passes run its code)
        struct PipelineImage
        {
            ShadowLibrary library;
            ~PipelineImage();
        } m_pipelineImage;
        const RenderPipelineDescriptor* m_pipeline = nullptr;
        RenderPipelineStatus m_pipelineStatus;
        std::filesystem::path m_pipelineDefault;
        void* m_pipelineWatch = nullptr; // a change-notification HANDLE on the default DLL's directory
        bool m_pipelineChanged = false;
        std::chrono::steady_clock::time_point m_pipelineChangedAt;
        std::filesystem::file_time_type m_pipelineWritten;

        // declared before the scene state: destroyed after it, so the scenes
        // can release their buffers through the still-live resource manager
        std::unique_ptr<RenderingManager> m_rendering;
        std::unique_ptr<RenderViews> m_views;
        std::unique_ptr<SunShadows> m_sunShadows;
        ShadowCascadeCountsSingleton* m_shadowCounts = nullptr; // owned by the service container
        std::unique_ptr<SceneGpuStream> m_sceneGpuStream;
        SceneGpuStatsSingleton* m_sceneGpuStats = nullptr;
        CameraMatricesSingleton* m_cameraMatrices = nullptr;
        // scene id = index; a detached scene leaves a null entry, so ids are never reused
        std::vector<std::unique_ptr<WorldGpuScene>> m_scenes;
    };
} // namespace spite
