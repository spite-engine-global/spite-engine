#pragma once

#include <filesystem>
#include <memory>

#include "base/ApplicationConfig.hpp"
#include "base/MathTypes.hpp"
#include "editor/EditorExport.hpp"

namespace spite
{
    class Engine;

    // How the editor's confirm, prompt and file dialogs resolve (EditorDialogs): Ask shows them to the person;
    // Confirm and Cancel answer each one at once as if the person pressed that button (a prompt confirms with its
    // default text, a file dialog with its starting folder and name when that is a valid choice and, for a save, not
    // an existing file, else it cancels), for an unattended run (`--auto-dialogs`).
    enum class EditorDialogPolicy : u8
    {
        Ask,
        Confirm,
        Cancel
    };

    struct EditorShellDesc
    {
        // Loaded at construction (the default layout when missing or not a
        // layout), written at destruction and by the saveLayout command;
        // relative to the content root. Empty: no persistence, the
        // saveLayout command and a loadLayout without a layout refuse. Named layouts (saveLayout name=)
        // are files in the layouts folder beside it.
        std::filesystem::path layoutFile = "config/user/editor-layout.json";
        // The editor settings (grid, fly boost, theme, UI and text scale, reduced motion, language): loaded and applied
        // at construction, written by the editorSettings command; relative to the content root. Empty (the default):
        // no persistence, the default theme and the engine's display settings and locale; a host that owns user state
        // sets it (config/user/editor-settings.json).
        std::filesystem::path settingsFile;
        // The editor hotkeys: input mappings (the mappingsFile schema, chords
        // by "modifiers") for the editor input context.
        std::filesystem::path hotkeysFile = "config/project/editor/hotkeys.json";
        // The Forge token file: its dataPresets (the editorSettings dataColor names, in its order) and the theme of
        // each variant; relative to the content root. Without presets only a #rrggbb data colour is valid.
        std::filesystem::path forgeTokensFile = "assets/ui/forge/tokens.json";
        EditorDialogPolicy dialogs = EditorDialogPolicy::Ask;
    };

    // The editor shell , in the UI world "editor" it adds to the engine: a top bar with
    // the File, Edit, Window and Help menus and the Play, Pause, Step and Stop toolbar over
    // a dock space holding the editor panels (hierarchy left, scene and game view tabs centre,
    // inspector right, project, console, log, stats and passes bottom). The
    // hierarchy lists the scene's transform hierarchy and sets the selection;
    // the inspector renders the selection (the select command) from component
    // schemas; the console runs control requests, the log, stats and passes
    // panels view their control tables; the project panel browses and
    // imports assets and instantiates models dragged onto the hierarchy or
    // the scene view. The centre panels show the editor views as images of their view targets (SCENE_VIEW follows the
    // game, GAME_VIEW is the game view, both offscreen; Engine::setViewFollowsGame, setGameView): Scene the
    // edit world through the editor camera, Game the game camera; while playing
    // both show the play world, and the selection moves to the play clones on
    // Play and back on Stop (the editor model, EditorModel.hpp, which the shell builds first and destroys last). Each
    // panel's rect is its view's window rect at once and, once the size holds
    // for a few frames with no drag in progress, its view extent (the target
    // reallocates then). A press on the Game panel (the gameFocus command) gives
    // the game input focus (Engine::setGameInputFocus) until a press or focus
    // lands elsewhere in the editor or the panel hides; while focused, presses
    // on it reach the game (EditorGameToolbar: the panel's camera and aspect choosers, Stats box and input state,
    // the gameViewFit and gameViewStats commands and the gamePanel query). Play shows and focuses the Game panel; Stop,
    // from any source, takes the focus back. The Scene panel draws through the editor camera, picks on click and
    // outlines the selection (EditorSceneView: the editorCamera and pick commands, the sceneView query).
    //
    // Every GUI action is a control command: toolbar buttons and menu items queue
    // play, pause, resume, step, stop, dock and resetLayout requests, and dock
    // gestures queue dock requests; update() dispatches them through the control
    // server between frames. The Edit menu (undo, redo, copy, paste, duplicate,
    // delete) and the editor hotkeys, gizmo modes included, run the same
    // requests (EditorActions). The File menu (source/editor/EditorFileMenu.hpp,: New, Open, Open Recent,
    // Save, Save As, Import Model, Quit, with the editor.file* hotkeys and a Save / Discard / Cancel choice before a
    // scene is replaced or the editor quits) runs the scene-state commands; Help > Keyboard Shortcuts shows the
    // hotkeys' chords; the command palette (source/editor/EditorCommandPalette.hpp,: Ctrl+K, the header field
    // `editor.bar.palette`, Help > Command Palette; the commandPalette query and commandPaletteOpen view command, owner
    // editorCommandPalette) searches and runs the editor actions and control commands; the Scene tab reads `Scene*`
    // while the open scene is dirty. Dock and layout commands stay out of
    // undo. The shell registers the commands dock (a UiDockOp: kind
    // dock|float|move|close|activate|resize|maximize|restore, panel, target, zone center|left|right|top|bottom, ratio,
    // x, y, w, h), saveLayout, loadLayout (from the layout file, or a "layout" JSON arg), resetLayout and gameFocus
    // (on: true|false; refused while the Game panel is hidden), and the query dockLayout (the layout JSON and its
    // file), under owner "editorShell". Under the dock sits the status bar (source/editor/EditorStatusBar.hpp): the
    // play state, scene, selection, jobs, log counts and frame time, with the toasts above it; the Play, Pause and Stop
    // states also tint the toolbar. Its control entry is the statusBar query (owner editorStatusBar); the toast
    // commands and the toasts query are the editor model's. Confirm, prompt and file dialogs
    // (source/editor/EditorDialogs.hpp,) are a modal over the whole shell that a panel reaches
    // through EditorPanelContext::dialogs; the dialogs query and the answerDialog view command (owner editorModel) list
    // and resolve the open one. The chrome itself is a document view: `editor.shell` over
    // assets/ui/editor/shell.json owns the toolbar's containers, the container the dock fills and the one the status
    // bar fills, and where they sit; code fills the slots shell.toolbar.menus / .gizmo / .transport / .end (the
    // buttons; the palette field, the frame readout, the agent chip and Help are in .end), shell.toolbar.tint,
    // shell.dock and shell.statusbar. The dock and the status bar are sized by their containers, never by the shell;
    // setSize only sizes the root the document fills. The panels are not listed in the shell: each registers itself in
    // the editor panel registry (source/editor/EditorPanelRegistry.hpp), which the shell reads when constructed.
    // Registers the engine font and stylesheet, as StatsOverlay does. Its Scene panel's GPU work (picks, outline,
    // gizmo) runs through the SceneViewOverlay service it registers on the scene world's services.
    class SPITE_EDITOR_API EditorShell
    {
    public:
        // the Scene panel's view, drawn from the editor camera; the Game panel's, the game camera
        static constexpr u32 SCENE_VIEW = 0;
        static constexpr u32 GAME_VIEW = 1;

        explicit EditorShell(Engine& engine, EditorShellDesc desc = {});
        ~EditorShell();
        EditorShell(const EditorShell&) = delete;
        EditorShell& operator=(const EditorShell&) = delete;

        // Once per frame before Engine::update: dispatches the queued requests,
        // follows the window size and syncs the toolbar with the play state.
        // Writes UI only when something changed.
        void update();
        // The shell's size in UI pixels; update() sets it from the window. The document's layout sizes the chrome.
        void setSize(Vec2 size);
        // Whether the Scene and Game panels show their views: needs config.graphics.renderViewCount >= 2.
        [[nodiscard]] bool editorViews() const;

        // The views the editor needs of a graphics config: SCENE_VIEW and GAME_VIEW exist, and the probe capture view
        // (the bake's, never a panel's) is not one of them: a capture view named 1 moves to the first view past them.
        static void reserveViews(GraphicsConfig& graphics);

    private:
        friend struct EditorShellTestSeam;
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
