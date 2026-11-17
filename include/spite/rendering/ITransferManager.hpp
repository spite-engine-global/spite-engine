#pragma once
#include <future>
#include <span>

#include "base/Schema.hpp"
#include "rendering/RenderResourceHandles.hpp"

namespace spite
{
    class IRenderCommandBuffer;

    // SameFrame uploads enqueued before processTransfers are recorded by it and
    // visible to that frame's graphics work: they record ahead of Deferred ones
    // they do not overlap, processTransfers waits for staging space when the ring
    // is full of in-flight uploads, and the manager pins a wait on the next
    // graphics submission. Deferred uploads belong
    // to the streaming path — their TransferHandle resolves to the timeline value
    // that the caller must hand to IRenderer::addAsyncTransferWait before the
    // consuming frame is submitted.
    enum class TransferSync : u8
    {
        Deferred,
        SameFrame
    };

    struct BufferUploadBatch
    {
        BufferHandle buffer;
        const void* data;
        sizet size;
        sizet offset;
    };

    // One queued upload stream feeding the dedicated transfer queue. Uploads are
    // batched, submitted against a timeline semaphore, and handed back to the
    // graphics queue through queue-ownership release/acquire barriers.
    // cumulative since construction; perf tooling diffs them over a window
    struct TransferStats
    {
        u64 requests = 0; // requests recorded into submissions
        u64 bytesStaged = 0; // bytes copied through the staging ring
        u64 submissions = 0;
        u64 hazardBarriers = 0; // in-batch barriers between overlapping transfers
        u64 sameFrameWaits = 0; // host waits for staging space a SameFrame upload needed
        u64 frameWrites = 0; // writeBufferInFrame edits queued (material records), which stage nothing
        SPITE_OBSERVED_SCHEMA(TransferStats, requests, bytesStaged, submissions, hazardBarriers, sameFrameWaits,
                              frameWrites)
    };

    // One block of a texture upload: `size` bytes at `dataOffset` fill the width x height texels of mip level
    // `mipLevel` of array layer `arrayLayer` (a cube face: +X, -X, +Y, -Y, +Z, -Z, then the next cube's six),
    // tightly packed (rows of size / height bytes, a multiple of the format's texel size). A region past the
    // manager's staging chunk splits into bands of whole rows on the way in.
    // A block-compressed format (BC1, BC4, BC5, BC7: 4x4 texel blocks) is packed in block rows, ceil(width / 4)
    // blocks each and ceil(height / 4) of them: width and height stay texels, each a multiple of 4 or the mip
    // level's own extent (a level below one block still holds a whole block), and a band is whole block rows.
    struct TextureUploadRegion
    {
        sizet dataOffset = 0;
        sizet size = 0;
        u32 mipLevel = 0;
        u32 width = 1;
        u32 height = 1;
        u32 arrayLayer = 0;
    };

    class ITransferManager
    {
    public:
        virtual ~ITransferManager() = default;

        using TransferHandle = std::future<u64>;

        virtual TransferHandle uploadBuffer(BufferHandle destination, const void* data, sizet size, sizet offset,
                                            TransferSync sync = TransferSync::Deferred) = 0;
        virtual TransferHandle uploadBuffersBatch(std::span<const BufferUploadBatch> uploads,
                                                  TransferSync sync = TransferSync::Deferred) = 0;

        virtual TransferHandle uploadTexture(TextureHandle destination, const void* data, sizet size, u32 width,
                                             u32 height, u32 depth, TransferSync sync = TransferSync::Deferred) = 0;

        // Uploads every region (any mip level and array layer) of one texture in one logical upload: the handle
        // resolves to the timeline value after which the whole texture, every level and layer of its image, is
        // readable on the graphics queue. A region or a set of regions larger than half the staging ring goes out in
        // several submissions; the texture stays in TRANSFER_DST_OPTIMAL between them and is released to graphics
        // only by the last.
        // A texture upload makes the whole image's contents undefined first: regions must cover every level and layer
        // that is sampled afterwards.
        virtual TransferHandle uploadTextureRegions(TextureHandle destination, const void* data,
                                                    std::span<const TextureUploadRegion> regions,
                                                    TransferSync sync = TransferSync::Deferred) = 0;

        // Device-to-device copy on the transfer queue — the resize/compaction
        // primitive behind buffer growth. Ordered after the queue's prior
        // submissions (uploads) by its timeline chaining; the caller must keep
        // the source alive until the returned timeline value completes.
        virtual TransferHandle copyBuffer(BufferHandle source, BufferHandle destination, sizet size,
                                          sizet sourceOffset = 0, sizet destinationOffset = 0,
                                          TransferSync sync = TransferSync::Deferred) = 0;

        // Frame-ordered in-place edit of live data frames in flight may read
        // (size <= 64 KiB, offset and size multiples of 4): recorded on the
        // graphics queue into the next frame's primary command buffer, after
        // every earlier frame's reads and before this frame's. Without a
        // renderer no frame reads it and it goes through the transfer queue.
        // The destination must not be a heap that can grow: a growth copies
        // the old buffer on the transfer queue and a later in-frame write to
        // the old buffer is lost.
        virtual void writeBufferInFrame(BufferHandle destination, const void* data, sizet size, sizet offset) = 0;

        // Submits every queued upload as one batch. Call once per frame before rendering.
        virtual void processTransfers() = 0;

        // The timeline counter's current value: every submission up to this
        // value has completed on the device. Readiness without pinning — a
        // caller whose upload resolved to a timeline value compares it here.
        virtual u64 completedTransfers() = 0;

        // Host-drains all queued and in-flight uploads (loading screens, shutdown).
        virtual void waitForTransfers() = 0;

        // nothing queued and every submission completed on the device
        virtual bool idle() = 0;

        virtual TransferStats stats() const = 0;

        // Records queue-ownership acquire barriers, then the queued
        // writeBufferInFrame edits, into the frame's primary command buffer.
        virtual void recordPendingAcquireBarriers(IRenderCommandBuffer& cmd) = 0;
    };
} // namespace spite
