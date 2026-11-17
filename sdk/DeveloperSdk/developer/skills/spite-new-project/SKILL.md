---
name: spite-new-project
description: Use when starting a new SPITE game or scene-only project.
---

# New SPITE project

1. Pick a path that is missing or an empty directory, outside the SDK. `new` refuses anything else.
2. Create the project with the global `spite` (from any directory):
   - A game with C++ code: `spite new <dir>`. `--name <N>` sets the project and module name.
   - Scenes only, no code: `spite new <dir> --content-only`. This kind needs no build.
3. `cd <dir>`. Check the tools: `spite doctor --profile build` for a C++ project or `--profile editor` for a scene-only one. Use the spite-setup skill if something is missing.
4. Build a C++ project with `spite build`.
5. Launch it:
   - `spite run` starts the editor under agent control (stdio). Use the spite-session skill for the session.
   - `spite run --control none` starts it for the user to drive.
   - `spite run --game` starts the game host.
   - Pass host options after `--`, for example `spite run -- --scene <scene-path>`.

Run every project command from the project directory or a subfolder. On Linux the same commands work; only `attach`, `request`, `sql`, `modules` and `mcp` are Windows-only.

## What the project contains

- Only the project's own files: a C++ rendering-extension sample (`CMakeLists.txt`, `<Name>.cpp`) with a custom shader in `shaders/custom`; the scene-only variant leaves these out. Engine assets, shaders and config stay in the SDK and are found through the pin; never copy them in.
- `ship.json` for packaging, empty `assets_baked`, `assets/audio`, `scenes` and `models`, and writable folders that belong to the project.
- Identical generated `AGENTS.md` and `CLAUDE.md`, with plain skill copies in `.agents/skills/` and `.claude/skills/`. Keep `.spite/sdk-managed-files.json` in version control: its per-file hashes let upgrade preserve edited and user-added files while refreshing unchanged SDK files.
- `.spite/sdk.json` pins the SDK `version` with its `target`, `apiStamp` and `toolsetStamp` (no path, so the project moves between machines). There is no per-project launcher.
- Empty input bindings in `config/project/input/actions.json`. Add the game's own actions there. The SDK ships no key presets.
- `build/` holds the assembled content root and build output. Project-layer `saveConfig` writes to the project's `config/project`; saved settings survive deleting `build/`. User-layer settings keep their user destination.

Keep every project file inside the project. Never write into the SDK.

For a complete gameplay example, follow `<sdk>/samples/SalvageGame/README.md` (`<sdk>` is `SdkRoot` from `spite project`). Its `Prepare.py` creates a new project from the sample: `spite python <sdk>/samples/SalvageGame/Prepare.py --sdk <sdk> --project <new-dir>`.
