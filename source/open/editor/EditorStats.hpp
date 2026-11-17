#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class Widget;

    // Engine counters in four sections (Frame, Entities, Memory, Rendering) on the panel list pattern, under a
    // frame-time chart: the frame, each world's ECS stats (the `worlds` query) and the GPU scene stats
    // (SceneGpuStatsSingleton, from its schema). They refresh every REFRESH_FRAMES frames while shown (a hidden panel
    // reads nothing) and only rows whose text changed are rewritten; the chart takes every shown frame's time with the
    // refresh. A header row closes its section (Enter, a second click or `statsSection`). Takes over StatsOverlay's
    // role in the editor. Query `statsView`: rows (every counter by name), sections, chart and the list's view.
    class EditorStats
    {
    public:
        explicit EditorStats(Engine& engine);
        ~EditorStats();
        EditorStats(const EditorStats&) = delete;
        EditorStats& operator=(const EditorStats&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
