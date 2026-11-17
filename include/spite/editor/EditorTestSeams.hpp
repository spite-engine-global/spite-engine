#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

#include "editor/EditorExport.hpp"
#include "engine/CaptureStore.hpp"
#include "engine/ModuleHost.hpp"

namespace spite
{
    class EditorShell;

    // Test seams into the editor shell's panels: states a test cannot reach through control, like a worker answer
    // that arrives after the request was replaced. Not for hosts.
    struct SPITE_EDITOR_API EditorShellTestSeam
    {
        // The token of the Profiler timeline's current view request.
        static std::uint64_t profilerViewToken(EditorShell& shell);
        // The token of the Profiler timeline's current match count request.
        static std::uint64_t profilerMatchToken(EditorShell& shell);
        // Hands the Profiler timeline a match count's answer as the worker would: `count` zones for `name` in
        // `capture`, with the given request token.
        static void injectProfilerMatches(EditorShell& shell, CaptureId capture, std::uint64_t token,
                                          const std::string& name, std::uint32_t count);
        // The build state's .build directory (empty: the executable's) and the loaded modules its snapshot is joined
        // with (empty function: the host's), then one read on the calling thread: a Modules panel over fixture status
        // files.
        static void setBuildSource(EditorShell& shell, std::filesystem::path statusDirectory,
                                   std::function<std::vector<AvailableModule>()> loaded = {});
        // The wall clock (epoch milliseconds, UTC) the agent layer's editing window reads, in place of the system's.
        static void setAgentLayerClock(EditorShell& shell, std::function<std::int64_t()> nowMs);
        // Hands the Profiler timeline one lane's Spans event as the worker would, with the given request token.
        static void injectProfilerSpans(EditorShell& shell, CaptureId capture, std::uint64_t token, std::uint32_t lane,
                                        std::int64_t fromNs, std::int64_t toNs, std::uint32_t columns,
                                        std::vector<CaptureSpan> spans);
        // What the Profiler's Reveal action calls with the file to show, instead of starting the file manager, and
        // (when given) the monotonic clock in milliseconds its rate limit reads.
        static void setProfilerReveal(EditorShell& shell, std::function<bool(const std::filesystem::path&)> reveal,
                                      std::function<std::uint64_t()> nowMs = {});
        // The same for the Project panel's details pane (its Reveal button and projectReveal).
        static void setProjectReveal(EditorShell& shell, std::function<bool(const std::filesystem::path&)> reveal,
                                     std::function<std::uint64_t()> nowMs = {});
        // The same for the Capture panel's Reveal button and captureReveal.
        static void setCaptureReveal(EditorShell& shell, std::function<bool(const std::filesystem::path&)> reveal,
                                     std::function<std::uint64_t()> nowMs = {});
    };
} // namespace spite
