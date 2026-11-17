#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ecs/EcsTypes.hpp"
#include "editor/EditorExport.hpp"

namespace spite
{
    class Engine;

    struct EditorModelDesc
    {
        // The editor settings file (EditorShellDesc::settingsFile): empty, no persistence. The recent scenes persist
        // beside it (editor-recent-scenes.json); without a settings file they are kept in memory only.
        std::filesystem::path settingsFile;
        // The Forge token file (EditorShellDesc::forgeTokensFile).
        std::filesystem::path forgeTokensFile = "assets/ui/forge/tokens.json";
    };

    // The editor model: the editor state control commands change and queries read, built from an Engine
    // with no EditorShell, panel, font or layout. It adds the UI world "editor" (its stylesheet holds the themes the
    // settings name), hosts the editor settings (the editorSettings, moduleSearchPathAdd and moduleSearchPathRemove
    // commands and the editorSettingsState query, owner editorSettings) and owns the selection: the select command
    // (world, ids or id; the reply is the world and the selection; owner editorModel), and on Play the selection moves
    // onto the play clones, on Stop back onto the edit entities they were cloned from (an entity without a counterpart
    // leaves it). It also holds the transform gizmo's settings: the gizmoMode command and the gizmo query (owner
    // editorModel; the query's pivot, axes, handles and drag come from the shell's EditorGizmo, which draws and drags).
    // The dialog requests (confirm, choice, notice, prompt and file dialogs, their policy and log) are its state too:
    // the dialogs query and the answerDialog command (owner editorModel); EditorDialogs, the view, shows the open one.
    // The toast log is its state too: the postToast, dismissToast and clearToasts view commands and the
    // toasts query (owner editorModel; the host advances the log's clock, the shell's status bar once per frame). So
    // is the clipboard of the selection: copy (the selection's topmost entities as a scene document), paste (it
    // instantiates the document in the active world or `world`, then selects what it made) and the selection query
    // (the world and the ids, as select replies).
    // EditorShell builds one first and destroys it last; its panels read it and are views of it.
    class SPITE_EDITOR_API EditorModel
    {
    public:
        explicit EditorModel(Engine& engine, EditorModelDesc desc = {});
        ~EditorModel();
        EditorModel(const EditorModel&) = delete;
        EditorModel& operator=(const EditorModel&) = delete;

        // The selection the select command set and its world's control name ("scene" until a select names another).
        // It may hold entities destroyed since the last pruneSelection(): EditorShell prunes it at the start of every
        // frame before any panel updates (so its views read live entities) and the inspector before every rebuild; a
        // caller outside a shell frame (a model-only host or test) prunes before it reads.
        [[nodiscard]] const std::string& selectionWorld() const;
        [[nodiscard]] const std::vector<Entity>& selection() const;
        // Bumped by every change of the selection (a select that replaced it, Play, Stop, a prune that dropped an
        // entity): a view compares it instead of copying the list.
        [[nodiscard]] u64 selectionRevision() const;
        // Drops the selected entities that are no longer alive, all of them when the world is gone; runs no listener.
        void pruneSelection();
        // One listener (the inspector's; empty functions clear it, setting one over another asserts), run inside the
        // select command before its reply:
        // `replacing` before a select puts another world or other entities in place (the old selection still holds),
        // `selected` after every select that landed and after Play or Stop carried the selection.
        void setSelectListener(std::function<void()> replacing, std::function<void()> selected);

        // The recent scenes, newest first, at most ten paths under the scene root; the recentScenes
        // query (owner editorModel) lists them with the file they persist in ({"scenes": [...]}; empty: not persisted).
        [[nodiscard]] const std::vector<std::string>& recentScenes() const;
        // The host tells the open scene's path (empty while untitled) once per frame, EditorShell from its status
        // bar: a path that differs from the last one told goes first in the list (a repeat moves it up, names compare
        // ignoring ASCII case) and the file is written. Nothing happens while the path is the one last told.
        void followScene(std::string_view path);
        // Drops a path from the list (a scene whose file is gone) and rewrites the file when it was there.
        void forgetRecentScene(std::string_view path);

    private:
        friend struct EditorModelAccess; // the parts that stay inside SpiteEditor (EditorContext.hpp)
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
