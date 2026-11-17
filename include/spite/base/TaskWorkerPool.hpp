#pragma once
#include <any>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

#include "base/BaseExport.hpp"
#include "base/Expected.hpp"
#include "base/InternedString.hpp"

namespace spite
{
    // Unique identifier for a submitted task
    struct TaskId
    {
        u64 value = 0;

        bool isValid() const { return value != 0; }
        auto operator<=>(const TaskId&) const = default;

        struct Hash
        {
            sizet operator()(const TaskId& id) const { return std::hash<u64>{}(id.value); }
        };
    };

    // Result of any IO task
    struct TaskResult
    {
        TaskId taskId;
        InternedString identifier; // Path or other identifier
        Expected<std::any> result;

        TaskResult() = default;

        TaskResult(TaskId id, InternedString ident, Expected<std::any> res) :
            taskId(id), identifier(ident), result(std::move(res))
        {
        }

        TaskResult(TaskResult&& other) noexcept = default;
        TaskResult& operator=(TaskResult&& other) noexcept = default;
        TaskResult(const TaskResult&) = delete;
        TaskResult& operator=(const TaskResult&) = delete;
    };

    using WorkerTaskFunc = std::function<Expected<std::any>()>;

    // Completion callback signature - called on worker thread when task completes
    using TaskCompletionCallback = std::function<void(TaskResult)>;

    // Configuration for the worker pool
    struct TaskWorkerPoolConfig
    {
        u32 workerCount = 0; // 0 = auto-detect (hardware_concurrency - 1, min 1)
        const char* debugName = "TaskWorkerPool";
    };

    // A task pending submission or execution
    struct PendingTask
    {
        TaskId taskId;
        InternedString identifier;
        WorkerTaskFunc task;
        TaskCompletionCallback completionCallback;
        std::promise<TaskResult> promise;
    };

    // Multi-threaded worker pool for async IO operations
    class SPITE_BASE_API TaskWorkerPool
    {
    public:
        TaskWorkerPool();
        ~TaskWorkerPool();

        TaskWorkerPool(const TaskWorkerPool&) = delete;
        TaskWorkerPool(TaskWorkerPool&&) = delete;
        TaskWorkerPool& operator=(const TaskWorkerPool&) = delete;
        TaskWorkerPool& operator=(TaskWorkerPool&&) = delete;

        // Initialize the pool with the given configuration and completion callback
        void initialize(const TaskWorkerPoolConfig& config);

        // Gracefully shutdown all workers, waiting for pending tasks to complete;
        // later calls return at once
        void shutdown();

        // Submit a task for execution. Returns a TaskId for tracking.
        TaskId submit(const InternedString& identifier, WorkerTaskFunc&& task,
                      TaskCompletionCallback completionCallback);

        // Submit a task and get a future for the result
        std::future<TaskResult> submitWithFuture(const InternedString& identifier, WorkerTaskFunc&& task,
                                                 TaskCompletionCallback completionCallback);

        // Check if the pool is running
        bool isRunning() const { return m_running.load(std::memory_order_acquire); }

        // Get the number of pending tasks in the queue
        sizet getPendingCount() const;

        // Get the number of worker threads
        u32 getWorkerCount() const { return static_cast<u32>(m_workers.size()); }

    private:
        void workerLoop(u32);
        TaskId generateTaskId();

        std::vector<std::thread> m_workers;

        std::deque<PendingTask> m_taskQueue;
        mutable std::mutex m_queueMutex;
        std::condition_variable m_condition;

        std::atomic<bool> m_running{false};
        std::atomic<u64> m_nextTaskId{1};

        std::string m_debugName;
    };
} // namespace spite
