#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/UiWidgets.hpp"

namespace spite
{
    class Engine;
    class EditorActions;
    class EditorDialogs;
    class EditorFeedback;
    class UiScope;

    // The command palette (Parity with board P): Ctrl+K, the header field and Help > Command Palette
    // open a searchable popup list (PopupList over `editor.palette`: a search box, the rows as options, Up and Down,
    // Enter to run, Esc to close and give the focus back) over three groups: the editor's actions (Undo, Copy, the
    // File actions ... with their hotkeys; a disabled one cannot be chosen), a Show row for every panel, and every
    // registered control command (name, the first sentence of its doc, `needs <args>` for the required ones). The
    // search matches the whole row text, so a name, a description or a hotkey finds it.
    //
    // Choosing runs it as the control protocol would: an action as the hotkey would (EditorActions::request), a
    // command or a panel's Show as a request at the next frame start (ControlServer::runAtFrameStart), so it carries
    // the Editor source, lands in the command log and stays undoable if the command is. A command with required
    // arguments asks first: a prompt (EditorDialogs) holding `name=value` pairs, started with the required names,
    // or one JSON object. A refusal (the prompt's text, the command's reply) is a toast; a run is one too.
    //
    // The header field `editor.bar.palette` is a button the shell places (its text follows the hotkey and the
    // language, written only when either changes). Idle cost: update() compares two revisions. The rows are built
    // when the list opens (and again if the language changes while it is open), never while it is closed.
    //
    // Control (owner editorCommandPalette): query `commandPalette` (open, filter, how many rows shown, and every row:
    // id, kind, name, label, description, hotkey, enabled, needsArgs, args), view command `commandPaletteOpen`
    // [open=true|false] (refused while a dialog blocks the list).
    class EditorCommandPalette
    {
    public:
        struct PanelRow
        {
            std::string name;
            std::string title;
        };
        using PanelsFn = std::function<std::vector<PanelRow>()>;

        // `field` is the header button; `fallback` is what the list opens beside while the field is hidden.
        EditorCommandPalette(Engine& engine, UiScope& scope, Button field, Widget fallback, EditorDialogs& dialogs,
                             EditorActions& actions, EditorFeedback& feedback, PanelsFn panels);
        ~EditorCommandPalette();
        EditorCommandPalette(const EditorCommandPalette&) = delete;
        EditorCommandPalette& operator=(const EditorCommandPalette&) = delete;

        // Opens the list, or closes it when it is open (the hotkey toggles).
        void open();
        void close();
        [[nodiscard]] bool isOpen() const;
        // Once per frame: relabels the field after a language or rebinding change; costs two compares otherwise.
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
