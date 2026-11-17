#pragma once

#include "base/InternedString.hpp"
#include "base/Schema.hpp"
#include "rendering/GpuHeapAllocator.hpp"
#include "rendering/RenderResourceHandles.hpp"
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "rendering/RenderingExport.hpp"

namespace spite
{
    class IRenderDevice;
    struct BufferDesc;

    // One heap as a table row: its allocator's occupancy.
    struct GpuHeapRow
    {
        std::string heap;
        u64 totalBytes = 0;
        u64 usedBytes = 0;
        u64 freeBytes = 0;
        u32 allocationCount = 0;
        SPITE_OBSERVED_SCHEMA(GpuHeapRow, heap, totalBytes, usedBytes, freeBytes, allocationCount)
    };

    // Manages named, heap-allocated global GPU buffers.
    class SPITE_RENDERING_API GpuResourceHeapManager
    {
    public:
        GpuResourceHeapManager(IRenderDevice& device);

        GpuResourceHeapManager(const GpuResourceHeapManager&) = delete;
        GpuResourceHeapManager& operator=(const GpuResourceHeapManager&) = delete;
        GpuResourceHeapManager(GpuResourceHeapManager&&) = delete;
        GpuResourceHeapManager& operator=(GpuResourceHeapManager&&) = delete;

        // Creates and registers a new heap-allocated global buffer.
        void createHeap(InternedString name, const BufferDesc& desc);

        struct HeapGrowResult
        {
            bool ok = false;
            BufferHandle oldBuffer;
            BufferHandle newBuffer;
            sizet oldSize = 0;
            sizet newSize = 0;
            std::vector<GpuAllocation> remapped; // index-aligned with the live input
        };

        // First half of a growth: creates the heap's grown buffer
        // (geometrically, >= minCapacity) and migrates the allocator's live
        // ranges at identical offsets (gpu-streaming spec phase 4). getBuffer
        // keeps returning the old buffer until commitGrowth, so the device
        // allocation stays outside GpuResourceStore's upload-ordering lock.
        // Returns ok=false (untouched) when the layout cannot be reproduced or
        // the buffer allocation fails. The caller serialises growths of a heap.
        HeapGrowResult prepareGrowth(InternedString name, sizet minCapacity, std::span<const GpuAllocation> live);
        // Second half: makes the grown buffer authoritative. The CALLER must
        // copy old -> new content, adopt `remapped`, and destroy oldBuffer only
        // once the copy settled (a queued copy can lag frames behind under
        // load, outliving the frame-slot defer window).
        void commitGrowth(InternedString name, const HeapGrowResult& growth);

        GpuHeapAllocator& getAllocator(InternedString name);
        const GpuHeapAllocator& getAllocator(InternedString name) const;

        // The heap's current buffer. Uploads must not cache it across a growth:
        // GpuResourceStore resolves heap destinations at submit time (UploadJob::destinationHeap).
        BufferHandle getBuffer(InternedString name) const;
        // True when `buffer` is some heap's current buffer.
        bool isHeapBuffer(BufferHandle buffer) const;
        IRenderDevice& device() const { return m_device; }
        // One row per heap by name (the heaps table).
        [[nodiscard]] std::vector<GpuHeapRow> heapRows() const;

    private:
        IRenderDevice& m_device;

        // Guards the maps against commitGrowth's swap; allocator contents are the
        // GpuAllocationManager's to synchronise.
        mutable std::mutex m_mutex;
        // Maps a heap's name to its buffer and allocator.
        std::unordered_map<InternedString, BufferHandle> m_buffers;
        std::unordered_map<InternedString, GpuHeapAllocator> m_allocators;
    };
} // namespace spite
