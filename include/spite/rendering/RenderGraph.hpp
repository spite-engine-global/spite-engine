#pragma once
#include <algorithm>
#include <any>
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "base/ApplicationConfig.hpp"
#include "base/Schema.hpp"
#include "rendering/GraphicsDescs.hpp"
#include "rendering/IBindlessResourceManager.hpp"
#include "rendering/RenderingExport.hpp"
#include "rendering/RenderGraphUsages.hpp"
#include "rendering/RenderResourceHandles.hpp"
#include "rendering/Synchronization.hpp"
#include "base/InternedString.hpp"

namespace spite
{
    class ServiceLocator;
    constexpr const char* SWAPCHAIN_NAME = "SwapchainOutput";

    // A graph name inside view k: "name#k"; view 0 keeps the plain name.
    SPITE_RENDERING_API InternedString viewScopedName(InternedString name, u32 view);
    // What an offscreen view renders into: a per-frame-slot graph texture at the
    // view's extent in the swapchain format, sampled and read back by name. A
    // pass that writes viewTargetName(0) writes its own view's output: this
    // texture, or the swapchain for the swapchain view (RenderGraph::viewOutput).
    inline InternedString viewTargetName(const u32 view) { return viewScopedName("ViewTarget", view); }

    // The render scales a view accepts: below UpscalerConfig::MIN_SCALE the upscale has too little to work from,
    // above 1 the view would supersample (no path for it); a non-finite or non-positive scale means 1.
    constexpr f32 clampRenderScale(const f32 scale)
    {
        return scale >= UpscalerConfig::MIN_SCALE ? (scale < 1.0f ? scale : 1.0f)
                                                  : (scale > 0.0f ? UpscalerConfig::MIN_SCALE : 1.0f);
    }
    // A view's render extent (TextureExtent::VIEWPORT) from its display extent: the display's rounded to the scale
    // on each axis, at least 1; the display extent itself at scale 1 and when it is empty (a headless view).
    inline Extent2D renderExtentFor(const Extent2D display, const f32 scale)
    {
        const f32 clamped = clampRenderScale(scale);
        if (clamped == 1.0f || display.width == 0 || display.height == 0)
        {
            return display;
        }
        return {std::max(1u, static_cast<u32>(std::lround(static_cast<f32>(display.width) * clamped))),
                std::max(1u, static_cast<u32>(std::lround(static_cast<f32>(display.height) * clamped)))};
    }
    // The render pixel under a display pixel's centre (a click): the pixel itself at scale 1. A pixel outside the
    // display stays outside the render extent, so it reads as a miss there too.
    constexpr u32 renderPixelFor(const u32 displayPixel, const u32 displaySize, const u32 renderSize)
    {
        if (displayPixel >= displaySize || renderSize == 0)
        {
            return displayPixel;
        }
        return static_cast<u32>((u64{2} * displayPixel + 1) * renderSize / (u64{2} * displaySize));
    }
    constexpr u32 INVALID_PASS_INDEX = std::numeric_limits<u32>::max();

    class NamedBufferRegistry;
    class ISecondaryRenderCommandBuffer;
    class IRenderer;
    struct ShaderDescription;
    class IRenderDevice;
    class IRenderCommandBuffer;

    class RGPass;
    class RGBuilder;
    class RenderGraph;

    enum class RGPassType : u8
    {
        UNDEFINED, // Will cause assertion if not set
        GRAPHICS,
        COMPUTE
    };

    // A declared pass as a table row: culled is the last compile's verdict;
    // clearsSwapchain marks the frame's first swapchain write when it LOADs;
    // barriers counts the barriers recorded before the pass, exitBarriers its
    // group's brackets recorded after it; skipped: its group did not run in the
    // last executed frame.
    struct RenderGraphPassRow
    {
        std::string pass;
        u32 view = 0;
        std::string owner;
        std::string type;
        bool active = true;
        bool culled = false;
        bool clearsSwapchain = false;
        u32 barriers = 0;
        std::string group;
        bool skipped = false;
        u32 exitBarriers = 0;
        u32 reads = 0; // declared resource reads
        u32 writes = 0; // declared attachment and storage writes
        bool global = false; // view-independent: runs whatever view 0 shows
        bool turnedOff = false; // inactive because a control command (setPassEnabled, setPassesEnabled) said so
        SPITE_OBSERVED_SCHEMA(RenderGraphPassRow, pass, view, owner, type, active, culled, clearsSwapchain, barriers,
                              group, skipped, exitBarriers, reads, writes, global, turnedOff)
    };

    // A graph resource as a table row. A texture's width, height and mipLevels are the last compile's resolved
    // values; format is the Format enum value; bufferBytes is 0 for a texture.
    struct RenderGraphResourceRow
    {
        std::string name;
        std::string kind; // "texture" or "buffer"
        u32 view = 0;
        bool imported = false;
        bool perFrame = false;
        bool persistent = false;
        u32 width = 0;
        u32 height = 0;
        u32 mipLevels = 0;
        u32 arrayLayers = 0;
        u32 format = 0;
        u64 bufferBytes = 0;
        SPITE_OBSERVED_SCHEMA(RenderGraphResourceRow, name, kind, view, imported, perFrame, persistent, width, height,
                              mipLevels, arrayLayers, format, bufferBytes)
    };

    // Why RenderingManager compiles the graph: each rebuild is counted in RenderGraphStats and logged.
    enum class RenderGraphRebuild : u8
    {
        INITIAL,
        RETRY, // a failed build, once descriptor capacity returned
        GENERATION, // RenderGraphGenerationSingleton advanced (its source names the requester)
        SWAPCHAIN_VIEW,
        VIEW_EXTENT,
        RENDER_SCALE,
        SWAPCHAIN, // the renderer recreated the swapchain (resize, vsync, out of date, suboptimal)
        COUNT
    };
    [[nodiscard]] SPITE_RENDERING_API std::string_view renderGraphRebuildName(RenderGraphRebuild reason);

    // Graph-wide barrier state (the RenderGraph stats table): barriers recorded
    // outside any pass, and how often barriers were derived.
    struct RenderGraphStats
    {
        u32 initBarriers = 0; // UNDEFINED -> rest of persistent textures, after the last compile only
        u32 frameEndBarriers = 0; // persistent textures back to rest after the last pass
        u64 barrierCalculations = 0;
        u32 groups = 0;
        u32 groupsSkipped = 0; // in the last executed frame
        bool ready = false;
        u64 compileFailures = 0;
        u64 sampledDemand = 0;
        u64 storageDemand = 0;
        // RenderingManager rebuilds by RenderGraphRebuild reason
        u64 rebuildsRetry = 0;
        u64 rebuildsGeneration = 0;
        u64 rebuildsSwapchainView = 0;
        u64 rebuildsViewExtent = 0;
        u64 rebuildsRenderScale = 0;
        u64 rebuildsSwapchain = 0;
        SPITE_OBSERVED_SCHEMA(RenderGraphStats, initBarriers, frameEndBarriers, barrierCalculations, groups,
                              groupsSkipped, ready, compileFailures, sampledDemand, storageDemand, rebuildsRetry,
                              rebuildsGeneration, rebuildsSwapchainView, rebuildsViewExtent, rebuildsRenderScale,
                              rebuildsSwapchain)
    };

    // One barrier of the last compile as a table row. list: "preClear" or "pass" (recorded before the pass), "exit"
    // (its group's bracket after it), "init" or "frameEnd" (outside any pass: pass is empty); kind: "memory",
    // "buffer" or "texture"; stages and access are '|'-joined flag names, layouts the ImageLayout names (textures).
    struct RenderGraphBarrierRow
    {
        std::string pass;
        u32 view = 0;
        std::string list;
        std::string kind;
        std::string resource;
        std::string srcStage;
        std::string srcAccess;
        std::string dstStage;
        std::string dstAccess;
        std::string oldLayout;
        std::string newLayout;
        u32 baseMip = 0;
        u32 mipCount = 0;
        u32 baseLayer = 0;
        u32 layerCount = 0;
        SPITE_OBSERVED_SCHEMA(RenderGraphBarrierRow, pass, view, list, kind, resource, srcStage, srcAccess, dstStage,
                              dstAccess, oldLayout, newLayout, baseMip, mipCount, baseLayer, layerCount)
    };

    // One declared access of a pass as a table row: access is "read" (a sampled or storage read), "color" or "depth"
    // (an attachment) or "storage" (a storage write); write: it writes the resource.
    struct RenderGraphAccessRow
    {
        std::string pass;
        u32 view = 0;
        std::string resource;
        std::string access;
        bool write = false;
        SPITE_OBSERVED_SCHEMA(RenderGraphAccessRow, pass, view, resource, access, write)
    };

    // The --drop-barriers-before targets for barriers no pass records before itself.
    constexpr std::string_view DROP_INIT_BARRIERS = "graph:init";
    constexpr std::string_view DROP_FRAME_END_BARRIERS = "graph:frameEnd";
    constexpr std::string_view DROP_EXIT_BARRIERS_SUFFIX = ":exit"; // "<pass>:exit"

    // How to determine render area for graphics passes
    enum class RGRenderAreaMode : u8
    {
        EXPLICIT, // User provides exact dimensions
        MATCH_SWAPCHAIN, // Match swapchain dimensions
        MATCH_ATTACHMENT // Match first color attachment dimensions
    };

    // Describes how a written attachment/buffer should be handled
    struct RGWriteAttachmentInfo
    {
        AttachmentLoadOp loadOp = AttachmentLoadOp::DONT_CARE;
        AttachmentStoreOp storeOp = AttachmentStoreOp::STORE;
        // Used if loadOp is CLEAR for textures
        std::optional<ClearValue> clearValue = std::nullopt;
        // Used for buffer fill operations (alternative to texture clear)
        std::optional<u32> bufferFillValue = std::nullopt;
        // The one layer of a layered texture (arrayLayers > 1) an attachment renders into; required there.
        std::optional<u32> layer = std::nullopt;
    };

    struct PassRenderingInfo
    {
        std::vector<Format> colorAttachmentFormats;
        Format depthAttachmentFormat = Format::UNDEFINED;
        Rect2D renderArea;
    };

    // A record of a resource being read in a pass
    struct RGReadAccess
    {
        RGResourceHandle handle;
        RGResourceUsage usage;
    };

    // A record of a resource being written to in a pass, including attachment details
    struct RGWriteAccess
    {
        RGResourceAccess resource;
        RGWriteAttachmentInfo attachment;
    };

    // Explicit depth attachment info for a pass
    struct RGDepthAttachment
    {
        RGResourceHandle handle;
        RGResourceUsage usage;
        RGWriteAttachmentInfo attachmentInfo;
        bool isReadOnly = false;
    };

    // Color attachment with explicit slot index
    struct RGColorAttachment
    {
        u32 slot = 0; // Attachment index in the render pass
        RGResourceHandle handle;
        RGResourceUsage usage;
        RGWriteAttachmentInfo attachmentInfo;
    };

    class SPITE_RENDERING_API RGPass
    {
        friend class RenderGraph;
        friend class RGBuilder;

    public:
        template <typename PassData>
        const PassData& getData() const
        {
            return std::any_cast<const PassData&>(m_passData);
        }

        explicit RGPass(InternedString name);

        RGPass(RGPass&&) = default;
        RGPass& operator=(RGPass&&) noexcept = default;

        bool isGraphicsPass() const { return m_passType == RGPassType::GRAPHICS; }
        bool isComputePass() const { return m_passType == RGPassType::COMPUTE; }
        InternedString getName() const { return m_name; }
        RGPassType getPassType() const { return m_passType; }
        bool isActive() const { return m_isActive; }
        bool isTurnedOff() const { return m_turnedOff; }

    private:
        InternedString m_name;
        RGPassType m_passType = RGPassType::UNDEFINED;
        bool m_isActive = true;
        // the frame's first swapchain write LOADs it: cleared instead (calculatePassBarriers)
        bool m_clearsSwapchain = false;
        u32 m_barrierCount = 0; // recorded before it (preClear + pass), last barrier calculation
        u32 m_exitBarrierCount = 0; // its group's brackets recorded after it
        u32 m_group = INVALID_PASS_INDEX; // RGBuilder::inGroup, this compile
        u32 m_view = 0; // the view whose pass chain declared it
        bool m_global = false; // view-independent (declared in view 0 scope): runs whatever view 0 shows
        bool m_turnedOff = false; // inactive by a control command, not by the engine's own toggling
        InternedString m_owner; // the module whose setup declared it; removePasses drops by it

        std::any m_passData;

        std::function<void(RGBuilder&)> m_setup;
        std::function<void(IRenderCommandBuffer&, ServiceLocator*)> m_execute;

        u32 m_inDegree = 0;
        bool m_isCulled = true;
        bool m_cullRequested = false; // RGBuilder::cull, this compile
        std::vector<u32> m_successors{};

        // Resource accesses
        std::vector<RGReadAccess> m_reads{};
        std::vector<RGColorAttachment> m_colorAttachments{};
        std::optional<RGDepthAttachment> m_depthAttachment;
        std::vector<RGWriteAccess> m_storageWrites{};
        std::vector<InternedString> m_usedUbos{};

        // Render state (graphics passes only)
        RGRenderAreaMode m_renderAreaMode = RGRenderAreaMode::MATCH_SWAPCHAIN;
        Rect2D m_explicitRenderArea{};
        Rect2D m_renderArea{}; // Resolved during compile
        std::vector<Format> m_colorAttachmentFormats{};
        Format m_depthAttachmentFormat = Format::UNDEFINED;

        std::vector<InternedString> m_declaredUbos{};
    };

    class SPITE_RENDERING_API RenderGraph
    {
    private:
        friend class RGBuilder;
        friend class RGPass;

        struct RGResource
        {
            InternedString name;
            bool isImported = false;
            bool isPerFrame = false;
            u32 view = 0; // its extent classes resolve against this view

            std::variant<GpuResourceHandle, std::vector<GpuResourceHandle>> physicalHandles;
            std::variant<TextureDesc, BufferDesc> desc;
            // Bindless slots of a managed texture, one per frame slot when per-frame.
            std::vector<BindlessHandle> sampledIndices;
            // Per frame slot; a texture with mipViews has one per mip (slot-major).
            std::vector<BindlessHandle> storageIndices;
            // Resolved at compile (extent classes; TextureDesc::mipLevels 0 = full chain).
            Extent2D extent{};
            u32 mipLevels = 1;
            // Single-mip views of a chained storage texture, slot-major.
            std::vector<ImageViewHandle> mipViews;
            // Single-layer attachment views of a layered texture, slot-major.
            std::vector<ImageViewHandle> layerViews;
            // A persistent texture: contents survive frames, and every frame starts and ends in this usage's layout.
            std::optional<RGResourceUsage> rest;

            GpuResourceHandle getPhysicalHandle(u32 frameIndex) const
            {
                if (isPerFrame)
                {
                    return std::get<std::vector<GpuResourceHandle>>(physicalHandles)[frameIndex];
                }
                return std::get<GpuResourceHandle>(physicalHandles);
            }

            bool isTexture() const { return std::holds_alternative<TextureDesc>(desc); }
            bool isBuffer() const { return std::holds_alternative<BufferDesc>(desc); }

            const TextureDesc& getTextureDesc() const { return std::get<TextureDesc>(desc); }
            const BufferDesc& getBufferDesc() const { return std::get<BufferDesc>(desc); }
        };

        struct PhysicalResourceState
        {
            PipelineStage stage = PipelineStage::TOP_OF_PIPE;
            AccessFlags access = AccessFlags::NONE;
            ImageLayout layout = ImageLayout::UNDEFINED;
        };
        // The swapchain image as the acquire hands it over: VulkanRenderer waits the acquire semaphore at
        // COLOR_ATTACHMENT_OUTPUT (and TRANSFER), so a transition from here chains after that wait; from TOP_OF_PIPE
        // it could run while the presentation engine still reads the image.
        static constexpr PhysicalResourceState ACQUIRED_SWAPCHAIN{PipelineStage::COLOR_ATTACHMENT_OUTPUT,
                                                                  AccessFlags::NONE, ImageLayout::UNDEFINED};

        using BarrierList = std::vector<std::variant<MemoryBarrier2, BufferBarrier2, TextureBarrier2>>;

        // Graph-declared clears/fills run between the first two lists; exit
        // (group brackets) follows the pass.
        struct PassBarriers
        {
            BarrierList preClear;
            BarrierList pass;
            BarrierList exit;
        };

        struct PassGroup
        {
            InternedString name;
            InternedString owner;
            std::function<bool()> runs;
            bool skipped = false; // the last executed frame's verdict
        };

        GraphicsConfig m_config;

        IRenderDevice& m_device;
        ServiceLocator* m_services = nullptr;
        IBindlessResourceManager* m_bindless = nullptr;

        std::deque<RGResource> m_resources;
        std::deque<RGPass> m_passes;
        std::vector<u32> m_sortedPasses;
        std::vector<PassBarriers> m_passBarriers;
        // Persistent textures created by the last compile: UNDEFINED -> rest, recorded by the next execute only.
        BarrierList m_initBarriers;
        // After the last pass: persistent textures a live pass left outside their rest state go back to it.
        BarrierList m_frameEndBarriers;
        bool m_barriersDirty = false;
        u32 m_initBarrierCount = 0; // m_initBarriers as the last compile made them
        u64 m_barrierCalculations = 0;
        std::vector<PassGroup> m_groups;
        std::unordered_map<InternedString, u32> m_groupLookup;
        std::vector<Extent2D> m_viewExtents{Extent2D{}}; // display
        std::vector<Extent2D> m_renderExtents{Extent2D{}}; // m_viewExtents at each view's render scale
        u32 m_swapchainView = 0;
        std::vector<bool> m_viewActive; // per view, kept across recompiles (setViewActive)
        u32 m_declaringView = 0;
        bool m_declaringGlobal = false;
        InternedString m_declaringOwner;
        u32 m_buildingView = 0;
        u32 m_executingView = 0;
        u32 m_executingPass = INVALID_PASS_INDEX;
        bool m_ready = false;
        bool m_compileFailed = false;
        u64 m_compileFailures = 0;
        u64 m_sampledDemand = 0;
        u64 m_storageDemand = 0;
        std::array<u64, static_cast<sizet>(RenderGraphRebuild::COUNT)> m_rebuilds{};
        RenderGraphRebuild m_lastRebuild = RenderGraphRebuild::INITIAL;
        mutable std::unordered_map<u64, InternedString> m_viewNames;
        // after the last pass touching the swapchain: what the present barrier leaves from (as acquired while nothing
        // records: no compile, an unready graph, or no live pass touching it)
        PhysicalResourceState m_swapchainFinalState = ACQUIRED_SWAPCHAIN;

        std::unordered_map<InternedString, u32> m_passLookup;
        std::unordered_map<InternedString, RGResourceHandle> m_resourceNameLookup;
        std::unordered_map<u32, ImageViewHandle> m_managedImageViews;
        // barrier emission scratch, reused every pass so execute does not allocate
        std::vector<MemoryBarrier2> m_scratchMemoryBarriers;
        std::vector<BufferBarrier2> m_scratchBufferBarriers;
        std::vector<TextureBarrier2> m_scratchTextureBarriers;

        RGResourceHandle createManagedTexture(InternedString name, const TextureDesc& desc);
        RGResourceHandle createPerFrameManagedTexture(InternedString name, const TextureDesc& desc);
        RGResourceHandle createPersistentManagedTexture(InternedString name, const TextureDesc& desc,
                                                        const RGResourceUsage& rest);
        RGResourceHandle createManagedBuffer(InternedString name, const BufferDesc& desc);
        RGResourceHandle createPerFrameManagedBuffer(InternedString name, const BufferDesc& desc);

        void read(InternedString name, u32 passIndex, const RGResourceUsage& usage);
        void writeColorAttachment(InternedString name, u32 passIndex, u32 slot, const RGResourceUsage& usage,
                                  const RGWriteAttachmentInfo& attachmentInfo);
        void writeDepth(InternedString name, u32 passIndex, const RGResourceUsage& usage,
                        const RGWriteAttachmentInfo& attachmentInfo, bool isReadOnly);
        void writeStorage(InternedString name, u32 passIndex, const RGResourceUsage& usage,
                          const RGWriteAttachmentInfo& attachmentInfo);

        RGResource& getResource(RGResourceHandle handle);
        const RGResource& getResource(RGResourceHandle handle) const;

        // a TransferSrc read of the named resource among the pass's declared reads
        bool copiesFrom(const RGPass& pass, InternedString name) const;

        // Swapchain helpers
        bool isSwapchainResource(const RGResource& resource) const;
        [[nodiscard]] bool isLive(const RGPass& pass) const
        {
            return pass.m_isActive && (pass.m_global || m_viewActive[pass.m_view]);
        }
        [[nodiscard]] bool isSkipped(const RGPass& pass) const
        {
            return pass.m_group != INVALID_PASS_INDEX && m_groups[pass.m_group].skipped;
        }
        // visit(resource id, access kind, writes, reads): a LOAD attachment both writes and reads
        template <typename Visit>
        static void forEachAccess(const RGPass& pass, Visit&& visit);

    public:
        bool isSwapchainResource(InternedString name) const;

    private:
        // Barrier generation helpers
        void generateBarrierForResource(u32 resourceId, const RGResourceUsage& requiredUsage, AccessFlags loadAccess,
                                        AccessFlags storeAccess,
                                        std::unordered_map<u32, PhysicalResourceState>& resourceStates,
                                        BarrierList& barriers);
        bool hasGraphClear(const RGWriteAccess& write) const;

        bool needsBarrier(const PhysicalResourceState& current, const PhysicalResourceState& required) const;

        void buildDependencyGraph();
        void topologicalSort();
        bool createPhysicalResources(Extent2D swapchainExtent);
        bool hasDescriptorCapacity() const;
        void compilePassAttachments(Format swapchainFormat, Extent2D swapchainExtent);
        // the extent a resource of the given view resolves its extent class against
        [[nodiscard]] Extent2D resolveExtent(const TextureDesc& desc, u32 view, Extent2D swapchainExtent) const;
        void calculatePassBarriers();

        void executeGraphicsPass(RGPass& pass, IRenderCommandBuffer& cmd, IRenderer* renderer, u32 frameIndex);
        void executeComputePass(RGPass& pass, IRenderCommandBuffer& cmd);

        void emitBarriers(IRenderCommandBuffer& cmd, const BarrierList& barriers, IRenderer* renderer, u32 frameIndex);

        void clearPassResources(IRenderCommandBuffer& cmd, const RGPass& pass, u32 frameIndex);
        // the view an attachment renders through: its layer's view on a layered texture
        ImageViewHandle attachmentView(const RGResource& resource, const RGWriteAttachmentInfo& info,
                                       u32 frameIndex) const;
        bool registerBindless(RGResource& resource);
        static u32 bindlessIndex(const std::vector<BindlessHandle>& indices, const RGResource& resource, u32 frameIndex,
                                 u32 entriesPerSlot, u32 entry);

    public:
        // services are optional: pass lambdas receive them when present, and
        // viewport-target extent resolution falls back to the swapchain extent
        // without them (headless/test graphs). Without a bindless manager the
        // graph's textures get no bindless indices.
        RenderGraph(IRenderDevice& device, ServiceLocator* services, GraphicsConfig config,
                    IBindlessResourceManager* bindless = nullptr);

        RenderGraph(const RenderGraph& other) = delete;
        RenderGraph(RenderGraph&& other) noexcept = delete;
        RenderGraph operator=(const RenderGraph& other) = delete;
        RenderGraph operator=(RenderGraph&& other) noexcept = delete;

        ~RenderGraph();

        RGPass& addPass(InternedString name, std::function<void(RGBuilder&)> setup,
                        std::function<void(IRenderCommandBuffer&, ServiceLocator*)> execute = nullptr);

        // viewExtents: one display extent per view (renderViewCount). swapchainView
        // renders onto the swapchain, every other view into its view target;
        // NO_SWAPCHAIN_VIEW leaves the swapchain to view-independent passes (UI).
        // renderScales: per view (missing: 1), the render extent's share of the display extent (renderExtentFor).
        // Descriptor exhaustion logs one failed build and leaves the graph unready; execute then records no passes.
        void compile(Format swapchainFormat, Extent2D swapchainExtent, std::span<const Extent2D> viewExtents,
                     u32 swapchainView = 0, std::span<const f32> renderScales = {});
        void execute(IRenderCommandBuffer& cmd, IRenderer* renderer, u32 frameIndex);
        [[nodiscard]] bool ready() const { return m_ready; }
        // Capacity has returned after a failed build; rebuild imports and compile as usual.
        [[nodiscard]] bool canRetryCompile() const { return m_compileFailed && hasDescriptorCapacity(); }
        // The caller's reason for the next compile: counted in stats() and named if that build fails.
        void noteRebuild(RenderGraphRebuild reason);
        void clear();

        RGResourceHandle importTexture(InternedString name, TextureHandle handle, const TextureDesc& desc);
        RGResourceHandle importBuffer(InternedString name, BufferHandle handle, const BufferDesc& desc);
        RGResourceHandle importPerFrameBuffer(InternedString name, const std::vector<BufferHandle>& handles,
                                              const BufferDesc& desc);
        RGResourceHandle importPerFrameTexture(InternedString name, const std::vector<ImageViewHandle>& handles,
                                               const TextureDesc& desc);
        // Rebinds an already-imported buffer to new physical handles (buffer
        // growth): the RGResource identity and its barrier graph stay intact.
        void updateImportedBuffer(InternedString name, BufferHandle handle, const BufferDesc& desc);
        void updateImportedPerFrameBuffer(InternedString name, const std::vector<BufferHandle>& handles,
                                          const BufferDesc& desc);

        RGPass& getPass(u32 passIndex);
        const RGPass& getPass(u32 passIndex) const;
        RGResourceHandle getResource(InternedString name) const;
        // non-asserting variant: true when a pass or import registered the name
        [[nodiscard]] bool hasResource(InternedString name) const;
        // The graph's own textures (imports and the swapchain excluded) with their descs; names are view-scoped.
        // findTexture: null when the name is no such texture.
        void forEachTexture(const std::function<void(InternedString, const TextureDesc&)>& visit) const;
        [[nodiscard]] const TextureDesc* findTexture(InternedString name) const;
        // Every graph buffer, imports included, with its declared desc (an import's at import or its last update).
        void forEachBuffer(const std::function<void(InternedString, const BufferDesc&)>& visit) const;
        ImageViewHandle getImageView(InternedString name, u32 frameIndex) const;
        TextureHandle getTexture(InternedString name, u32 frameIndex) const;
        BufferHandle getBuffer(InternedString name, u32 frameIndex) const;
        // Bindless array indices of a managed texture (sampled / storage usage), assigned at compile.
        u32 textureIndex(InternedString name, u32 frameIndex) const;
        // A chained storage texture has one storage index per mip.
        u32 storageImageIndex(InternedString name, u32 frameIndex, u32 mip = 0) const;
        u32 mipLevels(InternedString name) const;
        // compiled size of a managed texture's mip 0
        Extent2D textureExtent(InternedString name) const;
        // the executing pass's view render extent (TextureExtent::VIEWPORT) and output extent (TextureExtent::DISPLAY)
        [[nodiscard]] Extent2D viewportExtent() const { return m_renderExtents[m_executingView]; }
        [[nodiscard]] Extent2D displayExtent() const { return m_viewExtents[m_executingView]; }

        // View scope: passes added while view k is declaring belong
        // to view k's pass chain, and their names, like every resource name
        // they use, resolve through viewName.
        void setDeclaringView(u32 view) { m_declaringView = view; }
        [[nodiscard]] u32 declaringView() const { return m_declaringView; }
        // Passes added while global is set are view-independent: an inactive view 0 does not skip them.
        void setDeclaringGlobal(const bool global) { m_declaringGlobal = global; }
        [[nodiscard]] bool declaringGlobal() const { return m_declaringGlobal; }
        // Passes added while an owner is declaring carry it, so the owner's
        // passes (and the closures into its module) can be dropped together.
        void setDeclaringOwner(InternedString owner) { m_declaringOwner = owner; }
        [[nodiscard]] InternedString declaringOwner() const { return m_declaringOwner; }
        // Drops the owner's passes; the graph executes nothing until the next
        // clear() + compile().
        void removePasses(InternedString owner);
        [[nodiscard]] u32 viewCount() const { return m_config.renderViewCount; }
        // The first view reserved for light probe capture (GraphicsConfig::probeCaptureView), or NO_SWAPCHAIN_VIEW
        // (~0u): only the modules that say onCaptureView declare passes in the capture views.
        [[nodiscard]] u32 captureView() const { return m_config.captureView(); }
        // whether the view is one of the capture views (GraphicsConfig::probeCaptureViews from captureView on)
        [[nodiscard]] bool isCaptureView(const u32 view) const { return m_config.isCaptureView(view); }
        [[nodiscard]] u32 captureViewCount() const { return m_config.captureViewCount(); }
        [[nodiscard]] u32 framesInFlight() const { return m_config.maxFramesInFlight; }
        [[nodiscard]] u32 executingView() const { return m_executingView; }
        // True while a pass executes that declared a TransferSrc read of the resource (the copy passes' readbacks).
        [[nodiscard]] bool executingPassCopiesFrom(InternedString name) const;
        // True when a pass of the last compile, active or not (culled ones excluded), declared a TransferSrc read
        // of the resource.
        [[nodiscard]] bool anyPassCopiesFrom(InternedString name) const;
        [[nodiscard]] u32 swapchainView() const { return m_swapchainView; }
        // what view k renders into: the swapchain or its view target
        [[nodiscard]] InternedString viewOutput(const u32 view) const
        {
            return view == m_swapchainView ? InternedString(SWAPCHAIN_NAME) : viewTargetName(view);
        }
        // name inside view k: viewTargetName(0) is the view's output; the plain
        // name for view 0, the swapchain and shared imports; anything else name#k.
        [[nodiscard]] InternedString viewName(InternedString name, u32 view) const;
        PassRenderingInfo getPassRenderingInfo(InternedString passName) const;
        // Barriers are re-derived for the new active set before the next execute.
        // Applies to the pass in every view.
        // byCommand: the switch is a control command's (the pass row's turnedOff while it stays off).
        void setPassActive(InternedString passName, bool active, bool byCommand = false);
        // Only the pass of this exact name (a row of the passes table), in no other view.
        void setPassActiveExact(InternedString passName, bool active, bool byCommand = false);
        // A skippable pass group in the declaring view (named like a pass:
        // "name#k" in view k), dropped with its owner's passes. Passes join it
        // in setup (RGBuilder::inGroup). Every execute reads runs() once: when
        // false, the group's passes record nothing. Every resource the group
        // touches leaves it in the state it entered with, so skipping changes
        // no other barrier and re-derives none (unlike setPassActive).
        void addPassGroup(InternedString name, std::function<bool()> runs);
        [[nodiscard]] RenderGraphStats stats() const;
        // An inactive view records none of its per-view passes (readers of its target
        // see it unwritten); global passes (setDeclaringGlobal) run regardless.
        void setViewActive(u32 view, bool active);
        [[nodiscard]] bool isViewActive(const u32 view) const { return m_viewActive[view]; }
        // The pass by its view-0 name; null when no setup declared it.
        [[nodiscard]] const RGPass* findPass(InternedString passName) const;
        // One row per declared pass, every view's, in declaration order (the `passes` table).
        [[nodiscard]] std::vector<RenderGraphPassRow> passRows() const;
        // One row per graph resource, in declaration order (the resources table).
        [[nodiscard]] std::vector<RenderGraphResourceRow> resourceRows() const;
        // One row per barrier of the last compile, in recording order (the barriers table).
        [[nodiscard]] std::vector<RenderGraphBarrierRow> barrierRows() const;
        // One row per declared access of every pass, in declaration order (the accesses table).
        [[nodiscard]] std::vector<RenderGraphAccessRow> accessRows() const;
        // The passes that stop receiving what the pass writes when setPassActive switches it off (the name in every
        // view): live passes, not among those, that read a resource whose last writer before them is one of those, in
        // declaration order, named as the passes table names them. Empty for a name no pass has.
        [[nodiscard]] std::vector<std::string> passDependents(InternedString passName) const;
        // Transition to present from wherever the last swapchain pass left the image
        // (a color write, or a transfer read such as a readback).
        [[nodiscard]] TextureBarrier2 presentBarrier(TextureHandle swapchainTexture) const;

        const std::vector<u32>& getSortedPassIds() const { return m_sortedPasses; }
    };

    class SPITE_RENDERING_API RGBuilder
    {
    public:
        RGBuilder(RenderGraph& graph, u32 passIndex);

        void asComputePass();

        // Declare this as a graphics pass with swapchain-sized render area
        void asGraphicsPass();

        void asGraphicsPass(const Rect2D& renderArea);

        // Declare this as a graphics pass, matching first attachment size
        void asGraphicsPassMatchingAttachment();

        // Instead of declaring anything: the pass sits out this compile (culled
        // in the passes table), as when the graph's shape makes it redundant.
        void cull();
        // The pass joins the group of this name in its view (RenderGraph::addPassGroup).
        void inGroup(InternedString group) const;
        // the view whose pass chain declared the pass
        [[nodiscard]] u32 view() const;

        RGResourceHandle createTexture(InternedString name, const TextureDesc& desc) const;
        // non-asserting existence query: has a producer or import registered the name
        [[nodiscard]] bool hasResource(InternedString name) const;
        // the same for a resource of another view's chain (the probe capture view's sky reads its host's cube)
        [[nodiscard]] bool hasResourceOfView(InternedString name, u32 view) const;
        RGResourceHandle createPerFrameTexture(InternedString name, const TextureDesc& desc) const;
        // A single-slot texture whose contents survive frames: it rests in rest.layout between frames and
        // leaves UNDEFINED once per created image (its contents are undefined until a pass writes them).
        RGResourceHandle createPersistentTexture(InternedString name, const TextureDesc& desc,
                                                 const RGResourceUsage& rest) const;
        RGResourceHandle createBuffer(InternedString name, const BufferDesc& desc) const;
        RGResourceHandle createPerFrameBuffer(InternedString name, const BufferDesc& desc) const;
        RGResourceHandle importBuffer(InternedString name, BufferHandle handle, const BufferDesc& desc) const;
        RGResourceHandle importPerFrameBuffer(InternedString name, const std::vector<BufferHandle>& handles,
                                              const BufferDesc& desc) const;
        RGResourceHandle importPerFrameTexture(InternedString name, const std::vector<ImageViewHandle>& handles,
                                               const TextureDesc& desc) const;

        void useUbo(InternedString uboName) const;

        // Read resource (any pass type)
        RGResourceHandle read(RGResourceHandle handle, const RGResourceUsage& usage) const;
        RGResourceHandle read(InternedString name, const RGResourceUsage& usage) const;
        // Read a resource of another view's chain (hasResourceOfView), as the pass's own view reads its own
        RGResourceHandle readOfView(InternedString name, u32 view, const RGResourceUsage& usage) const;

        // Write to color attachment at specific slot (graphics pass only)
        RGResourceHandle writeColorAttachment(InternedString name, u32 slot, const RGResourceUsage& usage,
                                              const RGWriteAttachmentInfo& attachmentInfo = {}) const;
        RGResourceHandle writeColorAttachment(RGResourceHandle handle, u32 slot, const RGResourceUsage& usage,
                                              const RGWriteAttachmentInfo& attachmentInfo = {}) const;

        // Write to depth attachment (graphics pass only)
        RGResourceHandle writeDepth(InternedString name, const RGResourceUsage& usage,
                                    const RGWriteAttachmentInfo& attachmentInfo = {}) const;

        // Read depth attachment (graphics pass only, read-only depth test)
        RGResourceHandle readDepth(InternedString name, const RGResourceUsage& usage) const;

        // Write to storage resource (buffer or image, any pass type)
        RGResourceHandle writeStorage(InternedString name, const RGResourceUsage& usage,
                                      const RGWriteAttachmentInfo& attachmentInfo = {}) const;
        RGResourceHandle writeStorage(RGResourceHandle handle, const RGResourceUsage& usage,
                                      const RGWriteAttachmentInfo& attachmentInfo = {}) const;

    private:
        RenderGraph& m_graph;
        u32 m_passIndex;

        // the name inside this pass's view (RenderGraph::viewName)
        InternedString scoped(InternedString name) const;
        bool isDepthStencilUsage(const RGResourceUsage& usage) const;
        bool isColorAttachmentUsage(const RGResourceUsage& usage) const;
        bool isStorageUsage(const RGResourceUsage& usage) const;
    };
} // namespace spite
