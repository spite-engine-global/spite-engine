#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace spite
{
    class Engine;
    class EditorDialogs;
    class EditorFeedback;
    class Widget;
    struct DockSpace;

    // The editor's project panel: the manifest's models, materials, textures and sprite atlases, the scene
    // files of the scene root and the UI documents of the UI worlds, plus imports not yet in the manifest (queued,
    // baking, failed), as a virtualized table (Name, Type with its icon glyph, State, Refs, Size KB, Path). State is
    // the asset's load status (unloaded, loading, ready, failed) and Refs its reference count, read again when a
    // command ran or a load settled (writing only the cells that changed); a scene is "file", a UI document "open".
    // Enter on a scene row opens it as the File menu's Open Recent does (Save / Discard / Cancel first when the open
    // scene has unsaved changes).
    // Type buttons and a name filter (case-insensitive substring) send `projectFilter` (type
    // all|model|material|texture|atlas|scene|ui, text); a chip beside the path field names the filters that are on and
    // clears them; header sorts and resizes send
    // `projectTable` (a UiTableOp: kind sort|resize, column, descending,
    // width). The path field and Import button (or Enter) send importModel
    // with the path relative to the import root. Dragging rows out starts a
    // UiDragDrop drag of the baked assets of the first row's type (payload
    // asset.model|material|texture, text the names, newline-separated; the
    // table is the drag source, so `uiDrag from=project.table` drags its
    // selection); releasing a model drag over a hierarchy row
    // sends instantiateModel with that entity as parent, over the hierarchy's
    // background without one (a batch, one undo step, for several; at the origin) and over the shown scene view at
    // the `dropPoint` under the pointer (the surface or ground there, snapped by the gizmo's step; EditorSceneView
    // shows its ghost while dragging), into the world the hierarchy shows, and selects the created roots. A refusal
    // (a model removed during the drag, a pointer off the ground) is a toast and the message line. Inspector asset
    // fields take the others. Filter and table ops are view state: outside undo, as dock ops are.
    //
    // The details pane (the document's slot `project.details`) shows beside the table while exactly one row
    // is selected: the asset's facts first (a model's meshlet, vertex and material
    // counts, bounds and submeshes; a material's factor, textures and users; a texture's size, format and users; an
    // atlas's sprites), then the row's own name, type, state, references and path, and a texture's image once it is
    // loaded (a 3D model or material preview is deferred). The facts are read when the selection or the manifest
    // moved, the state and references when a command ran or a load settled; `projectSelect` (type, name) selects a
    // row and `projectView` reports the pane as `details` (a model's submeshes: the first 32). Remove is disabled
    // where its command would refuse (`removeBlocked`: a type without one or a material in use) and its
    // confirmation says when the asset is loaded or used.
    //
    // A selected model's details pane has a second tab, Import settings (EditorProjectImport.hpp;
    // `projectDetailsTab` details|import): its importer options as a property form, Reset, Reimport, the model's state
    // against its bake and its newest import job. An edit is `setImportSettings` (the sidecar, no bake), Reset the
    // same with the defaults, Reimport `reimportModel`; a refusal shows under the row or in the job line. A model whose
    // sidecar differs from the settings its bake used (the manifest records them) is "Outdated" in its State cell and
    // `needsReimport` in `projectView` (rows and details.import). Sidecars are read again for the visible rows and
    // the selected model when the imports or the manifest moved (a settings write moves the imports) or the selection
    // did, not per frame and not for an unrelated command.
    //
    // A selected material's details pane has a second tab, Edit (EditorProjectMaterial.hpp;
    // `projectDetailsTab` details|import|edit): its albedo colour (the swatch opens a ColorPicker), its three texture
    // slots (a Pick list of the manifest's textures; a typed name is checked by the command), Duplicate, and who uses
    // it (the models that list it and the entities of the shown world that bind it, the first 32 by id). An edit is
    // `updateMaterial` by the material's name (a vanished one is refused under the row), the steps of one drag or
    // picker gesture one undo step (`merge`); Duplicate and the bar's New material are `createMaterial` under a free
    // name (Material, Material_2, ... and <name>_copy, <name>_copy_2, ...) and select the row; Remove is
    // `deleteMaterial` (off, with its reason, while a model or entity uses the material). `projectView`
    // details.material reports the fields, rows (value, modified, error), users, picker and texture list; the
    // descriptor is read again when the manifest moved, the users when the scene's state moved and the tab shows.
    //
    // Follows the assets without per-frame work: re-reads the manifest only
    // when shown and the command generation moved (or an import it lists
    // settled while baking: then it polls the imports each frame). Changes
    // made outside the protocol show at the next command. Query
    // `projectView` (owner editorProject): the filter, the shown rows in
    // view order, the selection and stats; details.requestedWidth, details.resolvedWidth and details.rowsWidth are
    // unscaled UI pixels. An import that settles, from any source, posts a toast (done: info, failed: error), and a
    // refused importModel an error toast; the queued and baking imports are the jobs "import" (EditorFeedback).
    class EditorProject
    {
    public:
        // `dock` holds the scene view panel; `hierarchy` is the hierarchy's tree.
        // `openScene` is the File menu's Open Recent (the unsaved-changes prompt, the toasts): a scene row opens
        // through it.
        EditorProject(Engine& engine, const DockSpace& dock, const Widget& hierarchy, EditorFeedback& feedback,
                      EditorDialogs& dialogs, std::function<void(std::string_view path)> openScene);
        ~EditorProject();
        EditorProject(const EditorProject&) = delete;
        EditorProject& operator=(const EditorProject&) = delete;

        [[nodiscard]] Widget content() const;
        // Once per frame: dispatches the queued requests, then follows the assets.
        void update();
        // What the details pane's Reveal calls with the file to show, instead of starting the file manager, and (when
        // given) the monotonic clock in milliseconds its rate limit reads. For tests.
        void setReveal(std::function<bool(const std::filesystem::path&)> reveal,
                       std::function<std::uint64_t()> nowMs = {});

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
