#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class EditorBuildState;
    class EditorFeedback;
    class EditorSettingsService;
    class Widget;

    // A searchable module list beside the selected module's Overview, Parameters and Commands. Compact docks
    // show the list or detail with a Back action. Build/Cancel, explicit Reload, runtime Load/Unload and independent
    // Load on startup act on the selection. Search-folder management and fixture visibility are advanced controls.
    // A separate Build dock panel selects projects independently and shows their result and copyable diagnostics.
    // Missing configured modules can be removed without trying to load them. Fixtures remain listed while loaded,
    // selected or explicitly enabled. Parameters apply through their owning commands; defaults reset only writable
    // fields with valid manifest defaults, and command results can be copied.
    //
    // Every action is a control command: Load, Unload and Reload send loadModule, unloadModule and reloadModule (a
    // GUI session's log replays all three), an edit of a parameter row (a property form, applied at once) and a
    // command's Run send the command the manifest or the module names, the browser sends moduleSearchPathAdd, and the
    // panel's own state changes are the view commands modulesView, moduleSelect, moduleOptions, moduleRefresh,
    // moduleBrowse, moduleCommandSelect and moduleField (owner editorModules). Query `modulesState` reads all of it:
    // rows, scan problems, the options, the persisted list, the parameters and their values, the commands, the argument
    // fields, the output and the counters.
    //
    // Each row also says how its project built last (the build state is the editor model's,
    // EditorBuildState.hpp): buildState (ok, failed, building, none), needsReload, untracked, abandoned, ageSec and the
    // first diagnostics, read from the status files Build.ps1 writes beside the executable; otherBuilds lists the
    // projects no row names. The model's command moduleReloadBuilt (owner editorBuild) reloads every module rebuilt
    // since it loaded; moduleRefresh reads the status files again too.
    //
    // Build/Cancel and Reload stay in the selected-module header across the detail tabs. Reload never happens
    // automatically, is refused while building and is unavailable for engine-owned libraries. The independent Build
    // panel shows hints, structured diagnostics and other projects' status; modulesView build opens it.
    // moduleFilter searches by name/path, moduleBrowser returns to the compact list, moduleActivity toggles the
    // Build panel, and moduleCopyPath/moduleReveal/moduleCopyResult/moduleCopyDiagnostic expose the UI actions.
    // moduleResetDefaults validates explicit defaults and queues only writable parameter commands. Queued edits
    // retain their originating module identity so navigation cannot direct their feedback into another form.
    //
    // Directories are scanned and listed on a worker thread (ModuleHost::scan inspects library exports, never runs
    // module code, skips links and placeholders, and is bounded in entries, files and time), so a slow share never
    // stalls a frame; a result that arrives after a newer request is dropped. The loaded flags come from the host at
    // read time. With both panels hidden, work is a revision compare and an atomic load. A failed action and a scan
    // with an issue post an error or warning toast, a load and an unload an info one, and the running scan and listing
    // count as the jobs "modules" (EditorFeedback).
    class EditorModules
    {
    public:
        EditorModules(Engine& engine, const EditorSettingsService& settings, EditorFeedback& feedback,
                      EditorBuildState& build);
        ~EditorModules();
        EditorModules(const EditorModules&) = delete;
        EditorModules& operator=(const EditorModules&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
