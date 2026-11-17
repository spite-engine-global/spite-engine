---
name: spite-scene
description: Use when building or changing a SPITE scene.
---

# Author a scene

Work through a live session (spite-session skill). Call `describe` for each command before using it.

## Hierarchy

- Create a child with `createPrimitive` and `parent`.
- Move an existing entity with `reparent`, passing `id` and `parent`. It keeps the local transform. Read the world transform back after the hierarchy updates.
- Never reorder or delete entities by editing scene JSON. Parent references there are array indices.

## Components

- A component needs at least one persisted schema field, or the scene save drops it. Query it before saving and again after reopening.
- Component references and pointers can become invalid after structural changes anywhere in the world. Copy needed values before adding/removing components, then reacquire references. Entity handles are world-lifetime identities; resolve them again after scene/world replacement, and do not assume names are unique.
- Load a module before opening a scene that uses its components.
- An enum field takes its registered label only through generic `set`; `addComponent` values and scene JSON take the number. Read the choices with the `components` query.
- `set` cannot change `MaterialComponent.materialHandle`. Remove the component, then add it again with `value:={"materialHandle":{"name":"<material>","assetType":3,"subAssetIndex":0}}`. Use a material that exists in the project.

## Models

`instantiateModel` transform arguments overwrite the imported root's rotation and scale. To keep the source basis, create a placement parent and instantiate the model under it without transform overrides.
Use live relation queries during module attachment; compiled hierarchy queries may not be ready until compilation. Discover supported `find` filters rather than assuming a parent filter exists.
To keep chosen instances at full detail while others keep cluster LOD, add `RenderLodPolicyComponent` to each renderable that holds a mesh, not to the model root. It is not inherited by children. See the section "Full-detail LOD for chosen renderables" in `<sdk>/guides/AgentWorkflows.md`.

## Cameras and lights

- `CameraComponent.fov` is in radians. `editorCamera` takes degrees.
- A saved camera binding may not survive a restart. Call `bindCamera` on the game view before Play.
- When the game host should pick the camera on its own, keep a single scene camera.
- The stock torus spins around local Y with its ring in XZ. Its ring radius is 0.35 and its tube radius 0.15 before scaling. Rotate it to stand it upright.

## Finish

1. Put related edits in one named `batch`.
2. Read the values back.
3. Save the scene, reopen it, and find the entities by name to get their new IDs.
4. Check parents, world transforms and materials again.

Report the batch names so the user can find them in undo history.

More detail is in `<sdk>/guides/AgentWorkflows.md` (`<sdk>` is `SdkRoot` from `spite project`), sections "Scene and game-host checks" and "Models, Play and runtime modules".
