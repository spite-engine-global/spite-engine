# SalvageGame

The authored scene is `scenes/SalvageGame.json`. JSON matches component fields by
name, so keep it while changing native component layouts. A binary `.spsd` save
requires its original component layout; export JSON before rebuilding a changed
schema if you have chosen to save a binary copy.

This native sample registers scene components and a world system, uses Axis movement
and Press actions, binds a world-owned HUD, and plays a generated audio clip. WASD
moves; Space collects nearby salvage; P pauses; R or Enter restarts. Collect three to win.

From an installed SDK (`spite project` prints its `SdkRoot` inside any project; a
plain `spite list` shows versions), prepare a **new** external project. Python and the SDK
build prerequisites are required (`spite doctor`):

```powershell
python <sdk>/samples/SalvageGame/Prepare.py --sdk <sdk> --project <new-project>
```

The generated `ship.json` is:

```json
{
  "format": 1,
  "modules": ["SalvageGame"],
  "files": ["config/project/input/actions.json", "scenes/SalvageGame.json"],
  "scene": "scenes/SalvageGame.json"
}
```

Ship the prepared project to a new directory outside the SDK and project, for example:

```powershell
cd C:\Games\SalvageProject
spite ship C:\GameReleases\SalvageRelease
```

The bounded driver creates and builds the project, authors primitives and a scene
through one persistent control session, reloads the DLL explicitly, reopens the scene,
starts paused Play and checks the player and HUD font. It creates the project with
`new --content-only` (the font and other engine content come from the SDK, not the
project) and generates the WAV locally. It refuses an existing project; choose a new
path for each run. The project's `.spite/sdk.json` pins its SDK version. Rebuild with
`spite build` after changes and explicitly reload the module. The generated scene belongs to the project; generate a fresh one
when moving the project to avoid stale source paths in imported primitive metadata.

Run the game or its bounded gameplay check against the prepared project:

```powershell
python <sdk>/samples/SalvageGame/Play.py <project>
python <sdk>/samples/SalvageGame/Check.py <project>
```

Play focuses the Game panel and closes on Enter; `--timeout` sets its positive host
lifetime in seconds (default 1800). Check uses a 600-second host, explicitly reloads
the module, binds the authored camera, steps paused Play and checks world transforms,
pause, score, win and restart. It releases injected actions on exit. The screenshot
path is printed in its output; inspect the image rather than treating file creation
as proof of its contents. Audio status shows backend state, not audibility. Scripts
run from the SDK that contains them and use its `project` command for
host paths. When the SDK has no native CLI, the scripts use its stdio launcher
and terminate its process group at the configured lifetime. The check changes only the transient Play world.


An authored `CameraComponent.fov` is in radians: for a 37-degree field of
view use `math.radians(37)` (about 0.646). The `editorCamera` command instead
accepts degrees. The camera entity persists in the scene, but a selected render
view binding need not persist across launches. Bind the intended scene camera to
the game view explicitly before Play, as `Runtime.py` does.
