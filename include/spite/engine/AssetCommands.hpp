#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include "engine/EngineToolsExport.hpp"

namespace spite
{
    class AssetRuntime;
    class ControlServer;
    struct SceneRecoveryAccess;

    // Registers the asset queries and commands (owner "engine") on control.
    // Assets are named as the manifest names them; type is model, material, texture or
    // environment (the assets query also lists skeleton and clip rows). Material fields are the BakedMaterialWriteData
    // schema: albedoFactor [r,g,b,a], metallicFactor, roughnessFactor, occlusionStrength, alphaCutoff (0..1),
    // emissiveFactor [r,g,b] (0..1), emissiveStrength, normalScale, alphaMode (0 Opaque, 1 Mask, 2 Blend, 3
    // Premultiplied, 4 Additive), doubleSided, castsShadow (a Blend material casts a shadow dithered by its alpha;
    // false by default, ignored by the other modes), unlit (KHR_materials_unlit: the surface shows its base colour
    // plus the emission with no lighting, shadows, ambient or reflections; false by default), and per texture slot
    // (albedo, normal, metallicRoughness, occlusion,
    // emissive) <slot>TextureName (a baked texture, "" the default) and <slot>TextureSpace (0 SRGB, 1 LINEAR),
    // <slot>WrapU and <slot>WrapV (0 Repeat, 1 MirroredRepeat, 2 ClampToEdge), the material's textureFilter (0 Smooth,
    // 1 Linear, 2 Nearest) and its texture transform uvOffset [x,y], uvScale [x,y], uvRotation (KHR_texture_transform:
    // the image's top-left origin, v down); a fields arg sets the members it names, unknown members and values outside
    // a range are refused. A factor multiplies its texture's sample, so a bound metallicRoughness texture needs
    // metallicFactor 1. An opaque terrain profile also takes terrainMask, terrain{Grass,Soil,Rock}{Albedo,Normal,
    // Roughness} and terrainDetailMetres; all ten terrain textures and a base-color proxy are required together, and
    // an ordinary normal texture is refused (terrain owns its normals at every distance).
    // The bake is a tagged, versioned .spitemat (asset/WriteBakedFunctions.hpp).
    //   assets [type]                    -> assets: [{name, type, status, refs, baked, materials, source,
    //   authoringSource (model),
    //                                       skeleton, clips (a rigged model), model, joints (skeleton), skeleton,
    //                                       model, duration, tracks (clip: seconds, one track per joint),
    //                                       source, format, width, height, mips, bytes (environment);
    //                                       format, bytes (texture: "BC7 sRGB", "BC5", "RGBA8 sRGB"..., the bytes
    //                                       of the baked chain, which is what the GPU holds)}]
    //                                       a skeleton and a clip are baked with a rigged model (a mesh with bones) as
    //                                       <model> and <model>.<clip>; they are baked data the animation module
    //                                       reads, not loadable assets: status stays unloaded, loadAsset refuses them
    //                                       status: unloaded|loading|ready|failed; refs counts registry
    //                                       references (the load's own and one per binding)
    //   material name                    -> name, status, fields
    //   loadAsset type name [priority]   -> subAssets, prefetchQueued? (with a priority: demands still waiting for
    //   budget) (a model loads every submesh); residency, not undoable;
    //       a priority queues it as streaming demand (AssetRuntime::prefetchAsset) instead of loading at once
    //   instantiateModel model [name] [parent] [transform]
    //                                    -> id, name, model, parent?, entities   inverse: deleteEntity id
    //       the model's hierarchy enters the request's world at once (bindings load after); the root takes
    //       name (default: the model name, suffixed _2, _3... while taken) and transform's members over the
    //       model's root transform (TransformComponent schema)
    //   createMaterial name [fields]     -> name, fields                        inverse: deleteMaterial name
    //   updateMaterial name fields [merge] -> name, fields, previous            inverse: updateMaterial previous
    //       updates of one material naming the same members under one merge key fold into one undo step (a drag)
    //   deleteMaterial name              -> name, fields                        inverse: createMaterial fields
    //       refused while a model references the material or an entity binds it
    //   importModel path [name] [settings] [wait] -> import, name, path, status (the imports row when wait),
    //                                       settings                            inverse: removeModel name
    //       path: a model file (glTF, or another format the baker reads) relative to importRoot, no '..';
    //       name: the model's manifest name (default: the file's stem). The bake runs on an IO worker and
    //       the reply comes at once with status queued|baking; poll `imports` until done or failed. wait
    //       bakes on the frame thread and replies when done (scripts; the history and log record every
    //       import with it, so undo, redo and replay bake before the commands after them). Buffers and
    //       images the model names must resolve under importRoot, else the import fails. settings: an object of
    //       importer options (scale, upAxis, normals, textures, clipSampleRate, stripScaleTracks, usesLayers,
    //       usesIk) written to the source's sidecar <path>.import.json over its current ones when the import is
    //       queued (settings equal to the defaults delete it). An import that is refused, fails or is cancelled puts
    //       the sidecar back byte for byte (unless something else changed it since); undoing a finished import keeps
    //       it. The bake uses the options as queued.
    //   importExternalModel path [name] [settings] [wait] -> like importModel, with an absolute glTF/GLB path;
    //       dependencies must stay under its source directory. The imports row exposes the absolute source.
    //   importExternalImage path -> import id; externalImageImport import polls the bake, publishExternalImage
    //       import publishes a ready result.
    //       HDR files become environments; supported SDR images become textures. Existing names are refused.
    //   importExternalAudio path -> import id; externalAudioImport import polls a WAV, MP3 or FLAC copy into
    //       assets/audio. The audio runtime validates decoding when the clip is bound; existing files are refused.
    //   prepareModelImport path [name, settings] -> preparation, status (asynchronous private glTF bake)
    //   prepareModelUpdate model [settings] -> preparation, status (asynchronous private glTF re-bake)
    //   modelPreparation preparation     -> status, model, staged entries, fingerprints and imported instance roots
    //   previewModelPreparation preparation id model -> scene changes/conflicts against the staged hierarchy
    //   validateModelPreparationResolutions preparation id model sceneRevision assetRevision [resolutions]
    //                                   -> planned command count, or a conflict/stale/unsupported error; no scene edit
    //   prepareModelScene preparation model sceneRevision assetRevision instances -> preparing
    //                                   instances contains an id and resolutions array for every imported instance;
    //                                   prepares an isolated scene copy asynchronously (runtime modules must be absent)
    //   modelScenePreparation preparation -> notStarted, preparing, failed, cancelled, stale or ready.
    //       Ready returns scene JSON; final publication must recheck all inputs.
    //
    //   cancelModelPreparation preparation -> status; a running private bake is discarded when it settles
    //       Preparation never publishes or writes the import sidecar. Existing applyImportedInstance applies only
    //       an already-published model, not a staged preparation.
    //   importSettings [path|model]      -> options: [{name, type number|enum|bool, doc, default, min, max | choices}],
    //                                       and with a source: path, sidecar, sidecarExists, settings; with a model
    //                                       also bakedWith (the settings its bake used) and needsReimport
    //       the importer options with defaults, ranges and docs; path is a model file under importRoot, model an
    //       imported model (its recorded source); a sidecar that does not parse is an error naming the file.
    //       needsReimport: the sidecar's settings differ from bakedWith (the manifest entry records them)
    //   setImportSettings name settings -> settings, previous, needsReimport     inverse: setImportSettings previous
    //       writes the importer options over the sidecar's to the model's source sidecar without baking (a view
    //       command, undoable); the model then needs a reimport. Refused like reimportModel for an unknown
    //       model, one whose source is not a file under importRoot or is gone, and for an invalid option
    //   reimportModel name [settings] [wait] -> the importModel reply, reimport: true, settings, previous
    //                                                                           inverse: reimportModel bakedWith
    //       bakes the model's recorded source again and replaces its bake and manifest entry whole or not at
    //       all (a refused or failed one leaves the sidecar and the model as they were). Loaded submeshes take
    //       the new geometry in place within a few frames: the instances bound to them (disabled ones too) get the
    //       new bounds and meshlets without a release in between; their node transforms stay as they are
    //       (instantiate again for a changed hierarchy). A changed material or texture is baked under a new name
    //       (a loaded one is never rewritten, the reply's materials and textures name the new ones): a loaded
    //       instance still on its submesh's old material moves to the new one (scenes on disk keep the old name),
    //       one given another material keeps it. A pending
    //       reimport of the model is cancelled in its favour (so undoing a pending reimport works). The inverse bakes
    //       with the settings the model has (or its pending reimport would have): undo restores the bake, and the
    //       sidecar follows it. Refused for an unknown model and one whose source is not a file under importRoot or is
    //       gone. The reply's materials and textures also list what the model's earlier imports added.
    //   importEnvironment path [name] [format] -> name, baked, source, format, width, height, mips, bytes
    //       bakes an HDR environment (asset/EnvironmentImport.hpp): path under importRoot is an equirect image (2:1;
    //       a Radiance .hdr keeps its range), a cube cross image (4:3 or 3:4) or a folder holding the six faces px,
    //       nx, py, ny, pz, nz; every layout bakes to the same equirect asset with a full mip chain, in format
    //       rgba16f (default), rgb9e5 or r11g11b10. name: default the file's stem. Bakes before replying and
    //       replaces an environment of the same name (a loaded copy stays until released); not undoable. loadAsset
    //       type=environment creates its GPU texture through the asset factory (a sampled 2D texture with every mip)
    //   imports [import]                 -> imports: [{import, name, path, status, error?, materials,
    //                                       textures, bakeMs}]  status: queued|baking|done|failed|cancelled;
    //                                       materials/textures: what the import added to the manifest
    //   removeModel name                 -> name, path, recoveryId, cancelled, materials, textures
    //       Removes the baked model and rig, saving their files and records for undo. Materials and textures remain.
    //       Source availability and location do not restrict baked model removal. Pending imports are cancelled;
    //       their inverse reimports the source under importRoot.
    //   restoreDeletedModel id           -> id, name, source, restored
    //       Restores saved baked files and records without the original source or a bake; inverse: removeModel name.
    //   deletedModels                    -> deletedModels: [{id, name, source, restored}]
    //       Lists durable recovery records; snapshots are not automatically purged.
    // Material edits are global (every world sees them) and are recorded in the
    // request's world history; a loaded material takes an edit in place, one
    // material-slot upload with no rebind.
    SPITE_ENGINE_TOOLS_API void registerAssetCommands(ControlServer& control, AssetRuntime& assets,
                                                      std::filesystem::path importRoot);
    SPITE_ENGINE_TOOLS_API void registerAssetCommands(ControlServer& control, AssetRuntime& assets,
                                                      std::filesystem::path importRoot, SceneRecoveryAccess* recovery,
                                                      std::function<std::shared_ptr<void>()> acquireSceneCopyLease);
} // namespace spite
