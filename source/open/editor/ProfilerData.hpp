#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/Platform.hpp"
#include "engine/CaptureStore.hpp"

namespace spite
{
    // The most frames the frame graph holds: the series capacity, 1 MiB a series. A longer capture shows its first
    // frames and says so (ProfilerEvent::capped).
    inline constexpr u32 PROFILER_MAX_GRAPH_FRAMES = 1u << 18;
    inline constexpr u32 PROFILER_PAGE_FRAMES = 8192;
    inline constexpr u32 PROFILER_MIN_PAGE_FRAMES = 256;
    inline constexpr u32 PROFILER_MAX_RETRIES = 100; // consecutive busy or interrupted answers before a load fails
    inline constexpr u32 PROFILER_MARK_ROWS = 4096; // frame boundaries a view reads at most
    inline constexpr u64 PROFILER_MAX_STAT_ZONES = 4'000'000; // zones one name's statistics cover at most
    inline constexpr u64 PROFILER_MAX_LANE_SPANS = 400000; // spans one lane of a view holds at most
    inline constexpr double PROFILER_BUDGET_MS = 1000.0 / 60.0;
    inline constexpr double PROFILER_BUDGET_SLOW_MS = 1000.0 / 30.0;

    struct ProfilerFrameStats
    {
        u64 count = 0;
        double meanMs = 0.0, minMs = 0.0, p50Ms = 0.0, p99Ms = 0.0, maxMs = 0.0;
        u64 overBudget = 0; // frames over 16.7 ms
        u64 overSlow = 0; // frames over 33.3 ms
    };

    // CPU frame statistics over durations in ns (sorted by the call).
    [[nodiscard]] ProfilerFrameStats profilerFrameStats(std::vector<u32>& cpuNs);

    // The kinds of request the worker serves; each has its own supersede token, so a newer request of a kind replaces
    // the one in flight and requests of other kinds go on.
    enum class ProfilerSlot : u8
    {
        Load,
        View, // the timeline's spans for a window
        Hover, // the zone detail of the tooltip
        Select, // the zone detail of the selection
        Zoom, // the zone detail of a zoom target
        Stats, // the statistics of a zone name
        Find, // next/previous occurrence of a name
        Count
    };

    // What the worker hands the frame thread, in order: Lanes, Page..., then Done or Error for a load; Files for a
    // scan; Marks, Spans... then ViewDone for a view; one Detail, Stats or Find for those requests.
    struct ProfilerEvent
    {
        enum class Kind : u8
        {
            Lanes,
            Page,
            Done,
            Error,
            Files,
            Task, // a view's task (requestTask): its payload, or error; below Marks: kinds from Marks on are the
                  // timeline's
            Marks,
            Spans,
            ViewDone,
            Detail,
            Stats,
            Find,
        };
        Kind kind = Kind::Error;
        u64 token = 0; // the request's (its slot's)
        CaptureId capture = 0;
        std::vector<u64> laneZones; // Lanes: zones per lane
        std::vector<CaptureThread> lanes; // Lanes
        std::int64_t durationNs = 0; // Lanes
        u64 frames = 0; // Lanes: the capture's frames
        bool gpuCalibrated = false; // Lanes
        u64 first = 0; // Page: the frame index of the first row
        std::vector<CaptureStripFrame> rows; // Page
        ProfilerFrameStats stats; // Done
        bool capped = false; // Done: the capture has more frames than the graph holds
        std::string
            error; // Error, Task, ViewDone, Detail, Stats, Find: why the request failed ("superseded": replaced)
        std::vector<CaptureFile> files; // Files
        bool truncated = false; // Files; Marks: more frames than PROFILER_MARK_ROWS
        std::vector<CaptureFrame> marks; // Marks: the window's frames
        u32 lane = 0; // Spans; Detail: the lane asked for
        std::vector<CaptureSpan> spans; // Spans: one lane's, sorted by (depth, start)
        bool scanCut = false; // Spans: the lane's scan was cut short (the row bound)
        std::int64_t fromNs = 0, toNs = 0; // Spans: the window the spans were cut for
        u32 columns = 0; // Spans: at this many pixel columns
        CaptureZoneDetail detail; // Detail
        u64 tag = 0; // Detail: what the caller wanted it for; Find: the caller's id
        std::string name; // Stats
        CaptureZoneStats zoneStats; // Stats
        CaptureFindResult found; // Find
        u32 view = 0; // Task: the id the view gave (token: the generation it ran for)
        std::shared_ptr<void> payload; // Task: what the closure returned, null on an error
    };

    // What a view asks of the store: every listed lane's spans for [fromNs, toNs) at `columns` pixel columns.
    struct ProfilerViewRequest
    {
        CaptureId capture = 0;
        std::int64_t fromNs = 0, toNs = 1;
        u32 columns = 1024;
        u32 maxDepth = CAPTURE_MAX_SPAN_DEPTH;
        std::vector<u32> lanes;
    };

    // The Profiler panel's store reader (/434/435/436): every query that can run long (the sidecar scan, a
    // capture's lanes and frame strip, paged; a view's spans lane by lane; zone statistics; a find) runs on one
    // below-normal worker, so the frame thread only takes finished results. The store is thread-safe and bounds every
    // call (150 ms, rows, bytes); a call that hits its deadline is retried at half the size, and at the smallest size
    // (or when the capture is busy with another query) again after a pause, up to PROFILER_MAX_RETRIES in a row. A
    // view, a statistics scan and a find are resumable: the worker turns one call at a time and puts the rest at the
    // back of the queue, so a zone detail (put at the front) never waits behind a long scan. The thread starts with
    // the first request. The destructor waits for the call in flight.
    // Invariants: P1 the frame thread is the only requester and the only reader of the event queue, so `busy` counts
    // exactly the requests whose last event it has not taken (Done, Error, Files, ViewDone, Detail, Stats or Find);
    // P2 the worker touches the store, which is safe from any thread, and nothing else of the panel; P3 a superseded
    // request ends with one terminal event with error "superseded" and queues nothing after the worker sees the new
    // token, while events already queued keep their token and the panel drops them.
    class ProfilerData
    {
    public:
        explicit ProfilerData(CaptureStore& store);
        ~ProfilerData();
        ProfilerData(const ProfilerData&) = delete;
        ProfilerData& operator=(const ProfilerData&) = delete;

        void requestFiles();
        // Loads the capture; a later requestLoad (any id, 0: just cancel) supersedes it: the worker stops at the next
        // page and answers Error "superseded".
        void requestLoad(CaptureId capture, u64 token);
        void requestView(ProfilerViewRequest request, u64 token);
        // Slot Hover or Select.
        void requestDetail(ProfilerSlot slot, CaptureId capture, u32 lane, u64 seq, u64 token, u64 tag);
        void requestStats(CaptureId capture, std::string name, u64 token);
        void requestFind(CaptureId capture, CaptureFindQuery query, std::int64_t lane, u64 token);
        // Supersedes the slot's request without a new one (the view of a capture that went away).
        void cancel(ProfilerSlot slot, u64 token);
        // A view's job on the same worker: `run` gets a cancelled that turns true once
        // `generation` no longer holds `value` (the view asked for something newer) or the data is being destroyed,
        // and returns the payload or fills `error`. Answers one Task event (token = value) unless superseded before it
        // started, which still answers with error "superseded". Runs after the jobs queued before it.
        using TaskFn = std::function<std::shared_ptr<void>(const CaptureCancelFn& cancelled, std::string& error)>;
        // longRunning (a child process, seconds): a second worker serves it, so the timeline's and the other views'
        // jobs never wait behind it.
        void requestTask(u32 view, std::shared_ptr<std::atomic<u64>> generation, u64 value, TaskFn run,
                         bool longRunning = false);
        // The next event, false when none.
        bool take(ProfilerEvent& out);
        // Tasks requested since the data was made (a hidden view requests none)
        [[nodiscard]] u64 tasksRequested() const { return m_tasks.load(std::memory_order_relaxed); }
        // A request has no terminal event taken yet: the frame thread polls only while this holds.
        [[nodiscard]] bool busy() const { return m_open.load(std::memory_order_relaxed) != 0; }

    private:
        enum class JobKind : u8
        {
            Files,
            Load,
            View,
            Detail,
            Stats,
            Find,
            Task
        };

        struct Job
        {
            JobKind kind = JobKind::Load;
            ProfilerSlot slot = ProfilerSlot::Load;
            CaptureId capture = 0;
            u64 token = 0;
            u64 tag = 0;
            // View: where the turn is
            ProfilerViewRequest view;
            bool marksDone = false;
            u32 laneAt = 0;
            std::int64_t after = -1;
            std::vector<CaptureSpan> spans; // the lane so far
            // Detail
            u32 lane = 0;
            u64 seq = 0;
            // Stats, Find: where the search is
            CaptureStatsCursor statsCursor;
            CaptureFindCursor findCursor;
            u32 retries = 0;
            // Task
            TaskFn task;
            u32 taskView = 0;
            std::shared_ptr<std::atomic<u64>> generation;
        };

        void run(std::deque<Job>& queue);
        void scan();
        void load(const Job& job);
        bool viewStep(Job& job); // true: more turns follow
        void detail(const Job& job);
        bool searchStep(Job& job); // Stats or Find; true: more turns follow
        void runTask(const Job& job);
        void push(ProfilerEvent event);
        void enqueue(Job job, bool front);
        void startWorker(bool longQueue = false); // under m_mutex
        // The error of a store call that is worth repeating (the capture is busy, or the deadline hit)
        [[nodiscard]] static bool transient(const std::string& error);
        // True while the request should go on after a pause; false: superseded or out of retries (error filled)
        bool pause(const Job& job, u32& retries, std::string& error);
        [[nodiscard]] bool superseded(const Job& job) const
        {
            return m_tokens[static_cast<sizet>(job.slot)].load() != job.token;
        }
        void finish(const Job& job, ProfilerEvent::Kind kind, std::string error);

        CaptureStore* m_store;
        std::mutex m_mutex;
        std::condition_variable m_wake;
        std::deque<Job> m_jobs;
        std::deque<Job> m_longJobs; // requestTask(longRunning)
        std::deque<ProfilerEvent> m_events;
        std::array<std::atomic<u64>, static_cast<sizet>(ProfilerSlot::Count)> m_tokens{};
        std::atomic<u32> m_open{0};
        std::atomic<u64> m_tasks{0};
        bool m_stopping = false;
        std::atomic<bool> m_quit{false}; // m_stopping for a task's cancelled(), read without the mutex
        bool m_filesQueued = false; // a scan job waits: another request adds nothing
        std::thread m_worker; // started by the first request
        std::thread m_longWorker; // last: started by the first long task
    };
} // namespace spite
