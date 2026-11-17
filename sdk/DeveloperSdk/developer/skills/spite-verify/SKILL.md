---
name: spite-verify
description: Use before claiming that a SPITE change works, shows on screen, or behaves correctly in play.
---

# Prove it in the engine

Each claim needs its own proof. Report every row separately, and mark rows you did not check with the reason. `<sdk>` is `SdkRoot` from `spite project`; sessions are in the spite-session skill.

| Claim | Counts as proof | Does not count |
|---|---|---|
| Build | Exit status and build log | |
| Load or reload | `reloadModule` reply and the active binary's identity | A successful build |
| Persistence | Saved, reopened, and queried again by name | A readback before saving |
| Gameplay | Transforms, physics and spawned entities the engine observed, compared with expected values | The module's own status flag |
| Visible output | A windowed capture you opened and looked at | `uiTree`, a successful capture reply, a headless run |
| Sound | A person listening, or a loopback recording | `audioStatus` showing `playing` |

## Gameplay

1. Read `inputActions` for each action's type, binding and context. In the editor, use `gameFocus` when the Game panel must own input.
2. Start Play paused.
3. Send `injectAction` with an explicit world and frame count.
4. Advance with `step` and `fixed:=<count>`. The value is a number of steps, not seconds.
5. Release held input in a `finally` block, even when an assertion fails.
6. Record the start scene, seed, every press and release, step counts, expected and observed values, and the active binary.

For end-to-end application device input, omit `world` and use the editor's `gameFocus` gate. For counted world injection use the explicit-world lane above; state which lane was tested. A `Press` action observes a press edge for one frame: inject press, observe, then release. Mouse buttons route at the retained pointer position unless moved explicitly; a click outside Game may transfer focus, so inspect `views` and reacquire Game focus before continuing. Absolute pointer injection does not establish captured relative mouse behavior.

## Visuals

- Use a windowed host with a Vulkan device. Headless capture may be unavailable.
- `captureScene path=<file>.png settle:=8` renders through a copy of the editor camera. `screenshot` captures the window. Open the PNG and look at it.
- For a capture through a game camera, start Play paused and step a few frames first. Frame zero can come out white.
- Check `uiFonts` before trusting text on screen.
- For a HUD at a fixed resolution, capture the game host's swapchain. A Game panel capture has the panel's size.
- An editor Play check does not replace a game host check. Launch the game host with `spite run --game` (stdio control, spite-session skill) and inspect it separately.

## Report

- Keep log paths, capture paths, the SDK's `sourceHead` and `apiStamp` (in `.spite/sdk.json` and `<sdk>/sdk-manifest.json`), and the module identity.
- Respect the scope the user asked for, and name the checks you skipped.

More detail is in `<sdk>/guides/AgentWorkflows.md`, sections "Verify behavior and visible UI", "Windowed scene capture" and "Native gameplay: action values and Play capture".
