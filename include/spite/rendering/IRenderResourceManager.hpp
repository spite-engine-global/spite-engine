#pragma once
#include <future>

#include "base/Schema.hpp"
#include "rendering/GraphicsDescs.hpp"
#include "rendering/RenderResourceHandles.hpp"

namespace spite
{
    class IBuffer;
    class IImageView;
    class ISampler;

    using TransferHandle = std::future<u64>;

    // Live resource counts. A destroyed resource leaves its count at once; its GPU object waits in pendingDestroys
    // until its frame slot's fence retires it.
    struct RenderResourceStats
    {
        u32 buffersLive = 0;
        u32 texturesLive = 0;
        u32 imageViewsLive = 0;
        u32 samplersLive = 0;
        u32 pendingDestroys = 0;
        SPITE_OBSERVED_SCHEMA(RenderResourceStats, buffersLive, texturesLive, imageViewsLive, samplersLive,
                              pendingDestroys)
    };

    class IRenderResourceManager
    {
    public:
        virtual ~IRenderResourceManager() = default;

        // --- Frame lifecycle ---
        // Called once per frame slot (0..maxFramesInFlight-1) before the slot's
        // work is recorded. Destroys requested while slot N was in flight are
        // applied here: the slot's fence has proven no submission references
        // them anymore. Must be called before any destroy of the new frame.
        virtual void onFrameBegin(u32 frameSlot) = 0;

        // --- Resource Management ---
        virtual BufferHandle createBuffer(const BufferDesc& desc, const char* name = nullptr) = 0;
        virtual void destroyBuffer(BufferHandle handle) = 0;
        // CPU_ONLY / CPU_TO_GPU / GPU_TO_CPU buffers are mapped at creation: mapBuffer returns that pointer and
        // unmapBuffer does nothing. Writes need no unmap to reach the GPU; ordering against in-flight frames is the
        // caller's (per-frame-slot buffers).
        virtual void* mapBuffer(BufferHandle handle) = 0;
        virtual void unmapBuffer(BufferHandle handle) = 0;
        // Makes host reads of a mapped range written by the GPU coherent
        // (no-op on host-coherent memory).
        virtual void invalidateBufferRange(BufferHandle handle, u64 offset, u64 size) = 0;
        virtual IBuffer& getBuffer(BufferHandle handle) = 0;
        virtual const IBuffer& getBuffer(BufferHandle handle) const = 0;
        virtual const BufferDesc& getBufferDesc(BufferHandle handle) const = 0;
        // Non-asserting liveness check: false once the buffer was destroyed.
        virtual bool isBufferValid(BufferHandle handle) const = 0;

        virtual TextureHandle createTexture(const TextureDesc& desc, const char* name = nullptr) = 0;
        virtual void destroyTexture(TextureHandle handle) = 0;
        virtual const TextureDesc& getTextureDesc(TextureHandle handle) const = 0;

        virtual ImageViewHandle createImageView(const ImageViewDesc& desc, const char* name = nullptr) = 0;
        virtual void destroyImageView(ImageViewHandle handle) = 0;
        virtual IImageView& getImageView(ImageViewHandle handle) = 0;
        virtual const IImageView& getImageView(ImageViewHandle handle) const = 0;

        virtual SamplerHandle createSampler(const SamplerDesc& desc, const char* name = nullptr) = 0;
        virtual void destroySampler(SamplerHandle handle) = 0;
        virtual ISampler& getSampler(SamplerHandle handle) = 0;
        virtual const ISampler& getSampler(SamplerHandle handle) const = 0;

        virtual RenderResourceStats stats() const = 0;
    };
} // namespace spite
