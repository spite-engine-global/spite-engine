#pragma once

#include <functional>
#include <mutex>
#include <unordered_map>

#include <future>

#include "rendering/GpuHeapAllocator.hpp"
#include "rendering/GpuResourceHeapManager.hpp"
#include "rendering/ITransferManager.hpp"
#include "rendering/RenderResourceHandles.hpp"
#include "rendering/ResourcePool.hpp"
#include "rendering/RenderingExport.hpp"
#include "base/InternedString.hpp"
#include "base/Schema.hpp"

namespace spite
{
    struct GpuAllocationHandle : GpuResourceHandle
    {
    };

    struct GpuAllocationHeapStats
    {
        InternedString heapName;
        sizet activeBytes = 0;
        u32 activeAllocations = 0;
        SPITE_OBSERVED_SCHEMA(GpuAllocationHeapStats, heapName, activeBytes, activeAllocations)
    };

    struct GpuAllocationManagerStats
    {
        sizet activeBytes = 0;
        u32 activeAllocations = 0;
        u32 heapGrowths = 0; // exhausted heaps grown in place (spec phase 4)
        u32 growthDenials = 0; // budget-denied growths (overflow graceful degradation)
        std::vector<GpuAllocationHeapStats> perHeap; // rows of their own, not a field
        SPITE_OBSERVED_SCHEMA(GpuAllocationManagerStats, activeBytes, activeAllocations, heapGrowths, growthDenials)
    };


    // A grown heap awaiting publication. Allocations keep their offsets, so
    // ranges allocated before or after the growth are valid in newBuffer once
    // `copy` (old content -> newBuffer) has completed.
    struct GpuHeapSwap
    {
        InternedString heapName;
        BufferHandle oldBuffer;
        BufferHandle newBuffer;
        ITransferManager::TransferHandle copy;
    };

    // Internal struct to track an allocation.
    struct TrackedAllocation
    {
        GpuAllocation allocation;
        InternedString heapName; // Need to know which heap to free from
    };


    // Manages the lifecycle of all GpuAllocation objects via handles using both
    // garbage collection (for transient data) and reference counting (for shared assets).
    class GpuResourceStore;

    class SPITE_RENDERING_API GpuAllocationManager
    {
    public:
        GpuAllocationManager(GpuResourceHeapManager& heapManager, GpuResourceStore& store);
        ~GpuAllocationManager();

        GpuAllocationManager(const GpuAllocationManager&) = delete;
        GpuAllocationManager& operator=(const GpuAllocationManager&) = delete;

        GpuAllocationHandle allocate(InternedString heapName, sizet size, sizet alignment = 1);

        void free(GpuAllocationHandle handle);

        // Frame-slot aware deferral: once tracking is enabled, free() only
        // reserves the range for later release, and the actual heap free runs
        // in onFrameBegin(slot) - after the slot fence proved no in-flight
        // submission writes the range anymore. Must mirror the resource
        // manager onFrameBegin calls.
        void onFrameBegin(u32 frameSlot);

        // Frame retirement clock: frames are numbered by onFrameBegin, from 1;
        // 0 means no frame tracking, so nothing is in flight. Every frame
        // numbered at or below retiredFrame() has completed on the GPU: a
        // slot's begin follows its fence, which covers the frame that last
        // used the slot and, by submission order, every earlier one.
        u64 currentFrame() const;
        u64 retiredFrame() const;

        // Growths whose old -> new content copy the transfer queue has
        // submitted, oldest first, moved into `out`. Frame thread, at a frame
        // boundary: the caller makes the frame wait on `copy`, publishes
        // newBuffer and retires oldBuffer after the frames in flight.
        void takeSubmittedHeapSwaps(std::vector<GpuHeapSwap>& out);

        // Set by the host: true when the budget admits a heap growth of this
        // size. Unset = always admit.
        void setGrowthAdmission(std::function<bool(sizet growthBytes)> admission)
        {
            m_growthAdmission = std::move(admission);
        }

        // Counts the times a range of the heap went back to its allocator (a deferred free lands at its slot's
        // begin) or the heap grew: a caller refused room retries when the number moved, not on a timer.
        u64 heapReleases(InternedString heapName) const;

        std::vector<GpuAllocationHandle> getAllTransientHandles() const;

        const GpuAllocation& getAllocation(GpuAllocationHandle handle) const;
        GpuAllocationManagerStats getStats() const;

    private:
        struct DeferredFree
        {
            InternedString heapName;
            GpuAllocation allocation;
        };

        // caller holds m_mutex; returns false when growth is denied/failed
        bool growHeapLocked(InternedString heapName, sizet minCapacity);

        GpuResourceHeapManager& m_heapManager;
        GpuResourceStore& m_store;
        std::function<bool(sizet)> m_growthAdmission;
        std::vector<GpuHeapSwap> m_pendingSwaps;
        u32 m_heapGrowths = 0;
        u32 m_growthDenials = 0;
        std::unordered_map<InternedString, u64> m_releasesByHeap;
        ResourcePool<TrackedAllocation> m_allocations;
        std::vector<std::vector<DeferredFree>> m_garbageByFrameSlot;
        u32 m_currentFrameSlot = 0;
        bool m_frameTracking = false;
        u64 m_currentFrame = 0;
        u64 m_retiredFrame = 0;
        std::vector<u64> m_frameBySlot; // the frame that last began in each slot
        mutable std::mutex m_mutex;
    };
} // namespace spite
