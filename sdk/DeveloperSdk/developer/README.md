# SPITE developer SDK

Author scenes and build native game modules without an engine checkout. Agents
should read [AGENTS.md](AGENTS.md). The SDK is installed and selected by the global
`spite` command; projects live outside it.

The package targets one OS/ABI: Windows x86-64 or Linux x86-64. Both editor and game
hosts need Vulkan 1.3 and the required GPU features; headless mode still uses Vulkan.
`sdk-manifest.json` identifies the version, source revision, API/toolset ABI and
prerequisites. The installer verifies the archive SHA-256 before extracting. Native
modules use Release and must match this SDK's compiler ABI. macOS is not supported.

## One project workflow

```text
spite new <path>        # any path, missing or empty; --name N sets project and module name
cd <path>
spite build
spite run               # --control none for a person-driven session; --game for the game host
spite ship <out-dir>
```

`new` creates a self-contained project: a magenta-stripe C++ rendering-extension and
custom-shader sample (`CMakeLists.txt`, `<Name>.cpp`, `shaders/custom`), `ship.json`,
empty `assets_baked`, `assets/audio`, `scenes`, `models`, identical `AGENTS.md` and
`CLAUDE.md`, `.gitignore`, `.spite/sdk.json` and `.spite/sdk-managed-files.json`.
Eleven agent skills are plain copies in both `.agents/skills/` and `.claude/skills/`.
`--content-only` omits the C++ sample (no build needed; use
`spite doctor --profile editor`). The pin records `{format: 2, version, target, apiStamp,
toolsetStamp, sourceHead}`; it holds no path, so the project moves between machines
with its SDK version. A pin made by an older SDK (absolute `sdkRoot`) is migrated on
first use (backup `.spite/sdk.json.bak`).

After successful `spite upgrade`, unchanged SDK-owned instruction files and
skills refresh to the selected SDK. Per-file hashes preserve and report edited files,
including either instruction file, and leave user-added files alone. Keep the ownership
marker in version control. Review preserved paths and merge desired updates manually;
deleting the marker does not authorize overwriting existing files.
Native projects refresh guidance after their rebuild succeeds; scene-only projects
refresh without requiring a compiler. Byte changes, including line endings, count as edits.
If a guidance refresh fails because a file is locked, unlock it and rerun `spite upgrade`.
Completed SDK writes remain tracked for retry; files you edited remain preserved.

Engine assets, shaders and config are not copied into the project: they come from
the pinned SDK. The host reads one content root, so `run` and `project` assemble it
under `build/spite-content` from the SDK's content plus the project's files (the
project's directories are linked; a project file replaces the SDK's file of the same
path). Project-layer `saveConfig` writes directly to the project's `config/project`;
the host arguments select this authoritative layer separately from staged defaults
and generated module paths. User-layer settings retain their user-config destination.
Delete `build/` any time; saved project settings survive. Input bindings start empty in
`config/project/input/actions.json`; the SDK supplies no gameplay key presets.

`spite upgrade` re-pins to the version `spite` runs (the newest installed, or
`spite +<ver> upgrade`), saving the old pin as `.spite/sdk.json.bak`, then rebuilds.
The pinned version must equal the running SDK's, and the stamps must match the
installed SDK; a mismatch is an error, never a warning.

`build` first runs the fast doctor checks (failing with the `spite setup` line rather
than a CMake error), then configures and builds Release with the selected CMake and compiles
`shaders/custom` into `shaders/bin`. Use public C++ headers under `include/spite` and
link the matching `Spite::` CMake targets. Follow the headers and starter for module
lifecycle and rendering extension interfaces; editor and test interfaces are not
gameplay dependencies. Load the absolute resulting DLL/shared-object path with
`loadModule` and explicitly invoke `reloadModule` after rebuilding; a build does not
load or reload the module.

`run` launches the editor in the foreground with the project's content and writable
roots. It defaults to `--control stdio`: keep stdin open and exchange JSON lines with
the host; closing stdin ends the session. Additional host options follow `--`, for
example `spite run -- --scene <scene-path>`.

For named sessions and MCP (Windows), obtain the same roots from `spite project`:

```powershell
$project = spite project | ConvertFrom-Json
$hostArgs = $project.HostArgs
& $project.Cli run --session my-game --detach --timeout 600 --wait 30 '--' @hostArgs
& $project.Cli request --session my-game '{"query":"worlds"}'
```

The detached host owns a bounded 600-second lifetime; shut it down when finished.
For repeated requests use one persistent `Session.attach` connection as shown in
[AgentWorkflows.md](guides/AgentWorkflows.md), not a client launch per call.
`bin/spite.exe` and its MCP adapter are Windows-only; use direct host stdio on Linux.
See [tools/SpiteCli/README.md](tools/SpiteCli/README.md) in the Windows package.

On Linux, `spite desktop` writes desktop entries with the SDK icon for the current
project under `$XDG_DATA_HOME/applications` (default `~/.local/share/applications`),
or `--output <dir>`. Run it again after moving the SDK. GUI launchers use the project's
content and writable roots; baking/compiler launchers open a terminal.

## Customized source provenance

`spite source build <project> <module> <source>` records a content snapshot and its
source hash; it does not require Git. To claim a committed fork, add
`--fork-revision <full-commit-id>`. The complete module source tree, including its
recipe and private headers, must match that commit byte for byte, with no extra
untracked or ignored files. A changed tree must be built as a snapshot or committed
before claiming a revision. Existing builds with the older undifferentiated revision
record must be rebuilt before selection.

## Assets and shipping

Drop supported model, texture, HDR environment or audio files onto the Scene view,
or discover import commands through the live registry. Offline tools are in the
SDK's `bin` (use the path `spite project` prints as `SdkRoot`):

```text
SpiteBake <model-path> <project>/assets_baked
ShaderCompiler --sources <slang-directory> --output <project>/shaders/bin --spirv-only
```

Append `.exe` on Windows. Keep authoring sources in the project. Samples under
`samples/AssetConsumer` and `samples/RenderConsumer` demonstrate importing and modules.

Edit `ship.json` to list module IDs, every intended runtime file and an optional
startup scene. Audio clips assigned in included scenes are included automatically from
`assets/audio`; include any other runtime audio explicitly. Include fonts, locales, UI,
animation controllers, configurations and compiled custom shaders of the project.
The SDK's own runtime content is added automatically; a listed project file replaces
the SDK file of the same path. Baked manifest dependencies are included; editor/user
state, SDK tools, source models and code are excluded. Content-only projects need
module/build configuration before using the modular-game packager.

For example, a SalvageGame project can use this complete `ship.json`:

```json
{
  "format": 1,
  "modules": ["SalvageGame"],
  "files": ["config/project/input/actions.json", "scenes/SalvageGame.json"],
  "scene": "scenes/SalvageGame.json"
}
```

```text
spite ship <new-release-directory> [--smoke] [--module <module-binary>]...
```

The release directory must be new and outside both the project and the SDK. `ship`
finds each module named in `ship.json` in `build/` (`<Name>.dll`, or `lib<Name>.so`
on Linux) and fails with "run `spite build`" if it is missing; `--module` overrides
the search, one binary per module ID in order. The supported profile is `modular-game`,
with UI/font/localization support; the cooked prototype is separate.

Shipping validates baked content with native engine loaders and produces `game`,
a ZIP/tarball, SHA-256 and manifests/logs. `--smoke` additionally runs eight frames;
omit it when runtime checks are not requested. Native validation is not proof of
visible UI or arbitrary game references. Exercise intended gameplay states separately.
Use `Game.cmd` or `Game.sh` from the package to launch its configured startup scene.

## Optional SDK acceptance

```text
spite verify --output <new-directory-outside-SDK>
```

This intentionally builds/bakes/loads/saves/ships/runs a sample game outside the
installation. It needs a suitable GPU and compiler. Publisher `--verify-consumer`
uses this same command and records evidence. Do not run it when tests are excluded.

`spite tools --profile build` inspects selected paths/versions/hashes;
`spite python <script> [args]` runs a Python helper with the selected interpreter.

The additional Windows visual/render acceptance suite lives at
`samples/verification/VerifySdk.ps1 -OutputRoot <new-directory>` and requires
PowerShell 7 and Node.js 22.2+. Those tools are optional for the normal workflow.
See [guides/AgentWorkflows.md](guides/AgentWorkflows.md) for detailed engine recipes.

## SDK internals

The SDK ships `sdk.cmd` and `sdk.sh` (SDK root), `Setup.ps1`/`Setup.sh` and `internal/`.
They are implementation details of `spite`: it runs the project commands through
`sdk.cmd` / `sh sdk.sh`, which find the SDK's managed Python and run `internal/Sdk.py`.
Do not run them by hand; `spite doctor` names any setup script it needs.

For session control, see [spite-session](skills/spite-session/SKILL.md).

## Anonymous SDK activity

By default the SDK CLI attempts one anonymous report per UTC day per local user
profile on the first `spite new`, `spite build`, or unbounded `spite run` (including
`--game`). Shipped game startup is never reported. CI, verification and bounded
runs are excluded. Run `spite usage off` to disable it persistently, or
`spite usage on` to re-enable it; `SPITE_USAGE_DISABLE=1` or `DO_NOT_TRACK=1`
disables reporting for the environment.

The request contains only the actual SDK version and coarse platform (windows,
linux or unknown). The server keeps daily aggregates for 90 days. There is no
installation identifier; counts are estimates, not unique users, and can undercount
offline or opted-out use. The first qualifying version of the day is counted.
Offline failures are discarded without retries that day.

`spite usage off` waits for any report already in progress; after it completes, no new report starts until reporting is enabled again. It cannot recall data already sent.


AudioWorld exposes `setMasterGain(float)` and `masterGain()` per world. Gain is linear, defaults to 1, and accepts finite values from 0 through 1; it affects active and future voices. The `setAudioMasterGain` view command takes `world` and `gain`; `audioStatus` reports `masterGain`. Mixer gain is runtime state, not scene data.

`AudioVoiceSettingsComponent.category` defaults to the empty category. `AudioWorld::setCategoryGain(category, gain)` and `categoryGain(category)` control named categories per world; unset categories have gain 1. `setAudioCategoryGain` takes `world`, `category` (an empty string selects the default), and `gain`. Gains must be finite and in [0, 1]; changes affect existing and future voices without changing the emitter's volume. For a loaded voice, `audioStatus` reports `category`, `categoryGain`, and `effectiveGain`: the applied emitter volume times category gain times master gain, before spatial attenuation. This is not an output level or audibility measurement.

Add AudioVoiceSettingsComponent alongside an emitter to set pitch and spatial distances. Without it, pitch is 1, inverse attenuation uses minDistance 1, maxDistance FLT_MAX and rolloff 1, and category is empty. AudioWorld::details(entity) returns the applied parameters, category and pre-attenuation effective gain; status(entity) retains its loaded, playing and ended booleans.
