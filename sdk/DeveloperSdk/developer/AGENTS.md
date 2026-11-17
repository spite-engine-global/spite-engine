# Develop games with SPITE

For projects made with the SPITE binary SDK. Everything goes through the global
`spite` command; engine checkout scripts are not part of this workflow.

## Start a project

With the SDK installed and `spite` on PATH:

```text
spite doctor [--json]     check the machine; builds a small module and loads it in the SDK host (needs a Vulkan GPU;
                          --fast skips this probe); never installs or elevates
spite setup               install Python, CMake, Ninja (no admin) and the C++ compiler (one admin prompt, ~1-3 GB,
                          skipped when Visual Studio with an MSVC 14.51+ toolset exists); Build Tools are Microsoft
                          software under the Visual Studio licence, downloaded from Microsoft
spite new <path>          create a self-contained project at any path (missing or empty); --name N, --content-only
cd <path>
spite build               configure and build the native module and custom shaders (Release); says `spite setup` if the toolchain is missing
spite run                 launch the editor; stdio JSON control by default; --control none, --game, -- <host args>
spite ship <out-dir>      validate and archive a standalone game; finds the built module; --smoke, --module <file>
spite update              install the newest SPITE and make it the default
spite self uninstall      remove SPITE from this user (--yes skips the question); other files in the SPITE folder stay
spite +0.2.0 <command>    run one command with another installed version
spite upgrade             re-pin this project to the newest installed SDK and rebuild
```

## Before you run `spite setup`

- Windows: tell the user that an administrator (UAC) prompt will appear and ask them to approve
  it; do not start it while nobody can watch the screen. Without an interactive desktop (SSH,
  service, CI) setup stops and prints the exact command for an administrator terminal: ask the
  user to run it, then run `spite doctor`.
- The compiler install takes 5-20 minutes. Run `spite setup` with a command timeout of at least
  30 minutes, or in the background and poll `spite doctor --fast`. The elevated installer keeps
  running if your command times out; `spite setup` again waits for it (`--no-wait` only reports
  it). Never start a second Visual Studio installer by hand.
- Linux: use the Ubuntu 24.04 baseline and the exact compiler/libstdc++ stamp reported by doctor.
  If the compiler ABI probe fails, inspect the error; do not repeat setup as a remedy.
  When setup prints a `sudo ...` command, ask the user to run it and wait for their word.
  Do not work around it: no sudo password handling, no other package sources, no manual
  toolchain downloads.

Commands run in the project directory (any subfolder works). A project pins one
SDK version in `.spite/sdk.json`; `spite` runs that version, and tells you when a
newer one is installed. It never upgrades a project by itself; `spite upgrade` backs
up the pin as `.spite/sdk.json.bak`, re-pins and rebuilds. Game modules must be
rebuilt for each SDK version.

A project holds only its own files: game source, `CMakeLists.txt`, `ship.json`,
its assets and config overrides. Engine assets, shaders and config stay in the SDK
and are found through the pin; never copy them into the project. `spite project`
prints the SDK root, the content root the host reads (engine content overlaid by
the project, under `build/`) and the host arguments, as JSON.
Project-layer `saveConfig` writes to the project's `config/project`, so deleting
`build/` preserves saved settings. The host uses its staged config as a read-only
fallback, including generated module paths; user-layer settings keep their user root.

`spite new` installs identical project `AGENTS.md` and `CLAUDE.md` plus plain skill
copies in `.agents/skills/` and `.claude/skills/`. Keep `.spite/sdk-managed-files.json`:
upgrade uses its per-file hashes to refresh unchanged SDK files, preserve user edits
and additions, and report the paths needing a manual merge. Do not delete it to force an update.

`run` owns the project's content and writable roots (`user`, `saves`, `scenes`, ...
under the project, git-ignored) and speaks newline-delimited JSON on stdin/stdout.
Keep stdin open for the session. After a timed-out write, inspect state before retrying.
Python and CMake are not bundled: `spite tools` reports the selected ones; see
[guides/Setup.md](guides/Setup.md) for explicit paths and offline use.

## Work through the live engine

- Discover `searchCapabilities`, `describe`, `help`, `components`, `worlds` and
  `availableModules`; read the live schema before using a capability. Do not invent APIs.
- Use returned entity IDs and asset identities. Query bounded results and inspect
  failure details. Group related edits in named undoable batches and read back results.
- Author persistent content in `scene`. `play` is a transient clone; stop discards its
  changes. Save, reopen and inspect the scene to establish persistence.
- Import/bake into the project's roots, never the SDK. Keep source assets separate
  from `assets_baked`; inspect import job errors and readiness before placing results.
- Game logic is native C++. Use public headers under `include/spite` and matching
  SDK CMake targets; follow the starter for module and rendering-extension APIs.
  Attach systems/services to the world supplied to the module, release owned callbacks
  on unload, and explicitly `reloadModule` after rebuilding. Check optional services.
- Release injected/held input in `finally`. Compare actual world state with expected
  behavior. UI structure or a headless run cannot establish visible output.

For an end-to-end native gameplay starting point, see [SalvageGame](samples/SalvageGame/README.md).

Use the relevant [agent workflow](guides/AgentWorkflows.md): [procedural reimport](guides/AgentWorkflows.md#procedural-asset-round-trip),
[audio](guides/AgentWorkflows.md#audio-import-update-and-playback), [capture](guides/AgentWorkflows.md#windowed-scene-capture),
[flipbooks](guides/AgentWorkflows.md#flipbook-cards) or [full-detail LOD](guides/AgentWorkflows.md#full-detail-lod-for-chosen-renderables). Other sections cover modules,
input and provenance. Read only the section needed for the task.

## Build and deliver

`spite build` makes Release modules and custom shaders. List intended runtime files
and modules in `ship.json`, then `spite ship <new-dir>`; see the README.
Keep editor state, caches and sources out of runtime content. Optional `spite verify`
runs end-to-end SDK acceptance; it is not required after every project edit.

Report build, load/reload, persistence, gameplay and visible-output evidence separately.
Preserve useful logs and active SDK/module identities. Respect the user's requested
verification scope; state skipped checks and unavailable platforms accurately.

For session control, use [spite-session](skills/spite-session/SKILL.md).

For requested engine feedback, use [spite-feedback](skills/spite-feedback/SKILL.md).
