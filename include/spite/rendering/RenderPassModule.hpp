#pragma once

#include <functional>
#include <unordered_map>
#include <vector>

#include "base/InternedString.hpp"
#include "base/ServiceLocator.hpp"
#include "base/StringInterner.hpp"
#include "rendering/GraphicsDescs.hpp"
#include "rendering/IBindlessResourceManager.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderPassSetupService.hpp"
#include "rendering/RenderingExport.hpp"
#include "rendering/ShaderManager.hpp"
#include "rendering/Synchronization.hpp"

namespace spite
{
    class IBindlessResourceManager;
    class IRenderCommandBuffer;

    // What a pass execute lambda receives: the recorded command buffer plus the
    // services the pass needs (camera singletons, renderer frame index, graph
    // resources). Graph resources reach shaders as bindless indices and device
    // addresses in the pass's push constants; the global tables are
    // bound by the pass wrappers.
    // Helpers acquire service handles internally: graph lookups write RenderGraph, buffer-address lookups
    // also write IRenderResourceManager, and frameIndex reads IRenderer. Release any handle to the needed
    // service before calling a helper; holding it across the call can deadlock on the non-recursive lock.
    class SPITE_RENDERING_API RenderPassExecutionContext
    {
    private:
        IRenderCommandBuffer& m_commandBuffer;
        ServiceLocator& m_services;

        void pushConstantBytes(const void* data, u32 size) const;
        // a graph name inside the executing pass's view (RenderGraph::viewName)
        InternedString scoped(InternedString name) const;

    public:
        RenderPassExecutionContext(IRenderCommandBuffer& commandBuffer, ServiceLocator& services);

        IRenderCommandBuffer& commandBuffer() const;
        ServiceLocator& services() const;

        IBindlessResourceManager& bindless() const;
        RenderGraph& renderGraph() const;
        u32 frameIndex() const;
        // the view's render extent (RenderGraph::viewportExtent) and output extent (displayExtent): the same at scale 1
        Extent2D viewportExtent() const;
        Extent2D displayExtent() const;
        // the view whose pass chain is executing; graph names below resolve inside it
        u32 view() const;

        // compiled mip 0 size and mip count of a graph texture in this view
        Extent2D textureExtent(InternedString name) const;
        u32 mipLevels(InternedString name) const;
        // This frame slot's bindless index / device address of a graph resource.
        u32 textureIndex(InternedString name) const;
        // the same for a resource of another view's chain (RGBuilder::readOfView declared the read)
        u32 textureIndexOfView(InternedString name, u32 view) const;
        u32 storageImageIndex(InternedString name, u32 mip = 0) const;
        u64 bufferAddress(InternedString name) const;
        // The caller owns the buffer and synchronizes host writes with frame slots.
        u64 bufferAddress(BufferHandle buffer) const;
        // the same for a buffer of another view's chain
        u64 bufferAddressOfView(InternedString name, u32 view) const;

        template <typename T>
        void pushConstants(const T& data) const
        {
            static_assert(sizeof(T) <= PUSH_CONSTANT_BYTES, "push constants exceed the shared block");
            pushConstantBytes(&data, sizeof(T));
        }

        void copyBuffer(BufferHandle src, BufferHandle dst, u64 srcOffset, u64 dstOffset, u64 size) const;
        void copyImageToBuffer(TextureHandle src, BufferHandle dst, u64 dstOffset, u32 mipLevel, u32 arrayLayer,
                               u32 width, u32 height, u32 rowPitchBytes) const;
        void bindVertexBuffer(BufferHandle buffer, u32 binding = 0, u64 offset = 0) const;
        void bindIndexBuffer(BufferHandle buffer, u64 offset = 0) const;
        void draw(u32 vertexCount, u32 instanceCount = 1, u32 firstVertex = 0, u32 firstInstance = 0) const;
        void dispatch(u32 x, u32 y, u32 z = 1) const;
        void dispatchForViewport(u32 groupSizeX, u32 groupSizeY, u32 groupSizeZ = 1) const;
        void dispatchIndirectFromGraphBuffer(InternedString bufferName, u64 offset) const;
        void drawMeshTasksIndirectFromGraphBuffer(InternedString bufferName, u64 offset, u32 drawCount,
                                                  u32 stride) const;
        void drawIndexedIndirectFromGraphBuffer(InternedString bufferName, u64 offset, u32 drawCount, u32 stride) const;
        void drawIndexedIndirectCountFromGraphBuffer(InternedString bufferName, u64 offset,
                                                     InternedString countBufferName, u64 countBufferOffset,
                                                     u32 maxDrawCount, u32 stride) const;
        void drawIndirectCountFromGraphBuffer(InternedString bufferName, u64 offset, InternedString countBufferName,
                                              u64 countBufferOffset, u32 maxDrawCount, u32 stride) const;
    };

    // Authors one pass module: declares its pipelines from compact specs (all
    // share the bindless layout) and adds its passes to the render graph.
    //
    // LIFECYCLE — the lambdas captured here do not all re-run equally:
    //   - setup() (the module definition): runs ONCE per finalize. Anything
    //     captured into the lambdas below from runtime state (config values,
    //     capacities) is FROZEN forever — growth or reconfigure will never be
    //     seen again. Resolve such state through services inside the execute
    //     lambda instead (see the visible-capacity flicker fix).
    //   - GraphSetupFn (the per-pass builder lambda): re-runs on every render
    //     graph recompile (generation bump, resize), so declare-time resource
    //     sizing there tracks the current state.
    //   - ExecuteFn / dispatch / draw: run every frame.
    class SPITE_RENDERING_API RenderModuleBuilder
    {
    public:
        using GraphSetupFn = std::function<void(RGBuilder&)>;
        using ExecuteFn = std::function<void(RenderPassExecutionContext&)>;
        using PipelineResolverFn = std::function<PipelineHandle(RenderPassExecutionContext&)>;

    private:
        ServiceLocator& m_services;
        ShaderManager& m_shaderManager;
        RenderGraph& m_renderGraph;
        std::unordered_map<InternedString, PipelineHandle, InternedString::hash> m_pipelineHandles;

    public:
        explicit RenderModuleBuilder(ServiceLocator& services);

        ServiceLocator& services() const { return m_services; }
        RenderGraph& renderGraph() const { return m_renderGraph; }

        PipelineHandle declareGraphicsPipeline(InternedString name, const GraphicsPipelineSpec& spec);
        PipelineHandle declareComputePipeline(InternedString name, const ComputePipelineSpec& spec);
        PipelineHandle pipeline(InternedString name) const;

        // Raw pass: the execute lambda binds its own pipeline and the global tables.
        void addPass(InternedString name, GraphSetupFn setup, ExecuteFn execute = {});
        // Transfer-only pass (no pipeline, no dispatch): the setup declares
        // reads, the execute records copies (readback staging, debug dumps).
        void addCopyPass(InternedString name, GraphSetupFn setup, ExecuteFn execute = {});
        // The wrappers bind the pipeline and the global tables, then call draw/dispatch.
        void addGraphicsPass(InternedString name, GraphSetupFn setup, PipelineResolverFn resolvePipeline,
                             ExecuteFn draw);
        void addGraphicsPass(InternedString name, GraphSetupFn setup, PipelineHandle pipeline, ExecuteFn draw);
        // Draws one fullscreen triangle after beforeDraw (push constants).
        void addFullscreenPass(InternedString name, GraphSetupFn setup, PipelineResolverFn resolvePipeline,
                               ExecuteFn beforeDraw = {});
        void addFullscreenPass(InternedString name, GraphSetupFn setup, PipelineHandle pipeline,
                               ExecuteFn beforeDraw = {});
        void addComputePass(InternedString name, GraphSetupFn setup, PipelineResolverFn resolvePipeline,
                            ExecuteFn dispatch);
        void addComputePass(InternedString name, GraphSetupFn setup, PipelineHandle pipeline, ExecuteFn dispatch);
    };

    struct HighLevelRenderModuleDefinition
    {
        InternedString passName;
        RenderPassImplementation implementation = RenderPassImplementation::Default;
        int priority = 0;
        bool perView = false; // RenderPassModuleDefinition::perView
        RenderPassPlacement placement = RenderPassPlacement::RegistrationOrder;
        bool onCaptureView = false; // RenderPassModuleDefinition::onCaptureView
        std::function<bool(const RenderDeviceCapabilities&, const RenderPassSetupRuntimeConfig&)> isSupported;
        std::function<void(RenderModuleBuilder&)> setup;
    };

    SPITE_RENDERING_API RenderPassModuleDefinition
    createRenderPassModule(const HighLevelRenderModuleDefinition& definition);
} // namespace spite
