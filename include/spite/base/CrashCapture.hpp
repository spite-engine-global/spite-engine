#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string_view>

#include "BaseExport.hpp"

namespace spite
{
    // Engine state a crash report records, published with plain stores: no locks, no allocation, safe to read from a
    // dying process. The frame number costs one store per frame, the pass one pointer store per pass.
    struct CrashContext
    {
        std::atomic<std::uint64_t> frame{0};
        // the executing render-graph pass's InternedString id, 0 outside graph execution
        std::atomic<std::uint64_t> pass{0};
    };
    SPITE_BASE_API extern CrashContext g_crashContext;

    // Copies the text (truncated) into the ring of recent log lines / control commands the report carries.
    SPITE_BASE_API void crashRecordLog(std::uint8_t level, std::uint32_t threadId, std::string_view text);
    SPITE_BASE_API void crashRecordCommand(std::string_view text);

    // Writes <crash root>/<id>/crash.dmp and crash.json for the calling thread's current state, for a fatal error the
    // process reports itself (a failed Vulkan call). kind names the cause in crash.json. attachPath (optional) is
    // moved into the directory (the Aftermath GPU dump). Returns the directory ("" when capture is off or failed).
    // One capture per process: a later call (and the unhandled-exception filter) waits for it and returns without
    // capturing; the directory is the one the process wrote, "" when nothing was written.
    SPITE_BASE_API std::string_view captureCrash(const char* kind, const char* message,
                                                 const char* attachPath = nullptr);

    // Retain the first fatal diagnostic for a fault during stack unwinding (bounded, later calls are ignored).
    SPITE_BASE_API void setPendingFatalMessage(const char* message);

    // Set once after parsing the host's crash-directory command line override.
    SPITE_BASE_API void configureCrashDirectory(const std::filesystem::path& path);

    // The directory the process wrote its capture to, or an empty string before a successful capture.
    SPITE_BASE_API std::string_view crashDirectory();

    // Test hook: faults inside SpiteBase's own CRT and code ("abort": std::abort(); "access": an access violation on a
    // worker thread), the path a fault in any DLL takes.
    SPITE_BASE_API void crashRaiseForTest(const char* mode);
} // namespace spite
