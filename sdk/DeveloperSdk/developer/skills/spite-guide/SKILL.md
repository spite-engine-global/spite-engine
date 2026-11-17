---
name: spite-guide
description: Use when someone asks what SPITE is, how it works or what it can do, or wants it shown in the editor.
---

# Explain and show SPITE

Answer from the installed SDK and the live engine, not from memory. Run the global `spite` launcher from the project. `spite project` prints `SdkRoot` as JSON; below, `<sdk>` is that path. `.spite/sdk.json` records the SDK version and identity stamps; the launcher resolves the installed SDK location.

## Where the answers come from

| Question | Source |
|---|---|
| What SPITE is, platforms, setup, packaging | `<sdk>/README.md` |
| How an agent works with it | `<sdk>/AGENTS.md`, then one section of `<sdk>/guides/AgentWorkflows.md` |
| Which version the project uses | `spite version` (`spite list` shows every installed one), and `sourceHead` and `apiStamp` from `<sdk>/sdk-manifest.json` |
| What the engine can do right now | The live registry: `searchCapabilities text="<intent>"`, `help topic=<word>`, `describe name=<entry>` |
| What the editor's panels are called | The live `editorPanels` query. Titles are in `<sdk>/assets/loc/en/editor` |
| A complete game to look at | `<sdk>/samples/SalvageGame/README.md` |

If none of these confirms a feature, say that SPITE does not offer it yet. The spite-gap-tools skill lists what can be baked outside the engine instead.
Before concluding that a capability is absent, inspect relevant config sections and public schemas too: capability search may not index individual config fields.

## What to tell

Pick what fits the question. Keep it short and offer a demo.

- **What it is.** A native engine with an editor (`SpiteEditor`) and a game host (`SpiteGame`). Users get it as a binary SDK. Their projects live outside the SDK and are created with `spite new`.
- **Platforms.** Windows x86-64 and Linux x86-64. No macOS. The editor and the game need a GPU with Vulkan 1.3, even headless.
- **Maturity.** Version 0.x (`spite version`). Before 1.0, a minor release may break compatibility: a project pins one SDK version, game modules are rebuilt for each version, and `spite upgrade` moves the project.
- **One control registry.** The editor UI, the `spite` CLI and agents call the same commands and queries. The editor's own panels send `select` and `editorCamera` through it. Every request is attributed, so the user sees agent edits in the Agent panel and can keep or revert each operation.
- **Two worlds.** `scene` holds authored content and is saved. `play` is a clone that runs the game, and `stop` throws it away.
- **Game code.** Gameplay and rendering extensions are native C++ modules built against the SDK headers. The engine loads them at runtime and reloads them after a rebuild without restarting the editor.
- **Content.** Drop models, textures, HDR environments and audio onto the Scene view. `SpiteBake` and `ShaderCompiler` in `<sdk>/bin` work offline. Shaders are written in Slang.
- **Delivery.** `ship.json` lists the runtime files, and `spite ship <out-dir>` builds a playable release.
- **Measurement.** Built-in profiler captures, frame and pass statistics, and table snapshots that `spite sql` can query.
- **Editor panels.** Discover the current set and names through `editorPanels`; panels and layouts can vary by SDK and loaded modules.

## Open an editor the user can work in

The user needs a project. Use the current one, or create a scene-only project with the spite-new-project skill (`--content-only` needs no build). Check the tools with `spite doctor --profile editor` first.

### Windows

Start a windowed editor that stays open until the user closes it, and give it a session name so you can drive it:

```powershell
$p = spite project | ConvertFrom-Json
$argv = @($p.HostArgs) + '--control', 'pipe', '--session', 'guide' | ForEach-Object { '"{0}"' -f $_ }
Start-Process (Join-Path $p.SdkRoot 'bin\SpiteEditor.exe') -WorkingDirectory $p.ProjectRoot -ArgumentList ($argv -join ' ')
$deadline = (Get-Date).AddSeconds(90)
do { Start-Sleep 2; spite request --session guide worlds } until ($LASTEXITCODE -ne 2 -or (Get-Date) -gt $deadline)
```

`request` exits with 2 while no editor listens yet, so poll until the window has started.

- This editor belongs to the user. Never send `shutdown` to it. When you finish, tell the user it stays open.
- If the user already has an editor open, attach to it instead: `spite request worlds`. Add `--session <name>` if it was started with one, and never launch, restart or shut it down.
- Do not use `spite run` for this. It blocks, ends the host when its stdin closes, and runs without user state, so the panel layout is not kept. It fits a short demo that you run alone.
- Session details are in the spite-session skill and in `<sdk>/guides/AgentWorkflows.md`, section "Session ownership and focused identity".

### Linux

`attach`, `request`, `sql` and `mcp` are Windows-only. Start the editor for the user with `spite run --control none`. You cannot drive that window, so explain what to click, and show results from your own stdio host (`spite run`, driven as in the spite-session skill) with captures.

## Show a feature in the editor

1. Ask what the user wants to see, or offer a tour of three to five stops from the table below.
2. Before each stop, say what will happen and which panel to look at. Do one stop at a time and wait for the user.
3. Use `describe` before the first call of each command. Never guess arguments.

| To show | Do this |
|---|---|
| A panel | `editorPanels` for its `name`, then `dock kind=show panel=<name>` |
| An entity | `select world=scene id=<id>`, then `editorCamera frame=selection`. The Inspector shows its components |
| The whole scene | `editorCamera frame=all`, then `editorCamera orbit:=[30,0]` to turn around it |
| Undoable editing | Make a small change in one named `batch`. Show it in History, then call `undo` |
| Agent review | Run the demo edits in a Python script inside `with s.activity("Demo: <title>", why="...")`. The Agent panel shows one row with Keep changes and Revert changes |
| Play | `play`, then show the Game panel. `stop` discards everything that changed during Play |
| Finding features | `searchCapabilities text="<intent>"`. Read the results aloud as the engine's own answer |
| A view for the chat | `captureScene path=<file>.png settle:=8`, open the PNG and show it. Use it when the user cannot see the window |

## Keep the user's work safe

- Do not edit or save the user's scene for a demo unless they agree. Show edits in a new scene (`newScene`), or revert them with `undo` after the stop.
- Never write into the SDK.
- Shut down only hosts you started for yourself.
