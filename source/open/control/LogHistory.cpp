#include "control/LogHistory.hpp"

#include <algorithm>
#include <vector>

#include "base/ServiceLocator.hpp"

namespace spite
{
    namespace
    {
        const char* levelName(const LogLevel level)
        {
            switch (level)
            {
            case LogLevel::Debug:
                return "debug";
            case LogLevel::Info:
                return "info";
            case LogLevel::Success:
                return "success";
            case LogLevel::Warning:
                return "warning";
            case LogLevel::Error:
                return "error";
            default:
                return "none";
            }
        }

        class HistorySink final : public ILogSink
        {
        public:
            explicit HistorySink(std::shared_ptr<LogHistory> history) : m_history(std::move(history)) {}
            void consume(const LogEvent& event) override { m_history->record(event); }
            void flush() override {}

        private:
            std::shared_ptr<LogHistory> m_history;
        };
    } // namespace

    LogHistory::LogHistory(const sizet capacity) : m_capacity(capacity) {}

    std::shared_ptr<LogHistory> LogHistory::attach(ServiceLocator& services, const sizet capacity)
    {
        auto history = std::make_shared<LogHistory>(capacity);
        services.registerService<LogHistory>(history);
        services.registerTable<LogHistory>("entries", LogEntryRow::spiteSchema(),
                                           [](const LogHistory& history, const TableRowSink& sink)
                                           {
                                               for (const LogEntryRow& row : history.rows())
                                               {
                                                   sink(&row);
                                               }
                                           });
        if (LogService::isInitialized())
        {
            LogService::instance()->addSink(std::make_unique<HistorySink>(history));
        }
        return history;
    }

    std::vector<LogEntryRow> LogHistory::rows() const
    {
        std::lock_guard lock(m_mutex);
        return {m_rows.begin(), m_rows.end()};
    }

    void LogHistory::read(const u64 first, const u64 count,
                          const std::function<void(u64 index, const LogEntryRow& row)>& fn) const
    {
        std::lock_guard lock(m_mutex);
        const u64 recorded = m_recorded.load(std::memory_order_relaxed);
        const u64 oldest = recorded - m_rows.size();
        for (u64 index = std::max(first, oldest); index < std::min(first + count, recorded); ++index)
        {
            fn(index, m_rows[index - oldest]);
        }
    }

    void LogHistory::detach()
    {
        std::lock_guard lock(m_mutex);
        m_detached = true;
    }

    void LogHistory::record(const LogEvent& event)
    {
        LogEntryRow row{.sequence = event.sequence,
                        .timeNs = static_cast<u64>(event.timestampNs),
                        .thread = event.threadId,
                        .level = levelName(event.level),
                        .file = event.location.file_name(),
                        .line = event.location.line(),
                        .message = event.message};
        std::lock_guard lock(m_mutex);
        if (m_detached || m_capacity == 0)
        {
            return;
        }
        if (m_rows.size() == m_capacity)
        {
            m_rows.pop_front();
        }
        m_rows.push_back(std::move(row));
        m_recorded.fetch_add(1, std::memory_order_release);
    }
} // namespace spite
