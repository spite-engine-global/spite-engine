#pragma once

#include <future>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/ApplicationConfig.hpp"
#include "base/Schema.hpp"
#include "rendering/GpuBudget.hpp"
#include "rendering/GpuResourceHeapManager.hpp"
#include "rendering/GraphicsDescs.hpp"
#include "rendering/ITransferManager.hpp"
#include "rendering/RenderResourceHandles.hpp"
#include "rendering/RenderingExport.hpp"

namespace spite
{
    class IRenderDevice;
    class ITransferManager;

    // Whether an allocation may be denied by the soft budget. Essential
    // allocations (load-time, scene-mandated) bypass try-semantics and fail
    // loudly through the backend; elastic ones are streaming growth and
    // return an invalid handle on denial.
    enum class GpuAdmission : u8
    {
        Essential = 0,
        Elastic = 1,
    };

    enum class UploadState : u8
    {
        Queued = 0,
        Submitted,
        Completed,
        Cancelled,
        // nothing landed: the destination was destroyed before submit, or the
        // transfer refused the request (larger than the staging ring)
        Failed,
    };

    struct UploadTicket
    {
        u64 id = 0; // 0 = invalid
        bool isValid() const { return id != 0; }
    };

    // Cumulative mechanism telemetry (perf-gate inputs).
    struct GpuResourceStoreStats
    {
        u64 uploadsSubmitted = 0;
        u64 uploadsCancelled = 0;
        u64 uploadsFailed = 0;
        u64 bytesSubmitted = 0;
        u64 elasticAllocDenials = 0;
        SPITE_OBSERVED_SCHEMA(GpuResourceStoreStats, uploadsSubmitted, uploadsCancelled, uploadsFailed, bytesSubmitted,
                              elasticAllocDenials)
    };

    // One queued streaming upload. The job owns its bytes: safe to build on
    // load threads and release immediately after upload() returns.
    struct UploadJob
    {
        BufferHandle destination;
        // Set for uploads into a growable heap: overrides `destination` with the
        // heap's current buffer at submit time, so a growth between building
        // the job and submitting it cannot lose or overwrite the upload.
        // `destination` must never be a heap's buffer (asserted on upload).
        InternedString destinationHeap{};
        u64 destinationOffset = 0;
        std::vector<std::byte> data;
        // higher submits first, FIFO within a priority; an earlier queued job
        // writing an overlapping range still submits first
        u32 priority = 0;
        TransferSync sync = TransferSync::Deferred;
    };

    // One live upload ticket as a table row; reclaimed tickets are gone.
    struct UploadTicketRow
    {
        u64 ticket = 0;
        std::string state; // queued, submitted, completed, cancelled or failed
        u64 bytes = 0;
        SPITE_OBSERVED_SCHEMA(UploadTicketRow, ticket, state, bytes)
    };

    // The mechanism tier of the streaming architecture: budget-admitted
    // allocation, ticketed uploads, and deferred destruction — no residency
    // policy lives here. Synchronous calls, async effects. The job queue is
    // thread-safe (load threads produce, the render thread ticks).
    class SPITE_RENDERING_API GpuResourceStore
    {
    public:
        // `heaps` resolves UploadJob::destinationHeap and is required by growHeap.
        GpuResourceStore(IRenderDevice& device, ITransferManager& transferManager, const GraphicsConfig& config,
                         GpuResourceHeapManager* heaps = nullptr);

        GpuResourceStore(const GpuResourceStore&) = delete;
        GpuResourceStore& operator=(const GpuResourceStore&) = delete;

        // The soft ceiling elastic allocations are admitted against
        // (min(user knob, device-local heap budget - margin)).
        GpuBudget budget() const;

        // Device-to-device copy transport (resize copies, later compaction);
        // thin passthrough so callers above never touch ITransferManager.
        // Sync matters: Deferred copies are NOT made visible to the graphics
        // queue — use SameFrame when the destination VA is (or may become)
        // shader-visible through the address UBOs before the copy lands.
        ITransferManager::TransferHandle copyBuffer(BufferHandle source, BufferHandle destination, sizet size,
                                                    sizet sourceOffset = 0, sizet destinationOffset = 0,
                                                    TransferSync sync = TransferSync::Deferred)
        {
            return m_transferManager.copyBuffer(source, destination, size, sourceOffset, destinationOffset, sync);
        }

        // Whole-buffer allocation. Elastic admission checks the budget and may
        // return an invalid handle; essential always allocates.
        BufferHandle allocDedicated(const BufferDesc& desc, const char* name, GpuAdmission admission);
        // Frees through the frame-slot garbage path: safe while frames are in flight.
        void freeDedicated(BufferHandle handle);

        // Queues a streaming upload; actual submission follows priority order,
        // bounded by the per-frame byte budget passed to tick(). SameFrame jobs
        // bypass the budget (submitSameFrame).
        UploadTicket upload(UploadJob job);
        // One ticket over several slices: submits as a single batched staging
        // submission (one staging allocation + one copy submission).
        UploadTicket uploadBatch(std::vector<UploadJob> jobs);
        // Only valid while the ticket is still Queued.
        bool cancel(UploadTicket ticket);
        // Completed tickets are reclaimed at the next tick; querying a
        // reclaimed ticket keeps reporting Completed.
        UploadState ticketState(UploadTicket ticket) const;
        // Once per frame on the render thread, before processTransfers:
        // resolves ticket states, submits every SameFrame group (submitSameFrame),
        // then submits queued jobs in priority order until admitBudgetBytes is
        // spent. A single job larger than the whole budget still submits
        // (progress guarantee).
        void tick(u64 admitBudgetBytes);
        // Submits every queued SameFrame group outside the admit budget, with
        // any earlier queued group writing an overlapping range ahead of it.
        // Called after jobs a frame renders were queued and before that
        // frame's processTransfers, so they reach its graphics submission.
        void submitSameFrame();

        struct HeapGrowth
        {
            GpuResourceHeapManager::HeapGrowResult result;
            ITransferManager::TransferHandle copy; // old -> new content, valid when result.ok
        };
        // Heap growth's ordering point. Creates the grown buffer unlocked
        // (GpuResourceHeapManager::prepareGrowth), then, atomic against every
        // submission: submits all queued uploads (heap destinations resolve to
        // the pre-growth buffer, so the copy carries them), swaps the heap's
        // buffer (commitGrowth) and enqueues the content copy. Uploads
        // submitted afterwards resolve to the new buffer and queue behind the
        // copy. Enqueues only: safe on load threads. The caller serialises
        // growths of a heap (GpuAllocationManager's lock).
        HeapGrowth growHeap(InternedString heapName, sizet minCapacity, std::span<const GpuAllocation> live);

        ITransferManager& transferManager() { return m_transferManager; }

        GpuResourceStoreStats stats() const;
        // The tickets not yet reclaimed, by id (the tickets table).
        [[nodiscard]] std::vector<UploadTicketRow> ticketRows() const;

    private:
        struct TicketRecord
        {
            UploadState state = UploadState::Queued;
            std::shared_future<u64> handle{}; // shared by the tickets of one transfer request
            u64 bytes = 0;
        };
        // a pending group is one ticket over one-or-more slices
        using PendingGroup = std::pair<UploadTicket, std::vector<UploadJob>>;

        static u64 bytesOf(const std::vector<UploadJob>& group);
        // caller holds m_ticketMutex
        void submitGroupsLocked(std::span<const PendingGroup> groups);
        void submitSameFrameLocked();
        void admitDeferredLocked(u64 admitBudgetBytes);
        BufferHandle resolveDestination(const UploadJob& job) const;
        void flushPendingUploadsLocked();

        IRenderDevice& m_device;
        ITransferManager& m_transferManager;
        GraphicsConfig m_config;
        GpuResourceHeapManager* m_heaps;

        mutable std::mutex m_ticketMutex;
        u64 m_nextTicketId = 1;
        std::unordered_map<u64, TicketRecord> m_tickets;
        std::vector<PendingGroup> m_pendingJobs;
        GpuResourceStoreStats m_stats;
    };
} // namespace spite
