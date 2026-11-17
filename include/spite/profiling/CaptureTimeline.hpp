#pragma once

// The capture timeline: every zone a collector session recorded, per thread, with nesting depth, plus frame
// spans, counters per frame, memory events and thread names. Built by endSessionAndWriteReport when asked for, from
// the same ring snapshot the zones report reads; owned by the profiling module (its heap) and released with
// releaseCaptureTimeline. Plain views over module-owned arrays: no pointer into a ring or a module image survives the
// build, so a timeline outlives the session, later sessions and unloaded modules. Times are the profiler clock in ns
// (steady_clock since the module's first use), the clock every zone, frame and memory event carries.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "profiling/Profiling.hpp"
#include "profiling/ProfilingExport.hpp"

namespace spite::prof
{
    // TimelineZone::flags
    // open when the session stopped: endNs is the stop time, not the zone's end
    inline constexpr std::uint8_t TIMELINE_ZONE_OPEN_AT_STOP = 1;
    // begun before this session (a zone an earlier session left open and this one kept recording): startNs is the
    // session's start, not the zone's
    inline constexpr std::uint8_t TIMELINE_ZONE_OPEN_AT_START = 2;

    struct TimelineZone
    {
        std::uint64_t startNs;
        std::uint64_t endNs;
        std::uint32_t nameId; // CaptureTimeline::names
        // summary zones of its thread open when it began that the profiler saw begin: a zone begun while no session
        // armed is not counted nor recorded (its children start at 0); one an earlier session left open is, and its
        // children start below it
        std::uint16_t depth;
        ZoneKind kind;
        std::uint8_t flags;
    };
    static_assert(sizeof(TimelineZone) == 24);

    struct TimelineThread
    {
        // OS thread id; the OS reuses ids, so two lanes (a worker pool restarted) can share one: lanes are told apart
        // by their index in CaptureTimeline::threads, not by this id
        std::uint32_t threadId;
        std::uint32_t nameId; // setThreadName's name; the empty string when it was never named
        // in begin order: sorted by startNs, a zone before the zones nested in it, ties (one clock tick) in the order
        // the zones began; a zone's parent is the nearest earlier zone one depth up
        const TimelineZone* zones;
        std::uint64_t zoneCount;
        std::uint32_t maxDepth;
    };

    // One closed Frame zone, sorted by start; the frame in flight at the stop is a zone (flagged) but not a frame, as
    // in the zones report, so frameCount equals the report's frames.count for a session without a load window.
    // startNs can precede CaptureTimeline::startNs: a frame that began before the session and ended inside it is a
    // frame whose Frame zone is clipped to the session start and flagged TIMELINE_ZONE_OPEN_AT_START.
    struct TimelineFrame
    {
        std::uint64_t startNs;
        std::uint64_t endNs;
        std::uint64_t allocBytes; // heap allocations since the previous frame ended
        std::uint32_t allocCount;
        std::uint32_t threadId;
    };

    // The per-frame sum of one frame counter, attributed as the zones report attributes it; a frame where the counter
    // was never added has no sample. Sorted by (nameId, frame).
    struct TimelineCounterSample
    {
        std::uint32_t frame; // CaptureTimeline::frames index
        std::uint32_t nameId;
        std::uint64_t value;
    };

    // A big-allocation event (RAM >= 256 KB, GPU >= 64 KB), sorted by time; space 0 RAM, 1 GPU; kind 0 alloc, 1 free,
    // 2 grow (prevBytes is the capacity before it).
    struct TimelineMemoryEvent
    {
        std::uint64_t tNs;
        std::uint64_t bytes;
        std::uint64_t prevBytes;
        std::uint32_t tagId; // CaptureTimeline::names
        std::uint8_t space;
        std::uint8_t kind;
        std::uint16_t pad;
    };

    // A memory tag's totals over the session, as the zones report's memory.<space>.byTag lists them: every
    // allocation made under the tag, not only the big-allocation events; space 0 RAM, 1 GPU.
    struct TimelineMemoryTag
    {
        std::uint32_t tagId; // CaptureTimeline::names
        std::uint8_t space;
        std::uint8_t pad[3];
        std::uint64_t allocCount;
        std::uint64_t allocBytes;
        std::uint64_t maxBytes;
    };

    // A memory space's totals over the session, as the report's memory.<space> lists them (frees and the end net are
    // GPU-only today).
    struct TimelineMemorySpace
    {
        std::uint64_t allocCount, allocBytes, freeCount, freeBytes, netEndBytes, netPeakBytes;
    };

    // One GPU frame (the graphics queue's timestamps of one submit), in order of submission. startNs is its first
    // timestamp on the profiler clock when CaptureTimeline::gpuCalibrated, otherwise the CPU time its command buffer
    // began recording (the GPU start is unknown: only offsets within the frame are meaningful).
    // durationNs = outsideGraphNs + the passes' durations (within rounding), as the zones report's gpu.frame.
    // Frame starts carry the calibration error (gpuCalibrationDeviationMaxNs at most, per anchor): consecutive frames
    // can overlap or leave a gap by that much where the clock anchor changes.
    struct TimelineGpuFrame
    {
        std::uint64_t startNs;
        std::uint32_t durationNs; // saturates at 4.29 s
        std::uint32_t outsideGraphNs; // frame work outside the render graph's passes
        std::uint64_t firstPass; // index into CaptureTimeline::gpuPasses
        std::uint32_t passCount;
        std::uint32_t pad;
    };
    static_assert(sizeof(TimelineGpuFrame) == 32);

    // One executed render graph pass of a GPU frame, in execution order: [start, start + duration) is the pass's
    // timestamp segment (its barriers, clears and work), so the next pass starts where it ends.
    struct TimelineGpuPass
    {
        std::uint32_t nameId; // CaptureTimeline::names
        std::uint32_t startOffsetNs; // from the frame's startNs
        std::uint32_t durationNs;
    };
    static_assert(sizeof(TimelineGpuPass) == 12);

    struct CaptureTimeline
    {
        std::uint64_t startNs; // the session began
        std::uint64_t stopNs; // the session stopped: no zone ends later
        // records the rings refused past their budget, load window included: a timeline with drops has holes and is
        // invalid, like a truncated live capture
        std::uint64_t droppedRecords;
        std::uint64_t memoryEventsDropped;
        bool valid; // droppedRecords == 0
        std::uint32_t openAtStopZones; // zones flagged TIMELINE_ZONE_OPEN_AT_STOP
        // zones open at the stop that the recorder cannot see: past its 32-entry open-zone stack (allocation zones
        // count there too); their records, if they end while draining, are kept, otherwise they are absent
        std::uint32_t openZonesLost;
        double buildMs; // building it inside endSessionAndWriteReport
        std::uint64_t byteSize; // heap held by this timeline

        const char* const* names; // zone, counter, memory tag and thread names; ids index here
        std::uint32_t nameCount;
        const TimelineThread* threads; // threads that recorded a zone, in the order they first recorded or were named
        std::uint32_t threadCount;
        std::uint64_t zoneCount; // over all threads
        const TimelineFrame* frames;
        std::uint64_t frameCount;
        const TimelineCounterSample* counters;
        std::uint64_t counterSampleCount;
        const TimelineMemoryEvent* memoryEvents;
        std::uint64_t memoryEventCount;
        // true when every GPU frame's startNs is a calibrated-timestamp conversion onto the profiler clock
        // (VK_KHR_calibrated_timestamps); false: startNs is the CPU recording time, see TimelineGpuFrame
        bool gpuCalibrated;
        const TimelineGpuFrame* gpuFrames;
        std::uint64_t gpuFrameCount;
        const TimelineGpuPass* gpuPasses;
        std::uint64_t gpuPassCount;
        // calibration quality: the deviation Vulkan reported for the anchors used (each anchor's offset error is at
        // most its deviation; the last and the largest over the capture) and the calibrations skipped for exceeding the
        // ceiling while the capture ran
        std::uint32_t gpuCalibrationDeviationLastNs;
        std::uint32_t gpuCalibrationDeviationMaxNs;
        std::uint64_t gpuCalibrationsSkipped;
        // GPU frames of the capture the lane does not hold: refused past its memory cap (256 MiB while recording), or
        // collected before the renderer had a clock anchor (noteGpuFrameUnplaced); the lane then has a hole
        std::uint64_t gpuFramesDropped;
        // memory totals as the report's memory section: set when the session collected memory
        bool memoryTotals;
        TimelineMemorySpace memorySpaces[2]; // ram, gpu
        const TimelineMemoryTag* memoryTags;
        std::uint64_t memoryTagCount;
    };

    // The zones report's percentile of ascending values (non-empty): linear interpolation between neighbouring ranks.
    SPITE_PROFILING_API double percentileOfSorted(const std::uint64_t* sorted, std::size_t count, double fraction);
    inline double percentileOfSorted(const std::vector<std::uint64_t>& sorted, const double fraction)
    {
        return percentileOfSorted(sorted.data(), sorted.size(), fraction);
    }

    // endSessionAndWriteReport that also builds the session's timeline into *timeline (null when the report could not
    // be written). The caller owns it until releaseCaptureTimeline.
    SPITE_PROFILING_API bool endSessionAndWriteReport(const char* path, const CaptureTimeline** timeline);
    SPITE_PROFILING_API void releaseCaptureTimeline(const CaptureTimeline* timeline);

    // The stop split in two: beginStopAndWriteReport does only the ordering-critical part on the calling
    // thread (it ends the session, freezes each ring's record count and open-zone stack, takes the GPU lane and
    // series and the memory snapshot: well under a millisecond), then a below-normal-priority worker copies the
    // records, builds the timeline, writes the report and releases the live chunks, and calls `done` on that worker.
    // The data is the synchronous endSessionAndWriteReport's. Until `done` returns the session's rings are in use:
    // sessionArmed() stays true (module images stay mapped), a second async stop is refused, and beginSession and
    // its variants wait. `done` must not start a session or call waitStopIdle; it must not throw (it is guarded: a
    // throw is dropped).
    struct StopResult
    {
        bool written = false; // the zones report is on disk
        bool cancelled = false; // cancelStop ended the job; nothing was written
        const CaptureTimeline* timeline = nullptr; // the callee owns it (releaseCaptureTimeline); null: none built
        const char* timelineRefusal = ""; // why no timeline although one was asked for; valid during the call
        const char* error = ""; // an exception inside the job; valid during the call
        double workMs = 0.0; // the worker's time, freeze excluded
    };
    using StopDoneFn = void (*)(void* context, const StopResult& result);

    // What the freeze decided, answered by beginStopAndWriteReport.
    struct StopTicket
    {
        bool accepted = false; // false: a previous stop is still pending, or the worker could not start
        bool timelineWillBuild = false; // asked for and not refused for its memory budget
        char timelineRefusal[192] = {};
        double freezeMs = 0.0; // what the calling thread spent
    };
    // `done` null: the worker releases the timeline and drops the result.
    SPITE_PROFILING_API StopTicket beginStopAndWriteReport(const char* path, bool buildTimeline, StopDoneFn done,
                                                           void* context);
    // True from the freeze until `done` has returned.
    [[nodiscard]] SPITE_PROFILING_API bool stopPending();
    // Asks the pending job to end at its next phase boundary (before the timeline, before the report); the live chunks
    // are still released and `done` still called, with cancelled set.
    SPITE_PROFILING_API void cancelStop();
    // Blocks until no stop job runs and its worker has exited.
    SPITE_PROFILING_API void waitStopIdle();

    // Why the last endSessionAndWriteReport that was asked for a timeline handed out none although the report was
    // written (the timeline would exceed its memory budget); empty otherwise. Valid until the next such call, which
    // clears it, also one that asked for no timeline.
    SPITE_PROFILING_API const char* timelineRefusalReason();
} // namespace spite::prof
