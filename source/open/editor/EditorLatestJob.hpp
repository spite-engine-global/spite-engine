#pragma once

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace spite
{
    // Runs one computation at a time off the frame thread: a newer start drops the older result (and asks the older
    // job to stop). A job stuck in the file system keeps its thread until it returns; at most `maxRunning` are alive,
    // past that start refuses. Result has a `std::string error`: a job that throws (bad_alloc, filesystem_error)
    // yields a default Result carrying the message, so a worker never terminates the process.
    //
    // The destructor joins every job, so a worker stuck in a call to an unresponsive share delays shutdown until that
    // call returns (cancel only stops cooperative loops). Detaching would let it run into an unloaded SpiteEditor.dll.
    template <typename Result>
    class EditorLatestJob
    {
    public:
        using Fn = std::function<Result(const std::atomic<bool>& cancel)>;

        explicit EditorLatestJob(const unsigned maxRunning = 4) : m_maxRunning(maxRunning) {}
        EditorLatestJob(const EditorLatestJob&) = delete;
        EditorLatestJob& operator=(const EditorLatestJob&) = delete;
        ~EditorLatestJob()
        {
            for (const std::shared_ptr<Job>& job : m_jobs)
            {
                job->cancel.store(true);
            }
            for (const std::shared_ptr<Job>& job : m_jobs)
            {
                job->thread.join();
            }
        }

        // False when maxRunning are still alive.
        bool start(Fn fn)
        {
            reap();
            if (m_jobs.size() >= m_maxRunning)
            {
                return false;
            }
            if (m_current)
            {
                m_current->cancel.store(true);
            }
            auto job = std::make_shared<Job>();
            m_current = job;
            m_jobs.push_back(job);
            job->thread = std::thread(
                [job, fn = std::move(fn)]
                {
                    try
                    {
                        job->result = fn(job->cancel);
                    }
                    catch (const std::exception& error)
                    {
                        job->result = Result{};
                        job->result->error = error.what();
                    }
                    catch (...)
                    {
                        job->result = Result{};
                        job->result->error = "unknown error";
                    }
                    job->done.store(true, std::memory_order_release);
                });
            return true;
        }

        // The finished current job's result, once.
        std::optional<Result> poll()
        {
            if (!m_current || !m_current->done.load(std::memory_order_acquire))
            {
                return std::nullopt;
            }
            std::optional<Result> result = std::move(m_current->result);
            m_current.reset();
            reap();
            return result;
        }

        [[nodiscard]] bool busy() const { return m_current != nullptr; }

    private:
        struct Job
        {
            std::atomic<bool> cancel{false};
            std::atomic<bool> done{false};
            std::optional<Result> result;
            std::thread thread;
        };

        void reap()
        {
            std::erase_if(m_jobs,
                          [](const std::shared_ptr<Job>& job)
                          {
                              if (!job->done.load(std::memory_order_acquire))
                              {
                                  return false;
                              }
                              job->thread.join();
                              return true;
                          });
        }

        unsigned m_maxRunning;
        std::vector<std::shared_ptr<Job>> m_jobs;
        std::shared_ptr<Job> m_current;
    };
} // namespace spite
