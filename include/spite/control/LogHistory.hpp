#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/Logging.hpp"
#include "base/Schema.hpp"
#include "control/ControlExport.hpp"

namespace spite
{
    class ServiceLocator;

    // One log event as a table row.
    struct LogEntryRow
    {
        u64 sequence = 0;
        u64 timeNs = 0; // system_clock, since its epoch
        u32 thread = 0;
        std::string level;
        std::string file;
        u32 line = 0;
        std::string message;
        SPITE_OBSERVED_SCHEMA(LogEntryRow, sequence, timeNs, thread, level, file, line, message)
    };

    // The recent log as a queryable table: a service whose `entries` table
    // holds the last `capacity` events (oldest roll off), so `tables` lists it
    // and snapshots carry it as LogHistory_entries.
    class SPITE_CONTROL_API LogHistory
    {
    public:
        static constexpr sizet DEFAULT_CAPACITY = 16384;

        explicit LogHistory(sizet capacity = DEFAULT_CAPACITY);

        // Registers this service and its table on services, and feeds it from
        // the log service when one is running.
        static std::shared_ptr<LogHistory> attach(ServiceLocator& services, sizet capacity = DEFAULT_CAPACITY);
        // Stops recording; the log service keeps its sink, which then drops events.
        void detach();

        void record(const LogEvent& event);
        // oldest first
        [[nodiscard]] std::vector<LogEntryRow> rows() const;
        // Events recorded since construction: the newest has index
        // recorded() - 1. A viewer polls it per frame (one atomic load) to
        // learn of new rows.
        [[nodiscard]] u64 recorded() const { return m_recorded.load(std::memory_order_acquire); }
        // The oldest held index: rows before it rolled off.
        [[nodiscard]] u64 oldest() const
        {
            const u64 count = recorded();
            return count > m_capacity ? count - m_capacity : 0;
        }
        // Calls fn for the held rows with indices in [first, first + count),
        // oldest first, under the lock: no copy of the table.
        void read(u64 first, u64 count, const std::function<void(u64 index, const LogEntryRow& row)>& fn) const;

    private:
        mutable std::mutex m_mutex;
        std::deque<LogEntryRow> m_rows;
        sizet m_capacity;
        std::atomic<u64> m_recorded = 0;
        bool m_detached = false;
    };
} // namespace spite
