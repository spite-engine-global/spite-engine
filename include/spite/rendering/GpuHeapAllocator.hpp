#pragma once

#include <mutex>
#include <span>
#include <vector>

#include "rendering/RenderingExport.hpp"
#include "base/Platform.hpp"
#include "base/Schema.hpp"

namespace spite
{
    class HeapAllocator;

    struct GpuHeapAllocatorStats
    {
        sizet totalSize = 0;
        sizet usedBytes = 0;
        sizet freeBytes = 0;
        u32 allocationCount = 0;
        SPITE_OBSERVED_SCHEMA(GpuHeapAllocatorStats, totalSize, usedBytes, freeBytes, allocationCount)
    };

    // Represents a sub-allocation within a larger GPU buffer. Backend-agnostic:
    // customData carries whatever opaque tag the backend needs to release it.
    struct GpuAllocation
    {
        sizet offset = static_cast<sizet>(-1);
        sizet size = 0;
        sizet alignment = 1; // recorded at allocate; heap growth replays it
        void* customData = nullptr;

        bool isValid() const { return offset != static_cast<sizet>(-1); }
    };

    // Manages sub-allocations within a single large GPU buffer using the backend's
    // virtual allocator. This class is thread-safe, non-copyable, and non-movable.
    class SPITE_RENDERING_API GpuHeapAllocator
    {
    public:
        GpuHeapAllocator(sizet totalSize);
        ~GpuHeapAllocator();

        GpuHeapAllocator(const GpuHeapAllocator&) = delete;
        GpuHeapAllocator& operator=(const GpuHeapAllocator&) = delete;
        GpuHeapAllocator(GpuHeapAllocator&&) = delete;
        GpuHeapAllocator& operator=(GpuHeapAllocator&&) = delete;

        GpuAllocation allocate(sizet size, sizet alignment = 1);
        void free(GpuAllocation allocation);

        // Frees every outstanding virtual allocation (metadata only - the
        // buffer content is untouched). The heap-growth migration abandons the
        // old block wholesale: its live ranges were re-created in the new one,
        // and its dead ranges have nothing to release.
        void clear();

        // Heap-growth migration (gpu-streaming spec phase 4): this allocator
        // must be FRESH (nothing allocated). Recreates every live range at its
        // identical offset; out[i] corresponds to live[i] (same offset/size,
        // new customData). Returns false and frees everything it placed when
        // the layout does not reproduce exactly - the caller keeps the old
        // allocator authoritative.
        bool recreateAll(std::span<const GpuAllocation> live, std::vector<GpuAllocation>& out);

        sizet getTotalSize() const { return m_totalSize; }
        GpuHeapAllocatorStats getStats() const;

    private:
        sizet m_totalSize = 0;
        void* m_virtualBlock = nullptr; // backend-owned virtual allocator block
        mutable std::mutex m_mutex;
    };
} // namespace spite
