#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class EditorDialogs;
    class Widget;

    // The render graph passes table (RenderGraph.passes) with an on/off toggle per pass that queues setPassEnabled (a
    // pass that live passes read from asks first, through the dialogs), a stage column to group by, a detail of the
    // selected pass (reads, writes, barriers, dependents, GPU time) and a badge with Re-enable All for the passes a
    // command turned off (setPassesEnabled). Re-reads the table only when shown and a command ran, the graph's
    // generation moved or the locale changed; a client's commands are read within a few frames. Queries and
    // commands: passesView (read from the graph, also while hidden), passesSelect, passesTab.
    class EditorPasses
    {
    public:
        EditorPasses(Engine& engine, EditorDialogs& dialogs);
        ~EditorPasses();
        EditorPasses(const EditorPasses&) = delete;
        EditorPasses& operator=(const EditorPasses&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
