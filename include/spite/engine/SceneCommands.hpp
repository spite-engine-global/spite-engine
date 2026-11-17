#pragma once

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "base/Expected.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include "engine/EngineToolsExport.hpp"

namespace spite
{
    class World;
    class ControlServer;
    class EntityTransformManager;
    struct ApplyResult;
    struct SceneDocument;

    // The scene layer a root of the edit world belongs to, carried by roots only: an untagged root, and
    // every entity under a root, belong to its root's layer, 0 being the open scene. Observed, not persisted: saves
    // and the Play clone never copy it. Read-only to tools: set, addComponent and removeComponent are refused; a root
    // changes layer only through reparent layer=.
    struct SceneLayerComponent : IEngineComponent
    {
        u32 layer = 0;
        SPITE_OBSERVED_SCHEMA(SceneLayerComponent, SPITE_FIELD(layer, OBSERVED | READONLY))
    };

    // Called after loadScene, openScene, instantiate or loadSceneLayer applied a document into world, and after
    // newScene emptied it (an empty result).
    using SceneLoadedFn = std::function<void(World& world, const ApplyResult& loaded)>;

    // Frame-thread capture and startup-only restore of the edit scene and its layer metadata.
    struct SceneRecoveryAccess
    {
        std::function<Expected<std::string>()> capture;
        std::function<Expected<void>(std::string_view)> validate;
        std::function<Expected<void>()> ready;
        std::function<Expected<void>(std::string_view)> restore;
        // On success, returns the captured entity IDs mapped to their new IDs. Output is unchanged on refusal.
        std::function<Expected<void>(std::string_view, std::unordered_map<u64, u64>&)> restoreWithMapping;
        std::function<void()> clearRestored;
    };

    // Registers the scene editing commands (owner "engine") on control. Each acts
    // on the request's world through the EntityTransformManager in that world's
    // services (the scene and play worlds), refuses UI elements (runtime state
    // their UiContext owns) and records its inverse for undo:
    //   createEntity [name] [parent]           -> id               inverse: deleteEntity id
    //   deleteEntity id                        -> parent, deleted  inverse: instantiate the subtree, collected first
    //   duplicateEntity id                     -> id, entities     inverse: deleteEntity id
    //   reparent id [parent] (none = root)     -> previous         inverse: reparent id previous
    //   setEntityEnabled id enabled            -> enabled, previous, active (DisabledComponent, the entity's
    //                                                              own flag; active = it and every ancestor enabled)
    //                                                              inverse: setEntityEnabled id previous; an unchanged
    //                                                              value records no step and leaves the layer clean
    //   instantiate path|document [parent]          -> roots, entities, skippedComponents, changes
    //                                                              inverse: deleteEntity per root
    //   saveScene [id path overwrite] (id: independent snapshot of this entity and descendants;
    //                              never adopts its file or clears an enclosing scene's dirty state;
    //                              a file backing the open scene or any loaded layer is refused)
    //   saveScene [path] (JSON; .spsd: same-build binary cache)    no world change, not undoable;
    //                                                   only the "scene" world (play and UI worlds are refused).
    //                                                   No path: the open scene's file (untitled: refused). A path
    //                                                   naming the open scene's file saves it; any other path writes
    //                                                   a copy (overwriting) and leaves the open scene as it is
    //   saveSceneAs path [overwrite]                    writes the file, which becomes the open scene's (clean);
    //                                                   an existing file other than the open scene's needs overwrite
    //   openScene path [force]                          replaces the scene with the file, which becomes the open scene
    //                                                   (clean, undo history ended, logged for replay); refused while
    //                                                   Play is active, and while dirty without force
    //   newScene [force]                                empties the scene into an untitled open scene, as openScene
    //   sceneInfo (query)                               path (empty: untitled), name, dirty, world, entityCount (the
    //                                                   open scene's), worldEntityCount (layers included), playing,
    //                                                   lastSavedMs?
    //   loadScene path|document (replaces the scene) -> deleted, entities, skippedComponents, changes
    //                                                   inverse: loadScene document=<the scene, collected first>
    //                                                   an undoable edit: dirty, and the open path stays; refused
    //                                                   while scene layers are loaded
    // Scene layers, the scene world only: saved scenes loaded beside the open scene (layer 0).
    //   sceneLayers (query)                          -> active, layers[{id, path, active, dirty, entityCount,
    //                                                   lastSavedMs?}], unsavedUnloads[path]
    //   loadSceneLayer path                          -> layer, entities, skippedComponents, changes
    //                                                   inverse: unloadSceneLayer layer; redo re-applies the
    //                                                   document loaded then, not the file
    //   unloadSceneLayer layer [force]               -> deleted, wasDirty; refused while dirty without force (also
    //                                                   in a replay of an old log)
    //                                                   inverse: loadSceneLayer of the collected layer, same id
    //   restoreUnsavedLayer path                     -> restore an unloaded unsaved layer, undoable without its file;
    //                                                   incoming references require the scene to be unchanged since
    //                                                   recovery
    //   setActiveSaveTarget layer                    -> previous; saveScene and saveSceneAs write the active layer
    // New roots join the active layer or the given one (createEntity, instantiate [layer]; redo keeps the layer
    // resolved first); duplicates and reparents to the root keep their layer (reparent [layer]); a layer arg beside a
    // parent is refused. openScene and newScene end every layer: a dirty layer, or one unloaded dirty and not saved
    // since, needs force. References out of a layer (to the open scene or another layer) are not kept: a layer's file
    // cannot name them, and its re-load (undo of an unload) leaves them undefined.
    // A layer, the open scene (layer 0) included, is dirty while the scene world's undo history is not where its last
    // save, load or open left it for that layer (ControlServer::layerDirty): each undo step records the layers it
    // touched, so an edit to one layer leaves the others clean and undoing back to a save is clean again.
    // unloadSceneLayer refuses a dirty layer, and openScene and newScene a dirty layer or open scene, without
    // force=true. Edits that bypass the commands and the undoable view commands (camera binding, pass toggles, asset
    // and UI document edits) are not seen, nor are the earlier commands of a batch by a later command of it. Paths
    // naming one file (A.json, a.json, ./a.json) are one open scene.
    // changes lists what loading met against the current schemas, one entry per
    // component, field and kind: {component, field?, change: defaulted|dropped|
    // unreadable|skipped, count, detail?}. A binary file whose component layouts
    // changed since the save is refused.
    // A document is a scene document as saveScene writes JSON. Re-applied
    // entities keep their scene data, names included, but not references from
    // outside them or view-camera bindings. Scene paths are relative to sceneRoot;
    // absolute paths and ".." are refused.
    // Also installs control's component edit guard: addComponent/removeComponent
    // refuse UI elements and UI components, removing the TransformComponent
    // of an entity with a parent or children, and InactiveComponent and
    // DisabledComponent (setEntityEnabled writes the flag).
    SPITE_ENGINE_TOOLS_API void registerSceneCommands(ControlServer& control, std::filesystem::path sceneRoot,
                                                      SceneLoadedFn sceneLoaded = {},
                                                      SceneRecoveryAccess* recovery = nullptr);
    // Private scene copies reuse schemas already registered by the host and must not mutate the global registry.
    SPITE_ENGINE_TOOLS_API void registerSceneCommandsForCopy(ControlServer& control, std::filesystem::path sceneRoot,
                                                             SceneRecoveryAccess* recovery);

    // The world's scene: every root (an entity without a transform parent) and
    // its subtree. UI elements are runtime state built by code, never scene
    // content, so they stay out.
    SPITE_ENGINE_TOOLS_API Expected<SceneDocument> collectWorldScene(World& world, EntityTransformManager& transforms);
    // True if a scene component outside these entities refers to one of them.
    SPITE_ENGINE_TOOLS_API bool hasExternalSceneReferences(const World& world, std::span<const Entity> entities);
} // namespace spite
