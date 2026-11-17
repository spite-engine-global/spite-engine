#pragma once

#include <optional>

#include <memory>

#include "EditorHierarchyFilter.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    class Engine;
    class EditorAgentLayer;
    class EditorFeedback;
    class EditorModel;
    class UiHierarchyManager;
    class Widget;

    // The editor's hierarchy panel: the scene world's transform
    // hierarchy and standalone authored entities as a virtualized tree, named by EntityNameComponent (unnamed:
    // "<primary component> #<index>", with a
    // colour chip of the primary), children in entity-index order. While playing it shows the
    // play world: rows, selection and actions target "play" until Stop
    // discards it with its edits. Selecting rows sends select (the inspector
    // holds the selection; a select from elsewhere re-syncs the tree and
    // reveals the nodes). Dragging rows onto another parent sends reparent (a
    // batch, one undo step, for several); the tree refuses drops into the
    // dragged nodes (a toast says so), reparent refuses cycles. Siblings have no
    // order in the scene (children are listed by entity index), so a drop that
    // keeps every parent does nothing and shows no indicator; elsewhere the
    // indicator is a line before or after a row, or the whole row for "into".
    // A drag resting 0.6 s on a collapsed node with children opens it; Escape
    // (or a modal) cancels the drag and closes what it opened; a drop opens the
    // way to the moved nodes and scrolls to the first. The query's `drag`
    // shows the drop in flight. F2 on the cursor row, or a second plain click
    // on the only selected row between 0.4 and 2 s after the first, opens the
    // inline rename that the context menu opens. The context
    // menu sends createEntity (root or child), duplicateEntity and
    // deleteEntity (the selection's top-most nodes, batched), exports the clicked entity's
    // subtree with Save Scene through the model's file flow, and opens an
    // inline rename that commits set EntityNameComponent.name (addComponent
    // for an unnamed entity; Enter commits, Escape cancels). Created and duplicated entities are selected.
    //
    // Follows the world without per-frame work: the model (roots, then
    // children lists read lazily for expanded nodes) rebuilds only when the
    // transform manager's structure generation moves; a new command generation
    // re-checks the shown labels and the selection. Labels written outside
    // the protocol show at the next change. A hidden panel does nothing.
    // The agent layer (EditorAgentLayer) tags rows at their end: AGENT on the entity a control client is
    // editing, EDITED on the entities with unreviewed non-editor changes, both washed with the agent colour; the
    // rows re-read when the layer's revision moves, never per frame. The query rows carry `mark`: agent, edited or
    // empty, the layer's tokens, whatever the locale.
    // The hierarchy query reports subtree costs: the summed average of the distinct systems and phases touching the
    // subtree's archetypes, not a per-entity cost. The shell supplies telemetry and budget settings via setTelemetry.
    // Inactive entities: a row whose entity is inactive dims its text and chip and has a tooltip
    // that says whether the entity is disabled itself or by a parent; the rows re-read when the transform manager's
    // activity generation moves.
    // UI section: below the scene's roots, one read-only "UI" row for the shown world's UI layer
    // (the play world's while playing, else the scene's; the row exists while the layer has elements) whose children
    // are the UI hierarchy's roots (surfaces) and, lazily, theirs: labelled by UiIdentityComponent.id, else
    // "<widget kind> #<index>". It is collapsed until opened and follows the UI's structure only while open, so a
    // closed section costs a counter compare per frame. Selecting a UI row selects the entity (the inspector shows it);
    // no create, duplicate, delete, rename or drop applies to a UI row (a drop onto or of one is refused with a toast),
    // and the section row itself is never selected (a click or an arrow on it only highlights it). Its id is not an
    // entity (the query reports id 0, section true).
    // Scope: the scope dropdown (the scope slot, shown while the world has UI elements or the scope narrows
    // the rows) lists the scene's entities, the UI elements or both; the UI scope opens the section once. With a text,
    // a UI element matches by its id too (find does not read it). hierarchyFilter takes text and scope; neither clears.
    // Model drop: while a Project model is dragged over the panel (a UiDragDrop drag of
    // MODEL_DRAG_PAYLOAD that its target accepts), the drop indicator marks the row it will be parented under (the
    // whole row) or, over the background, the end of the rows (the roots); a UI row or the section shows none. It
    // clears with the drag (release, Escape, the pointer leaving) and the query's `modelDrop` {active, parent} says
    // the same (parent 0: the roots). A quiet panel does one null check of the drag per frame.
    // Query `hierarchy` (owner editorHierarchy): the telemetry density and budget, the visible rows (id, name,
    // label, primary, ui, section, uiKind, enabled, active, mark, costMs, costSystems, costOver, costBar (the readout
    // bar's share, negative: none), icon and the detail the row shows; current even while hidden or before the frame
    // follows), filter {text, scope, ...} and stats (the query leaves them alone). Where a dragged Project model
    // dropped at `hit` (the deepest element under the pointer) lands in the panel `content`: `inside` is whether the
    // hit lies in it, `accepts` whether it takes a drop (the scene's entity row, or the background); `parent` is the
    // entity row's entity, undefined over the background (the roots). The UI section and the UI rows are read-only:
    // inside, never accepting. One answer for the Project panel's accept, its drop and the indicator.
    struct HierarchyModelDrop
    {
        bool inside = false;
        bool accepts = false;
        Entity parent = Entity::undefined();
        std::optional<u32> layer;
    };
    [[nodiscard]] HierarchyModelDrop hierarchyModelDrop(Engine& engine, const UiHierarchyManager& editorUi,
                                                        const Widget& content, Entity hit);

    class EditorHierarchy
    {
    public:
        EditorHierarchy(Engine& engine, const EditorModel& editorModel, const EditorAgentLayer& agent,
                        EditorFeedback& feedback);
        ~EditorHierarchy();
        EditorHierarchy(const EditorHierarchy&) = delete;
        EditorHierarchy& operator=(const EditorHierarchy&) = delete;

        [[nodiscard]] Widget content() const;
        // The tab-bar tag (DockSpace::setPanelTag): "{n} over {budget} ms" while rows listed are over the
        // system budget (the warning class), else "ms / frame" under full telemetry, else hidden. It follows the
        // costs' own refresh and the telemetry setting, and writes only when what it shows changes.
        [[nodiscard]] Widget tag() const;
        // The telemetry density (full, else quiet) and the system budget (ms; 0: nothing is over it) in force.
        void setTelemetry(bool full, float systemBudgetMs);
        // Once per frame after the inspector's: dispatches the queued
        // requests, then follows the world.
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
