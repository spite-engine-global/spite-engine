#pragma once
#include <atomic>
#include <mutex>

#include "base/BaseExport.hpp"
#include "base/Expected.hpp"
#include "base/TaskWorkerPool.hpp"

namespace spite
{
    // Function signature for a load task
    using LoadTaskFunc = std::function<Expected<std::any>()>;

    // Function signature for a save task
    using SaveTaskFunc = std::function<Expected<void>()>;

    // Completion event types
    enum class IOEventType : u8
    {
        LoadCompleted,
        LoadFailed,
        SaveCompleted,
        SaveFailed
    };

    // Event dispatched when an IO operation completes
    struct IOCompletionEvent
    {
        IOEventType type;
        TaskId taskId;
        InternedString path;
        Expected<std::any> result; // For loads; empty for saves

        bool isSuccess() const { return type == IOEventType::LoadCompleted || type == IOEventType::SaveCompleted; }
        bool isLoad() const { return type == IOEventType::LoadCompleted || type == IOEventType::LoadFailed; }
    };

    // Callback invoked when IO operations complete (called on worker thread)
    using IOCompletionHandler = std::function<void(const IOCompletionEvent&)>;

    struct SaveRequest
    {
        TaskId taskId; // the pending save's when one was already pending; invalid when the workers are stopped
        bool submitted = false; // false: the task and handler were dropped
    };

    // Configuration for the IOManager
    struct IOManagerConfig
    {
        u32 workerThreadCount = 0; // 0 = auto-detect
    };

    // Unified manager for all async IO operations
    // - Handles both loading and saving
    // - Deduplicates in-flight requests
    // - Event-driven completion model
    class SPITE_BASE_API IOManager
    {
    public:
        IOManager(const IOManagerConfig& config);
        ~IOManager();

        IOManager(const IOManager&) = delete;
        IOManager(IOManager&&) = delete;
        IOManager& operator=(const IOManager&) = delete;
        IOManager& operator=(IOManager&&) = delete;

        // Request an asset load. Returns TaskId for tracking.
        // If the asset is already loading, returns the existing TaskId.
        // The loaded result will be dispatched via completion events.
        TaskId requestLoad(const InternedString& path, LoadTaskFunc&& task, const IOCompletionHandler& handler);

        // Check if an asset is currently being loaded
        bool isLoadPending(const InternedString& path) const;

        // Cancel a pending load request (best-effort, may still complete)
        void cancelLoad(const InternedString& path);

        // Request an asset save.
        // Invariant: two saves of one path never run at once or out of order: a save requested while one of the
        //   path is pending is refused (submitted false, task and handler dropped); the caller reports or retries it.
        SaveRequest requestSave(const InternedString& path, SaveTaskFunc&& task, const IOCompletionHandler& handler);

        // Get the number of pending operations (loads + saves)
        sizet getPendingCount() const;

        // Total load requests accepted (including deduplicated joins); lets
        // callers observe whether an operation triggered IO at all
        u32 getLoadRequestCount() const { return m_loadRequestCount.load(std::memory_order_relaxed); }

        // Get the underlying worker pool for advanced usage
        TaskWorkerPool& getWorkerPool() { return m_workerPool; }

    private:
        void onLoadTaskCompleted(TaskResult result);
        void onSaveTaskCompleted(TaskResult result, IOCompletionHandler handler);

        // Request tracking for deduplication
        struct PendingRequest
        {
            TaskId taskId;
            bool isLoadRequest = true;
        };

        struct PendingLoadRequest
        {
            TaskId taskId;
            std::vector<IOCompletionHandler> completionHandlers;
        };

        TaskWorkerPool m_workerPool;

        // Pending request tracking for deduplication
        std::unordered_map<InternedString, PendingLoadRequest> m_pendingLoads;
        std::unordered_map<InternedString, PendingRequest> m_pendingSaves;
        mutable std::mutex m_pendingMutex;
        std::atomic<u32> m_loadRequestCount{0};
    };
} // namespace spite
