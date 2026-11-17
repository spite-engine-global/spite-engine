#pragma once

#include <atomic>
#include <cstdint>
#include <format>
#include <memory>
#include <source_location>
#include <string>
#include <utility>

#include "BaseExport.hpp"

namespace spite
{
    enum class LogLevel
    {
        Debug,
        Info,
        Success,
        Warning,
        Error,
        None
    };

    struct LogEvent
    {
        std::uint64_t sequence = 0;
        // Plain integers keep <chrono>/<thread> out of a header nearly every file includes.
        std::int64_t timestampNs = 0; // system_clock, since its epoch
        std::uint32_t threadId = 0; // OS thread id
        LogLevel level = LogLevel::Debug;
        std::source_location location;
        std::string message;
    };

    class SPITE_BASE_API ILogSink
    {
    public:
        virtual ~ILogSink() = default;

        virtual void consume(const LogEvent& event) = 0;
        virtual void flush() = 0;
    };

    class SPITE_BASE_API LogService
    {
    public:
        LogService();
        ~LogService();

        LogService(const LogService& other) = delete;
        LogService(LogService&& other) noexcept = delete;
        LogService& operator=(const LogService& other) = delete;
        LogService& operator=(LogService&& other) noexcept = delete;

        static void initialize();
        static void destroy();
        static LogService* instance();
        static bool isInitialized();

        void addSink(std::unique_ptr<ILogSink> sink);
        // destroys the sink; no event reaches it afterwards
        void removeSink(const ILogSink* sink);
        void start();
        void flush();

        void setLogLevel(LogLevel level) { m_minLogLevel.store(level, std::memory_order_release); }
        LogLevel getLogLevel() const { return m_minLogLevel.load(std::memory_order_acquire); }

        template <typename... Args>
        void print(const std::source_location& location, std::format_string<Args...> fmt, Args&&... args)
        {
            log(LogLevel::Debug, location, fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void printError(const std::source_location& location, std::format_string<Args...> fmt, Args&&... args)
        {
            log(LogLevel::Error, location, fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void printInfo(const std::source_location& location, std::format_string<Args...> fmt, Args&&... args)
        {
            log(LogLevel::Info, location, fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void printWarning(const std::source_location& location, std::format_string<Args...> fmt, Args&&... args)
        {
            log(LogLevel::Warning, location, fmt, std::forward<Args>(args)...);
        }

        template <typename... Args>
        void printSuccess(const std::source_location& location, std::format_string<Args...> fmt, Args&&... args)
        {
            log(LogLevel::Success, location, fmt, std::forward<Args>(args)...);
        }

    private:
        struct Impl;

        std::atomic<LogLevel> m_minLogLevel = LogLevel::Debug;
        std::unique_ptr<Impl> m_impl;

        template <typename... Args>
        void log(LogLevel level, const std::source_location& location, std::format_string<Args...> fmt, Args&&... args)
        {
            if (getLogLevel() > level)
            {
                return;
            }

            write(level, location, std::format(fmt, std::forward<Args>(args)...));
        }

        void write(LogLevel level, const std::source_location& location, std::string message);
    };

    // Every main that starts the log service owns one, declared before anything that logs in its destructor: its
    // end drains the queue and joins the dispatcher before main returns. Otherwise the dispatcher still runs sinks
    // while exit() destroys the exe's iostreams and flushes its stdio (each module links its own CRT), and a sink
    // writing std::cerr can leave the stderr lock held forever, hanging exit; the service is then leaked unflushed.
    class LogServiceScope
    {
    public:
        LogServiceScope() { LogService::initialize(); }
        ~LogServiceScope() { LogService::destroy(); }

        LogServiceScope(const LogServiceScope& other) = delete;
        LogServiceScope& operator=(const LogServiceScope& other) = delete;
    };

#if SPITE_DEBUG_BUILD == 1
#define SDEBUG_LOG(...) spite::LogService::instance()->print(std::source_location::current(), __VA_ARGS__)
#else
#define SDEBUG_LOG(...) ((void)(0))
#endif

#define SERROR_LOG(...) spite::LogService::instance()->printError(std::source_location::current(), __VA_ARGS__)
#define SWARNING_LOG(...) spite::LogService::instance()->printWarning(std::source_location::current(), __VA_ARGS__)
#define SSUCCESS_LOG(...) spite::LogService::instance()->printSuccess(std::source_location::current(), __VA_ARGS__)
#define SINFO_LOG(...) spite::LogService::instance()->printInfo(std::source_location::current(), __VA_ARGS__)
} // namespace spite
