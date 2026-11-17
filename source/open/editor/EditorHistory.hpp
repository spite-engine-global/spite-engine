#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class EditorDialogs;
    class EditorFeedback;
    class Widget;

    // The History panel: the displayed world's undo steps (the play world while playing, else the
    // scene), newest first, each with its command and args summary and its time, a leading state (current: the newest
    // done step, done, undone) and, last, the initial state before every step. Selecting a row (click, or the keyboard
    // moving the selection) jumps there: the command historyJump undoes or redoes exactly the steps between the
    // positions; a step that left the history meanwhile (a new edit dropped the undone ones) refuses, and the
    // selection returns to the current row. The selection always shows the current position.
    //
    // Save Log opens a file dialog over the captures root and runs the saveLog query on the displayed world: the
    // command log as request lines `spite run` replays.
    //
    // The rows are the CommandHistory service's `steps` table read in process, only when the history's revision
    // moved or the displayed world changed, never per frame and not for a view command (a splitter drag) or a query:
    // a quiescent editor reads and writes nothing. Query `historyView` (owner editorHistory) answers from the history
    // itself, so it is current while the panel is hidden.
    class EditorHistory
    {
    public:
        EditorHistory(Engine& engine, EditorFeedback& feedback, EditorDialogs& dialogs);
        ~EditorHistory();
        EditorHistory(const EditorHistory&) = delete;
        EditorHistory& operator=(const EditorHistory&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
