#pragma once

// Public surface of the profiling module: session control and the recording
// primitives the SPITE_* zone macros are built on. Standard library only; the
// macro layer lives in Zone.hpp.

#include <atomic>
#include <cstdint>
#include <string_view>

#include "ProfilingExport.hpp"

namespace spite::prof
{
    // Role of a summary zone; zones reports group by kind, never by name.
    enum class ZoneKind : std::uint8_t
    {
        Generic = 0,
        Frame,
        Stage,
        System,
        Parallel,
        // idle time (fence, acquire, present waits): reported per name and
        // subtracted from the work of the stage or generic zone around it
        Wait,
    };

    // Fan-out counters shared between a parallel-call scope and the worker
    // threads executing its ranges. Lives inside the caller's scope object:
    // no allocation, no locking beyond the two atomics.
    struct ParallelCallContext
    {
        std::atomic<std::uint32_t> threadMask{0};
        std::atomic<std::uint64_t> workNs{0};
    };

    // Sessions are exclusive: beginSession clears collection, a report closes
    // it. Threads record only between begin and end; outside a session every
    // call here degrades to one relaxed load.
    SPITE_PROFILING_API void beginSession();

    // A session for an interactive capture of unknown length: every thread admits 8M records (48 B each, allocated on
    // demand and released when the report is written; a 10k-cube scene records ~86k per second across its threads) from
    // the start instead of the load window's 256k. Records past it still count as collectorDrops: the capture is
    // invalid. maxRecordsPerThread (clamped to 1..8M) lowers the budget.
    SPITE_PROFILING_API void beginLiveSession(std::uint64_t maxRecordsPerThread = std::uint64_t{1} << 23);

    // Splits the session: zones of frames starting before this call form the
    // load window (report key "load"), later ones the profile window (the
    // report's top level). GPU frames are kept for the profile window only.
    // A session that never calls it is all profile window.
    SPITE_PROFILING_API void beginProfileWindow();

    // Zones report schema; the comparison tooling skips reports below its minimum.
    inline constexpr int REPORT_SCHEMA_VERSION = 4;

    // Closes the session and writes the zones report (JSON, same schema the
    // comparison tooling consumes) to `path`. The file is written by the
    // module on purpose: with per-DLL static CRTs, a string returned across
    // the boundary would be freed on the wrong heap.
    SPITE_PROFILING_API bool endSessionAndWriteReport(const char* path);

    // Between beginSession and the report (also while an asynchronous stop, beginStopAndWriteReport, is still writing
    // it): the session holds zone and counter names by pointer, so an image those
    // literals live in must stay mapped until the report is written.
    [[nodiscard]] SPITE_PROFILING_API bool sessionArmed();

    // Names the calling thread in every later capture timeline (CaptureTimeline.hpp); copied, at most 63 bytes kept,
    // and named again the last name wins. Session or not.
    SPITE_PROFILING_API void setThreadName(const char* name);

    // Zone recording. startNs == 0 means the session was inactive at entry
    // and the matching end call becomes a no-op. During a session every heap
    // allocation is also counted under the innermost zone open on its thread
    // (the report's "allocsByZone"; "(no zone)" outside any).
    // The kind is kept for a zone still open when the session stops (the capture timeline closes it there). The
    // one-argument form keeps the export a module built against the older header links to; it records Generic. A
    // zone ends on the thread that began it (ZoneScope).
    SPITE_PROFILING_API std::int64_t beginZone(const char* name);
    SPITE_PROFILING_API std::int64_t beginZone(const char* name, ZoneKind kind);
    SPITE_PROFILING_API void endZone(const char* name, ZoneKind kind, std::int64_t startNs);

    // An allocation zone without a zone record: allocations inside it count
    // under `name` in "allocsByZone" and nothing is published to the collector,
    // so hot per-frame call sites cost the rings nothing. begin returns whether
    // it pushed (session active); only then is end called.
    SPITE_PROFILING_API bool beginAllocationZone(const char* name);
    SPITE_PROFILING_API void endAllocationZone();

    // Parallel fan-out recording. The scope opens like a zone; worker threads
    // report each executed range; the closing event carries the aggregates.
    // As an allocation zone it covers the calling thread only: allocations in
    // the ranges count under whatever zone their worker thread has open.
    SPITE_PROFILING_API std::int64_t beginParallel(const char* name);
    SPITE_PROFILING_API void recordParallelRange(ParallelCallContext& ctx, std::uint32_t threadNum,
                                                 std::int64_t workNs);
    SPITE_PROFILING_API void endParallel(const char* name, std::int64_t startNs, std::uint32_t itemCount,
                                         ParallelCallContext& ctx);

    // One executed render graph pass of a GPU frame. The name is borrowed
    // like memory tags: it must outlive the report, it is read at report time.
    struct GpuPassSample
    {
        std::string_view name;
        double ms;
        double startMs = 0.0; // from the frame's first timestamp
    };

    // Where a GPU frame sits on the profiler clock. calibrated: startNs is its first timestamp converted with
    // calibrated timestamps; otherwise it is only the CPU time the frame was recorded at.
    struct GpuFrameClock
    {
        std::uint64_t startNs;
        bool calibrated;
        // the calibration this frame was converted with: the deviation Vulkan reported for its anchor (the anchor's
        // offset error is at most this) and the calibrations skipped so far for exceeding the ceiling
        std::uint32_t deviationNs = 0;
        std::uint32_t calibrationsSkipped = 0;
    };

    // True while a live capture (beginLiveSession) runs: the only session that builds a timeline, so the GPU lane's
    // per-frame work (clock reads, calibration) is only done then.
    [[nodiscard]] SPITE_PROFILING_API bool gpuLaneArmed();

    // One GPU frame (graphics-queue work, from timestamp queries) in
    // milliseconds: frameMs = outsideGraphMs + the passes' sum. Ignored
    // outside a session; the report carries "gpu.frame", and "gpu.passes"
    // plus "gpu.outsideGraph" once any frame had passes.
    // clock (optional) also places the frame and its passes on the capture timeline's GPU lane.
    SPITE_PROFILING_API void recordGpuFrame(double frameMs, double outsideGraphMs, const GpuPassSample* passes,
                                            std::uint32_t passCount, const GpuFrameClock* clock = nullptr);

    // A GPU frame of the running capture the timeline's GPU lane cannot place (the renderer has no clock anchor yet):
    // counted in CaptureTimeline::gpuFramesDropped, so the lane's gap is reported. Call it with the frame's
    // recordGpuFrame, which carries no clock then.
    SPITE_PROFILING_API void noteGpuFrameUnplaced();

    // The profiler clock (ns, the clock of every zone), and a Windows QueryPerformanceCounter reading on it (0 for a
    // reading before the clock's first use): calibrated timestamps sample the counter.
    SPITE_PROFILING_API std::uint64_t profilerNowNs();
    SPITE_PROFILING_API std::uint64_t profilerNsFromPerformanceCounter(std::uint64_t counter);

    // Arms memory collection on a process-wide timeline, independent of the
    // zone session: call once at startup, before engine init, and the report's
    // "memory" section covers engine init and scene build, not just the
    // profiled window. beginSession arms it as a session-scoped fallback when
    // nobody did and disarms it when the report is written, so each such session
    // counts only itself; an explicitly armed collection is never reset or
    // disarmed by session calls, and the report snapshots it without clearing.
    SPITE_PROFILING_API void beginMemoryCollection();

    // Disarms an explicit beginMemoryCollection; the next report still shows what was collected until then.
    SPITE_PROFILING_API void endMemoryCollection();

    // Per-frame counter: adds `value` to the named counter of the frame in
    // flight; the report sums each counter per frame (series + distribution).
    // The name is borrowed like zone names. Outside a session: one relaxed load.
    SPITE_PROFILING_API void addCounter(const char* name, std::uint64_t value);

    // Big-allocation events, threshold-filtered inside the module (RAM 256 KB,
    // GPU 64 KB): the report's "memory" section carries per-tag totals plus a
    // capped event list. RAM has one catch-all recorder in SpiteBase's operator
    // new (other modules allocate from their own static CRT heaps, unseen),
    // labeled by the thread-local tag; during a session it also counts every
    // allocation it sees, any size, into the report's per-frame "allocs"; GPU recorders sit on the buffer/
    // texture create/destroy and heap-growth funnels. Grow events are semantic
    // markers only — the underlying buffer alloc/free is recorded separately —
    // so occupancy is alloc minus free. With collection inactive every call
    // degrades to one relaxed load. Tags must outlive the report (static
    // strings), they are read at report time.
    SPITE_PROFILING_API void recordRamAlloc(std::uint64_t bytes);
    SPITE_PROFILING_API void recordGpuAlloc(std::uint64_t bytes, const char* tag);
    SPITE_PROFILING_API void recordGpuFree(std::uint64_t bytes, const char* tag);
    SPITE_PROFILING_API void recordGpuGrow(std::uint64_t newBytes, std::uint64_t prevBytes, const char* tag);

    // Thread-local label for the RAM catch-all: scopes at the big-allocation
    // sites (file reads, bake buffers, upload staging) rename their thread's
    // allocations; everything unlabeled lands under "ram.new". During a session
    // every allocation under a tag, any size, is also counted into the report's
    // per-frame "allocsByTag" (hot paths tag themselves to prove they are
    // allocation-free). activeMemoryTag is null when untagged.
    SPITE_PROFILING_API const char* activeMemoryTag();
    SPITE_PROFILING_API void setMemoryTag(const char* tag);

    // Counts this thread's heap allocations (SpiteBase's operator new) into a
    // caller counter, session or not; returns the previous counter.
    SPITE_PROFILING_API std::uint64_t* setAllocationCounter(std::uint64_t* counter);

    class AllocationCountScope
    {
    public:
        explicit AllocationCountScope(std::uint64_t& counter) : m_previous(setAllocationCounter(&counter)) {}
        AllocationCountScope(const AllocationCountScope&) = delete;
        AllocationCountScope& operator=(const AllocationCountScope&) = delete;
        ~AllocationCountScope() { setAllocationCounter(m_previous); }

    private:
        std::uint64_t* m_previous;
    };

    class AllocationZoneScope
    {
    public:
        explicit AllocationZoneScope(const char* name) : m_pushed(beginAllocationZone(name)) {}
        AllocationZoneScope(const AllocationZoneScope&) = delete;
        AllocationZoneScope& operator=(const AllocationZoneScope&) = delete;
        ~AllocationZoneScope()
        {
            if (m_pushed)
            {
                endAllocationZone();
            }
        }

    private:
        bool m_pushed;
    };

    class MemoryTagScope
    {
    public:
        explicit MemoryTagScope(const char* tag) : m_previous(activeMemoryTag()) { setMemoryTag(tag); }
        MemoryTagScope(const MemoryTagScope&) = delete;
        MemoryTagScope& operator=(const MemoryTagScope&) = delete;
        ~MemoryTagScope() { setMemoryTag(m_previous); }

    private:
        const char* m_previous;
    };

    // RAII helpers; the SPITE_* macros in Zone.hpp are the intended entry.
    class ZoneScope
    {
    public:
        ZoneScope(const char* name, ZoneKind kind) : m_name(name), m_kind(kind), m_startNs(beginZone(name, kind)) {}
        ZoneScope(const ZoneScope&) = delete;
        ZoneScope& operator=(const ZoneScope&) = delete;
        ~ZoneScope() { endZone(m_name, m_kind, m_startNs); }

    private:
        const char* m_name;
        ZoneKind m_kind;
        std::int64_t m_startNs;
    };

    class ParallelCallScope
    {
    public:
        ParallelCallScope(const char* name, std::uint32_t itemCount) :
            m_name(name), m_itemCount(itemCount), m_startNs(beginParallel(name))
        {
        }
        ParallelCallScope(const ParallelCallScope&) = delete;
        ParallelCallScope& operator=(const ParallelCallScope&) = delete;
        ~ParallelCallScope() { endParallel(m_name, m_startNs, m_itemCount, m_ctx); }

        ParallelCallContext& context() { return m_ctx; }

    private:
        const char* m_name;
        std::uint32_t m_itemCount;
        std::int64_t m_startNs;
        ParallelCallContext m_ctx;
    };
} // namespace spite::prof
