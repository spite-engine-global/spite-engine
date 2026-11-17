#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/FunctionRef.hpp"
#include "base/Platform.hpp"
#include "editor/EditorExport.hpp"
#include "engine/ModuleHost.hpp"

namespace spite
{
    class Engine;
    class EditorFeedback;
    class IFormatWriter;

    struct BuildDiagnostic
    {
        std::string file, severity, code, message;
        u32 line = 0;
    };

    // One project of the snapshot as the Modules panel shows it (the facts writeModule writes, typed).
    struct BuildRecord
    {
        std::string project, state, configuration, module; // state: ok, failed, building or none
        bool needsReload = false, untracked = false, abandoned = false;
        std::optional<double> ageSec; // since the build finished; none without a record
        std::vector<BuildDiagnostic> diagnostics; // recorded diagnostics, bounded to 1000
        u32 diagnosticsTotal = 0;
        u32 pid = 0; // the process that wrote the record (0: none recorded)
    };

    struct ModuleWork
    {
        struct Note
        {
            std::string summary, why;
        };
        struct Check
        {
            std::string label;
            bool passed = false;
        };

        u64 operation = 0;
        std::string project, module, title;
        std::array<Note, 3> notes; // Inspect, Apply, Review
        std::string build = "pending";
        u64 binary = 0; // the selected binary's write time
        bool loaded = false; // that binary is active
        std::vector<Check> checks;
    };

    // A build's age at the granularity the panel shows it: just now (this clock minute), minutes, hours or days.
    enum class BuildAgeUnit : u8
    {
        Now,
        Minutes,
        Hours,
        Days
    };

    struct BuildAge
    {
        BuildAgeUnit unit = BuildAgeUnit::Now;
        u32 count = 0;
        bool operator==(const BuildAge&) const = default;
    };

    // The age of a build `ageSec` old at wall clock time `nowSec` (seconds), counted in whole minutes between the clock
    // minute the build finished in and the current one, so every project's age moves at the same clock minute and a
    // panel with many projects rewrites its texts once a minute, not whenever one of them crosses a minute of its own.
    // Without nowSec the system clock.
    [[nodiscard]] SPITE_EDITOR_API BuildAge buildAgeOf(double ageSec, double nowSec);
    [[nodiscard]] SPITE_EDITOR_API BuildAge buildAgeOf(double ageSec);

    // The editor's view of the last build of each project (the editor model's part of it): the buildStatus
    // reply as a snapshot, the command that reloads rebuilt modules, the toasts of finished builds and the builds the
    // editor started. The editor does not compile: buildProject starts Build.ps1 as a detached process
    // (EditorBuildProcess.hpp), Build.ps1 records the results and the engine's writeBuildStatus reads them
    // (BuildStatus.hpp).
    //
    // update(watched) runs once per frame from whoever shows the build state (the Modules panel; a model-only host
    // or a test calls it itself). While watched it wakes a worker thread every second of the feedback clock (and at
    // once after a module loads, unloads or reloads). The worker stats the status files and the DLLs they name and,
    // when that fingerprint is the one of its last full read and no build was recording, reads nothing (the frame only
    // moves the ages on); otherwise it reads, parses and fingerprints the reply off the frame thread. A quiet second
    // costs the frame no allocation and the worker none; hidden it only collects a finished read. refreshNow() reads on
    // the calling thread (the files are few and small): the control queries and moduleReloadBuilt use it, so they never
    // answer from a snapshot older than the call, and so does moduleRefresh. A refresh that finds a project finished
    // since the last snapshot posts a toast: one error per failed project (the first three, then one for the count of
    // the rest), one info for the ones that succeeded. The first snapshot posts none.
    //
    // Command moduleReloadBuilt (owner editorBuild, not the panel's editorModules: each part removes only what it
    // registered): sends reloadModule for each loaded module whose DLL was
    // rebuilt since it loaded (needsReload), in module name order, and stops at the first refusal. Reply: reloaded
    // (names) and failed ({name, message}, null when none refused). It never loads, unloads or builds anything else.
    //
    // Commands buildProject {project, configuration?} and cancelBuild {project} (owner editorBuild, so only the editor
    // host has them): the first starts a build of one project and keeps its process here, one per project; the second
    // ends that process's tree while it runs, unless the project's status record shows another live build. Their
    // describe text
    // lists the refusals. A started build is forgotten (its handles closed) once it has exited and a command or
    // startedBuild looks at it, or when the model goes; the build itself is never ended but by cancelBuild.
    class SPITE_EDITOR_API EditorBuildState
    {
    public:
        EditorBuildState(Engine& engine, EditorFeedback& feedback);
        ~EditorBuildState();
        EditorBuildState(const EditorBuildState&) = delete;
        EditorBuildState& operator=(const EditorBuildState&) = delete;

        void registerCommands();
        void update(bool watched);
        void pollModuleWork();
        void refreshNow();
#if !defined(_WIN32)
        // Persist completed editor-started builds even when no build panel is open.
        void pollStartedBuilds();
#endif

        // Bumped when a refresh changed anything but the ages.
        [[nodiscard]] u64 revision() const;
        [[nodiscard]] const ModuleWork& moduleWork() const;
        // Worker reads started and reads made on the calling thread since the model was built.
        [[nodiscard]] u64 workerReads() const;
        [[nodiscard]] u64 directReads() const;
        // Worker reads that found the status files, the DLLs and the loaded modules as they were at the last full read
        // (a stat per file, nothing read or parsed), and the heap allocations the worker made in all its reads.
        [[nodiscard]] u64 skippedReads() const;
        [[nodiscard]] u64 workerAllocations() const;
        // Whether a worker read is out (its result is collected by update).
        [[nodiscard]] bool reading() const;
        // revision() plus the times a shown age moved to another unit or count (BuildAge): what a view that shows ages
        // compares to know it must read again, once a minute at most for an unchanged build.
        [[nodiscard]] u64 displayRevision() const;
        // Whether the snapshot lists any project: false without a .build directory (a shipped game, a clean bin).
        [[nodiscard]] bool hasProjects() const;
        // A project is being built right now (a building record that is not abandoned).
        [[nodiscard]] bool building() const;
        // The pid of the build of <project> buildProject started, while it runs.
        [[nodiscard]] std::optional<u32> startedBuild(std::string_view project) const;
        // Last editor-started build's output, retained after its process exits. Empty for external builds.
        [[nodiscard]] const std::filesystem::path& outputPath(std::string_view project) const;
        // How many loaded modules moduleReloadBuilt would reload.
        [[nodiscard]] u32 reloadableCount() const;
        // The record of the module whose DLL is <stem>.dll (none: the snapshot has no such project).
        [[nodiscard]] std::optional<BuildRecord> record(std::string_view stem) const;
        // The projects of the snapshot whose stem `listed` does not name, as writeOthers selects them.
        [[nodiscard]] std::vector<BuildRecord> others(function_ref<bool(std::string_view)> listed) const;

        // Writes the build fields of the module whose DLL is <stem>.dll in the executable's directory: buildState (ok,
        // failed, building, none), needsReload, untracked, abandoned, ageSec (null without a record) and the
        // first diagnostics with their total (buildDiagnostics, buildDiagnosticsTotal).
        void writeModule(IFormatWriter& out, std::string_view stem) const;
        // The projects of the snapshot whose stem `listed` does not name (engine-owned DLLs, test programs), as the
        // array otherBuilds: project, buildState, ageSec, abandoned and the first diagnostics.
        void writeOthers(IFormatWriter& out, function_ref<bool(std::string_view)> listed) const;

        // Test seams: the .build directory to read (empty: the executable's) and the loaded modules the snapshot is
        // joined with (empty function: the engine's ModuleHost, which only the frame thread may read).
        void setStatusDirectory(std::filesystem::path directory);
        void setLoadedModules(std::function<std::vector<AvailableModule>()> source);
        // Called on the worker thread at the start of each read, before it looks at any file (empty: none).
        void setWorkerHook(std::function<void()> hook);

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
