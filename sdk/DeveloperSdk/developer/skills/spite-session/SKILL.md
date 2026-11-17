---
name: spite-session
description: Use when you need to read or change anything inside a running SPITE editor or game.
---

# Drive the live engine

Run `spite` from the project root (any subfolder works). It is the global launcher, `%LOCALAPPDATA%\Spite\bin\spite.exe`, on the user PATH. In a shell opened before the install, prepend that folder to PATH. `<sdk>` is `SdkRoot` from `spite project`, which prints JSON (its `note:` lines go to stderr).

`attach`, `request`, `sql`, `modules` and `mcp` are Windows-only. On Linux use the stdio host below.

## Open a session

**An existing editor owned by the user.** Connect with `--session <name>` when it has a named pipe session. Leave it off if the editor has no name.

```text
spite request --session <name> worlds
spite attach --json --session <name>
```

- Never launch, restart or shut down that editor, and never send `shutdown` to it. When you finish, tell the user you only closed your connection.
- `request` exits with 2 and "no engine is listening" when nothing serves that name. Ask the user for the name instead of starting an editor.
- Use the Agent panel: it is what a watching human sees (see Edit safely).

**A host of your own, over stdio.** `spite run` starts the editor with the project's roots and reads one JSON request per line from stdin, answering one line on stdout. Keep stdin open; closing it ends the host.

- `spite run --game` starts the game host. Host options go after `--`: `spite run -- --headless`, `spite run -- --scene <scene-path>`. `run` accepts only `--game`, `--control stdio|none` and `--`: there is no `--session`, `--detach` or `--timeout` in an installed SDK.
- Use a windowed host. Add `-- --headless` only for probes that need no screenshots or viewport.
- From Python, `Session(["spite", "run"], engine_log="engine.log", request_timeout=120)` runs it (see below). End a host you started with `{"command":"shutdown"}`.

**A named pipe host that outlives the command (Windows).** Start the SDK editor directly with the project's host arguments:

```powershell
$p = spite project | ConvertFrom-Json
$argv = @($p.HostArgs) + '--control', 'pipe', '--session', 'my-game' | ForEach-Object { '"{0}"' -f $_ }
Start-Process (Join-Path $p.SdkRoot 'bin\SpiteEditor.exe') -WorkingDirectory $p.ProjectRoot -ArgumentList ($argv -join ' ')
```

Then poll `spite request --session my-game worlds` until its exit code is not 2. Shut down only a host you started: `spite request --session my-game shutdown`.

## Send requests

```text
spite request --session my-game describe name=find
spite request --session my-game find world=scene limit:=5
spite request --session my-game --file request.json
```

- Flags come first, then the request: a JSON object, `name key=value key:=<json> ...`, or `--file <path>`.
- `key=value` sends a string. `key:=<json>` sends numbers, booleans, arrays and objects.
- Use `--file` for long or nested arguments instead of shell quoting.
- A failure returns `ok:false` with `error`, `details` and `argErrors`. Read them before you retry.

For more than three related requests, write a Python script and keep one connection. Run it with `spite python script.py` (the interpreter the SDK selected, `spite tools` shows it).

```python
import json, subprocess, sys
from pathlib import Path
sdk = Path(json.loads(subprocess.check_output(["spite", "project"], text=True))["SdkRoot"])
sys.path.insert(0, str(sdk / "tools/SpiteCli"))
from spite_agent import Session, SpiteError

with Session.attach(session="my-game") as s:  # no session= for an unnamed editor
    print(s.query("worlds"))
```

A refused request raises `SpiteError` and keeps the envelope in `.reply`. `Session.attach` uses the SDK's own `bin/spite.exe` by default. The helper also has a CLI: `spite python <sdk>/tools/SpiteCli/spite_agent.py [--session <name>] <verb>`.

## Discover before you call

- Find capabilities by intent: `searchCapabilities text="create light"`. Add `kind` (`recipe`, `command`, `query`, `component`) and `limit` when needed.
- Read the schema with `describe name=<entry>` before the first call. Use only the arguments it lists. Never guess command or field names.
- Look around with `worlds`, `availableModules`, `help topic=<topic>` and `components world=scene name=<Component> counts:=false`.
- A recipe returns `steps`. Supply its `inputs` yourself, and fill each `bind` from the earlier step's result.
- Search again after a module loads. Its commands appear only then.

## Edit safely

- Name the world for world-scoped edits and queries. Use `scene` for authored content and `play` for runtime experiments. Application-level device injection intentionally omits `world`. Play is a clone, and `stop` discards it.
- Keep queries bounded. Use `find` with `with`, `values` and `limit`, and page with `next` as `after`. Use `assets` with `type`, `text`, `limit` and `fields`.
- Use the IDs that queries return. Display names and order are not identifiers.
- Group related edits into one named `batch`. It becomes a single undo step.
- Read every edit back with `entity` or a projected `find`, and compare it with the expected values.
- In a script you may rerun, `find` by a distinctive name first. Create the entity if `total` is 0, reuse it if 1, and stop if more.
- After a write times out, the change may already have happened. Check `history` and the state before you retry.
- The editor's Agent panel shows what you do. Group a task's requests into one row with `with s.activity("Title", why="...") as a:`, and set `a.evidence` after you verify the result. Title, why and evidence limits are 120, 280 and 512 UTF-8 bytes. Review reverts the activity's latest scene undo step; earlier steps remain applied. Use one named batch when a group must be one undo step.
- Keep background reads out of the panel: `quiet=True` in Python, or `"quiet":true` at the top level of a JSON request (queries only). A `quiet:=true` word on the `spite request` line becomes an argument, not that flag.
- For large comparisons, call `tables`, then `snapshot`, then run `spite sql <snapshot.db> "select ..."`.

For SDKs supporting enum labels, generic `set` accepts the exact case-sensitive registered label or the numeric value; output remains numeric. Only generic `set` takes labels: `addComponent` values, config edits and scene JSON take numbers. Read schema choices rather than inventing labels. Project-layer `saveConfig` targets `config/project` in the project, separately from staged defaults and generated module paths.

Details are in `<sdk>/guides/AgentWorkflows.md` (sections "Discover, inspect, then edit" and "Session ownership and focused identity") and in `<sdk>/tools/SpiteCli/README.md`. Distinguish contributor-checkout launch options from installed SDK options; use the installed CLI's help.

For a failed or cancelled activity with no pending scene step, send `{"command":"reviewAgentActivity","args":{"id":<activity-id>,"action":"acknowledge"}}`. Evidence remains in `agentActivity`. `keep` and `revert` review only the latest linked scene undo step, not all activity edits.

Explicit `gameFocus` clears stale editor focus when the Game view is visible and no modal owns input; a later UI press can reclaim focus.

Graph descriptor exhaustion safely stops a graph build and retries when capacity returns; descriptor slots retired at the handle generation limit (`texturesRetired`, `storageImagesRetired` in bindless `stats`) count as occupied. Graph recreation retires pending descriptors after device idle before allocating the replacement. Graph `stats` count rebuilds by cause (`rebuilds*`) and Info logs name each rebuild's and swapchain recreation's cause; this does not establish the cause of a historical crash.

`Session.attach(reuse=owner)` borrows an existing Python session in the same
process. Pass it to nested tools instead of attaching again. A borrowed handle's
`close()` or context exit leaves the owner open; the owner must outlive all uses
and closes the connection. Do not combine `reuse` with connection options. This
does not share sessions across processes.

Query `agentActivity` returns its activity list in `operations`, not `rows`.
Query `lodStats` returns `views`; its GPU counters arrive asynchronously. Query
once to request collection, allow frames to advance, then query again and check
`measuredFrame` (zero until the first sample lands). For a fresh measurement,
record each view's prior value and wait for it to advance; bound the wait because
paused, frozen or inactive views may not advance. Use `describe` for each query's
current fields and semantics rather than assuming a common row shape.
