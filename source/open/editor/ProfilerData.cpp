#include "ProfilerData.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <numeric>
#include <tuple>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace spite
{
    ProfilerFrameStats profilerFrameStats(std::vector<u32>& cpuNs)
    {
        ProfilerFrameStats stats;
        if (cpuNs.empty())
        {
            return stats;
        }
        std::ranges::sort(cpuNs);
        const auto at = [&](const double q)
        { return static_cast<double>(cpuNs[static_cast<sizet>(q * static_cast<double>(cpuNs.size() - 1))]) * 1e-6; };
        stats.count = cpuNs.size();
        stats.meanMs = std::accumulate(cpuNs.begin(), cpuNs.end(), 0.0) / static_cast<double>(cpuNs.size()) * 1e-6;
        stats.minMs = static_cast<double>(cpuNs.front()) * 1e-6;
        stats.p50Ms = at(0.5);
        stats.p99Ms = at(0.99);
        stats.maxMs = static_cast<double>(cpuNs.back()) * 1e-6;
        const auto over = [&](const double ms)
        {
            const auto limit = static_cast<u32>(std::min(ms * 1e6, 4.0e9));
            return static_cast<u64>(cpuNs.end() - std::ranges::upper_bound(cpuNs, limit));
        };
        stats.overBudget = over(PROFILER_BUDGET_MS);
        stats.overSlow = over(PROFILER_BUDGET_SLOW_MS);
        return stats;
    }

    ProfilerData::ProfilerData(CaptureStore& store) : m_store(&store) {}

    ProfilerData::~ProfilerData()
    {
        {
            const std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        for (std::atomic<u64>& token : m_tokens)
        {
            token.store(0);
        }
        m_quit.store(true);
        m_wake.notify_all();
        if (m_worker.joinable())
        {
            m_worker.join();
        }
        if (m_longWorker.joinable())
        {
            m_longWorker.join();
        }
    }

    void ProfilerData::startWorker(const bool longQueue)
    {
        std::thread& worker = longQueue ? m_longWorker : m_worker;
        if (!worker.joinable())
        {
            std::deque<Job>& queue = longQueue ? m_longJobs : m_jobs;
            worker = std::thread([this, &queue] { run(queue); });
        }
    }

    bool ProfilerData::transient(const std::string& error)
    {
        return error.starts_with("busy") || error.starts_with("interrupted");
    }

    bool ProfilerData::pause(const Job& job, u32& retries, std::string& error)
    {
        if (++retries > PROFILER_MAX_RETRIES)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (superseded(job))
        {
            error = "superseded";
            return false;
        }
        return true;
    }

    void ProfilerData::enqueue(Job job, const bool front)
    {
        {
            const std::lock_guard lock(m_mutex);
            startWorker(); // before the job is queued: a thread that cannot start leaves nothing behind
            if (front)
            {
                m_jobs.push_front(std::move(job));
            }
            else
            {
                m_jobs.push_back(std::move(job));
            }
            m_open.fetch_add(1);
        }
        m_wake.notify_all();
    }

    void ProfilerData::requestFiles()
    {
        {
            const std::lock_guard lock(m_mutex);
            if (m_filesQueued)
            {
                return;
            }
            m_filesQueued = true;
        }
        enqueue({.kind = JobKind::Files}, false);
    }

    void ProfilerData::requestLoad(const CaptureId capture, const u64 token)
    {
        m_tokens[static_cast<sizet>(ProfilerSlot::Load)].store(token);
        if (capture != 0)
        {
            enqueue({.kind = JobKind::Load, .slot = ProfilerSlot::Load, .capture = capture, .token = token}, false);
        }
    }

    void ProfilerData::cancel(const ProfilerSlot slot, const u64 token)
    {
        m_tokens[static_cast<sizet>(slot)].store(token);
    }

    void ProfilerData::requestView(ProfilerViewRequest request, const u64 token)
    {
        m_tokens[static_cast<sizet>(ProfilerSlot::View)].store(token);
        Job job{.kind = JobKind::View, .slot = ProfilerSlot::View, .capture = request.capture, .token = token};
        job.view = std::move(request);
        enqueue(std::move(job), false);
    }

    void ProfilerData::requestDetail(const ProfilerSlot slot, const CaptureId capture, const u32 lane, const u64 seq,
                                     const u64 token, const u64 tag)
    {
        m_tokens[static_cast<sizet>(slot)].store(token);
        Job job{.kind = JobKind::Detail, .slot = slot, .capture = capture, .token = token, .tag = tag};
        job.lane = lane;
        job.seq = seq;
        enqueue(std::move(job), true); // a few microseconds of work: ahead of the scans
    }

    void ProfilerData::requestStats(const CaptureId capture, std::string name, const u64 token)
    {
        m_tokens[static_cast<sizet>(ProfilerSlot::Stats)].store(token);
        Job job{.kind = JobKind::Stats, .slot = ProfilerSlot::Stats, .capture = capture, .token = token};
        job.statsCursor = {.name = std::move(name), .lane = -1, .maxSamples = PROFILER_MAX_STAT_ZONES};
        enqueue(std::move(job), false);
    }

    void ProfilerData::requestFind(const CaptureId capture, CaptureFindQuery query, const std::int64_t lane,
                                   const u64 token)
    {
        m_tokens[static_cast<sizet>(ProfilerSlot::Find)].store(token);
        Job job{.kind = JobKind::Find, .slot = ProfilerSlot::Find, .capture = capture, .token = token};
        job.findCursor = {.query = std::move(query), .lane = lane};
        enqueue(std::move(job), false);
    }

    void ProfilerData::requestTask(const u32 view, std::shared_ptr<std::atomic<u64>> generation, const u64 value,
                                   TaskFn run, const bool longRunning)
    {
        {
            const std::lock_guard lock(m_mutex);
            startWorker(longRunning);
            (longRunning ? m_longJobs : m_jobs)
                .push_back({.kind = JobKind::Task,
                            .token = value,
                            .task = std::move(run),
                            .taskView = view,
                            .generation = std::move(generation)});
            m_open.fetch_add(1);
            m_tasks.fetch_add(1, std::memory_order_relaxed);
        }
        m_wake.notify_all();
    }

    bool ProfilerData::take(ProfilerEvent& out)
    {
        const std::lock_guard lock(m_mutex);
        if (m_events.empty())
        {
            return false;
        }
        out = std::move(m_events.front());
        m_events.pop_front();
        using Kind = ProfilerEvent::Kind;
        if (out.kind == Kind::Done || out.kind == Kind::Error || out.kind == Kind::Files ||
            out.kind == Kind::ViewDone || out.kind == Kind::Detail || out.kind == Kind::Stats ||
            out.kind == Kind::Find || out.kind == Kind::Task)
        {
            m_open.fetch_sub(1);
        }
        return true;
    }

    void ProfilerData::push(ProfilerEvent event)
    {
        const std::lock_guard lock(m_mutex);
        m_events.push_back(std::move(event));
    }

    void ProfilerData::run(std::deque<Job>& queue)
    {
#if defined(_WIN32)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
        nice(1);
#endif
        for (;;)
        {
            Job job;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [this, &queue] { return m_stopping || !queue.empty(); });
                if (m_stopping)
                {
                    return;
                }
                job = std::move(queue.front());
                queue.pop_front();
                m_filesQueued = m_filesQueued && job.kind != JobKind::Files;
            }
            try
            {
                switch (job.kind)
                {
                case JobKind::Files:
                    scan();
                    break;
                case JobKind::Load:
                    load(job);
                    break;
                case JobKind::View:
                    if (viewStep(job))
                    {
                        const std::lock_guard lock(m_mutex); // the rest of the view goes behind whatever else waits
                        m_jobs.push_back(std::move(job));
                    }
                    break;
                case JobKind::Detail:
                    detail(job);
                    break;
                case JobKind::Task:
                    runTask(job);
                    break;
                case JobKind::Stats:
                case JobKind::Find:
                    if (searchStep(job))
                    {
                        const std::lock_guard lock(m_mutex);
                        m_jobs.push_back(std::move(job));
                    }
                    break;
                }
            }
            catch (const std::exception& e)
            {
                // an allocation failure while reporting must not leave the thread: the request just ends
                try
                {
                    using Kind = ProfilerEvent::Kind;
                    static constexpr Kind ENDS[] = {Kind::Files, Kind::Error, Kind::ViewDone, Kind::Detail,
                                                    Kind::Stats, Kind::Find,  Kind::Task};
                    finish(job, ENDS[static_cast<sizet>(job.kind)], e.what());
                }
                catch (...)
                {
                    m_open.fetch_sub(1);
                }
            }
            catch (...)
            {
                m_open.fetch_sub(1);
            }
        }
    }

    void ProfilerData::runTask(const Job& job)
    {
        ProfilerEvent event;
        event.kind = ProfilerEvent::Kind::Task;
        event.view = job.taskView;
        event.token = job.token;
        const CaptureCancelFn cancelled = [this, &job]
        { return m_quit.load(std::memory_order_relaxed) || job.generation->load() != job.token; };
        if (cancelled())
        {
            event.error = "superseded";
        }
        else
        {
            event.payload = job.task(cancelled, event.error);
        }
        push(std::move(event));
    }

    void ProfilerData::scan()
    {
        ProfilerEvent event;
        event.kind = ProfilerEvent::Kind::Files;
        event.error = m_store->files(event.files, event.truncated);
        push(std::move(event));
    }

    void ProfilerData::load(const Job& job)
    {
        ProfilerEvent head;
        head.token = job.token;
        head.capture = job.capture;
        const auto fail = [&](std::string error)
        {
            head.kind = ProfilerEvent::Kind::Error;
            head.error = std::move(error);
            push(head);
        };
        if (superseded(job))
        {
            return fail("superseded");
        }
        CaptureInfo info;
        if (!m_store->info(job.capture, info) || info.state != CaptureInfo::State::Ready)
        {
            return fail("capture " + std::to_string(job.capture) + " is not ready");
        }
        std::vector<CaptureThread> lanes;
        u32 retries = 0;
        for (;;)
        {
            std::string error = m_store->threads(job.capture, lanes);
            if (error.empty())
            {
                break;
            }
            lanes.clear();
            if (!transient(error) || !pause(job, retries, error))
            {
                return fail(std::move(error));
            }
        }
        ProfilerEvent meta = head;
        meta.kind = ProfilerEvent::Kind::Lanes;
        meta.frames = info.frames;
        meta.gpuCalibrated = info.gpuCalibrated;
        meta.durationNs = info.durationNs;
        for (const CaptureThread& lane : lanes)
        {
            meta.laneZones.push_back(lane.zones);
        }
        meta.lanes = std::move(lanes);
        push(std::move(meta));

        std::vector<u32> cpuNs;
        const u64 wanted = std::min<u64>(info.frames, PROFILER_MAX_GRAPH_FRAMES);
        cpuNs.reserve(wanted);
        u64 page = PROFILER_PAGE_FRAMES;
        retries = 0;
        u32 successes = 0;
        while (cpuNs.size() < wanted)
        {
            if (superseded(job))
            {
                return fail("superseded");
            }
            CaptureRange range;
            range.offset = cpuNs.size();
            range.limit = std::min<u64>(page, wanted - cpuNs.size());
            CaptureStrip strip;
            if (std::string error = m_store->frameStrip(job.capture, range, strip); !error.empty())
            {
                // the store's deadline: the same frames in smaller pages; at the smallest, or when the capture is busy,
                // again after a pause
                successes = 0;
                if (error.starts_with("interrupted") && page > PROFILER_MIN_PAGE_FRAMES)
                {
                    page /= 2;
                    continue;
                }
                if (!transient(error) || !pause(job, retries, error))
                {
                    return fail(std::move(error));
                }
                continue;
            }
            retries = 0;
            if (++successes >= 4 && page < PROFILER_PAGE_FRAMES)
            {
                page = std::min<u64>(page * 2, PROFILER_PAGE_FRAMES); // the pressure passed
                successes = 0;
            }
            if (strip.rows.empty())
            {
                break;
            }
            for (const CaptureStripFrame& row : strip.rows)
            {
                cpuNs.push_back(row.cpuNs);
            }
            ProfilerEvent event = head;
            event.kind = ProfilerEvent::Kind::Page;
            event.first = strip.first;
            event.rows = std::move(strip.rows);
            push(std::move(event));
        }
        ProfilerEvent done = head;
        done.kind = ProfilerEvent::Kind::Done;
        done.capped = info.frames > wanted;
        done.stats = profilerFrameStats(cpuNs);
        push(std::move(done));
    }

    void ProfilerData::finish(const Job& job, const ProfilerEvent::Kind kind, std::string error)
    {
        ProfilerEvent event;
        event.kind = kind;
        event.token = job.token;
        event.capture = job.capture;
        event.tag = job.tag;
        event.lane = job.lane;
        event.view = job.taskView;
        event.error = std::move(error);
        push(std::move(event));
    }

    bool ProfilerData::viewStep(Job& job)
    {
        using Kind = ProfilerEvent::Kind;
        if (superseded(job))
        {
            finish(job, Kind::ViewDone, "superseded");
            return false;
        }
        const ProfilerViewRequest& view = job.view;
        std::string error;
        if (!job.marksDone)
        {
            CaptureRange range;
            range.fromNs = view.fromNs;
            range.toNs = view.toNs;
            range.limit = PROFILER_MARK_ROWS;
            CaptureSlice<CaptureFrame> marks;
            error = m_store->frameSpans(job.capture, range, marks);
            if (error.empty())
            {
                ProfilerEvent event;
                event.kind = Kind::Marks;
                event.token = job.token;
                event.capture = job.capture;
                event.marks = std::move(marks.rows);
                event.truncated = marks.truncated;
                push(std::move(event));
                job.marksDone = true;
                job.retries = 0;
                return true;
            }
        }
        else if (job.laneAt < view.lanes.size())
        {
            const u32 lane = view.lanes[job.laneAt];
            CaptureSpanSlice slice;
            error = m_store->zoneSpans(job.capture,
                                       {lane, view.fromNs, view.toNs, view.columns, view.maxDepth, job.after}, slice);
            if (error.empty())
            {
                job.retries = 0;
                job.spans.insert(job.spans.end(), std::make_move_iterator(slice.rows.begin()),
                                 std::make_move_iterator(slice.rows.end()));
                const bool cut = !slice.done && job.spans.size() >= PROFILER_MAX_LANE_SPANS;
                if (slice.done || cut)
                {
                    std::ranges::sort(job.spans, [](const CaptureSpan& a, const CaptureSpan& b)
                                      { return std::tie(a.depth, a.startNs) < std::tie(b.depth, b.startNs); });
                    ProfilerEvent event;
                    event.kind = Kind::Spans;
                    event.token = job.token;
                    event.capture = job.capture;
                    event.lane = lane;
                    event.scanCut = cut;
                    event.fromNs = view.fromNs;
                    event.toNs = view.toNs;
                    event.columns = view.columns;
                    event.spans = std::move(job.spans);
                    push(std::move(event));
                    job.spans.clear();
                    job.after = -1;
                    ++job.laneAt;
                }
                else
                {
                    job.after = static_cast<std::int64_t>(slice.next);
                }
                if (job.laneAt >= view.lanes.size())
                {
                    finish(job, Kind::ViewDone, {});
                    return false;
                }
                return true;
            }
        }
        else
        {
            finish(job, Kind::ViewDone, {});
            return false;
        }
        if (!transient(error) || !pause(job, job.retries, error))
        {
            finish(job, Kind::ViewDone, std::move(error));
            return false;
        }
        return true;
    }

    void ProfilerData::detail(const Job& job)
    {
        u32 retries = 0;
        ProfilerEvent event;
        event.kind = ProfilerEvent::Kind::Detail;
        event.token = job.token;
        event.capture = job.capture;
        event.tag = job.tag;
        event.lane = job.lane;
        for (;;)
        {
            if (superseded(job))
            {
                event.error = "superseded";
                break;
            }
            event.error = m_store->zoneDetail(job.capture, job.lane, job.seq, event.detail);
            if (event.error.empty() || !event.error.starts_with("busy") || !pause(job, retries, event.error))
            {
                break;
            }
        }
        push(std::move(event));
    }

    bool ProfilerData::searchStep(Job& job)
    {
        using Kind = ProfilerEvent::Kind;
        const bool isStats = job.kind == JobKind::Stats;
        ProfilerEvent event;
        event.kind = isStats ? Kind::Stats : Kind::Find;
        event.token = job.token;
        event.capture = job.capture;
        event.tag = job.tag;
        event.name = isStats ? job.statsCursor.name : job.findCursor.query.name;
        bool done = false;
        if (superseded(job))
        {
            event.error = "superseded";
        }
        else
        {
            event.error = isStats ? m_store->statisticsStep(job.capture, job.statsCursor, done, event.zoneStats)
                                  : m_store->findStep(job.capture, job.findCursor, done, event.found);
            if (event.error.empty())
            {
                job.retries = 0;
                if (!done)
                {
                    return true; // one bounded call per turn: a detail request goes ahead of the next
                }
            }
            else if (transient(event.error) && pause(job, job.retries, event.error))
            {
                return true;
            }
        }
        push(std::move(event));
        return false;
    }
} // namespace spite
