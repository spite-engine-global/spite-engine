#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class EditorFeedback;
    class Widget;

    // Runs control requests typed as CLI verbs (`entity id=5`) or JSON and shows the structured replies as
    // formatted JSON that wraps to the panel (at most 400 lines of one result, then a marker). Enter submits the
    // command `console` (line) — the CLI can type into it too — which records history and runs the line through
    // ControlServer::handle at the next update. Up/Down walk the history; Tab, or the list that opens under the
    // typing (a PopupList that leaves the focus on the input: Up/Down move in it, Tab or Enter choose), completes
    // verbs, the verb's argument names (describe), world names, component names, field paths, pass names and log
    // levels. Clear empties the output (`consoleClear`), Copy puts the last result whole on the clipboard
    // (`consoleCopy`). The input scrolls horizontally to keep its caret in view. Query `consoleView`: history,
    // output lines, completions and rows.
    class EditorConsole
    {
    public:
        EditorConsole(Engine& engine, EditorFeedback& feedback);
        ~EditorConsole();
        EditorConsole(const EditorConsole&) = delete;
        EditorConsole& operator=(const EditorConsole&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
