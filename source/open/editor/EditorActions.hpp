#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/MathTypes.hpp"

namespace spite
{
    struct InputBindingMapping;
    class Engine;
    class EditorFeedback;
    class EditorModel;

    // The editor's edit actions , each a control request: undo and
    // redo of the active world (play while playing, else scene), copy (the
    // selection as a scene document), paste (instantiate that document in the
    // active world), duplicate and delete of the selection's topmost entities
    // (one batch for several), the gizmo modes translate, rotate and scale and the scene camera's Frame.
    // Duplicate and paste select what they made.
    //
    // Hotkeys: an "editor" input context whose bindings load from the hotkeys
    // file (the input mapping schema, chords by "modifiers"), its actions
    // named by ACTION_NAMES; each pressed action runs its request. A focused
    // text input claims its own keys and chords (Ctrl+Z undoes the text, not
    // the scene) and leaves the rest, so the edit chords still reach here.
    // While a play world runs the context is disabled, so those keys stay the
    // game's; the Edit menu still acts.
    // Without an InputSystem service there are no hotkeys; run() still works
    // (the Edit menu).
    //
    // The file actions (New, Open, Save, Save As, Quit) are hotkeys only here: the shell's File menu owns
    // what they do (EditorFileMenu) and hands it through setFileHandler; they are not requests of their own.
    //
    // Registers no command of its own: copy and paste (the clipboard of the selection) belong to the editor model
    // and the gizmo modes send gizmoMode (EditorGizmo); this class holds the hotkeys and the requests.
    class EditorActions
    {
    public:
        enum class Action : unsigned char
        {
            Undo,
            Redo,
            Copy,
            Paste,
            Duplicate,
            Delete,
            GizmoTranslate,
            GizmoRotate,
            GizmoScale,
            FrameSelected, // F: the scene view's camera frames the selection, or everything with none
            DismissToast, // Esc: dismisses the toasts on screen; nothing is sent with none
            FileNew,
            FileOpen,
            FileSave,
            FileSaveAs,
            FileQuit,
            CommandPalette, // Ctrl+K: opens the command palette (setPaletteHandler); never a request of its own
            ZoomIn,
            ZoomOut,
            ZoomReset,
            Count
        };
        // the hotkey input action of each Action, in order
        static constexpr std::string_view ACTION_NAMES[]{
            "editor.undo",           "editor.redo",          "editor.copy",           "editor.paste",
            "editor.duplicate",      "editor.delete",        "editor.gizmoTranslate", "editor.gizmoRotate",
            "editor.gizmoScale",     "editor.frameSelected", "editor.dismissToast",   "editor.fileNew",
            "editor.fileOpen",       "editor.fileSave",      "editor.fileSaveAs",     "editor.fileQuit",
            "editor.commandPalette", "editor.zoomIn",        "editor.zoomOut",        "editor.zoomReset"};

        EditorActions(Engine& engine, const EditorModel& editorModel, const EditorFeedback& feedback,
                      const std::filesystem::path& hotkeysFile);
        ~EditorActions();
        EditorActions(const EditorActions&) = delete;
        EditorActions& operator=(const EditorActions&) = delete;

        // Queued: runs at the next update().
        void request(Action action);
        // What a File action (New to Quit) does when its hotkey is pressed or it is requested; none set: nothing.
        void setFileHandler(std::function<void(Action)> handler);
        // What the CommandPalette hotkey does; none set: nothing.
        void setPaletteHandler(std::function<void()> handler);
        // The chords an action is bound to, as the input system holds them now ("Ctrl+Shift+Z"); empty when unbound
        // or without an InputSystem.
        [[nodiscard]] std::vector<std::string> shortcuts(Action action) const;
        // Whether the action has anything to do now: Undo and Redo need a step in the active world's history (a
        // disabled one sends no request, so it leaves no refusal in the Log); the others are always enabled.
        [[nodiscard]] bool enabled(Action action) const;
        // A keyboard binding as the menus and the Help sheet write it ("Ctrl+Shift+Z").
        [[nodiscard]] static std::string chordText(const InputBindingMapping& binding);
        // Moves whenever any input context's bindings change (a rebinding, a reset, a load): what shows a chord
        // re-reads then. 0 without an InputSystem.
        [[nodiscard]] u64 bindingRevision() const;
        // Once per frame between frames, after UI input routed: runs the
        // pressed hotkeys' and the queued actions' requests. While the scene
        // view flies (flying), the gizmo-mode hotkeys are its fly keys.
        void update(bool flying, bool gameFocused = false);

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
