---
name: spite-game-module
description: Use when writing or changing C++ gameplay or rendering code for a SPITE project.
---

# Native game module

`<sdk>` is `SdkRoot` from `spite project`; run `spite` from the project root.

## Code rules

- Include only the public headers under `<sdk>/include/spite` and link the matching `Spite::` CMake targets. Editor and test interfaces are not gameplay dependencies.
- Build Release only. The module must match the `apiStamp` and `toolsetStamp` in `.spite/sdk.json`, and must be rebuilt for each SDK version (`spite upgrade` rebuilds).
- `load` registers the module's capabilities. `attachWorld` registers Play systems and services on the world it receives. Never use the edit scene for gameplay.
- Register systems, services and handlers under the module owner, and release every callback and subscription on unload.
- Check optional services before use. `ModelSpawner`, for example, needs an asset runtime and a bound world, and a host without a GPU has neither. Spawn on the frame thread, outside system execution.
- Check every `Expected` result and surface `error().message` instead of discarding it.
- A runtime HUD must register its font with the world's `UiViewContext::registerFont`. Fonts the editor registers do not exist in Play.
- For continuous movement, use `InputActionType::Axis` and read `InputSystem::getActionValue` each frame. For one-shot actions, use `Press` and `wasActionPressedThisFrame`. `Hold` reports elapsed time only after its threshold, so it does not work as movement input.
- Use `InputSystem::hasGameInputFocus()` when present in the pinned SDK to gate mouse look: it combines host ownership with observed window focus. Cursor capture is a request and does not prove focus or OS-granted capture. Focus is independent of pause; `views.gameInputFocused` is host ownership and `gameInputEffectiveFocus` includes native focus.
- Optional `try`/`has` inspection is empty for pending, unregistered, dead or stale entities. Strict access still requires an applied entity; typed system access still requires declared dependencies. Copy component values before structural edits and reacquire references afterward.
- At `attachWorld`, current relations exist before the compiled hierarchy index is ready. Use `currentParentOf`/`collectSubtree` as appropriate rather than assuming `parentOf` or `isInHierarchy` is current.
- Rigidbody/collider edits are consumed at fixed synchronization and survive intervening frames without a fixed step. Velocity setters wake eligible non-static bodies even for zero velocity. Collider dimensions ignore transform scale; offsets are body-local and rotate with the body.
- `TransformBehaviourMode::Teleport` is persistent: a changed pose is applied at fixed synchronization and clears non-static velocities. Wait for consumption before restoring the prior mode. For a one-shot world-space pose, use `PhysicsWorld::teleport`; it queues the request for the next fixed step.

## Build and reload

1. Build with `spite build`. This also compiles `shaders/custom`. A build loads nothing.
2. The first time, call `loadModule` with the absolute module path, for example `build/<Name>.dll`. After each rebuild, call `reloadModule` yourself.
3. Check the reply and the active module identity, including its path and binary hash.
4. Verify behavior with the spite-verify skill.

- Never compile inside the host and never set up automatic reload watchers.
- Reload keeps existing entities. It does not reset the scene.

## Module loop

`module-loop` builds through the editor's build configuration. Use it only for a project registered there; otherwise use the steps above. For a registered project:

```text
spite python <sdk>/tools/SpiteCli/spite_agent.py [--session <name>] module-loop <plan.json> --start-play
```

Copy the plan from `<sdk>/tools/SpiteCli/examples/sandbox-module-loop.json`.

## Change an engine module

You can change an open engine module from `<sdk>/source/open`:

1. Copy it: `spite source copy <project> <module> <destination>`.
2. Build it: `spite source build <project> <module> <source>`. Add `--fork-revision <commit>` only when the tree matches that commit byte for byte.
3. Select the build: `spite source select <project> --build <path>`.
4. To return to the stock module, run `spite source restore <project>`.

## Samples

- `<sdk>/samples/SalvageGame` covers gameplay, HUD, input and audio.
- `<sdk>/samples/RenderConsumer` covers a rendering extension.

Render-pass context helpers acquire service locks internally. Release corresponding service handles before graph, texture or buffer-address lookups; see the render-pass service locks section in `guides/AgentWorkflows.md`.
