#pragma once
#include <span>
#include <string_view>
#include <vector>

#include "base/InternedString.hpp"
#include "rendering/GraphicsDescs.hpp"
#include "rendering/GraphicsTypes.hpp"

namespace spite
{
    class IRenderCommandBuffer;
    class ReadbackManager;
    class RenderGraph;
    class ISecondaryRenderCommandBuffer;
    struct ImageViewHandle;
    struct TextureHandle;
    struct RenderDeviceCapabilities;

    struct GpuPassTiming
    {
        InternedString pass;
        double ms = 0.0;
    };

    // One timed GPU frame (graphics-queue timestamps, see GpuPassTimer): frame is the recorded-frame number it timed
    // (IRenderer::recordedFrames while it recorded); frameMs = outsideGraphMs + the passes' sum. passes is empty when
    // the device has no timestamps or per-pass timers are off.
    struct GpuFrameTimings
    {
        u64 frame = 0;
        double frameMs = 0.0;
        double outsideGraphMs = 0.0;
        std::vector<GpuPassTiming> passes;
    };

    // A frame fence that has not signaled within GraphicsConfig::gpuHangTimeoutMs: the renderer skips
    // frames while it lasts (the frame thread and the control channel keep running) and goes fatal with a crash dump
    // once it is gpuHangFatalMs old. slot is the frame slot whose fence waits, frame the recorded-frame number
    // submitted into it, lastCompletedFrame the last frame whose GPU timings were collected (0: none or timers off).
    struct GpuHang
    {
        u32 slot = 0;
        u64 frame = 0;
        u64 lastCompletedFrame = 0;
        u64 waitedMs = 0;
        u64 fatalAfterMs = 0; // 0: never
    };

    // How the swapchain presents: Fifo waits for the vertical blank (vsync); Mailbox and Immediate do not.
    enum class PresentMode : u8
    {
        Fifo,
        Mailbox,
        Immediate,
    };

    // The name the window query and a perf capture's environment block report
    [[nodiscard]] inline std::string_view presentModeName(const PresentMode mode)
    {
        switch (mode)
        {
        case PresentMode::Mailbox:
            return "mailbox";
        case PresentMode::Immediate:
            return "immediate";
        case PresentMode::Fifo:
        default:
            return "fifo";
        }
    }

    class Window;

    // an OS window's own presentation target, acquired and presented in the main swapchain's frame. Its
    // image is cleared and in COLOR_ATTACHMENT_OPTIMAL when the graph runs; a pass draws into `view` and leaves it
    // there, and the renderer presents it.
    struct WindowTarget
    {
        u64 window = 0;
        TextureHandle texture;
        ImageViewHandle view;
        Extent2D extent;
    };

    // The abstract interface for all rendering operations.
    class IRenderer
    {
    public:
        virtual ~IRenderer() = default;
        virtual const RenderDeviceCapabilities& getCapabilities() const = 0;
        virtual void waitIdle() = 0;
        // --- Frame Lifecycle ---
        virtual IRenderCommandBuffer* getPrimaryCb() = 0;
        virtual IRenderCommandBuffer* beginFrame() = 0;
        virtual IRenderCommandBuffer* beginEmptyFrame() = 0;
        virtual void renderFrame(IRenderCommandBuffer& commandBuffer) = 0;
        virtual void endFrameAndSubmit(IRenderCommandBuffer& commandBuffer, bool present) = 0;
        virtual void recreateSwapchain() = 0;
        virtual void setRenderGraph(RenderGraph* renderGraph) = 0;
        virtual bool wasSwapchainRecreated() = 0;
        virtual ImageViewHandle getCurrentSwapchainImageView() const = 0;
        virtual TextureHandle getCurrentSwapchainTextureHandle() const = 0;
        virtual Format getSwapchainFormat() const = 0;
        virtual Extent2D getSwapchainExtent() const = 0;
        // swapchain recreations since the renderer started
        virtual u32 getSwapchainRecreateCount() const = 0;
        virtual u32 getSwapchainImageCount() const = 0;
        // Asks for vsync (Fifo) or not (Mailbox, else Immediate, else Fifo); a change rebuilds the swapchain at the
        // start of the next beginFrame. Calling it every frame with the same value costs nothing.
        virtual void setVsync(bool vsync) = 0;
        // the mode the swapchain presents with now
        [[nodiscard]] virtual PresentMode presentMode() const = 0;
        virtual u32 getCurrentFrameIndex() const = 0;
        // Blocks until the submission recorded for this frame slot has fully
        // completed on the GPU (its fence signals). Debug/tooling use: forces
        // serial frame execution at the caller's request.
        virtual void waitForFrameComplete(u32 frameSlot) = 0;
        // Frame-slot-scoped GPU readback; the implementation drives its slot
        // completion from the frame fence waits.
        virtual ReadbackManager& getReadbackManager() = 0;

        virtual void addAsyncTransferWait(u64 waitValue) = 0;

        // Frames begun so far (beginFrame and beginEmptyFrame); while a frame records, its own number.
        [[nodiscard]] virtual u64 recordedFrames() const = 0;
        // The last frame whose timestamps were collected (when its slot came around again); frame 0 before any.
        [[nodiscard]] virtual const GpuFrameTimings& lastGpuFrameTimings() const = 0;
        // The frame fence hang in progress, null while frames complete (GpuHang).
        [[nodiscard]] virtual const GpuHang* gpuHang() const { return nullptr; }

        // a swapchain on the window's own surface, presenting without vsync (Mailbox, else Immediate, else
        // Fifo); false when its surface cannot present here (the window then shows nothing of ours).
        virtual bool addWindowTarget(Window& window)
        {
            (void)window;
            return false;
        }
        virtual void removeWindowTarget(u64 window) { (void)window; }
        // The targets this frame acquired an image of (a minimized or busy window sits a frame out).
        [[nodiscard]] virtual std::span<const WindowTarget> acquiredWindowTargets() const { return {}; }
    };
} // namespace spite
