#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include "base/Platform.hpp"
#include "engine/EngineExport.hpp"

namespace spite
{
    class ControlServer;
    class IFormatWriter;
    class ModuleHost;
    struct AvailableModule;

    // The Windows FILETIME (100 ns ticks since 1601) of a file's last write, or nothing when it cannot be read.
    [[nodiscard]] SPITE_ENGINE_API std::optional<u64> fileWriteFiletime(const std::filesystem::path& file);
    // The FILETIME a process was created at, or nothing when it cannot be opened.
    [[nodiscard]] SPITE_ENGINE_API std::optional<u64> processCreationFiletime(u32 pid);
    [[nodiscard]] SPITE_ENGINE_API u64 currentFiletime();

    // The buildStatus reply for the status files in statusDirectory (<binaries>/.build, beside the DLLs the
    // files describe): directory, projects[] and issues[]. Reads only that directory, once per call; a file that is
    // torn, unreadable, too large (MAX_FILE_BYTES) or of another schema becomes one issues entry and the other
    // projects are unaffected. A missing directory gives empty lists: nothing was built by Build.ps1 (a shipped game).
    // Otherwise each module DLL of modules without a file is listed with state "none". project, when not empty, keeps
    // that project only. nowFiletime is the clock ageSec and the two-hour rule of an abandoned build use.
    SPITE_ENGINE_API void writeBuildStatus(const std::filesystem::path& statusDirectory, std::string_view project,
                                           std::span<const AvailableModule> modules, u64 nowFiletime,
                                           IFormatWriter& out);

    inline constexpr sizet BUILD_STATUS_MAX_FILE_BYTES = sizet{256} << 10;
    inline constexpr sizet BUILD_STATUS_MAX_FILES = 256;

    // Registers on control (owner "engine") the query buildStatus [project]: writeBuildStatus over
    // <host.searchDirectory()>/.build, which is the executable's directory unless the host was given another.
    // now is the clock; tests replace it.
    SPITE_ENGINE_API void registerBuildStatusQuery(ControlServer& control, const ModuleHost& host,
                                                   std::function<u64()> now = currentFiletime);
} // namespace spite
