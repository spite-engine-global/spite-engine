#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class EditorModel;
    class Widget;

    // The ECS panel: what the worlds hold, in four tabs over one list area. Worlds: each world's entity,
    // archetype and chunk counts, chunk memory and wasted bytes, whether it is running or paused and which view shows
    // it; a click on a row inspects that world in the next two tabs. Archetypes: the inspected world's component sets
    // with entity and chunk counts, memory, wasted bytes and fill, on the panel list pattern (sort, filter); the
    // selected one is described in the note below. Services: the inspected world's services and the root
    // container's with their registered tables. Play map: while playing, the edit-scene entity each play entity was
    // cloned from, for the selected entities (all pairs, up to a page, with none selected).
    //
    // Read-only. The rows come from the worlds, tableRows (EcsTables archetypes), services and playMap queries, and
    // only for the shown tab: when the panel becomes shown, on the Refresh button, a tab or world change, a Play or
    // Stop, a locale change and (Play map) a selection change; an idle panel and a hidden one cost counter
    // compares. The memory figures are therefore as of the last read, not live; Refresh re-reads.
    //
    // Control (owner editorEcs): query `ecsView` (page, world, the rows of each tab read so far, the note, the
    // refresh counter and the archetype list's filter, sort and cells), view commands `ecsPage page=`, `ecsWorld
    // world=` and `ecsRefresh`, and the archetype list's `ecsFilter` and `ecsTable`.
    class EditorEcs
    {
    public:
        EditorEcs(Engine& engine, const EditorModel& editorModel);
        ~EditorEcs();
        EditorEcs(const EditorEcs&) = delete;
        EditorEcs& operator=(const EditorEcs&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
