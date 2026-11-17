# Drive SPITE with an agent

Use the live engine to discover capabilities, inspect a scene, apply edits, and verify the result. The CLI, MCP adapter, and editor use the same control registry. In the editor, a person can see attributed scene edits and review or revert them.

## The `spite` entry: install, versions, project commands

`spite` is one program on the user PATH. Source: `Launcher.cpp` (resolution, hand-off), `SdkManager.cpp` (the SPITE_HOME and installs), `SdkPlatform.cpp` (OS edge); `Main.cpp` (Windows) holds the engine control commands, `LinuxMain.cpp` the Linux `main` (control commands are Windows-only so far).

- `SPITE_HOME` (Windows `%LOCALAPPDATA%\Spite`, Linux `${XDG_DATA_HOME:-~/.local/share}/spite`): `bin/spite` (the entry copy), `sdk/<version>/`, `config.json` (`default`, `links`), `tools/` (the shared `SPITE_TOOLS_CACHE`), `tmp/` (staging).
- Manager commands, run by the entry copy itself, all with `--json`: `install [<ver>|latest]`, `update`, `list`, `default <ver>`, `uninstall <ver>` (not the default), `link <name> <checkout>` / `unlink`, `self install [--from <sdk-root>]`, `self uninstall [--yes]` (SPITE_HOME's own folders and the PATH entry; other files stay), `version`. Releases come from `spite-engine-global/spite-engine` (`SPITE_RELEASE_REPO`) through the system `curl` and `tar`, verified against `spite-release.json` or `SHA256SUMS`, staged in `tmp/` and renamed into `sdk/<ver>`. The token reaches curl only through a 0600 file in a private temp folder; entry replacement takes `entry.lock`. A private repo needs `SPITE_GITHUB_TOKEN` or `GITHUB_TOKEN`. `SPITE_NO_PATH=1` skips all PATH and profile edits.
- Every other command runs in the SDK chosen by: `spite +<ver> ...`, `SPITE_SDK`, the nearest `.spite/sdk.json` (`version`), then `config.json` `default`. A missing version is installed first (not with `--offline` / `SPITE_OFFLINE=1`) and the command is handed to that SDK's own `bin/spite` with `SPITE_SDK_RESOLVED=<ver>`. A pin older than the default prints one note and is never upgraded; `spite upgrade [--to <ver>]` moves it (`--to X` is `+X`).
- Names by context. In an installed SDK (`<exe>/../sdk-manifest.json`): `new <path>`, `build`, `run`, `ship`, `doctor`, `setup`, `upgrade` and every other command go to the SDK's Python tooling, as `sdk.cmd` / `sdk.sh` do. From an engine checkout's build: `run`, `build <project>`, `new <GameName>` are the contributor commands described below, and nothing is resolved unless `+<ver>` is given. `attach`, `request`, `sql`, `diff`, `mcp`, `modules`, `module-cmds`, `rdc` mean the same everywhere.
- `spite update` and `spite install` run the new SDK's `doctor --json --fast` (only when its tooling knows `--fast`) and warn `needs a newer compiler; run 'spite setup'` while still switching the default.

## Start a session

For a binary SDK installation, start with the installed `AGENTS.md` and `README.md`.
They describe project roots and the shipped `sdk new` command; the checkout build
commands below are for engine contributors. The consumer instructions are authored
in `sdk/DeveloperSdk/developer/AGENTS.md` in the engine source tree.

From the checkout root on Windows, build the editor and CLI if you need the binaries. See the [engine setup](../../README.md#build) for prerequisites.

```powershell
pwsh -File scripts/Build.ps1 -Projects SpiteCli,SpiteEditorHost -Configuration Release
$spite = Resolve-Path ./bin/x64/Release/spite.exe
```

Run a disposable session and send one JSON request per line. This example inspects the scene and closes the host.

```powershell
@(
    '{"query":"worlds"}'
    '{"query":"help","args":{"topic":"light"}}'
    '{"query":"describe","args":{"name":"find"}}'
    '{"query":"find","args":{"world":"scene","limit":2}}'
    '{"command":"shutdown"}'
) | & $spite run --json --client "Scene Agent" --session agent-quickstart --timeout 60 -- --headless
```

For a running editor, use `& $spite attach --json --client "Scene Agent"` from its checkout. Add `--session <name>` if it was launched with a named session. Keep the process open across requests. A competing attach waits up to two seconds for the single-client pipe, then reports that it is busy; pass `--wait <seconds>` to choose a longer wait. End input to detach from an existing editor. A `shutdown` command closes the host, so reserve it for sessions you own. Use a windowed session for screenshots and viewport operations.

An MCP client can launch the same executable with arguments `mcp attach --client "Scene Agent"` to connect to an open editor, or `mcp run --timeout 60 -- --headless` to own a bounded session. Use the absolute executable path and the correct checkout or session. `tools/list` derives tools and input schemas from the active registry. List again after loading or unloading a module. Read the engine's reply envelope in the tool's text content.

## Find the right tool

| Need | Start here |
| --- | --- |
| Find a capability by intent | `searchCapabilities` with search words and optional kind, then `describe name=<entry>` for a command or `components name=<component> counts=false` for a component |
| Find loaded worlds and module capabilities | `worlds`, `availableModules`, or `spite module-cmds <module>` |
| Inspect a component schema | `components world=scene name=<Component> counts=false` for field names, types, and declared limits |
| Find entities and read several components | `find` with `with`, `values`, and a small `limit`. Use the returned `next` value as `after` to page. |
| Inspect one entity | `entity world=scene id=<id>` |
| Find assets without reading the whole manifest | `assets` with `type`, `text`, `limit`, and `fields`. Use `nextOffset` as the next `offset` with the same filters. |
| Join or compare large datasets | `tables`, then `snapshot` with the needed table names and a relative capture path. Run `spite sql` on the returned file. |
| Verify what the renderer produced | `screenshot` for a windowed render and `help topic=pixelInfo` for per-pixel inspection |
| Trace what changed | `history` for undo steps and `provenance` for per-entity attribution and previous values |

The live registry reflects the current host and its loaded modules. `searchCapabilities` ranks commands, queries, components, and recipes. Search terms can match component fields, as in `light intensity`. It returns `total` and up to 12 short matches by default. With zero exact matches it returns up to three partial suggestions, including close spellings of component fields. The `owner` filter matches the registered owner for commands and queries, and the category for components.

Recipe matches include structured `steps`. Each step gives a `command` or `query` and literal `args`. Supply any required `inputs` yourself. A `bind` entry fills an argument from a zero-based earlier step and a JSON `pointer` into its result. In PowerShell or client code, prefer explicit JSON for arrays, objects, and exact numeric values.

## Python scripting kit

The kit uses Python 3.10 or later and the built `spite` CLI. From the checkout root, run a disposable scene probe or search an open editor.

```powershell
sdk.cmd python tools/SpiteCli/spite_agent.py scene-probe
sdk.cmd python tools/SpiteCli/spite_agent.py search "create light" --kind recipe --limit 3
```

Pass `--exe <path>` before the verb to select a different CLI binary. Use `--session <name>` to attach to a named session. The probe starts a bounded engine, searches for `find`, moves an entity in a named batch, and checks the move and its undo.

Routine engine output goes to a temporary log. Use `--engine-log <path>` before the verb to keep it, or `engine_log` when constructing a Python session. On a connection failure the client includes a short excerpt.

Import `Session` from `spite_agent.py` in your own scripts. `Session.attach()` waits for the pipe connection and reports an attachment failure before the first query. Retain the selected CLI, session/endpoint and OS error on failure; access denial can reflect endpoint permissions or a sandbox restriction and does not by itself prove an account mismatch. One connection serves all the requests. `describe` fetches a single live schema, and `call` uses it to choose whether to send a command or query. Use `query` and `command` directly when the kind is already known. A refusal raises `SpiteError` with the original envelope in `reply`. A request timeout closes the connection because a write may already have taken effect.

Use `quiet=True` on a Python query, or top-level `"quiet": true` on a JSON request, to leave the scene Agent timeline alone. This is useful for background inspections.

```python
page = spite.query("assets", type="model", text="lamp", limit=10, fields=["name", "status"])
state = spite.query("entity", world="scene", id=entity_id, quiet=True)
```

Asset pages use the existing order by type and then name. Keep the filters the same while paging, and restart if the manifest changes. The full list remains available with no filters or projection. Use `bounds=True` or `placement=True` when requesting fields derived from those reads.

For example, put this script beside `spite_agent.py` and run it in a disposable SandboxGame Play session. Change the Python rule to try another behavior in the same session.

```python
from spite_agent import Session

with Session.attach() as spite, spite.activity("Recover low health", why="Try a Play-world recovery rule") as activity:
    spite.command("pause", world="play")
    health = spite.query(
        "find", world="play", values=["SandboxHealth"], limit=10,
        **{"with": "SandboxHealth"},
    )["entities"]
    edits = [
        {"command": "set", "args": {
            "id": row["id"], "path": "SandboxHealth.health", "value": 50,
        }}
        for row in health if row["SandboxHealth"]["health"] < 30
    ]
    if edits:
        spite.batch("Recover low health", edits, world="play")
    step = spite.command("step", world="play", fixed=1)
    activity.evidence = f"Play reached fixed step {step['fixedSteps']}."
```

## Configure a flipbook card

In Edit mode, place a card from **Project → VFX** and select its textured mesh entity. From the checkout, run:

```powershell
sdk.cmd python tools/SpiteCli/spite_agent.py flipbook
```

The command reads `<model>.flipbook.json` from the editor's import root, validates the card through `placeFlipbook`, and adds or replaces playback in one Undo step. Run it again to replay from the first frame. The effect animates while the selected world's simulation is running, including in Edit mode. Editing while in Play affects only the Play world.

| Model | Columns × rows | Frames | FPS | Loop |
| --- | --- | ---: | ---: | --- |
| Explosion00 | 5 × 5 | 25 | 24 | Off |
| CandleSmoke01 | 20 × 4 | 80 | 24 | On |
| Flame02 | 16 × 4 | 64 | 24 | On |

For a new card, specify its grid. The frame count defaults to all cells; use `--frames` if there are unused cells at the end of the sheet. Playback defaults to 24 FPS, once, and follows the card's rotation. Use `--loop` for a continuous effect or `--camera-facing` for a blended card that casts no shadows.

```powershell
sdk.cmd python tools/SpiteCli/spite_agent.py flipbook --grid 8x4 --frames 30 --fps 20 --loop
```

To reuse the settings, save a preset beside the source card, directly in the import root, named after the baked model. For example, `Explosion00.flipbook.json` contains:

```json
{"columns": 5, "rows": 5, "frameCount": 25, "framesPerSecond": 24, "loop": false, "cameraFacing": false}
```

## Edit, verify, and hand off

1. Pick the world explicitly. Use `scene` for authored edits. Use `play` for runtime experiments. Play clones the scene, and Stop discards the play world.
2. Look up the entity and field against the current scene. For a disposable test, send `{"command":"createEntity","args":{"world":"scene","name":"AgentProbe"}}` and read `result.id`.
3. Group related undoable commands into a named `batch`. It creates one history step, and if a command fails, the engine attempts to roll back the earlier commands. Inspect failure details and read back the scene if rollback fails. Replace `ENTITY_ID` with the returned ID before sending this example as one line.

   ```json
   {"command":"batch","args":{"world":"scene","name":"Place AgentProbe","commands":[{"command":"set","args":{"id":"ENTITY_ID","path":"TransformComponent.position","value":[2,0,0]}}]}}
   ```

4. Read back `entity` or a projected `find` result and check the expected values. For a visual change, capture from the same camera before and after. In a disposable session, send `{"command":"undo","args":{"world":"scene"}}` and read back again to confirm the move reversed.
5. Report what changed, the named step, the verification result, and any capture paths. Save the scene separately when the task calls for persistence.

In `--json` mode, stdout carries one `{ok,result}` or `{ok:false,error,...}` envelope per request. Engine logs go to stderr. Read `details` and `argErrors` when present. If the connection drops during a write, inspect state and history before retrying.

The Agent panel combines scene edits, module builds and script runs. Applying changes shows an animated three-dot wave; reading or checking shows a breathing magnifier. A steady eye marks changes awaiting your review, and completion settles to a checkmark. The panel header counts changes awaiting review even after newer work finishes. Routine standalone inspections stay out of the panel; click anywhere on an operation row to expand its result, reason and review actions. Raw query and command names remain available through `agentActivity` and the Activity log. **Keep changes** and **Revert changes** accept or undo the operation's latest scene edit; earlier edits remain applied. A step can affect multiple entities, and a conflicting later edit can block its revert.

Group related requests with one operation ID. In Python, use `with spite.activity(title, kind="script", why=...) as activity:`. Use `kind="scene"` for a scene recipe so its inspections and writes share one row. Set `activity.evidence` after your script verifies its result. The feed labels that evidence as agent-reported. A raw client can send `beginAgentActivity`, include the returned ID in the top-level `activity` field of related requests, and close it with `finishAgentActivity`. Query `agentActivity` to read the same feed.

## Show game module work in the editor

Game module work appears in the Agent activity feed with its short decision notes and the evidence SPITE recorded. Keep summaries in plain language.

For the one-command workflow, load the module in the editor and make the C++ edit. Adapt the [SandboxGame plan](examples/sandbox-module-loop.json) with your decision notes and checks. Run the plan against a stopped SandboxGame editor if you want to check the starting score.

```powershell
sdk.cmd python tools/SpiteCli/spite_agent.py module-loop tools/SpiteCli/examples/sandbox-module-loop.json --start-play
```

The loop uses the editor's build configuration, waits for one compact build result, checks the linked binary before and after reload, and records the Play checks. On Windows the live build checks MSBuild's tracked inputs for loaded dependencies before compilation. A blocked or failed build returns the first diagnostic and build log path when available. `--start-play` starts Play if it is stopped. Omit it to require an existing Play session. A superseded build stops before reload. A build timeout leaves the editor's build running for inspection. The command prints a JSON result and exits with a nonzero status on failure.

An unchanged build returns `outcome: "unchanged"` and exits with status 3. For checks against the already loaded binary, add `--verify-active`. This pins the loaded binary's write time and runs the Review checks without a build or reload. The Agent header shows the current module phase, and the Game module panel keeps the result badge with the operation.

To drive individual stages yourself:

1. With the module loaded, call `beginAgentModuleWork` with its `project`, a `title`, an Inspect `summary`, and `why`. Keep the returned `operation` ID.
2. After editing the module, send an Apply note with `noteAgentModuleWork`. Include the `operation`, `stage: "apply"`, `summary`, and `why`. Pass the same `operation` to `buildProject` in the editor's configuration.
3. Query `waitAgentModuleBuild` with the operation ID for a bounded wait and a compact build result. After it succeeds, use `reloadModule` and confirm that the linked binary is active. The module timeline matches the build process and binary write time from SPITE's build records.
4. In Play, use `checkAgentModuleWork` with an operation ID, a plain-language label, a module-owned query or `uiTree`, optional query arguments, and a JSON `pointer` and `equals` value. The editor runs the query against the active build and records the check result. Add a Review note to explain what the check establishes.

The current module operation remains available through `agentModuleWork`. Its operation ID also identifies the corresponding feed row. Earlier feed rows stay in the editor session’s bounded history.

Use `uiTree` with `world="play"` for a game HUD, or `world="editor"` for an editor panel. For a single label, use its ID as `root` and set `depth=0`. A failed lookup suggests Play when that root resolves there.
