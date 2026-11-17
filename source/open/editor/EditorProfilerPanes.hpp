#pragma once

#include <array>
#include <memory>

#include "ProfilerViews.hpp"

namespace spite
{
    // The tab bar and the views under the Profiler panel's graph: counter tracks, the GPU
    // lane, the memory pane, the call tree. No tab is active at first and then no view asks the worker for
    // anything; `profilerPane pane=<counters|gpu|memory|calls|compare|none>` (what a tab click sends) shows one. The
    // views register their own commands (profilerCounterTrack, profilerMemory, profilerCalls; the GPU lane is shown by
    // profilerPane and its pass selected by profilerSelect; profilerCompare sets the Compare view's two captures) and
    // the host
    // the query `profilerPanes`, which answers the numbers every view shows.
    class ProfilerPanes
    {
    public:
        // The tab row and the gap under it
        [[nodiscard]] static float collapsedHeight(const panelui::Density& density)
        {
            return panelui::HEIGHT_BUTTON.of(density) + density.xs;
        }

        explicit ProfilerPanes(ProfilerViewHost host);
        ~ProfilerPanes();
        ProfilerPanes(const ProfilerPanes&) = delete;
        ProfilerPanes& operator=(const ProfilerPanes&) = delete;

        [[nodiscard]] Widget root() const { return m_root; }
        // -1: none
        [[nodiscard]] i32 active() const { return m_active; }
        // How much taller the panes are open than collapsed: the panel's skeleton scrolls for it (the tab bar keeps its
        // place)
        [[nodiscard]] float extraHeight() const { return height() - collapsedHeight(m_host.density->density()); }
        // The page's viewport height: an open view is as tall as that (its preferred height at least), so scrolling its
        // tab bar to the top shows the whole view; a change of the viewport is the only write
        void setViewportHeight(float viewport);
        [[nodiscard]] Widget tabs() const { return m_tabs; }
        void captureChanged(CaptureId capture);
        void selectionChanged(const ProfilerSelectionComponent& selection);
        // Routes a Task event to the view that asked for it
        void take(ProfilerEvent& event);
        void update();
        [[nodiscard]] bool busy() const;
        void writeState(IFormatWriter& out);

    private:
        [[nodiscard]] float height() const
        {
            return m_open > 0.0f ? m_open : collapsedHeight(m_host.density->density());
        }
        void setActive(i32 index);
        void registerCommands();

        ProfilerViewHost m_host;
        Panel m_root;
        TabBar m_tabs;
        Button m_close;
        float m_open = 0.0f; // the open view's height, its preferred one at least; 0 while collapsed
        float m_preferred = 0.0f; // the shown view's preferred height
        float m_viewport = 0.0f;
        Panel m_content;
        std::array<std::unique_ptr<ProfilerView>, PROFILER_VIEW_COUNT> m_views;
        i32 m_active = -1;
        CaptureId m_capture = 0;
    };
} // namespace spite
