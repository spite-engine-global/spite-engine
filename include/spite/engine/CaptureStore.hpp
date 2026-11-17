#pragma once

// The profile capture store: the captures the engine can answer timeline queries about. A live
// capture (profileStop) is handed over as the profiling module's CaptureTimeline, written as a SQLite sidecar by a
// worker and released; a capture opened from disk reads the same sidecar. Either way a capture is queried through its
// sidecar (indexed slices, nothing loaded whole), so a store entry costs a page cache, not the capture's zones.
// SQLite is private to the engine module: this header is std-only. Times are nanoseconds since the capture began
// (CaptureInfo::startNs on the profiler clock), signed because a frame or GPU frame can start slightly before it.
// Every method is safe from any thread. A capture file is untrusted input and the callers are on the frame thread: no
// method throws, every failure is an error string, and every query is bounded in time (Limits::queryMs), memory
// (an estimated reply byte budget applied while rows are read) and rows.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/CaptureAnalysis.hpp"
#include "engine/EngineExport.hpp"
#include "profiling/CaptureTimeline.hpp"

namespace spite
{
    using CaptureId = std::uint32_t;

    struct CaptureInfo
    {
        enum class State : std::uint8_t
        {
            Writing, // live capture: the sidecar is still being written; queries answer "not ready"
            Ready,
            Failed, // the sidecar could not be written; the zones report stands, error says why
        };

        CaptureId id = 0;
        std::string name; // the sidecar's file name
        std::filesystem::path path; // the sidecar
        State state = State::Ready;
        std::string error;
        bool live = false; // handed over by profileStop (otherwise opened from disk)
        bool valid = true; // no dropped records
        std::uint32_t schemaVersion = 0;
        std::uint64_t startNs = 0; // profiler clock
        std::int64_t durationNs = 0;
        std::uint64_t zones = 0, frames = 0, threads = 0, gpuFrames = 0, gpuPasses = 0, counterSamples = 0,
                      memoryEvents = 0;
        std::uint64_t droppedRecords = 0, openAtStopZones = 0;
        bool gpuCalibrated = false;
        std::uint64_t fileBytes = 0;
        double writeMs = 0.0; // live captures: the worker's write time
        std::string report; // the zones report's file name, when the sidecar was written beside one
    };

    struct CaptureThread
    {
        std::uint32_t index = 0; // the lane: OS ids repeat across lanes
        std::uint32_t osId = 0;
        std::string name;
        std::uint64_t zones = 0;
        std::uint32_t maxDepth = 0;
    };

    struct CaptureFrame
    {
        std::uint64_t index = 0;
        std::int64_t startNs = 0, endNs = 0;
        std::uint64_t allocBytes = 0;
        std::uint32_t allocCount = 0, osThread = 0;
    };

    struct CaptureZone
    {
        std::uint32_t thread = 0;
        std::uint64_t index = 0; // within its thread, in begin order
        std::int64_t startNs = 0, endNs = 0;
        std::uint32_t depth = 0;
        std::uint8_t kind = 0, flags = 0;
        std::uint64_t parent = 0; // index + 1 of the enclosing zone one depth up; 0 = none recorded
        std::string name;
    };

    struct CaptureGpuPass
    {
        std::string name;
        std::uint32_t startOffsetNs = 0, durationNs = 0;
    };

    struct CaptureGpuFrame
    {
        std::uint64_t index = 0;
        std::int64_t startNs = 0;
        std::uint32_t durationNs = 0, outsideGraphNs = 0;
        std::vector<CaptureGpuPass> passes;
    };

    struct CaptureCounter
    {
        std::string name;
        std::uint64_t frame = 0, value = 0;
    };

    // One frame of the frame strip: its CPU duration and the GPU frame that started inside it (0: none, or
    // the capture's GPU clock is not calibrated). Saturates at 4.29 s.
    struct CaptureStripFrame
    {
        std::uint32_t cpuNs = 0, gpuNs = 0;
    };

    // Consecutive frames from `first`.
    struct CaptureStrip
    {
        std::uint64_t first = 0;
        std::vector<CaptureStripFrame> rows;
        bool truncated = false;
        bool gpuCalibrated = false;
    };

    // A sidecar found under the artifacts root.
    struct CaptureFile
    {
        std::string path; // relative to the artifacts root, '/' separated: profileOpen's path argument
        std::string name; // the file name
        std::uint64_t bytes = 0;
        std::int64_t modifiedMs = 0; // unix time
        CaptureId openId = 0; // the store entry that holds it, 0 when none
    };

    // Frame statistics over the whole requested range (rows may be capped).
    struct CaptureFrameStats
    {
        std::uint64_t count = 0; // frames the statistics cover: the window's first Limits::maxStatFrames at most
        double meanMs = 0.0, minMs = 0.0, p50Ms = 0.0, p99Ms = 0.0, maxMs = 0.0;
        bool truncated = false; // the window held more frames than count
    };

    // A window is the frame range [firstFrame, lastFrame] when firstFrame is set, else [fromNs, toNs); unset bounds
    // are open. Rows come in index order; offset rows are skipped, then up to limit are kept; truncated says more
    // matched. A decimated query adds a field here and filters in the same scan.
    struct CaptureRange
    {
        std::int64_t fromNs = INT64_MIN, toNs = INT64_MAX;
        std::int64_t firstFrame = -1, lastFrame = -1;
        std::uint64_t offset = 0, limit = 1000;
    };

    struct CaptureZoneQuery
    {
        CaptureRange range;
        std::int64_t thread = -1; // lane index; -1 = every lane
        std::uint32_t minDepth = 0, maxDepth = UINT32_MAX;
        std::string name; // case-insensitive substring of the zone name; empty = any
        std::int64_t minDurationNs = 0;
        bool enclosing = true; // also the zones that began before the window and still run in it
    };

    // Level of detail for the Profiler timeline: one lane's zones in a window, as the spans a view draws.
    // The window [fromNs, toNs) is cut into `columns` pixel columns; a zone at least one column wide is a span of its
    // own, zones narrower than a column merge, per depth, into one span per column, so the rows track the columns
    // and the depth, never the zone count. The scan runs inside the store's query deadline: it answers what it
    // reached with done false and `next`, the seq to pass as `after` for the rest (the first call, after -1, also
    // returns the zones that began before the window and still run in it).
    struct CaptureSpanQuery
    {
        std::uint32_t thread = 0; // lane
        std::int64_t fromNs = 0, toNs = 1;
        std::uint32_t columns = 1024; // 1..CAPTURE_MAX_COLUMNS
        std::uint32_t maxDepth = 63; // deeper zones are not scanned
        std::int64_t after = -1;
    };

    inline constexpr std::uint32_t CAPTURE_MAX_COLUMNS = 8192;
    inline constexpr std::uint32_t CAPTURE_MAX_SPAN_DEPTH = 63;

    // count 1: one zone (firstSeq == longestSeq). Otherwise `count` zones of one depth that began in one column:
    // startNs the first start, endNs the latest end, totalNs the sum of their durations, longest* the longest one
    // (name is its name).
    struct CaptureSpan
    {
        std::uint32_t depth = 0;
        std::int64_t startNs = 0, endNs = 0;
        std::uint64_t count = 0;
        std::uint64_t firstSeq = 0, longestSeq = 0;
        std::uint64_t totalNs = 0, longestNs = 0;
        std::string name;
    };

    struct CaptureSpanSlice
    {
        std::vector<CaptureSpan> rows; // in scan order (a span is emitted when its column closes), not sorted
        bool done = true;
        std::uint64_t next = 0; // when !done: the seq to continue from
        std::uint64_t scanned = 0; // zones read
    };

    // One zone with the numbers a tooltip shows. selfNs is its duration less its direct children's; selfExact false
    // (selfNs the duration) when the children could not all be read within the deadline.
    struct CaptureZoneDetail
    {
        CaptureZone zone;
        std::string laneName;
        std::uint64_t children = 0;
        std::int64_t selfNs = 0;
        bool selfExact = true;
        bool hasFrame = false; // the zone began inside a frame
        std::uint64_t frame = 0;
    };

    // Durations of the zones named exactly `name` in one lane (the statistics of a name are over its lanes), from seq
    // `after`; resumable as the span query is.
    struct CaptureDurationQuery
    {
        std::uint32_t thread = 0;
        std::string name;
        std::uint64_t after = 0;
    };

    struct CaptureDurationSlice
    {
        std::vector<std::uint64_t> durations;
        bool done = true;
        std::uint64_t next = 0;
    };

    // The next or previous zone, by (start, lane, seq), whose name contains `name` (case-insensitive) after the
    // cursor; resumable per lane. `found` false with done true: none.
    struct CaptureFindQuery
    {
        std::uint32_t thread = 0;
        std::string name;
        std::int64_t cursorNs = 0; // the cursor zone's start; the cursor excludes itself
        std::uint32_t cursorLane = 0;
        std::uint64_t cursorSeq = 0;
        bool hasCursor = false; // false: the first (or, backwards, the last) match from cursorNs
        bool backwards = false;
        std::uint64_t after = 0; // resume point from a previous answer's next (0: begin at the cursor)
        bool resuming = false;
    };

    struct CaptureFindResult
    {
        bool found = false;
        bool done = true;
        std::uint64_t next = 0;
        CaptureZone zone;
    };

    // Statistics over zone durations in ns (every zone kind with the name; the report's 'generic' section counts only
    // ZoneKind::Generic), computed as the zones report does (linear-interpolated percentiles of
    // the sorted durations), plus a histogram of `bins` equal bins between min and max.
    struct CaptureZoneStats
    {
        std::uint64_t count = 0;
        double meanNs = 0.0, p50Ns = 0.0, p99Ns = 0.0;
        std::uint64_t minNs = 0, maxNs = 0, totalNs = 0;
        std::vector<std::uint64_t> histogram;
    };

    // Where a whole-capture search / statistics run stands (CaptureStore::findStep / statisticsStep). Fill query (name,
    // direction, cursor) or name, lane (-1: every lane) and maxSamples; the rest is the store's.
    struct CaptureFindCursor
    {
        CaptureFindQuery query;
        std::int64_t lane = -1;
        bool started = false;
        std::uint64_t thread = 0, last = 0;
        CaptureFindQuery step;
        CaptureFindResult best;
    };

    struct CaptureStatsCursor
    {
        std::string name;
        std::int64_t lane = -1;
        std::uint64_t maxSamples = 0;
        bool started = false;
        std::uint64_t thread = 0, last = 0, after = 0;
        std::vector<std::uint64_t> durations;
    };

    // Sorts `durations`; empty input gives count 0.
    [[nodiscard]] SPITE_ENGINE_API CaptureZoneStats captureZoneStats(std::vector<std::uint64_t>& durations,
                                                                     std::uint32_t bins = 24);

    // The store stops reading rows once their estimated reply size reaches this (truncated + nextOffset); the control
    // commands cut by the same estimate. Each estimate is the row's wire size, roughly.
    constexpr std::uint64_t CAPTURE_REPLY_BYTES = std::uint64_t{1} << 20;
    constexpr std::uint64_t CAPTURE_REPLY_BASE_BYTES = 512;
    inline std::uint64_t captureRowBytes(const CaptureFrame&) { return 60; }
    inline std::uint64_t captureRowBytes(const CaptureStripFrame&) { return 24; }
    inline std::uint64_t captureRowBytes(const CaptureZone& zone) { return 72 + zone.name.size(); }
    inline std::uint64_t captureRowBytes(const CaptureCounter& row) { return 32 + row.name.size(); }
    inline std::uint64_t captureRowBytes(const CaptureGpuFrame& frame)
    {
        std::uint64_t bytes = 64;
        for (const CaptureGpuPass& pass : frame.passes)
        {
            bytes += 32 + pass.name.size();
        }
        return bytes;
    }

    template <class Row>
    struct CaptureSlice
    {
        std::vector<Row> rows;
        bool truncated = false;
    };

    class SPITE_ENGINE_API CaptureStore
    {
    public:
        // Released by the store on a worker once the sidecar is written (or failed); null: nothing to release.
        using ReleaseFn = std::function<void(const prof::CaptureTimeline*)>;

        struct Limits
        {
            std::uint32_t maxOpened = 2; // captures opened from disk; opening one more closes the least recently used
            std::uint32_t maxLive = 2; // captures handed over by profileStop, the same rule
            std::uint32_t maxPendingWrites =
                2; // timelines awaiting their sidecar; a further submit is refused (returns 0)
            std::uint64_t maxFileBytes = std::uint64_t{8} << 30; // a sidecar larger than this is not opened
            std::uint32_t cacheKiB = 16384; // SQLite page cache per entry
            std::uint32_t queryMs = 150; // a query running longer is interrupted: "interrupted: query exceeded N ms"
            // A capture serves one query at a time; one that waits longer for it answers "busy: ... retry" (a caller
            // on the frame thread never waits behind a long query of a worker).
            std::uint32_t lockWaitMs = 30;
            std::uint64_t maxStatFrames = 200000; // frame statistics cover at most this many frames of a window
            // One zoneSpans call answers at most this many spans / estimated bytes (names clipped to 256 bytes); it
            // stops before the row that could exceed them (done false, next), so a flush always fits. Tests lower them.
            std::uint64_t maxSpanRows = 100000;
            std::uint64_t maxSpanBytes = std::uint64_t{16} << 20;
        };

        CaptureStore();
        explicit CaptureStore(const Limits& limits);
        // Aborts running writes (their temp files are removed) and releases their timelines.
        ~CaptureStore();
        CaptureStore(const CaptureStore&) = delete;
        CaptureStore& operator=(const CaptureStore&) = delete;

        // Takes the timeline without copying it and writes its sidecar at sidecarPath on a worker (a temp file renamed
        // into place, so a reader never sees half a file). report: the zones report's file name, recorded in the meta.
        // Returns at once; the entry is Writing until the worker finishes. Returns 0 and takes nothing (the caller
        // still owns and releases the timeline) when maxPendingWrites are in flight: never blocks the caller.
        CaptureId submit(const prof::CaptureTimeline* timeline, ReleaseFn release,
                         const std::filesystem::path& sidecarPath, std::string report);
        // Two-step submit for a capture whose timeline is not built yet: reserve returns the id at once
        // (0 and nothing taken when maxPendingWrites are reserved or in flight); the entry is Writing and counts as a
        // pending write. Then exactly one of: submit(id, ...) takes the timeline as above (false: the store is closing
        // or the timeline is null, the entry is gone, the caller still owns the timeline), or fail(id, error) marks the
        // entry Failed. The report name recorded in the meta is the one given to reserve().
        CaptureId reserve(const std::filesystem::path& sidecarPath, std::string report);
        bool submit(CaptureId id, const prof::CaptureTimeline* timeline, ReleaseFn release);
        void fail(CaptureId id, std::string error);
        // Opens a sidecar from disk; returns an error text (empty on success) and the entry's id.
        std::string open(const std::filesystem::path& sidecarPath, CaptureId& id);
        // False: no such capture. A Writing capture cannot be closed (the write must finish or the store go).
        bool close(CaptureId id, std::string& error);

        // Grows with every change of what list() would answer: an entry added, removed, evicted, or changing state.
        // A reader that mirrors the list compares one atomic load instead of copying it.
        [[nodiscard]] std::uint64_t generation() const;
        [[nodiscard]] std::vector<CaptureInfo> list() const;
        [[nodiscard]] bool info(CaptureId id, CaptureInfo& out) const;
        // The newest entry that is Ready (0 when none).
        [[nodiscard]] CaptureId newestReady() const;
        // Where files() looks: <root>/<machine>/prof/*.timeline.sqlite.
        void setArtifactsRoot(const std::filesystem::path& root);
        [[nodiscard]] std::filesystem::path artifactsRoot() const;
        // The sidecars under the artifacts root, newest first: at most MAX_FILES, from at most MAX_VISITED directory
        // entries and 200 ms (truncated says the scan stopped early or more existed). Untrusted names: links, non-files
        // and names that are not valid UTF-8 without control characters are skipped. Never throws; an error text
        // (empty on success) for an unreadable root.
        static constexpr std::size_t MAX_FILES = 256;
        static constexpr std::size_t MAX_VISITED = 4096;
        std::string files(std::vector<CaptureFile>& out, bool& truncated) const;
        // Removes <anything>.timeline.sqlite.tmp files in directory that a crash left: last written over 60 s ago and
        // before this store was created (no writer of this process owns them). Returns how many.
        std::size_t removeStaleTemps(const std::filesystem::path& directory) const;
        // Blocks until no write is in flight.
        void waitIdle() const;

        // Diagnostic seam (tests): runs fn while holding the capture's reader (no query of it runs meanwhile); the
        // error of a query that cannot get it, otherwise empty.
        std::string holdReader(CaptureId id, const std::function<void()>& fn);
        // Diagnostic seam (tests): fn runs on the thread of every query the moment it answers "busy", before the answer
        // returns, so a hold can be released at the first busy answer and the retry meets a free capture. An empty fn
        // clears it. fn runs concurrently on whichever threads query: it must be safe to call after the setter returns.
        void setBusyObserver(std::function<void()> fn);

        // Each returns an error text (empty on success): unknown id, still Writing, Failed, a corrupt file, "busy"
        // (another query holds the capture: retry).
        std::string threads(CaptureId id, std::vector<CaptureThread>& out);
        std::string frames(CaptureId id, const CaptureRange& range, CaptureSlice<CaptureFrame>& out,
                           CaptureFrameStats& stats);
        // The frames of the range (offset and limit apply) as CPU and GPU durations: no statistics, no pass rows, 8
        // bytes a frame, for a graph of every frame. The GPU duration of a frame is the longest GPU frame that starts
        // in [its start, the next frame's start).
        std::string frameStrip(CaptureId id, const CaptureRange& range, CaptureStrip& out);
        std::string zones(CaptureId id, const CaptureZoneQuery& query, CaptureSlice<CaptureZone>& out);
        // The frames of a window as spans (no statistics): up to range.limit rows, truncated beyond.
        std::string frameSpans(CaptureId id, const CaptureRange& range, CaptureSlice<CaptureFrame>& out);
        std::string zoneSpans(CaptureId id, const CaptureSpanQuery& query, CaptureSpanSlice& out);
        std::string zoneDetail(CaptureId id, std::uint32_t thread, std::uint64_t seq, CaptureZoneDetail& out);
        std::string zoneDurations(CaptureId id, const CaptureDurationQuery& query, CaptureDurationSlice& out);
        std::string zoneFind(CaptureId id, const CaptureFindQuery& query, CaptureFindResult& out);
        // Whole-capture answers built on the resumable calls above (the Profiler panel's worker and the control
        // commands share them, so both report the same numbers). `lane` -1: every lane. They retry a "busy" capture
        // and stop with "superseded" when cancelled() is true or "interrupted: ..." past budgetMs (0: no budget).
        // The zone after (or, backwards, before) the cursor by (start, lane, seq) whose name contains query.name.
        std::string findZone(CaptureId id, const CaptureFindQuery& query, std::int64_t lane,
                             const CaptureCancelFn& cancelled, std::uint32_t budgetMs, CaptureFindResult& out);
        // One bounded store call of the same search / statistics, for a caller that interleaves other work between the
        // calls (the Profiler worker): `done` true with `out` filled when the last call finished; an error ("busy",
        // "interrupted: ...") leaves the cursor as it was, so the call can be repeated.
        std::string findStep(CaptureId id, CaptureFindCursor& cursor, bool& done, CaptureFindResult& out);
        std::string statisticsStep(CaptureId id, CaptureStatsCursor& cursor, bool& done, CaptureZoneStats& out);
        // Statistics of the zones named exactly `name`; an error when more than maxSamples zones match.
        std::string zoneStatistics(CaptureId id, const std::string& name, std::int64_t lane, std::uint64_t maxSamples,
                                   const CaptureCancelFn& cancelled, std::uint32_t budgetMs, CaptureZoneStats& out);
        // GPU frames (their passes attached) whose start falls in the window; rows are frames.
        std::string gpuFrames(CaptureId id, const CaptureRange& range, CaptureSlice<CaptureGpuFrame>& out);
        // Per-frame counter sums of the frames in the window whose counter name contains name.
        std::string counters(CaptureId id, const CaptureRange& range, const std::string& name,
                             CaptureSlice<CaptureCounter>& out);

        // -- the Profiler GUI's views: the panel's worker and the control queries call these --
        // The capture's counters, by name, with the frames each has a sample in.
        std::string counterNames(CaptureId id, std::vector<CaptureCounterName>& out);
        // One counter (exact name) over frames [first, first + count), clipped to the capture: dense, as the zones
        // report's series. An unknown name is an error; count at most 262144 a call.
        std::string counterSeries(CaptureId id, const std::string& name, std::uint64_t first, std::uint64_t count,
                                  CaptureCounterSeries& out);
        // The memory section as the zones report has it (per-space and per-tag totals) and the big-allocation events
        // in the window, at most maxEvents of them (the largest by bytes, in time order).
        std::string memory(CaptureId id, const CaptureRange& window, std::uint64_t maxEvents, CaptureMemory& out);
        // One GPU frame by index with its passes (the Profiler's GPU-pass selection checks against it).
        std::string gpuFrame(CaptureId id, std::uint64_t index, CaptureGpuFrame& out);
        // The names of a zone and the zones enclosing it, outermost first, the zone last; at most 4096 deep.
        std::string zonePath(CaptureId id, std::uint32_t lane, std::uint64_t seq, std::vector<std::string>& names);
        // The call tree of the zones that began in the window (frames or ns; none: the whole capture), one lane or
        // every lane (-1). Runs as steps of at most a few milliseconds, each holding the capture briefly, retrying a
        // busy capture; stops with "superseded" when cancelled() is true and "interrupted: ..." past budgetMs (0: no
        // budget). Never throws.
        std::string callTree(CaptureId id, const CaptureRange& window, std::int64_t lane,
                             const CaptureCancelFn& cancelled, std::uint32_t budgetMs, CaptureCallTree& out);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace spite
