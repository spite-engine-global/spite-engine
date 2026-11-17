#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class EditorAgentLayer;
    class EditorDialogs;
    class EditorModel;
    class Widget;

    // The editor's inspector panel: the selected entities' components
    // rendered from their schemas, no per-type presenter. Rows are a
    // virtual list: the name field and enabled box (a summary for none or several), then a subtitle with the
    // matching systems count and last write for one selected entity.
    // then per component a header (with Remove, which asks
    // first for an engine component) and one row per field, nested structs,
    // fixed arrays and Mat4 columns indented under a group row, a collapsed
    // read-only Runtime section for the runtime-derived components, an Add
    // Component row and an Engine components row (the engine toggle). Labels
    // ellipsize in a flexible column and the full text is their tooltip.
    // Persisted, writable fields edit (number fields per lane,
    // text inputs, toggles); observed and read-only fields show their value.
    // A quaternion shows ZYX Euler angles in degrees with a button to the raw
    // x y z w (inspectorFilter rotationMode); with several entities selected a
    // value that differs shows Mixed and an edit writes only the edited lane to
    // all of them. Enter commits and lets go of the field; text that is not a
    // value turns its field to the error look and adds a note row under it
    // until an edit or another selection.
    // AssetHandle fields pick from the asset table (remove + add, so asset
    // bindings rehydrate). Add Component and Pick open one searchable popup
    // list: the Add list groups the components by category and hides the
    // UI-layer and runtime-derived ones unless the engine toggle is on.
    //
    // Every edit is a control command queued between frames: set, addComponent,
    // removeComponent, one batch (one undo step) for a multi-selection, a drag
    // folded into one undo step by set's merge arg. Values show the first
    // selected entity's. It shows the editor model's selection (the select command, EditorModel.hpp) and registers
    // the command inspectorFilter (the engine toggle) and the query inspector (selection, rows, stats, the Add list)
    // under owner "editorInspector".
    //
    // Idle cost: per frame the primary entity's aspect and its components'
    // modified bits are checked, nothing is written; rows re-read only for a
    // modified component or a new command generation, and only rows whose
    // value changed rebind.
    class EditorInspector
    {
    public:
        EditorInspector(Engine& engine, EditorModel& model);
        ~EditorInspector();
        EditorInspector(const EditorInspector&) = delete;
        EditorInspector& operator=(const EditorInspector&) = delete;

        // The panel content the dock sizes.
        [[nodiscard]] Widget content() const;
        // Once per frame between frames: dispatches the queued edits, then
        // follows selection, structure and value changes.
        void update();
        // The shell's dialogs, which confirm removing an engine component; without them it removes at once.
        void setDialogs(EditorDialogs* dialogs);
        // The agent layer: while one is set and switched on, a single selected entity with unreviewed
        // changes of a control client gets an agent block under its header (client and time, the count of changed
        // fields, Revert and Keep) and its changed fields a 2px agent edge and wash, with a "change" row under each
        // (client, time and the value it had,; its own Revert and Keep). Revert sends
        // revertStep for the entity's last step (a refusal's reason shows under the block), Keep sends review.
        void setAgentLayer(EditorAgentLayer* agent);
        // The telemetry density and system budget control the inspector's system readout. The subtitle's
        // tooltips show matching system phases and last-write provenance. Systems are matched when the entity's
        // aspect or schedule changes, and costs are sampled every 30 frames while the panel is visible.
        void setTelemetry(bool full, float systemBudgetMs);

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
