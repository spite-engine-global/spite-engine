---
name: spite-assets
description: Use when adding or updating models, textures, environments, audio or generated content in a SPITE project.
---

# Bring content into a project

## Rules

- Keep source files in the project and baked output in the project's `assets_baked`. Never import or bake into the SDK.
- Find import commands through the live session: `searchCapabilities text="import"`, then `describe`.
- Before placing a result, check the import job's errors and readiness.
- A reimport can create materials and textures with suffixed names. Use the names the command returns.
- A successful bake does not prove that scene instances use the new asset. Query the instance's material and texture.
- Reimport source models to obtain corrected cross-instance LOD seam locking (bake version 15, version 17 for skinned parts on separate nodes) and coarse LOD levels that no longer pull open borders inward at zero reported error (version 16; priced border collapses remain); older files still load. Genuine seams remain protected, so some geometry legitimately has few or no coarser levels. View-wide `lodStats` is not a per-mesh bake inventory; verify visible coverage separately. To keep chosen instances at level 0 without rebaking, use `RenderLodPolicyComponent` (spite-scene skill).
- A `glTF parse:` error reports whole-file parsing, including malformed material/texture references. Use its parser reason; do not assume terrain metadata caused it or invent a field path that was not reported.

## Offline tools

`<sdk>` is `SdkRoot` from `spite project`. Append `.exe` on Windows.

```text
<sdk>/bin/SpiteBake <model-path> <project>/assets_baked
<sdk>/bin/ShaderCompiler --sources <slang-dir> --output <project>/shaders/bin --spirv-only
```

`spite build` compiles the project's `shaders/custom` on its own.

## Large model updates

- Discover `prepareModelImport`, `prepareModelUpdate`, `modelPreparation`, `previewModelPreparation`, `planModelPreparation` and `prepareModelScene`.
- Keep preparation IDs and revisions, and review conflicts instead of forcing them through.
- A preview does not publish anything.

## Generated content

- Next to each generated file, store the generator version, seed, parameters and stable logical IDs, for example the glTF node `extras.spite_source_id`.
- Keep those IDs fixed when the seed changes.
- Runnable example: `<sdk>/samples/AssetConsumer`.

## Audio

1. Import a WAV, MP3 or FLAC with `importExternalAudio path=<absolute path>`.
2. Poll `externalAudioImport` until its status is `done` or `failed`.

- Import rejects a duplicate file name and never replaces a clip. To update a clip, back up `assets/audio/<name>`, replace it, and call `reloadAudio` on the live Play emitter.
- `audioStatus` shows what the backend reports. It does not prove that anyone heard the sound.

## Flipbook cards

1. Put a padded, row-major atlas under the import root that `roots` returns.
2. Call `prepareFlipbookCard` with `wait:=true`.
3. Import its result with `importModel` and `wait:=true`.
4. Instantiate the model.
5. Call `placeFlipbook` with the same metadata. If it says the asset is not ready, retry only that error.
6. Check `flipbook` and a windowed capture.

Full recipes are in `<sdk>/guides/AgentWorkflows.md`, sections "Procedural asset round trip", "Audio import, update and playback", "Flipbook cards" and "Prepared and generated content".
