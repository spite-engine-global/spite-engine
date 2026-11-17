#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include "engine/CaptureStore.hpp"

namespace spite
{
    class Engine;
    class EditorCaptureFiles;
    class EditorDialogs;
    class EditorFeedback;
    class Widget;
    struct DockSpace;

    // The Profiler panel (): the shell of the m-16 profiler GUI. Every action
    // is a control command and every piece of state is readable over control, so a click and a command give the
    // same result:
    // Start / Stop profileStart / profileStop, enabled by the capture's state
    //   Refresh            profilerRefresh: re-reads the store and rescans the artifacts folder
    //   Close              profileClose of the selected capture
    //   report row         the selected capture's report path with Copy (profilerCopyPath) and Reveal (profilerReveal)
    //   capture list       the store's captures (what profileCaptures reports) and the sidecars on disk (what
    //                      profileFiles reports, scanned on a worker), newest store capture first. A row is two
    //                      lines (EditorProfilerList): time and tag, then duration, frames and size; a click or Enter
    //                      is profilerSelect capture=<id>, or profileOpen path=<file> then profilerSelect. The list
    //                      is one Tab stop: the arrows move a cursor row, Delete asks to delete it. Unopened sidecars
    //                      under 1 MiB (stubs) are hidden until profilerListFilter showSmall=true (the Show small
    //                      button, which counts them); Show file is profilerReveal capture=|path=, Delete (after an
    //                      EditorDialogs confirm) profilerDelete capture=|path=
    //   selection          ProfilerSelection on the panel's root element, written only by profilerSelect (capture,
    //                      firstFrame/lastFrame, zoneLane/zoneIndex, clear), which refuses a value the store's
    //                      capture does not have; closing or evicting the capture clears it. The shown panel selects a
    //                      written stop's capture by itself (a stop made while hidden, on show) unless a profilerSelect
    // came after the stop: an explicit select always outranks the automatic one
    //   frame graph        a Chart of every frame's CPU time with the GPU time overlaid and the 16.7 / 33.3 ms budget
    //                      lines; click selects a frame, a drag a range (both profilerSelect)
    // Query `profilerState`: the selection, the list rows, the graph's load and statistics, and counters. It syncs
    // from the store and the worker first, so it is current whether or not the panel is shown (only the live mode,
    // which follows profileStatus, needs the panel shown). The worker's open requests are the jobs "profiler"
    // (EditorFeedback).
    //
    // The store's slow queries (the artifacts scan, a capture's lanes and frame strip) run on a worker; the frame
    // thread takes finished pages, 2 a frame (profilerState too). The store's generation is checked every frame, shown
    // or not (one atomic load), so the selection follows a closed capture while hidden. Per frame the panel does more
    // work only while shown, and then: the 30-frame status refresh (every frame while a stop is being written, every 6
    // while a sidecar is), one atomic load for the worker, and during a live capture one appended sample per series.
    class EditorProfiler
    {
    public:
        EditorProfiler(Engine& engine, EditorFeedback& feedback, EditorDialogs& dialogs, const DockSpace& dock,
                       EditorCaptureFiles& files);
        ~EditorProfiler();
        EditorProfiler(const EditorProfiler&) = delete;
        EditorProfiler& operator=(const EditorProfiler&) = delete;

        [[nodiscard]] Widget content() const;
        void update();
        // EditorShellTestSeam
        [[nodiscard]] std::uint64_t viewToken() const;
        [[nodiscard]] std::uint64_t matchToken() const;
        void injectMatches(CaptureId capture, std::uint64_t token, const std::string& name, std::uint32_t count);
        void injectSpans(CaptureId capture, std::uint64_t token, std::uint32_t lane, std::int64_t fromNs,
                         std::int64_t toNs, std::uint32_t columns, std::vector<CaptureSpan> spans);

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
