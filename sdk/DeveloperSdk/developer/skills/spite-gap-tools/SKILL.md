---
name: spite-gap-tools
description: Use when a SPITE scene or port needs something the engine cannot do at runtime.
---

# Bake what the engine lacks

When SPITE cannot do something at runtime, bake it outside the engine into a form SPITE already accepts. Import the result with the spite-assets skill, and check it with a capture you look at (spite-verify skill). `<sdk>` is `SdkRoot` from `spite project`.

| Engine input | How it gets in |
|---|---|
| GLB, static or skinned with clips | `importModel path=<file>` from the project's `models/` folder, offline `SpiteBake`, or `ModelSpawner` from code |
| PNG frame atlas | A flipbook card. See `<sdk>/guides/AgentWorkflows.md`, section "Flipbook cards" |
| Equirect `.hdr` | `importEnvironment path=<file>`, then set `EnvironmentComponent` to `mode:=4` (Hdri) and `hdri=<name>` |

SPITE does not accept VAT, VDB, Alembic, BVH or morph targets. It also rejects glTF files that require Draco, meshopt, KTX2 or `KHR_mesh_quantization`.

## Optional external tools

Examples use Blender, glTF Transform, gltfpack or an image-authoring Python environment.
These tools are not bundled SDK prerequisites. Use an existing installation where
available; install additional tools only within the user's authorized scope.

`<skill>` is the directory containing this `SKILL.md`; resolve helper paths there,
even when the shell stays at the project root. `atlas.py` needs Pillow: use an explicit
authoring-Python path with that package, rather than assuming PATH or `spite python`
selects it. Run Blender without a window:
`blender -b --factory-startup -P <skill>/scripts/<script>.py -- <arguments>`.
Use the installed Blender executable's full path if it is not on PATH.

## Gap to tool

Treat these as authoring examples, not qualification evidence for the pinned SDK.
Check the output's required extensions, import readiness and an actual rendered frame.
Consult the current upstream licence for each tool and asset before redistribution.

| Gap (three.js equivalent) | Bake into | Optional tooling | How |
|---|---|---|---|
| Fire, smoke, explosions (particles, Points) | Flipbook | Blender Mantaflow; other dedicated authoring tools are optional | Run Blender with `<skill>/scripts/smoke_frames.py <dir> 16 128 1`; an authoring Python runs `<skill>/scripts/atlas.py <dir> <out.png>` to pack frames and print metadata |
| Gradient sky, HemisphereLight | Nothing to bake | Built in: `EnvironmentComponent` `mode:=2` (Gradient) with `zenith`, `horizon`, `ground`, `ambientIntensity` | `describe` the component first |
| Daylight sky with sun | Nothing to bake | Built in: `mode:=3` (Procedural) draws an atmosphere lit by the scene's sun | |
| Custom sky, horizon haze | HDRI | Blender Sky Texture or an appropriately licensed HDRI | Blender with `<skill>/scripts/sky_hdri.py <out.hdr> 1024 15` |
| 3D text (TextGeometry) | GLB | Blender text | Blender with `<skill>/scripts/text_mesh.py <out.glb> "Text" ff4d2e` |
| Procedural geometry, lines, tubes, BufferGeometry | Static GLB | Blender geometry nodes, or a curve with bevel converted to mesh. Eight primitives are built in: `createPrimitive` | Write a script modelled on `text_mesh.py` |
| Vertex colors | Texture | Blender bake (Diffuse from Color Attribute), an external UV authoring tool if needed | |
| Trees, vegetation | GLB | A vegetation authoring tool with suitable export and licence terms | |
| Terrain | GLB | An external terrain authoring tool, or a height image displaced in Blender | Check the pinned SDK's collider APIs separately |
| PBR textures | PNG | A texture authoring tool or appropriately licensed source library | Check supported material inputs in the pinned schema |
| Models | GLB | An appropriately licensed asset source or generator | Inspect and clean the glTF afterwards |
| Compressed glTF from three.js or a store | GLB without unsupported required extensions | glTF Transform or gltfpack | Use dequantization for quantized attributes and inspect output extensions; do not assume a copy removes compression. Re-export uncompressed assets or obtain PNG texture sources where necessary. |
| Character rig | Skinned GLB | A rigging tool followed by validated glTF export | |
| Animations | Clips in GLB | Import a suitably licensed motion source into Blender, retarget, and export glTF | SPITE does not directly read BVH |
| Destruction | Skinned GLB | Blender Rigid Body, Bake to Keyframes, one bone per fragment | Not tested |
| Water | Nothing to bake | Built in: `WaterSurfaceComponent` | |

## Gaps no external tool closes

The engine itself does not have the following yet. Tell the user instead of faking them:

- depth fog;
- runtime particles;
- cloth, fluids and morph targets;
- raycasts;
- a character controller.

## Licences

Check the exact tool, model, texture, font and dataset terms at their primary source.
A tool's code licence does not grant redistribution rights for every asset it processes.
Do not assume that an example or a prior project's permission applies to new content.

## Pitfalls

- **Axis conventions.** Blender uses Z-up and glTF export normally converts to Y-up. Keep exporter and importer choices consistent, inspect the imported basis, and preserve it with a placement parent. Do not apply a blanket rotation to every model; `text_mesh.py` deliberately stands up Blender's default text before export.
- **Smoke emitter.** Hide it with `visible_camera=False`. With `hide_render=True` it emits nothing and the frames come out empty.
- **Reimport.** `importModel` refuses an existing model name. Use `reimportModel` or the prepared-update workflow for an existing asset instead of deleting it just to bypass the name check.
