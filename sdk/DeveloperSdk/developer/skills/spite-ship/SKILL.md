---
name: spite-ship
description: Use when someone needs a playable build of a SPITE game.
---

# Package the game

1. Build the Release modules with `spite build` from the project root.
2. List the game's contents in `ship.json`:
   - `modules` holds the module IDs.
   - `files` holds every runtime file: configs, scenes, fonts, locales, UI, animation controllers, compiled custom shaders, and any audio no included scene uses.
   - `scene` names the startup scene. It is optional.

   ```json
   {
     "format": 1,
     "modules": ["SalvageGame"],
     "files": ["config/project/input/actions.json", "scenes/SalvageGame.json"],
     "scene": "scenes/SalvageGame.json"
   }
   ```

   The packager adds two things itself: audio assigned in included scenes, and baked manifest dependencies. The SDK's own runtime content is added too; a listed project file replaces the SDK file of the same path. It leaves out editor state, user state, SDK tools, source models and code.
3. Run the packager:

   ```text
   spite ship <new-directory> [--smoke] [--module <module-binary>]...
   ```

   - Choose a new output directory outside both the project and the SDK.
   - `ship` finds each module named in `ship.json` in `build/` and tells you to run `spite build` if one is missing. `--module` overrides the search: one binary per module ID, in `ship.json` order.
   - Add `--smoke` to run eight frames before writing the output. Skip it when no runtime check was requested.
4. Hand over the output. It contains `game/`, an archive, a SHA-256 checksum, manifests and logs. The game starts with `Game.cmd` on Windows or `Game.sh` on Linux.

Packaging validates baked content, but it does not prove that the UI is visible or that gameplay works. Check the packaged game with the spite-verify skill.

A scene-only project needs a module and build configuration before it can be shipped.
