#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class EditorSettingsService;
    class Widget;

    // The Systems panel: the systems of the scene or the play world (a Scene / Play selector) as a table
    // in execution order: the update phase's stages, then the fixed phase's, a system in both phases listed once in
    // each. Columns: order, system, stage, phase, module, state and cost. Enter on a row, or a click on the state of
    // the selected row, queues setSystemEnabled (undoable, so the toggle is one undo step); a disabled system reads
    // "off". The cost column is the scheduler's measured average (SystemDescriptor::updateTiming, the
    // `systems` query) in 0.01 ms steps, in the warning class over the systemBudgetMs editor setting; a disabled
    // system reads 0.
    //
    // The rows come from the `systems` query again only when shown and the world's schedule changed (a module's
    // reload or unload: its rows vanish and return), a command ran, the locale changed or the world selection
    // moved, and every 30 frames for the cost; a hidden panel costs two counter compares. Query `systemsView`
    // (owner editorSystems): world, rows, counters and detail; view commands `systemsWorld world=scene|play` and
    // `systemsSelect name [module] [phase]`. While no play world exists the Play selector is off and the panel
    // shows the scene's systems.
    //
    // Below the table a strip shows the selected row's history: a line "name: now ms, peak ms" and a sparkline (a
    // Canvas series) of the row's average at each of the last 120 reads, one read per 30 frames (about a minute at
    // 60 fps), not per frame: the panel stays idle-cheap and reads the scheduler's own 64-frame average. Every row
    // keeps its history while the panel shows (a world change drops them); the series is rebuilt from it when the
    // selection moves and a read appends one sample. Nothing selected: a hint and no sparkline. The samples are a
    // 64-frame average taken 30 frames apart: neighbouring samples overlap and a one-frame spike is smeared; the
    // line's peak (the scheduler's 64-frame maximum) carries the spike. `systemsSelect` reads the rows first while
    // the panel is hidden, and its refusal lists the rows as module::name/phase.
    //
    // The Graph button (view command `systemsMode`, query field `mode`) swaps the table for the dependency graph of
    // one stage (EditorSystemsGraph.hpp): query field `graph`, commands `systemsLane` and
    // `systemsGraphView`. The graph is built from the `systems` query when the shown world's schedule changes, not
    // with the cost reads, and ignores the table's filter.
    class EditorSystems
    {
    public:
        EditorSystems(Engine& engine, const EditorSettingsService& settings);
        ~EditorSystems();
        EditorSystems(const EditorSystems&) = delete;
        EditorSystems& operator=(const EditorSystems&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
