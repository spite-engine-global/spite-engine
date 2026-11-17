#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/Platform.hpp"

namespace spite
{
    // On Windows, a Build.ps1 run the editor started (buildProject): `pwsh -NoProfile -NonInteractive -File
    // <root>/scripts/Build.ps1 -Projects <project> -Configuration <configuration>` from the checkout root (pwsh.exe
    // found on the PATH once, run by its full path), with MSBuild node reuse off and a PDB server endpoint of its own
    // in its environment, so every process of the build is its descendant and in its job; detached
    // (the editor never waits on it and it outlives the editor), without a console window, below normal priority, its
    // stdin on NUL and output in a per-build log file. Only these handles are inherited. It runs in a job of its own,
    // assigned before it runs any code, so end() reaches its whole process tree and nothing else. The object holds the
    // process handle, so the pid cannot be reused by another process while the object lives; destroying it closes the
    // handles and ends nothing. On Linux, a direct CMake build in the configured tree runs in its own process group.
    class EditorBuildProcess
    {
    public:
        // The started build run from `directory`, or null with the reason in `error`. An empty script selects the
        // configured CMake executable on Linux.
        [[nodiscard]] static std::unique_ptr<EditorBuildProcess> start(const std::filesystem::path& script,
                                                                       const std::filesystem::path& directory,
                                                                       std::string_view project,
                                                                       std::string_view configuration,
                                                                       std::string& error);
        // The script the environment variable SPITE_BUILD_SCRIPT names instead of the checkout's Build.ps1 (the panel
        // scenario's fake build, which takes the same arguments), empty when it is not set; a value that cannot be
        // read or names no file sets `error`.
        [[nodiscard]] static std::filesystem::path scriptOverride(std::string& error);
        ~EditorBuildProcess();
        EditorBuildProcess(const EditorBuildProcess&) = delete;
        EditorBuildProcess& operator=(const EditorBuildProcess&) = delete;

        [[nodiscard]] u32 pid() const { return m_pid; }
        [[nodiscard]] const std::filesystem::path& outputPath() const { return m_outputPath; }
        [[nodiscard]] bool running() const;
        // Ends the build's process tree and waits at most waitMs for its leader.
        bool end(u32 waitMs);
#if !defined(_WIN32)
        [[nodiscard]] const std::filesystem::path& programPath() const { return m_programPath; }
        [[nodiscard]] std::optional<int> exitCode() const;
#endif

    private:
#if defined(_WIN32)
        EditorBuildProcess(void* process, void* job, u32 pid, std::filesystem::path output) :
            m_process(process), m_job(job), m_pid(pid), m_outputPath(std::move(output))
        {
        }

        void* m_process;
        void* m_job;
#else
        EditorBuildProcess(std::filesystem::path program, std::filesystem::path output) :
            m_programPath(std::move(program)), m_pid(0), m_outputPath(std::move(output))
        {
        }

        std::filesystem::path m_programPath;
        int m_lockFd = -1;
#endif
        u32 m_pid;
        std::filesystem::path m_outputPath;
    };
} // namespace spite
