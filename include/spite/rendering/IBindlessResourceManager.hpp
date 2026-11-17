#pragma once

#include "base/Schema.hpp"
#include "rendering/GraphicsTypes.hpp"
#include "rendering/RenderResourceHandles.hpp"

namespace spite
{
    class IRenderCommandBuffer;

    // Every pipeline shares one layout: the three global tables (set 0 address
    // UBO, set 1 sampled and storage image arrays, set 2 samplers) and one
    // push-constant block of this size visible to every stage the device runs.
    constexpr u32 PUSH_CONSTANT_BYTES = 128;

    struct BindlessHandle
    {
        u32 index = 0;
        u32 generation = 0;
        bool isValid() const { return generation != 0; }
        BindlessHandle() = default;

        BindlessHandle(u32 idx, u32 gen) : index(idx), generation(gen) {}
    };

    // Table occupancy. Live counts entries registered or awaiting release
    // after an unregister (released once their frame retires), so the peak can
    // exceed what is registered at any one time.
    struct BindlessStats
    {
        u32 textureCapacity = 0;
        u32 texturesLive = 0;
        u32 texturesPeak = 0;
        u32 storageImageCapacity = 0;
        u32 storageImagesLive = 0;
        u32 storageImagesPeak = 0;
        u32 samplerSlotCapacity = 0;
        u32 samplerSlotsLive = 0; // the pinned slots included: 0 (fallback) and COMPARISON_SAMPLER_SLOT
        // a table was full: the register returned an invalid handle, or (samplers) bound slot 0. The oracle and
        // the idle check fail on a nonzero count
        u64 registrationFailures = 0;
        // Awaiting retirement, included in the corresponding live counts.
        u32 texturesPending = 0;
        u32 storageImagesPending = 0;
        // Slots retired for good when their generation reached its limit: outside the live counts, never registrable.
        u32 texturesRetired = 0;
        u32 storageImagesRetired = 0;
        SPITE_OBSERVED_SCHEMA(BindlessStats, textureCapacity, texturesLive, texturesPeak, storageImageCapacity,
                              storageImagesLive, storageImagesPeak, samplerSlotCapacity, samplerSlotsLive,
                              registrationFailures, texturesPending, storageImagesPending, texturesRetired,
                              storageImagesRetired)
    };

    class IBindlessResourceManager
    {
    public:
        virtual ~IBindlessResourceManager() = default;

        // The global address table of a frame slot as one view sees it.
        virtual void writeFrameUboDescriptor(u32 frameIndex, u32 view, BufferHandle uboToDescribe, sizet uboSize) = 0;

        virtual PipelineLayoutHandle pipelineLayout() const = 0;
        // Binds the global tables for the frame slot and view; valid for every pipeline.
        virtual void bind(IRenderCommandBuffer& cmd, PipelineBindPoint bindPoint, u32 frameIndex, u32 view) const = 0;
        // Writes the shared push-constant block from offset 0.
        virtual void pushConstants(IRenderCommandBuffer& cmd, const void* data, u32 size) const = 0;

        // Sampled image in the global texture array, sampled through the given sampler's table slot.
        virtual BindlessHandle registerTexture(ImageViewHandle view, SamplerHandle sampler) = 0;
        // Frames in flight may still sample the entry: its descriptor, sampler
        // slot and index are released once the frame that unregisters retires.
        virtual void unregisterTexture(BindlessHandle handle) = 0;

        // Storage image in the global storage-image array (declared without a format).
        virtual BindlessHandle registerStorageImage(ImageViewHandle view) = 0;
        // Deferred like unregisterTexture.
        virtual void unregisterStorageImage(BindlessHandle handle) = 0;

        // The frame clock (GpuAllocationManager::currentFrame/retiredFrame), once
        // per frame after it advances: releases unregistered entries whose frame
        // retired. Never called (frame 0) means nothing is in flight, and
        // unregisters release at once.
        virtual void onFrameBegin(u64 currentFrame, u64 retiredFrame) = 0;

        virtual BindlessStats stats() const = 0;
    };
} // namespace spite
