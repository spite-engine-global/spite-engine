#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class EditorDialogs;
    class EditorFeedback;
    class Widget;

    // The State viewer (inventory row 2): the CPU service tables of the worlds and of the root
    // container (the `worlds` and `services` queries; the scope column says the world or root) on the left, the picked
    // table's rows on the right (document view `editor.state`; code-owned slots state.toolbar, state.sources,
    // state.rows and state.detail). One TableView per table, built when it is first picked, over a runtime
    // schema of string cells: the columns are the table's fields, read from the query's reply.
    //
    // Paging is by explicit offset: Prev and Next send `first` to `tableRows`, and the page shown is the one the reply
    // answered, with its own first, row count and matched count; a table that shrank under the offset shows the last
    // page that has rows. The filter box is the query's filter (a field's text, or `field:text` for one column; the
    // offset counts matched rows). A cell is cut at 96 bytes (the status line says so); the selected row's fields are
    // listed in one line under the table (the query has them whole) and Copy Row puts its JSON on the clipboard.
    // Export writes the picked table to a snapshot file through the file dialog (`snapshot`).
    //
    // The GPU buffers `gpuTables` lists are sources too (scope gpu; a headless run lists none): a page is one
    // `gpuBuffer` read issued through ControlServer::handle with a reply sink, which only leaves the answer in a
    // mailbox. One read is in flight; its answer shows in a later frame when it is the newest ticket and the page
    // still wanted, else it is dropped and the wanted page asked. A refusal or the engine's expiry error is the status.
    // The filter applies to the rows read and never reads again; Export is off; Live reads one buffer page at a time.
    //
    // A table is read when the panel is shown, on Refresh and on a table, filter, page or size change; with Live on,
    // also every 15 frames while no play world is running (editing, or paused: the pause-and-inspect state). A running
    // world is never read per frame: Live waits for the pause and says so. A hidden or idle panel reads nothing.
    //
    // Control (owner editorState): query `stateView` (source, sources, columns, the page's cells, page, filter, live,
    // status, detail, selection, reads made), view commands `stateSource id=`, `stateFilter text=`, `statePage
    // first=`, `statePageSize size=`, `stateLive on=`, `stateRefresh`, `stateSelectRow row=` and `stateCopyRow`. The
    // query reads the picked table now, shown or hidden.
    class EditorStateViewer
    {
    public:
        EditorStateViewer(Engine& engine, EditorFeedback& feedback, EditorDialogs& dialogs);
        ~EditorStateViewer();
        EditorStateViewer(const EditorStateViewer&) = delete;
        EditorStateViewer& operator=(const EditorStateViewer&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
