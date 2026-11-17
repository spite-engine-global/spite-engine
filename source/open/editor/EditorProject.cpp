#include "base/WritableLayout.hpp"
#include "EditorProject.hpp"
#include "editor/EditorPreviewJobs.hpp"
#include "EditorAssetPreviews.hpp"
#include "EditorAssetCatalog.hpp"
#include "EditorRenderedPreviews.hpp"
#include "EditorMaterialLinks.hpp"
#include "EditorSceneView.hpp"
#include "EditorHierarchy.hpp"
#include "EditorProjectImport.hpp"
#include "EditorProjectMaterial.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorDialogs.hpp"
#include "EditorFeedback.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "animation/AnimationLoader.hpp"
#include "application/ApplicationEvents.hpp"
#include "application/EventBus.hpp"
#include "application/Shell.hpp"
#include "application/Window.hpp"
#include "asset/AssetBaker.hpp"
#include "asset/AssetManifest.hpp"
#include "asset/EnvironmentAsset.hpp"
#include "asset/LoadBakedFunctions.hpp"
#include "assetruntime/AssetComponents.hpp"
#include "assetruntime/AssetRuntime.hpp"
#include "base/ContentRoot.hpp"
#include "base/io/FileSystem.hpp"
#include "base/Logging.hpp"
#include "control/ControlServer.hpp"
#include "EditorPanelUi.hpp"
#include "ecs/Chunk.hpp"
#include "ecs/Query.hpp"
#include "ecs/QueryBuilder.hpp"
#include "engine/Engine.hpp"
#include "engine/PlayWorld.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "renderruntime/RenderRuntime.hpp"
#include "renderruntime/RenderView.hpp"
#include "serialization/FormatWalk.hpp"
#include "serialization/JsonFormat.hpp"
#include "ui/UiCanvas.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiDragDrop.hpp"
#include "ui/UiEvents.hpp"
#include "ui/UiScope.hpp"
#include "uiview/UiStyleSheet.hpp"
#include "uiview/UiViewContext.hpp"
#include "world/EntityName.hpp"

namespace spite
{
    namespace
    {
        using namespace panelui;
        using ModelImport = AssetRuntime::ModelImport;

        constexpr std::string_view OWNER = "editorProject";
        // the toolbar buttons (type buttons, chip, import) size to their label; a filter text longer than
        // CHIP_TEXT_POINTS shows its start, so the "Filter:" prefix and the clear mark always show and the path field
        // keeps room (CHIP_MAX_WIDTH bounds a long locale)
        constexpr float CHIP_MAX_WIDTH = 400.0f;
        constexpr float CHIP_PADDING = 8.0f;
        constexpr sizet CHIP_TEXT_POINTS = 12;
        constexpr std::string_view ROW_CLASS = "project.row"; // the rows' class while the slot names none

        // The panel as a UI document: the document owns the containers and the bars' fields (filter, path,
        // import button, the filter chip); the code owns the slots, the type buttons and the table.
        constexpr std::string_view VIEW = "editor.project";
        constexpr std::string_view TYPES_SLOT = "project.types";
        constexpr std::string_view ROWS_SLOT = "project.rows";
        constexpr std::string_view FILTER_FIELD = "project.filter";
        constexpr std::string_view IMPORT_BUTTON = "project.importButton";
        constexpr std::string_view CHIP = "project.chip";
        constexpr std::string_view MESSAGE = "project.message";
        constexpr std::string_view QUEUE_SLOT = "project.queue";
        constexpr float QUEUE_HEIGHT = 44.0f; // the header-less list shows two jobs and scrolls
        constexpr sizet MAX_JOBS = 50; // the queue's history
        constexpr sizet REASON_POINTS = 72; // a failure reason in a cell; the toast and projectView carry it whole
        constexpr std::string_view IMPORT_KEY = "editor-project.import.failed";
        constexpr std::string_view DOCUMENT = "assets/ui/editor/project.json";
        constexpr std::string_view DETAILS_SLOT = "project.details"; // the selected asset's facts and actions
        // The table keeps its name, type and state columns (386 px) beside the pane at 1024x600 (a 724 px panel) and
        // all five before the path (578 px) from 906 px up
        // The Edit tab's colour row needs 324 px (marker, label, swatch with four channels, Reset) and the scroll's
        // padding and thumb 18 more: the slot grows to this while the tab shows

        constexpr float PREVIEW_SIZE = 64.0f;
        constexpr u64 PREVIEW_IMAGE_SLOT = 0x5350495450524500ull; // the preview image's UiView image slot
        constexpr sizet MAX_SUBMESHES = 32; // the submesh rows a model's details list (and reply) carry
        constexpr sizet VALUE_POINTS = 16; // code points a value row holds until the list has a width to measure
        constexpr float MIN_VALUE_WIDTH = 24.0f; // a narrower value cell is no measure: the fallback cuts
        constexpr u64 REVEAL_GAP_MS = 1000; // the file manager is a window of another program: not in a loop
        constexpr std::string_view LOAD_FAILED_KEY = "editor-project.load.failed";
        constexpr std::string_view REMOVE_FAILED_KEY = "editor-project.remove.failed";
        constexpr std::string_view DROP_FAILED_KEY = "editor-project.drop.failed";

        // Indexed by the type filter; 0 is every type. The manifest and the scene, UI and animation roots supply rows.
        constexpr std::array<std::string_view, 13> TYPE_NAMES{
            "all",      "model",       "material", "texture",      "atlas",      "scene", "ui",
            "skeleton", "environment", "clip",     "oceanProfile", "controller", "vfx"};
        constexpr std::array<std::string_view, TYPE_NAMES.size()> TYPE_ICONS{
            "squares-four",          "cube", "palette",   "image",          "images", "film-slate", "browser", "bone",
            "globe-hemisphere-west", "play", "wave-sine", "tree-structure", "sparkle"};
        // the type's icon: a glyph the editor font has, so no icon asset is needed
        constexpr std::array<std::string_view, TYPE_NAMES.size()> TYPE_GLYPHS{"",
                                                                              "\xE2\x96\xA0",
                                                                              "\xE2\x97\x8F",
                                                                              "\xE2\x96\xA1",
                                                                              "\xE2\x97\x8A",
                                                                              "\xE2\x96\xBA",
                                                                              "\xE2\x97\x8B",
                                                                              "\xE2\x97\x8F",
                                                                              "\xE2\x97\x8B",
                                                                              "\xE2\x96\xBA",
                                                                              "\xE2\x97\x8A",
                                                                              "\xE2\x96\xA1",
                                                                              "\xE2\x97\x8A"};
        constexpr std::array<std::string_view, 6> COLUMN_NAMES{"name", "type", "state", "refs", "size", "path"};
        constexpr std::array<std::string_view, 2> OP_NAMES{"sort", "resize"}; // indexed by UiTableOpKind
        constexpr std::array<std::string_view, 3> DETAILS_TABS{"details", "import", "edit"};
        constexpr std::string_view NEW_MATERIAL_BUTTON = "project.newMaterial";
        constexpr std::string_view NEW_MATERIAL_NAME = "Material"; // the first free of Material, Material_2, ...
        constexpr std::string_view COPY_SUFFIX = "_copy";
        constexpr sizet MAX_MATERIAL_NAME = 64; // the bake file name limit createMaterial refuses beyond
        constexpr u8 KIND_MODEL = 1;
        constexpr u8 KIND_MATERIAL = 2;
        constexpr u8 KIND_TEXTURE = 3;
        constexpr u8 KIND_ATLAS = 4;
        constexpr u8 KIND_SCENE = 5;
        constexpr u8 KIND_UI = 6;
        constexpr u8 KIND_SKELETON = 7;
        constexpr u8 KIND_ENVIRONMENT = 8;
        constexpr u8 KIND_CLIP = 9;
        constexpr u8 KIND_OCEAN_PROFILE = 10;
        constexpr u8 KIND_CONTROLLER = 11;
        constexpr u8 KIND_VFX = 12; // a Project filter and label for a prepared flipbook model

        // What the table shows: display text (localized); Item keeps the raw type and state.
        struct ProjectRow
        {
            std::string name;
            std::string type; // the type's icon and localized name
            std::string state;
            u32 refs = 0;
            u32 size = 0; // KB
            std::string path;
            SPITE_OBSERVED_SCHEMA(ProjectRow, name, type, state, refs, size, path)
        };

        struct Item
        {
            ProjectRow row;
            Entity key = Entity::undefined();
            u8 kind = 0; // index into TYPE_NAMES
            // the load status (unloaded, loading, ready, failed), an import's (queued, baking, failed: <reason>),
            // file (a scene) or open (a UI document)
            std::string token;
            std::optional<AssetHandle> handle; // the asset whose status and references the row shows
            std::string spaces; // a texture: the colour spaces holding it, "sRGB, linear"
            bool pending = false; // an import that has not landed in the manifest
            bool vfxCard = false;
            bool stale = false; // a model whose sidecar differs from the settings its bake used (needs reimport)
            u64 staleImports = ~0ull; // the imports and manifest generations stale was read at
            u64 staleManifest = ~0ull;
        };

        u8 displayKind(const Item& item) { return item.vfxCard ? KIND_VFX : item.kind; }

        bool matchesKind(const Item& item, const u8 kind)
        {
            return kind == 0 || (kind == KIND_VFX ? item.vfxCard : item.kind == kind);
        }

        struct ProjectStats
        {
            u64 reads = 0; // the manifest, imports, scenes and UI documents re-read
            u64 refreshes = 0; // the table re-filtered and refreshed (a filter, or a status or reference change)
            u64 materialReads = 0; // material descriptors read to tell which materials a texture is used by
            u64 settingsReads = 0; // model sidecars read to tell which models need a reimport
            u64 editReads = 0; // the selected material's descriptor read for the Edit tab and the Details facts
            u64 userScans = 0; // the world scanned for the entities that bind the selected material
        };

        // What the rows' status or references may have moved with: a command or a load settling.
        struct StatusSignature
        {
            u64 commands = 0;
            u32 started = 0;
            u32 completed = 0;
            u32 failed = 0;
            u32 uploads = 0; // a loaded asset is Ready once its upload settles, which no load counter sees
            bool operator==(const StatusSignature&) const = default;
        };

        char lower(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

        bool containsIgnoringCase(const std::string_view text, const std::string_view part)
        {
            return std::ranges::search(text, part, {}, lower, lower).begin() != text.end() || part.empty();
        }

        const char* importStateName(const ModelImport::State state)
        {
            switch (state)
            {
            case ModelImport::State::Queued:
                return "queued";
            case ModelImport::State::Baking:
                return "baking";
            default:
                return "failed";
            }
        }

        const char* statusToken(const AssetRuntime::AssetStatus status)
        {
            switch (status)
            {
            case AssetRuntime::AssetStatus::Loading:
                return "loading";
            case AssetRuntime::AssetStatus::Ready:
                return "ready";
            case AssetRuntime::AssetStatus::Failed:
                return "failed";
            default:
                return "unloaded";
            }
        }

        // the first `limit` code points of the text, with dots when it was cut
        std::string clipped(const std::string_view text, const sizet limit)
        {
            sizet points = 0;
            for (sizet i = 0; i < text.size(); ++i)
            {
                if ((static_cast<u8>(text[i]) & 0xC0) != 0x80 && points++ == limit)
                {
                    return std::string(text.substr(0, i)) + "...";
                }
            }
            return std::string(text);
        }

        UiRectDesc fitButtonDesc(const float height = 0.0f)
        {
            UiRectDesc desc = barDesc(Vec2{0.0f, height});
            desc.contentSize =
                UiContentSizeComponent{.fitWidth = true, .fitHeight = false, .padding = Vec2{CHIP_PADDING, 0.0f}};
            return desc;
        }

        u32 kilobytes(const u64 bytes) { return static_cast<u32>(std::min<u64>((bytes + 1023) / 1024, ~0u)); }

        UiTableColumn column(const std::string_view field, const float width, const bool activates = false)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString("editor-project.column." + std::string(field)),
                    .activates = activates};
        }
    } // namespace

    struct EditorProject::State
    {
        struct Request
        {
            std::string line;
            bool selectCreated = false; // select the entities its reply created
            const char* failedKey = nullptr; // set: a refusal is told in a toast of this key ("error" is its arg)
            std::function<void(const std::string& error)> refused; // set: called with a refusal's text
            std::function<void(const Json& result)> accepted; // set: called with an accepted request's result
        };

        State(Engine& engine, const DockSpace& dock, const Widget& hierarchy, EditorFeedback& feedback,
              EditorDialogs& dialogs, std::function<void(std::string_view)> openScene) :
            engine(&engine), control(&engine.control()), feedback(&feedback), dialogs(&dialogs),
            openScene(std::move(openScene)), scope(editorUi(engine).ui()), dock(dock), hierarchy(hierarchy),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        EditorFeedback* feedback;
        EditorDialogs* dialogs;
        std::function<void(std::string_view)> openScene;
        UiScope scope;
        DensityFollower density{scope};
        DockSpace dock;
        Widget hierarchy;
        const Localization* localization; // not owned: the engine's service
        Panel root; // the docked content: the document view's root goes in it
        std::array<Button, TYPE_NAMES.size()> types;
        TextInput filter; // the document's fields, bound by the contract
        Button importButton;
        Button chip; // the active filter: a click clears it
        Label message; // the last refused import or drop, inline; hidden while empty
        std::string messageText;
        u32 messageAfter = 0; // the newest import when the message was set
        Widget queueSlot; // the import queue's slot, shown while the session has imports
        std::unique_ptr<Table> queue; // the imports, newest first, each with its action
        std::vector<ModelImport> jobs; // what the queue rows read
        bool picking = false; // a file dialog is open
        std::string selectName; // a model to select once the filter showing it has taken effect
        std::vector<FileDropEvent> dropped;
        std::vector<u64> audioImports;
        std::vector<u64> imageImports;
        struct PendingPlacement
        {
            u32 import;
            std::string world;
            Json point;
        };
        std::vector<PendingPlacement> placements;
        TableView table;
        VirtualList grid;
        struct GridPreview
        {
            Image image;
            Canvas checker;
            Label name;
            Vec2 checkerSize{};
            u64 checkerGeneration = 0;
            std::string key;
            std::filesystem::path source;
            u8 kind = 0;
            std::string assetName;
            bool registered = false;
            bool imageDone = false;
            bool pending = false;
            u32 currentSlot = 0;
            std::filesystem::path currentPath, pendingPath, attemptedPath;
        };
        struct GridSlot
        {
            Panel root;
            std::array<Button, 8> tiles;
            std::array<Label, 8> icons;
            std::array<GridPreview, 8> previews;
            u32 row = 0;
        };
        EditorAssetPreviews previews{(writableDirectory(WritableFolder::Cache, {}, false) / "previews")};
        std::unordered_set<std::string> previewRequests;
        EditorRenderedPreviews renderedPreviews{*engine};
        std::unordered_map<std::string, std::filesystem::path> renderedResults;
        std::unordered_map<std::string, std::filesystem::path> lastReady;
        std::string renderedActive;
        u64 previewRevision = 0;
        bool previewNeedsUpdate = true;
        std::vector<u32> previewRows;
        float previewTop = -1, previewWidth = -1, previewHeight = -1, previewRowHeight = -1;
        u64 previewColors = ~u64{0};

        static u64 gridImageSlot(u32 slot, u32 col, u32 bank = 0)
        {
            return 0x5350495400000000ull + u64(bank) * 0x100000 + u64(slot) * 8 + col;
        }

        void clearGridImage(u32 slot, u32 col)
        {
            auto& preview = gridSlots[slot].previews[col];
            auto& uiView = editorUi(*engine).uiView();
            uiView.clearImage(gridImageSlot(slot, col, 0));
            uiView.clearImage(gridImageSlot(slot, col, 1));
            preview.pending = false;
            preview.currentSlot = 0;
            preview.currentPath.clear();
            preview.pendingPath.clear();
            preview.attemptedPath.clear();
            preview.registered = false;
            preview.imageDone = false;
            if (preview.image.isValid())
                preview.image.setVisible(false);
            if (preview.checker.isValid())
                preview.checker.setVisible(false);
            if (gridSlots[slot].icons[col].isValid())
                gridSlots[slot].icons[col].setVisible(true);
        }

        struct ListPreview
        {
            std::string key;
            std::filesystem::path source;
            std::string assetName;
            u8 kind = 0;
            bool registered = false;
            bool done = false;
            float aspect = 1.0f;
            bool pending = false;
            u32 currentSlot = 0;
            std::filesystem::path currentPath, pendingPath, attemptedPath;
        };
        std::vector<ListPreview> listPreviews;
        static bool imageThumbnail(u8 kind) { return kind == KIND_TEXTURE || kind == KIND_ATLAS; }
        static bool hasThumbnail(u8 kind)
        {
            return imageThumbnail(kind) || kind == KIND_MODEL || kind == KIND_MATERIAL || kind == KIND_SCENE;
        }
        static bool previewable(const Item& item)
        {
            return !item.pending && item.token != "builtin" && item.kind >= KIND_MODEL && item.kind <= KIND_UI;
        }
        std::filesystem::path thumbnailSource(const Item& item) const
        {
            if (item.kind == KIND_TEXTURE)
                return resolveContentPath(pathFromUtf8(item.row.path));
            if (item.kind == KIND_ATLAS)
            {
                const auto& manifest = engine->assets().manifest();
                if (const auto atlas = manifest.getSpriteAtlasEntry(InternedString(item.row.name)))
                    return resolveContentPath(pathFromUtf8(manifest.getTextureBakedPath(atlas->texture).toView()));
            }
            if (item.kind == KIND_SCENE)
                return control->root("scene") / pathFromUtf8(item.row.path);
            return {};
        }
        void requestRenderedThumbnail(u8 kind, std::string_view name)
        {
            const auto directory = (writableDirectory(WritableFolder::Cache, {}, false) / "previews");
            if (kind == KIND_SCENE)
                renderedPreviews.requestCachedScene(control->root("scene") / pathFromUtf8(name), directory);
            else if (kind == KIND_MODEL)
                renderedPreviews.requestCachedModel(InternedString(name), directory);
            else
                renderedPreviews.requestCachedMaterial(InternedString(name), directory);
        }
        static u64 listImageSlot(u32 slot, u32 bank = 0) { return 0x5350495500000000ull + u64(bank) * 0x100000 + slot; }

        void clearListImage(u32 slot)
        {
            auto& preview = listPreviews[slot];
            editorUi(*engine).uiView().clearImage(listImageSlot(slot, 0));
            editorUi(*engine).uiView().clearImage(listImageSlot(slot, 1));
            preview = {};
        }

        UiTableDecoration listDecoration(u32 modelRow) const
        {
            if (modelRow >= shown.size())
                return {};
            const auto& item = items[shown[modelRow]];
            if (previewable(item) && hasThumbnail(item.kind))
                for (u32 slot = 0; slot < listPreviews.size(); ++slot)
                    if (!listPreviews[slot].key.empty() && listPreviews[slot].kind == item.kind &&
                        listPreviews[slot].assetName == item.row.name && listPreviews[slot].done &&
                        listPreviews[slot].aspect > 0.0f)
                        return {listImageSlot(slot, listPreviews[slot].currentSlot), {}, listPreviews[slot].aspect};
            return {0, TYPE_ICONS[displayKind(item)], 1.0f};
        }

        void updateListPreviews()
        {
            const auto visible = table.visibleRows();
            const u32 count =
                std::min(visible.slots,
                         static_cast<u32>(shown.size()) - std::min(visible.firstRow, static_cast<u32>(shown.size())));
            if (listPreviews.size() < count)
                listPreviews.resize(count);
            std::unordered_set<std::string> active;
            bool changed = false;
            for (u32 slot = 0; slot < listPreviews.size(); ++slot)
            {
                std::string key;
                std::filesystem::path source;
                const Item* item = nullptr;
                if (slot < count)
                {
                    item = &items[shown[table.modelRow(visible.firstRow + slot)]];
                    if (previewable(*item) && hasThumbnail(item->kind))
                    {
                        key = std::to_string(item->kind) + "/" + item->row.name;
                        source = thumbnailSource(*item);
                    }
                }
                auto& preview = listPreviews[slot];
                if (preview.key != key || preview.source != source)
                {
                    clearListImage(slot);
                    preview.key = key;
                    preview.source = source;
                    changed = true;
                }
                if (key.empty())
                    continue;
                preview.kind = item->kind;
                preview.assetName = item->row.name;
                if (active.insert(key).second && imageThumbnail(preview.kind) && !previewRequests.contains(key))
                    previews.request(key, source);
            }
            for (const auto& key : previewRequests)
                if (!active.contains(key))
                    previews.cancel(key);
            previewRequests = std::move(active);
            const auto& visibleRequests = previewRequests;
            if (!renderedActive.empty())
            {
                const auto status = renderedPreviews.status();
                if (status == EditorRenderedPreviews::Status::Ready ||
                    status == EditorRenderedPreviews::Status::Missing ||
                    status == EditorRenderedPreviews::Status::Failed)
                {
                    if (visibleRequests.contains(renderedActive))
                        renderedResults[renderedActive] = status == EditorRenderedPreviews::Status::Ready
                            ? renderedPreviews.path()
                            : std::filesystem::path{};
                    renderedActive.clear();
                }
            }
            if (lastReady.size() > 256)
                std::erase_if(lastReady, [&](const auto& entry) { return !visibleRequests.contains(entry.first); });
            if (renderedResults.size() > 256)
                std::erase_if(renderedResults,
                              [&](const auto& entry) { return !visibleRequests.contains(entry.first); });
            if (renderedActive.empty())
                for (const auto& preview : listPreviews)
                {
                    if ((preview.kind != KIND_MODEL && preview.kind != KIND_MATERIAL && preview.kind != KIND_SCENE) ||
                        preview.key.empty() || renderedResults.contains(preview.key))
                        continue;
                    renderedActive = preview.key;
                    requestRenderedThumbnail(preview.kind, preview.assetName);
                    break;
                }
            auto& uiView = editorUi(*engine).uiView();
            for (u32 slot = 0; slot < listPreviews.size(); ++slot)
            {
                auto& preview = listPreviews[slot];
                if (preview.key.empty())
                    continue;
                std::filesystem::path path;
                if (imageThumbnail(preview.kind))
                {
                    const auto result = previews.status(preview.key);
                    if (result.status == EditorAssetPreviews::Status::Pending)
                        previewNeedsUpdate = true;
                    if (result.status == EditorAssetPreviews::Status::Ready)
                        path = result.path;
                }
                else if (const auto result = renderedResults.find(preview.key);
                         result != renderedResults.end() && !result->second.empty())
                    path = result->second;
                if (path.empty())
                    if (const auto old = lastReady.find(preview.key); old != lastReady.end())
                        path = old->second;
                if (!preview.pending && !path.empty() && path != preview.currentPath && path != preview.attemptedPath)
                {
                    preview.pendingPath = path;
                    preview.attemptedPath = path;
                    uiView.registerImageFile(listImageSlot(slot, 1 - preview.currentSlot), utf8Path(path));
                    preview.pending = true;
                }
                if (!preview.pending)
                    continue;
                const auto info = uiView.imageInfo(listImageSlot(slot, 1 - preview.currentSlot));
                if (!info || info->state == UiImageState::Pending)
                {
                    previewNeedsUpdate = true;
                    continue;
                }
                preview.pending = false;
                if (info->state == UiImageState::Ready && info->width && info->height)
                {
                    uiView.clearImage(listImageSlot(slot, preview.currentSlot));
                    preview.currentSlot = 1 - preview.currentSlot;
                    preview.currentPath = preview.pendingPath;
                    preview.registered = preview.done = true;
                    preview.aspect = static_cast<float>(info->width) / info->height;
                    lastReady[preview.key] = preview.currentPath;
                    changed = true;
                }
                else
                    uiView.clearImage(listImageSlot(slot, 1 - preview.currentSlot));
            }
            if (changed)
                table.refreshRowDecorations();
        }

        void clearGridPreviews()
        {
            previewNeedsUpdate = true;
            previewRows.clear();
            for (u32 slot = 0; slot < gridSlots.size(); ++slot)
                for (u32 col = 0; col < 8; ++col)
                    clearGridImage(slot, col);
            for (u32 slot = 0; slot < listPreviews.size(); ++slot)
                clearListImage(slot);
            listPreviews.clear();
            previews.clear();
            previewRequests.clear();
            renderedPreviews.cancel();
            renderedActive.clear();
            renderedResults.clear();
        }

        void invalidateRenderedPreviews()
        {
            previewNeedsUpdate = true;
            renderedPreviews.cancel();
            renderedActive.clear();
            renderedResults.clear();
            auto& uiView = editorUi(*engine).uiView();
            for (u32 slot = 0; slot < gridSlots.size(); ++slot)
                for (u32 col = 0; col < 8; ++col)
                {
                    auto& preview = gridSlots[slot].previews[col];
                    if (preview.pending)
                        uiView.clearImage(gridImageSlot(slot, col, 1 - preview.currentSlot));
                    preview.pending = false;
                    preview.attemptedPath.clear();
                }
            for (u32 slot = 0; slot < listPreviews.size(); ++slot)
            {
                auto& preview = listPreviews[slot];
                if (preview.pending)
                    uiView.clearImage(listImageSlot(slot, 1 - preview.currentSlot));
                preview.pending = false;
                preview.attemptedPath.clear();
            }
        }

        void updateGridPreviews()
        {
            if (engine->services().contains<EditorPreviewJobs>())
            {
                const u64 revision = engine->services().get<EditorPreviewJobs>().revision();
                if (revision != previewRevision)
                {
                    previewRevision = revision;
                    invalidateRenderedPreviews();
                }
            }
            if (!isShown())
            {
                if (!previewRequests.empty() || !renderedActive.empty() || !renderedResults.empty())
                    clearGridPreviews();
                renderedPreviews.update();
                return;
            }
            if (!gridMode)
            {
                const auto visible = table.visibleRows();
                const u32 count = std::min(visible.slots,
                                           static_cast<u32>(shown.size()) -
                                               std::min(visible.firstRow, static_cast<u32>(shown.size())));
                if (previewRows.size() != count)
                {
                    previewRows.resize(count, ~u32{0});
                    previewNeedsUpdate = true;
                }
                for (u32 slot = 0; slot < count; ++slot)
                {
                    const u32 row = shown[table.modelRow(visible.firstRow + slot)];
                    if (previewRows[slot] != row)
                    {
                        previewRows[slot] = row;
                        previewNeedsUpdate = true;
                    }
                }
            }
            const Vec2 size = gridMode ? grid.size() : table.size();
            const float top = gridMode ? grid.scrollOffset().y : 0.0f;
            const float rowHeight = grid.rowHeight();
            const u64 colors = scope.colorTokens().generation();
            if (previewTop != top || previewWidth != size.x || previewHeight != size.y ||
                previewRowHeight != rowHeight || previewColors != colors)
            {
                previewTop = top;
                previewWidth = size.x;
                previewHeight = size.y;
                previewRowHeight = rowHeight;
                previewColors = colors;
                previewNeedsUpdate = true;
            }
            previews.update();
            renderedPreviews.update();
            if (!previewNeedsUpdate)
                return;
            previewNeedsUpdate = false;
            if (!gridMode)
            {
                updateListPreviews();
                previewNeedsUpdate = previewNeedsUpdate || !renderedActive.empty();
                return;
            }
            std::unordered_set<std::string> active;
            const float bottom = top + grid.size().y;
            auto& uiView = editorUi(*engine).uiView();
            for (u32 slot = 0; slot < gridSlots.size(); ++slot)
            {
                auto& row = gridSlots[slot];
                const float y = static_cast<float>(row.row) * grid.rowHeight();
                const bool onScreen = y < bottom && y + grid.rowHeight() > top;
                for (u32 col = 0; col < 8; ++col)
                {
                    auto& preview = row.previews[col];
                    if (!onScreen || preview.key.empty() || !row.tiles[col].isVisible())
                    {
                        if (preview.registered || preview.pending)
                            clearGridImage(slot, col);
                        continue;
                    }
                    if (active.insert(preview.key).second && imageThumbnail(preview.kind) &&
                        !previewRequests.contains(preview.key))
                        previews.request(preview.key, preview.source);
                }
            }
            for (const auto& key : previewRequests)
                if (!active.contains(key))
                    previews.cancel(key);
            previewRequests = std::move(active);
            const auto& visibleRequests = previewRequests;
            if (!renderedActive.empty())
            {
                const auto status = renderedPreviews.status();
                if (status == EditorRenderedPreviews::Status::Ready ||
                    status == EditorRenderedPreviews::Status::Missing ||
                    status == EditorRenderedPreviews::Status::Failed)
                {
                    if (visibleRequests.contains(renderedActive))
                        renderedResults[renderedActive] = status == EditorRenderedPreviews::Status::Ready
                            ? renderedPreviews.path()
                            : std::filesystem::path{};
                    renderedActive.clear();
                }
            }
            if (lastReady.size() > 256)
                std::erase_if(lastReady, [&](const auto& entry) { return !visibleRequests.contains(entry.first); });
            if (renderedResults.size() > 256)
                std::erase_if(renderedResults,
                              [&](const auto& entry) { return !visibleRequests.contains(entry.first); });
            if (renderedActive.empty())
                for (auto& row : gridSlots)
                {
                    for (auto& preview : row.previews)
                    {
                        if ((preview.kind != KIND_MODEL && preview.kind != KIND_MATERIAL &&
                             preview.kind != KIND_SCENE) ||
                            !visibleRequests.contains(preview.key) || renderedResults.contains(preview.key))
                            continue;
                        renderedActive = preview.key;
                        requestRenderedThumbnail(preview.kind, preview.assetName);
                        break;
                    }
                    if (!renderedActive.empty())
                        break;
                }
            for (u32 slot = 0; slot < gridSlots.size(); ++slot)
            {
                auto& row = gridSlots[slot];
                const float y = static_cast<float>(row.row) * grid.rowHeight();
                if (y >= bottom || y + grid.rowHeight() <= top)
                    continue;
                for (u32 col = 0; col < 8; ++col)
                {
                    auto& preview = row.previews[col];
                    if (preview.key.empty() || !row.tiles[col].isVisible())
                        continue;
                    std::filesystem::path path;
                    if (imageThumbnail(preview.kind))
                    {
                        const auto result = previews.status(preview.key);
                        if (result.status == EditorAssetPreviews::Status::Pending)
                            previewNeedsUpdate = true;
                        if (result.status == EditorAssetPreviews::Status::Ready)
                            path = result.path;
                    }
                    else if (const auto result = renderedResults.find(preview.key);
                             result != renderedResults.end() && !result->second.empty())
                        path = result->second;
                    if (path.empty())
                        if (const auto old = lastReady.find(preview.key); old != lastReady.end())
                            path = old->second;
                    if (!preview.pending && !path.empty() && path != preview.currentPath &&
                        path != preview.attemptedPath)
                    {
                        preview.pendingPath = path;
                        preview.attemptedPath = path;
                        uiView.registerImageFile(gridImageSlot(slot, col, 1 - preview.currentSlot), utf8Path(path));
                        preview.pending = true;
                    }
                    if (preview.pending)
                    {
                        const auto pendingInfo = uiView.imageInfo(gridImageSlot(slot, col, 1 - preview.currentSlot));
                        if (!pendingInfo || pendingInfo->state == UiImageState::Pending)
                            previewNeedsUpdate = true;
                        if (pendingInfo && pendingInfo->state != UiImageState::Pending)
                        {
                            preview.pending = false;
                            if (pendingInfo->state == UiImageState::Ready && pendingInfo->width && pendingInfo->height)
                            {
                                uiView.clearImage(gridImageSlot(slot, col, preview.currentSlot));
                                preview.currentSlot = 1 - preview.currentSlot;
                                preview.currentPath = preview.pendingPath;
                                preview.registered = true;
                                preview.imageDone = false;
                                preview.image.setImageSlot(gridImageSlot(slot, col, preview.currentSlot));
                                lastReady[preview.key] = preview.currentPath;
                            }
                            else
                                uiView.clearImage(gridImageSlot(slot, col, 1 - preview.currentSlot));
                        }
                    }
                    if (!preview.registered)
                        continue;
                    const Vec2 checkerSize{std::max(1.0f, grid.size().x / static_cast<float>(gridColumns) - 16.0f),
                                           std::max(1.0f, grid.rowHeight() - 32.0f)};
                    const u64 generation = scope.colorTokens().generation();
                    if (preview.checkerSize.x != checkerSize.x || preview.checkerSize.y != checkerSize.y ||
                        preview.checkerGeneration != generation)
                    {
                        if (preview.checkerSize.x != checkerSize.x || preview.checkerSize.y != checkerSize.y)
                            preview.imageDone = false;
                        preview.checkerSize = checkerSize;
                        preview.checkerGeneration = generation;
                        preview.checker.setOffset(Vec2{8.0f, 4.0f});
                        preview.checker.setSize(checkerSize);
                        const u32 light = canvasColor(scope, "canvas.project.checker.light");
                        const u32 dark = canvasColor(scope, "canvas.project.checker.dark");
                        preview.checker.update(
                            [=](UiCanvasPainter& painter)
                            {
                                const Vec2 cell{checkerSize.x / 8.0f, checkerSize.y / 6.0f};
                                painter.rect(Vec2{}, checkerSize, light);
                                for (u32 y = 0; y < 6; ++y)
                                    for (u32 x = 0; x < 8; ++x)
                                        if ((x + y) % 2)
                                            painter.rect(Vec2{x * cell.x, y * cell.y}, cell, dark);
                            });
                    }
                    if (preview.imageDone)
                        continue;
                    const auto info = uiView.imageInfo(gridImageSlot(slot, col, preview.currentSlot));
                    if (!info || info->state == UiImageState::Pending)
                    {
                        previewNeedsUpdate = true;
                        continue;
                    }
                    preview.imageDone = true;
                    if (info->state != UiImageState::Ready || !info->width || !info->height)
                        continue;
                    const float width = grid.size().x / static_cast<float>(gridColumns);
                    const float boxWidth = std::max(1.0f, width - 16.0f);
                    const float boxHeight = std::max(1.0f, grid.rowHeight() - 32.0f);
                    const float scale = std::min(boxWidth / info->width, boxHeight / info->height);
                    const Vec2 size{info->width * scale, info->height * scale};
                    preview.image.setSize(size);
                    preview.image.setOffset(Vec2{(width - size.x) * 0.5f, (boxHeight - size.y) * 0.5f + 4.0f});
                    preview.checker.setVisible(true);
                    preview.image.setVisible(true);
                    row.icons[col].setVisible(false);
                }
            }
            previewNeedsUpdate = previewNeedsUpdate || !renderedActive.empty();
        }
        std::vector<GridSlot> gridSlots;
        u32 gridColumns = 1;
        float gridWidth = -1.0f;
        float gridTileWidth = 140.0f;
        bool gridMode = true;
        Button gridButton, listButton;

        void refreshGrid()
        {
            if (!grid.isValid())
                return;
            grid.setRowCount((static_cast<u32>(shown.size()) + gridColumns - 1) / gridColumns);
            grid.refreshRows();
        }

        void showGridMode()
        {
            clearGridPreviews();
            table.setVisible(!gridMode);
            grid.setVisible(gridMode);
            if (gridButton.isValid())
                gridButton.setAccessibleSelected(gridMode);
            if (listButton.isValid())
                listButton.setAccessibleSelected(!gridMode);
            refreshGrid();
        }

        Widget createGridRow(u32 slot)
        {
            if (slot >= gridSlots.size())
                gridSlots.resize(slot + 1);
            auto& row = gridSlots[slot];
            for (u32 col = 0; col < row.tiles.size(); ++col)
                clearGridImage(slot, col);
            row.root = scope.createPanel("project.grid.row", topLeft());
            for (u32 col = 0; col < row.tiles.size(); ++col)
            {
                Button tile = scope.createButton("project.grid.tile", "", topLeft());
                tile.setParent(row.root);
                row.tiles[col] = tile;
                tile.onKeyDown(
                    [this](u32 key, u8 modifiers, bool repeat)
                    {
                        if (key == 40 || key == 88)
                        {
                            if (!repeat)
                                table.activate();
                            return true;
                        }
                        if (key < 79 || key > 82)
                            return false;
                        const bool extend = (modifiers & 1) != 0;
                        const bool forward = key == 79 || key == 81;
                        const u32 steps = key == 81 || key == 82 ? gridColumns : 1;
                        for (u32 i = 0; i < steps; ++i)
                            table.navigate(forward ? UiTreeCommand::Down : UiTreeCommand::Up, extend);
                        for (u32 i = 0; i < shown.size(); ++i)
                        {
                            if (items[shown[table.modelRow(i)]].key == table.cursor())
                            {
                                const float y = static_cast<float>(i / gridColumns) * grid.rowHeight();
                                const float offset = grid.scrollOffset().y;
                                if (y < offset)
                                    grid.setScrollOffset(Vec2{0.0f, y});
                                else if (y + grid.rowHeight() > offset + grid.size().y)
                                    grid.setScrollOffset(Vec2{0.0f, y + grid.rowHeight() - grid.size().y});
                                break;
                            }
                        }
                        refreshGrid();
                        return true;
                    });
                tile.onPress(
                    [this, slot, col](u8 button, Vec2)
                    {
                        const u32 view = gridSlots[slot].row * gridColumns + col;
                        if (button != 1 || col >= gridColumns || view >= shown.size())
                            return;
                        openAssetMenu(items[shown[table.modelRow(view)]].key, gridSlots[slot].tiles[col]);
                    });
                auto& preview = row.previews[col];
                preview.checker = scope.createCanvas("project.grid.checker", UiCanvasDesc{.rect = topLeft()});
                preview.checker.setParent(tile);
                preview.checker.setInteractable(false);
                preview.checker.setVisible(false);
                preview.checkerSize = {};
                preview.checkerGeneration = 0;
                preview.image = scope.createImage("project.grid.thumbnail", topLeft());
                preview.image.setParent(tile);
                preview.image.setInteractable(false);
                preview.image.setImageSlot(gridImageSlot(slot, col));
                preview.image.setVisible(false);
                Label icon = scope.createLabel("project.grid.icon", "", topLeft());
                icon.setParent(tile);
                icon.setStyleClass(InternedString("project.grid.badge"));
                icon.setInteractable(false);
                row.icons[col] = icon;
                preview.name = scope.createLabel("project.grid.name", "", topLeft());
                preview.name.setParent(tile);
                preview.name.setInteractable(false);
                preview.name.setStyleClass(InternedString("project.grid.name"));
                scope.setReaction(tile.entity(), UiEventKind::Click,
                                  [this, slot, col](const void* payload)
                                  {
                                      const auto& event = *static_cast<const UiClickEvent*>(payload);
                                      const u32 view = gridSlots[slot].row * gridColumns + col;
                                      if (event.button != 0 || col >= gridColumns || view >= shown.size())
                                          return;
                                      const Entity key = items[shown[table.modelRow(view)]].key;
                                      const bool ctrl = (event.modifiers & 2) != 0, shift = (event.modifiers & 1) != 0;
                                      table.select(key,
                                                   shift ? (ctrl ? UiTreeSelect::AddRange : UiTreeSelect::Range)
                                                         : (ctrl ? UiTreeSelect::Toggle : UiTreeSelect::Replace));
                                      refreshGrid();
                                      const auto now = std::chrono::steady_clock::now();
                                      const bool twice = event.modifiers == 0 && gridLastClick == key &&
                                          now - gridLastClickTime <= std::chrono::milliseconds(400);
                                      gridLastClick = event.modifiers == 0 && !twice ? key : Entity::undefined();
                                      gridLastClickTime = now;
                                      if (twice)
                                          openPreview(key);
                                  });
                scope.setReaction(tile.entity(), UiEventKind::DragStart,
                                  [this, slot, col](const void* payload)
                                  {
                                      const u32 view = gridSlots[slot].row * gridColumns + col;
                                      if (col >= gridColumns || view >= shown.size())
                                          return;
                                      const Entity key = items[shown[table.modelRow(view)]].key;
                                      if (std::ranges::find(table.selection(), key) == table.selection().end())
                                          table.select(key);
                                      dragRows(table.selection(),
                                               static_cast<const UiDragStartEvent*>(payload)->position, false);
                                      refreshGrid();
                                  });
                scope.setReaction(
                    tile.entity(), UiEventKind::DragMove, [this](const void* payload)
                    { dragRows(table.selection(), static_cast<const UiDragMoveEvent*>(payload)->position, false); });
                scope.setReaction(
                    tile.entity(), UiEventKind::DragEnd, [this](const void* payload)
                    { dragRows(table.selection(), static_cast<const UiDragEndEvent*>(payload)->position, true); });
            }
            return row.root;
        }

        void bindGridRow(u32 slot, u32 rowIndex)
        {
            previewNeedsUpdate = true;
            auto& row = gridSlots[slot];
            row.row = rowIndex;
            const float width = grid.size().x / static_cast<float>(gridColumns);
            for (u32 col = 0; col < row.tiles.size(); ++col)
            {
                Button& tile = row.tiles[col];
                const u32 view = rowIndex * gridColumns + col;
                const bool visible = col < gridColumns && view < shown.size();
                tile.setVisible(visible);
                if (!visible)
                {
                    clearGridImage(slot, col);
                    row.previews[col].key.clear();
                    continue;
                }
                const Item& item = items[shown[table.modelRow(view)]];
                tile.setOffset(Vec2{width * col, 0.0f});
                tile.setSize(Vec2{width, grid.rowHeight()});
                tile.setText("");
                auto& preview = row.previews[col];
                const std::string key = previewable(item) && hasThumbnail(item.kind)
                    ? std::to_string(item.kind) + "/" + item.row.name
                    : std::string{};
                const auto source = !key.empty() ? thumbnailSource(item) : std::filesystem::path{};
                if (preview.key != key || preview.source != source)
                {
                    clearGridImage(slot, col);
                    preview.key = key;
                    preview.source = source;
                }
                preview.kind = item.kind;
                preview.assetName = item.row.name;
                preview.name.setText(item.row.name);
                preview.name.setOffset(Vec2{4.0f, grid.rowHeight() - 24.0f});
                preview.name.setSize(Vec2{std::max(1.0f, width - 8.0f), 20.0f});
                row.icons[col].setIcon(TYPE_ICONS[displayKind(item)]);
                row.icons[col].setOffset(Vec2{width * 0.5f - 12.0f, 8.0f});
                row.icons[col].setSize(Vec2{24.0f, 24.0f});
                tile.setAccessibleName(item.row.name + ", " + item.row.type + ", " + item.row.state);
                const bool selected = std::ranges::find(table.selection(), item.key) != table.selection().end();
                tile.setAccessibleSelected(selected);
                tile.setStyleClass(InternedString(selected ? "project.grid.tile.selected" : "project.grid.tile"));
            }
        }
        Entity gridLastClick = Entity::undefined();
        std::chrono::steady_clock::time_point gridLastClickTime{};
        bool dragging = false; // a table drag began a UiDragDrop drag

        std::vector<Item> items; // model order: by type, then name
        std::vector<u32> shown; // items passing the filter
        std::unordered_map<std::string, u32> keys; // "type/name" -> key index, stable for the session
        u8 type = 0;
        std::string text;
        bool synced = false;
        bool filterDirty = false;
        bool statusMoved = false; // a status or reference read since the flag was cleared differed
        u64 seenManifest = 0;
        u64 seenImports = 0;
        StatusSignature seenStatus;
        u64 seenLocale = 0;
        u64 seenUiViews = 0;
        std::filesystem::file_time_type seenSceneTime{};
        u64 seenControllers = 0;
        std::array<std::string, TYPE_NAMES.size()> typeTitles; // the type buttons' text
        std::string chipLabel;
        // What the type buttons' widths were sized with (see typeKeyNow)
        struct TypeKey
        {
            u64 sheet = ~0ull;
            u64 tokens = 0;
            u64 locale = 0;
            float scale = 0.0f;
            float padding = 0.0f;
            bool operator==(const TypeKey&) const = default;
        };
        TypeKey typeKey;
        bool chipStale = true;
        u64 reportedGeneration = ~0ull; // the imports generation the jobs and toasts last followed
        std::unordered_set<u32> reportedImports; // the settled imports a toast told
        std::string pathText; // what the path field shows: a document apply writes the fields as authored
        std::vector<Request> pending;
        ProjectStats stats;

        // the details pane: the selected row's facts, read when the selection, its status or the manifest
        // moved, and written to the widgets only when they differ
        struct Fact
        {
            std::string id; // the localized key "editor-project.details.<id>"; empty: the previous fact continues
            std::string value;
            bool operator==(const Fact&) const = default;
        };
        struct Actions
        {
            bool load = false;
            bool remove = false;
            bool reveal = false;
            bool operator==(const Actions&) const = default;
        };
        Widget detailsSlot;
        Widget rowsSlot;
        Panel body;
        Panel previewFrame;
        Image preview;
        std::unique_ptr<Table> facts;
        std::unique_ptr<EditorMaterialLinks> materialLinks;
        std::vector<std::string> materialNames;
        // the details pane's tabs: Details, a model's Import settings and a material's Edit,
        // each with the pane and what it was last given
        Panel tabs;
        Button tabDetails;
        Button tabImport;
        Button tabEdit;
        std::unique_ptr<ProjectImportView> importView;
        ProjectImportModel importModel;
        u64 importReadAt = ~0ull; // the imports generation importModel was read at
        bool importTab = false; // the tab asked for; shown only while a model is selected
        bool importDirty = true; // importModel moved since the pane was written
        std::unique_ptr<ProjectMaterialView> materialView;
        ProjectMaterialModel materialModel;
        u64 materialUsersAt = ~0ull; // the scene state generation the entity users were scanned at
        bool editTab = false; // the tab asked for; shown only while a material is selected
        DockSpace innerDock;
        float innerSplitterSize = 0.0f;
        float browserRatio = 0.65f;
        bool materialDirty = true; // materialModel moved since the pane was written
        Button newMaterialButton;
        PopupList createPopup;
        PopupList assetPopup;
        std::vector<int> assetMenuActions;
        Entity assetMenuKey = Entity::undefined();
        Entity detailsKey = Entity::undefined(); // the one selected row the pane follows
        bool detailsDirty = true; // the status, the manifest or the locale moved since the pane was read
        bool detailsView = false; // the model changed: the widgets are written
        std::string detailsName;
        std::string detailsType; // the raw type name
        std::vector<Fact> detailFacts; // the file's facts, then the live ones
        std::vector<Fact> detailLive;
        std::vector<Fact> fileFacts; // read from the asset's files for fileKey at fileGeneration
        Entity fileKey = Entity::undefined();
        u64 fileGeneration = 0;
        // the materials that name each texture, read from the descriptors once per manifest generation: a texture's
        // Used by is a lookup, not a scan of every material on each selection
        std::unordered_map<InternedString, std::vector<std::string>> textureUsers;
        u64 usersGeneration = ~0ull;
        Vec2 previewPixels{}; // the texture's size in pixels
        std::vector<Fact> rows; // what the list shows: values cut to the value cell's width
        float cutWidth = 0.0f; // that width when they were cut
        Actions detailActions;
        std::string previewName; // the texture the preview image should show; empty: hidden
        std::string shownPreview; // the texture registered on the image
        std::string loadType; // the selected asset's loadAsset type and name
        std::string loadName;
        std::filesystem::path revealTarget;
        // why Remove is disabled (the button's tooltip and projectView's removeBlocked): a type with no remove command,
        // or a material that a model or an entity uses (deleteMaterial refuses)
        std::string removeBlocked;
        bool removeLoaded = false; // the asset is loaded (or loading): the confirmation says so
        u32 removeUses = 0; // the entities bound to it
        bool fileSourceOutside = false; // the model's source is not under the import root
        u32 fileModelUsers = 0; // the models that list the material
        u32 fileSubmeshes = 0; // the model's submeshes, of which MAX_SUBMESHES are listed
        std::function<bool(const std::filesystem::path&)> revealFile = revealInFileManager;
        std::function<u64()> revealClock = []
        {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
        };
        u64 lastRevealMs = 0;
        bool hasRevealed = false;
        u8 selectKind = 0; // the type of selectName

        ApplicationEventBus::Subscription drops;
        std::unique_ptr<UiDocumentView> view; // last: destroyed first

        [[nodiscard]] std::string_view worldName() const { return engine->playWorld() ? "play" : "scene"; }
        [[nodiscard]] World& world() const
        {
            PlayWorld* play = engine->playWorld();
            return play ? play->world() : engine->world();
        }

        // the table's resolved size is the slot's: the document places it
        [[nodiscard]] bool isShown() const { return panelui::shown(scope, root); }

        // -- model -------------------------------------------------------------------

        [[nodiscard]] std::string tr(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            if (!localization)
            {
                return std::string(key);
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(key), args, scratch));
        }

        void loadTexts()
        {
            for (u32 i = 0; i < typeTitles.size(); ++i)
            {
                typeTitles[i] = tr("editor-project.filter." + std::string(TYPE_NAMES[i]));
            }
            seenLocale = localization ? localization->revision() : 0;
        }

        [[nodiscard]] std::string typeText(const u8 kind) const
        {
            return std::string(TYPE_GLYPHS[kind]) + " " + tr("editor-project.type." + std::string(TYPE_NAMES[kind]));
        }

        // a failed import's reason as a cell shows it: paths relative to the import root, and cut when long
        [[nodiscard]] std::string reasonText(std::string reason) const
        {
            const std::string root = utf8Path(control->root("import"));
            std::string general = root;
            std::ranges::replace(general, '\\', '/');
            for (const std::string& base : {root, general})
            {
                for (const char separator : {'\\', '/'})
                {
                    const std::string prefix = base + separator;
                    for (sizet at = reason.find(prefix); !base.empty() && at != std::string::npos;
                         at = reason.find(prefix, at))
                    {
                        reason.erase(at, prefix.size());
                    }
                }
            }
            return clipped(reason, REASON_POINTS);
        }

        // a token's text: its localized head, then what follows its colon (a failed import's reason)
        [[nodiscard]] std::string stateText(const std::string& token) const
        {
            const sizet colon = token.find(':');
            return tr("editor-project.state." + token.substr(0, colon)) +
                (colon == std::string::npos ? std::string{}
                                            : ": " + reasonText(token.substr(std::min(colon + 2, token.size()))));
        }

        Entity keyOf(const std::string_view kind, const std::string_view name)
        {
            const auto [it, added] =
                keys.try_emplace(std::string(kind) + "/" + std::string(name), static_cast<u32>(keys.size() + 1));
            return Entity(it->second, 1u);
        }

        // the State cell: the load status, and for a model whose settings changed since its bake "Outdated"
        [[nodiscard]] std::string rowState(const Item& item) const
        {
            return stateText(item.token) +
                (item.stale ? " \xC2\xB7 " + tr("editor-project.state.stale") : std::string{});
        }

        Item& add(const std::string_view name, const u8 kind, std::string token, std::string path = {},
                  const u64 bytes = 0)
        {
            Item& item = items.emplace_back();
            item.row.name = std::string(name);
            item.row.type = typeText(kind);
            item.row.state = stateText(token);
            item.row.size = kilobytes(bytes);
            item.row.path = std::move(path);
            item.key = keyOf(TYPE_NAMES[kind], name);
            item.kind = kind;
            item.token = std::move(token);
            return item;
        }

        // an asset's load status and references; a texture's are over its colour spaces (a normal map is loaded
        // only in the linear one), with the spaces that hold it
        struct Live
        {
            AssetRuntime::AssetStatus status;
            u32 refs;
            std::string spaces;
        };

        [[nodiscard]] static Live liveOf(const AssetRuntime& assets, const AssetHandle handle)
        {
            if (handle.assetType != AssetType::TEXTURE)
            {
                return {assets.status(handle), assets.registry().getRefCount(handle), {}};
            }
            const AssetRuntime::TextureState state = assets.textureState(handle.name);
            std::string spaces;
            constexpr std::array<std::string_view, 2> SPACE_NAMES{"sRGB", "linear"};
            for (sizet i = 0; i < SPACE_NAMES.size(); ++i)
            {
                if (state.spaces[i] != AssetRuntime::AssetStatus::Unloaded)
                {
                    spaces += (spaces.empty() ? "" : ", ") + std::string(SPACE_NAMES[i]);
                }
            }
            return {state.status, state.refs, std::move(spaces)};
        }

        // an asset of the manifest: the load status and references of its handle
        Item& addAsset(const AssetRuntime& assets, const InternedString name, const u8 kind, const AssetHandle handle,
                       const std::string_view baked, const u64 bytes)
        {
            const Live live = liveOf(assets, handle);
            Item& item = add(name.toView(), kind, statusToken(live.status), std::string(baked), bytes);
            item.handle = handle;
            item.row.refs = live.refs;
            item.spaces = live.spaces;
            return item;
        }

        [[nodiscard]] std::filesystem::path sceneRoot() const { return control->root("scene"); }

        [[nodiscard]] u64 controllerRevision() const
        {
            const auto& services = engine->services();
            if (!services.contains<AnimationAssetLoader>())
                return 0;
            const auto loader = services.read<AnimationAssetLoader>();
            return loader->controllerVersion ? loader->controllerVersion() : 0;
        }

        void readControllers()
        {
            const std::filesystem::path root = control->root("animation");
            if (root.empty())
                return;
            std::error_code error;
            for (std::filesystem::recursive_directory_iterator
                     file(root, std::filesystem::directory_options::skip_permission_denied, error),
                 end;
                 !error && file != end; file.increment(error))
            {
                std::error_code fileError;
                if (!file->is_regular_file(fileError) || file->path().extension() != ".json")
                    continue;
                std::string name = utf8Path(file->path().lexically_relative(root));
                std::ranges::replace(name, '\\', '/');
                const auto bytes = file->file_size(fileError);
                add(name, KIND_CONTROLLER, "file", name, fileError ? 0 : bytes);
            }
        }

        [[nodiscard]] std::filesystem::file_time_type sceneStamp() const
        {
            std::error_code error;
            const std::filesystem::path directory = sceneRoot();
            const auto stamp = directory.empty() ? std::filesystem::file_time_type::min()
                                                 : std::filesystem::last_write_time(directory, error);
            return error ? std::filesystem::file_time_type::min() : stamp;
        }

        // the document views of the editor's and the scene's UI that were read from a file
        template <class Fn>
        void eachDocument(Fn&& fn) const
        {
            for (UiContext* ui : {&editorUi(*engine).ui(), &engine->sceneUi()})
            {
                for (const UiDocumentView* document : ui->documents().views())
                {
                    if (document->name() != "__projectUiPreview" && !document->path().empty() &&
                        (!document->adopted() || document->rootAlive()))
                    {
                        fn(*document);
                    }
                }
            }
        }

        void readModel()
        {
            invalidateRenderedPreviews();
            ++stats.reads;
            items.clear();
            const AssetRuntime& assets = engine->assets();
            const AssetManifest& manifest = assets.manifest();
            for (const auto& [name, entry] : manifest.getModels())
            {
                Item& item = addAsset(assets, name, KIND_MODEL, {name, AssetType::MESH, 0}, entry.baked.toView(),
                                      entry.gpuCosts.gpuBytes());
                item.vfxCard = entry.source.toView().ends_with(".flipbook.glb") ||
                    entry.authoringSource.toView().ends_with(".flipbook.glb");
                if (item.vfxCard)
                    item.row.type = typeText(KIND_VFX);
            }
            for (const auto& [name, entry] : manifest.getMaterials())
            {
                if (editorAssetVisible(AssetType::MATERIAL, name.toView()))
                {
                    addAsset(assets, name, KIND_MATERIAL, {name, AssetType::MATERIAL, 0}, entry.baked.toView(), 0);
                }
                else
                {
                    add(name.toView(), KIND_MATERIAL, "builtin", std::string(entry.baked.toView()));
                }
            }
            for (const auto& [name, entry] : manifest.getSkeletons())
                add(name.toView(), KIND_SKELETON, "file", std::string(entry.baked.toView()));
            for (const auto& [name, entry] : manifest.getAnimations())
                add(name.toView(), KIND_CLIP, "file", std::string(entry.baked.toView()));
            for (const auto& [name, entry] : manifest.getEnvironments())
                addAsset(assets, name, KIND_ENVIRONMENT, {name, AssetType::ENVIRONMENT, 0}, entry.baked.toView(),
                         entry.pixelBytes);
            for (const auto& [name, entry] : manifest.getOceanProfiles())
                addAsset(assets, name, KIND_OCEAN_PROFILE, {name, AssetType::OCEAN_PROFILE, 0}, entry.baked.toView(),
                         0);
            readControllers();
            for (const auto& [name, entry] : manifest.getTextures())
            {
                addAsset(assets, name, KIND_TEXTURE, {name, AssetType::TEXTURE, 0}, entry.baked.toView(),
                         entry.fileBytes ? entry.fileBytes : entry.pixelBytes + sizeof(SpiteTextureHeader));
            }
            for (const auto& [name, entry] : manifest.getSpriteAtlases())
            {
                // the page is a texture of the manifest: its status and references are the atlas's
                const auto page = manifest.getTextureEntry(entry.texture);
                const u64 fileBytes = page && page->fileBytes
                    ? page->fileBytes
                    : u64(entry.pageWidth) * entry.pageHeight * 4 + sizeof(SpiteTextureHeader);
                addAsset(assets, name, KIND_ATLAS, {entry.texture, AssetType::TEXTURE, 0}, entry.baked.toView(),
                         fileBytes);
            }
            // each name's latest import, while it has not landed in the manifest
            const std::vector<ModelImport> imports = assets.imports();
            for (auto it = imports.rbegin(); it != imports.rend(); ++it)
            {
                const bool latest = std::find_if(imports.rbegin(), it, [&it](const ModelImport& later)
                                                 { return later.name == it->name; }) == it;
                if (!latest || it->state == ModelImport::State::Done || it->state == ModelImport::State::Cancelled ||
                    manifest.hasModel(it->name))
                {
                    continue;
                }
                add(it->name.toView(), KIND_MODEL,
                    it->error.empty() ? std::string(importStateName(it->state))
                                      : std::string(importStateName(it->state)) + ": " + it->error)
                    .pending = true;
            }
            std::unordered_set<std::string> existing;
            for (const auto& item : items)
                if (!item.pending)
                    existing.insert(std::to_string(item.kind) + "/" + item.row.name);
            std::erase_if(lastReady, [&](const auto& entry) { return !existing.contains(entry.first); });
            seenSceneTime = sceneStamp();
            const std::filesystem::path scenes = sceneRoot();
            std::error_code error;
            for (std::filesystem::directory_iterator file(scenes, error), end; !error && file != end;
                 file.increment(error))
            {
                const std::filesystem::path& entry = file->path();
                std::error_code fileError;
                if (!file->is_regular_file(fileError) || (entry.extension() != ".json" && entry.extension() != ".spsd"))
                {
                    continue;
                }
                const std::string name = utf8Path(entry.filename());
                add(name, KIND_SCENE, "file", name, file->file_size(fileError));
            }
            seenUiViews = 0;
            std::unordered_set<std::string> documents;
            eachDocument(
                [&](const UiDocumentView& document)
                {
                    ++seenUiViews;
                    std::string file = utf8Path(document.path());
                    std::ranges::replace(file, '\\', '/');
                    if (const sizet at = file.rfind("assets/"); at != std::string::npos)
                    {
                        file.erase(0, at);
                    }
                    if (documents.insert(document.name()).second)
                    {
                        add(document.name(), KIND_UI, "open", std::move(file));
                    }
                });
            std::ranges::sort(items, [](const Item& a, const Item& b)
                              { return a.kind != b.kind ? a.kind < b.kind : a.row.name < b.row.name; });
            filterDirty = true;
            detailsDirty = true;
        }

        [[nodiscard]] StatusSignature signature() const
        {
            const auto loads = engine->assets().stats();
            return {control->stateGeneration(), loads.loadsStarted, loads.loadsCompleted, loads.loadsFailed,
                    loads.uploadsSettled};
        }

        // A row's load status and references, read when the table binds (or sorts) it: a refresh reads the visible
        // rows only, never every item.
        void readStatus(Item& item)
        {
            if (!item.handle)
            {
                return;
            }
            const AssetRuntime& assets = engine->assets();
            bool stateMoved = false;
            Live live = liveOf(assets, *item.handle);
            if (const char* token = statusToken(live.status); item.token != token)
            {
                item.token = token;
                stateMoved = true;
            }
            if (item.spaces != live.spaces)
            {
                item.spaces = std::move(live.spaces);
                statusMoved = true;
            }
            // a model's sidecar is read again when the imports or the manifest moved (a settings write moves the
            // imports), not for an unrelated command or a bind
            if (item.kind == KIND_MODEL)
            {
                const u64 imports = assets.importsGeneration();
                const u64 manifest = assets.manifest().generation();
                if (item.staleImports != imports || item.staleManifest != manifest)
                {
                    item.staleImports = imports;
                    item.staleManifest = manifest;
                    const bool stale = readSettings(item.handle->name).needsReimport;
                    stateMoved = stateMoved || stale != item.stale;
                    item.stale = stale;
                }
            }
            if (stateMoved)
            {
                item.row.state = rowState(item);
                statusMoved = true;
            }
            if (item.row.refs != live.refs)
            {
                item.row.refs = live.refs;
                statusMoved = true;
            }
        }

        [[nodiscard]] const void* rowOf(const u32 row)
        {
            Item& item = items[shown[row]];
            readStatus(item);
            return &item.row;
        }

        // After a command or a load settled the visible rows read again (the table writes only the cells that
        // changed); the scene files and the UI documents only move with a command.
        void refreshStatus(const bool commandRan)
        {
            if (commandRan)
            {
                u64 views = 0;
                eachDocument([&views](const UiDocumentView&) { ++views; });
                if (views != seenUiViews || sceneStamp() != seenSceneTime)
                {
                    readModel();
                    return;
                }
            }
            statusMoved = false;
            detailsDirty = true; // the selected row may be out of the visible ones
            table.refresh(static_cast<u32>(shown.size()));
            refreshGrid();
            stats.refreshes += statusMoved;
        }

        // -- details -----------------------------------------------------

        [[nodiscard]] Item* findItem(const Entity key)
        {
            const auto it = std::ranges::find(items, key, &Item::key);
            return it == items.end() ? nullptr : &*it;
        }

        // a model's source as the pane shows it: relative to the import root, else the file's name
        [[nodiscard]] std::string sourceText(const std::string_view source) const
        {
            const std::filesystem::path file = pathFromUtf8(source);
            const std::string relative = importRelative(file);
            return relative.empty() ? utf8Path(file.filename()) : relative;
        }

        [[nodiscard]] std::optional<SpiteTextureHeader> textureHeader(const InternedString texture) const
        {
            const InternedString baked = engine->assets().manifest().getTextureBakedPath(texture);
            SpiteTextureHeader header;
            if (!baked.isValid())
            {
                return std::nullopt;
            }
            std::ifstream file(resolveContentPath(pathFromUtf8(baked.toView())), std::ios::binary);
            if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
                std::string_view(header.magic, 4) != "SPTX")
            {
                return std::nullopt;
            }
            return header;
        }

        void readTextureUsers(const AssetManifest& manifest)
        {
            textureUsers.clear();
            usersGeneration = manifest.generation();
            for (const auto& [material, entry] : manifest.getMaterials())
            {
                ++stats.materialReads;
                const auto fields = readBakedMaterialDescriptor(material, manifest);
                if (!fields)
                {
                    continue;
                }
                for (const MaterialTextureSlot slot : MATERIAL_TEXTURE_SLOTS)
                {
                    const InternedString texture = fields->textureName(slot);
                    if (std::vector<std::string>& users = textureUsers[texture];
                        texture.isValid() && std::ranges::find(users, material.toView()) == users.end())
                    {
                        users.emplace_back(material.toView());
                    }
                }
            }
        }

        // The facts that come from the asset's files and the manifest: read when the selection or the manifest moved,
        // never for a status change (a command or load only moves the live facts).
        void readFacts(const Item& item)
        {
            fileFacts.clear();
            previewPixels = {};
            fileSourceOutside = false;
            fileModelUsers = 0;
            fileSubmeshes = 0;
            fileKey = item.key;
            const AssetManifest& manifest = engine->assets().manifest();
            fileGeneration = manifest.generation();
            const InternedString name(item.row.name.c_str());
            const auto add = [this](const char* id, std::string value) { fileFacts.push_back({id, std::move(value)}); };
            const auto count = [&add](const char* id, const u64 value) { add(id, std::to_string(value)); };
            const auto extent = [](const float x, const float y, const float z)
            { return std::format("{:.2f} \xC3\x97 {:.2f} \xC3\x97 {:.2f}", x, y, z); };
            switch (item.kind)
            {
            case KIND_SKELETON:
                if (const auto entry = manifest.getSkeletonEntry(name))
                {
                    add("model", std::string(entry->model.toView()));
                    count("joints", entry->jointCount);
                    if (const auto skeleton = loadBakedSkeleton(name, manifest))
                        add("identity", std::format("{:016x}", skeleton->identity));
                }
                break;
            case KIND_CLIP:
                if (const auto entry = manifest.getAnimationEntry(name))
                {
                    add("model", std::string(entry->model.toView()));
                    add("skeleton", std::string(entry->skeleton.toView()));
                    add("duration", std::format("{:.2f}", entry->duration));
                    count("tracks", entry->trackCount);
                }
                break;
            case KIND_ENVIRONMENT:
                if (const auto entry = manifest.getEnvironmentEntry(name))
                {
                    add("source", sourceText(entry->source.toView()));
                    add("format", std::string(bakedEnvironmentFormatName(entry->format)));
                    add("dimensions", std::format("{} × {}", entry->width, entry->height));
                    count("mips", entry->mipLevels);
                    count("bytes", entry->pixelBytes);
                }
                break;
            case KIND_OCEAN_PROFILE:
                if (const auto entry = manifest.getOceanProfileEntry(name))
                {
                    add("source", sourceText(entry->source.toView()));
                    add("dimensions", std::format("{} × {}", entry->width, entry->height));
                    count("frames", entry->frameCount);
                    add("framesPerSecond", std::format("{:.2f}", entry->framesPerSecond));
                    add("tileMetres", std::format("{:.2f}", entry->tileMetres));
                    add("maxDisplacementMetres", std::format("{:.2f}", entry->maxDisplacementMetres));
                    add("playback", tr(entry->loop ? "editor-project.playback.loop" : "editor-project.playback.once"));
                }
                break;
            case KIND_MODEL:
                if (const std::optional<AssetManifest::ModelEntry> entry = manifest.getModelEntry(name))
                {
                    if (item.vfxCard)
                        add("playback", tr("editor-project.playback.flipbook"));
                    count("meshlets", entry->gpuCosts.meshletCount);
                    count("vertices", entry->gpuCosts.vertexCount);
                    count("triangles", entry->gpuCosts.meshletTriangleIndexCount / 3);
                    if (const auto descriptor = loadBakedModelDescriptor(name, manifest))
                    {
                        const ModelHierarchyDescriptor& model = **descriptor;
                        BoundingBox total = model.submeshes.empty() ? BoundingBox{} : model.submeshes[0].localBounds;
                        for (const ModelSubmeshDescriptor& submesh : model.submeshes)
                        {
                            const BoundingBox& at = submesh.localBounds;
                            total.min = {std::min(total.min.x, at.min.x), std::min(total.min.y, at.min.y),
                                         std::min(total.min.z, at.min.z)};
                            total.max = {std::max(total.max.x, at.max.x), std::max(total.max.y, at.max.y),
                                         std::max(total.max.z, at.max.z)};
                        }
                        count("nodes", model.nodes.size());
                        add("bounds",
                            extent(total.max.x - total.min.x, total.max.y - total.min.y, total.max.z - total.min.z));
                        count("submeshes", model.submeshes.size());
                        fileSubmeshes = static_cast<u32>(model.submeshes.size());
                        for (const ModelSubmeshDescriptor& submesh :
                             std::span(model.submeshes).first(std::min(model.submeshes.size(), MAX_SUBMESHES)))
                        {
                            const BoundingBox& at = submesh.localBounds;
                            add("submesh",
                                std::string(submesh.name.toView()) + " (" +
                                    extent(at.max.x - at.min.x, at.max.y - at.min.y, at.max.z - at.min.z) + ")");
                        }
                        if (model.submeshes.size() > MAX_SUBMESHES)
                        {
                            add("submeshesCut", {}); // its value is localized when the pane is built
                        }
                    }
                    else
                    {
                        add("error", std::string(descriptor.error().message));
                    }
                    count("materials", entry->materials.size());
                    for (const InternedString& material : entry->materials)
                    {
                        add("material", std::string(material.toView()));
                    }
                    add("source", sourceText(entry->source.toView()));
                    fileSourceOutside = importRelative(pathFromUtf8(entry->source.toView())).empty();
                }
                break;
            case KIND_MATERIAL:
                if (item.token == "builtin")
                    break;
                if (const auto fields = readBakedMaterialDescriptor(name, manifest))
                {
                    add("albedo",
                        std::format("{:.2f} {:.2f} {:.2f} {:.2f}", fields->albedoFactor.x, fields->albedoFactor.y,
                                    fields->albedoFactor.z, fields->albedoFactor.w));
                    const std::array<std::pair<const char*, InternedString>, 3> maps{
                        {{"albedoTexture", fields->albedoTextureName},
                         {"normalTexture", fields->normalTextureName},
                         {"metallicRoughnessTexture", fields->metallicRoughnessTextureName}}};
                    for (const auto& [id, texture] : maps)
                    {
                        if (texture.isValid() && !texture.toView().empty())
                        {
                            add(id, std::string(texture.toView()));
                        }
                    }
                }
                else
                {
                    add("error", std::string(fields.error().message));
                }
                for (const auto& [model, entry] : manifest.getModels())
                {
                    if (std::ranges::find(entry.materials, name) != entry.materials.end())
                    {
                        add("usedBy", std::string(model.toView()));
                        ++fileModelUsers;
                    }
                }
                break;
            case KIND_TEXTURE:
            case KIND_ATLAS:
                {
                    const InternedString texture = item.kind == KIND_ATLAS ? item.handle->name : name;
                    if (const std::optional<SpiteTextureHeader> header = textureHeader(texture))
                    {
                        previewPixels = Vec2{static_cast<float>(header->width), static_cast<float>(header->height)};
                        add("size", std::format("{} \xC3\x97 {}", header->width, header->height));
                        const u8 format = bakedTextureFormat(*header);
                        add("format",
                            bakedTextureFormatKnown(format) ? std::string(bakedTextureFormatName(format))
                                                            : "format " + std::to_string(header->format));
                        if (const u32 mips = bakedTextureMipLevels(*header))
                        {
                            count("mips", mips);
                            count("bytes", bakedTextureChainBytes(format, header->width, header->height, mips));
                        }
                    }
                    if (item.kind == KIND_ATLAS)
                    {
                        for (const auto& [atlas, entry] : manifest.getSpriteAtlases())
                        {
                            if (atlas == name)
                            {
                                count("sprites", entry.spriteCount);
                                add("page", std::string(entry.texture.toView()));
                            }
                        }
                        break;
                    }
                    if (usersGeneration != fileGeneration)
                    {
                        readTextureUsers(manifest);
                    }
                    if (const auto users = textureUsers.find(name); users != textureUsers.end())
                    {
                        for (const std::string& material : users->second)
                        {
                            add("usedBy", material);
                        }
                    }
                    break;
                }
            default:
                break;
            }
        }

        // A model's sidecar settings against the ones its bake used; shown is false for a name without an entry.
        [[nodiscard]] ProjectImportModel readSettings(const InternedString name)
        {
            ++stats.settingsReads;
            ProjectImportModel model;
            const std::optional<AssetManifest::ModelEntry> entry = engine->assets().manifest().getModelEntry(name);
            if (!entry)
            {
                return model;
            }
            const std::string source(entry->source.toView());
            model.shown = true;
            model.name = name.toView();
            model.source = source;
            model.baked = model.settings = entry->importSettings;
            if (importRelative(pathFromUtf8(source)).empty() && !entry->sourceRoot.isValid())
            {
                model.blocked = "outside";
                return model;
            }
            if (importRelative(pathFromUtf8(source)).empty())
            {
                std::error_code error;
                const auto root = std::filesystem::canonical(pathFromUtf8(entry->sourceRoot.toView()), error);
                if (error)
                {
                    model.blocked = "outside";
                    return model;
                }
                const auto file = std::filesystem::canonical(pathFromUtf8(source), error);
                if (error)
                {
                    model.blocked = "gone";
                    return model;
                }
                const auto sidecar =
                    std::filesystem::weakly_canonical(pathFromUtf8(modelImportSidecarPath(source)), error);
                if (error || file.parent_path() != root || sidecar.parent_path() != root)
                {
                    model.blocked = "outside";
                    return model;
                }
            }
            const Expected<ModelImportSettings> sidecar = loadModelImportSettings(source);
            if (sidecar)
            {
                model.settings = *sidecar;
            }
            else
            {
                model.error = sidecar.error().message;
            }
            model.needsReimport = !sidecar || model.settings != model.baked;
            return model;
        }

        // The import tab's model: the settings, whether the source and sidecar are there, and the newest import job
        [[nodiscard]] ProjectImportModel readImport(const Item& item)
        {
            ProjectImportModel model = item.kind == KIND_MODEL && !item.pending
                ? readSettings(InternedString(item.row.name.c_str()))
                : ProjectImportModel{};
            if (!model.shown)
            {
                return model;
            }
            std::error_code error;
            if (model.blocked.empty())
            {
                model.blocked = std::filesystem::is_regular_file(pathFromUtf8(model.source), error) ? "" : "gone";
                model.sidecarExists =
                    std::filesystem::exists(pathFromUtf8(modelImportSidecarPath(model.source)), error);
            }
            const std::vector<ModelImport> imports = engine->assets().imports();
            const auto latest = std::find_if(imports.rbegin(), imports.rend(), [&item](const ModelImport& job)
                                             { return job.name.toView() == item.row.name; });
            if (latest != imports.rend())
            {
                model.job = latest->id;
                model.jobState = jobToken(latest->state);
                model.jobError = latest->error;
                model.bakeMs = latest->bakeMs;
            }
            return model;
        }

        // -- material editor -----------------------------------------------

        // The Edit tab's model from the descriptor and the manifest: the fields and the models that list the material
        [[nodiscard]] ProjectMaterialModel readMaterial(const Item& item)
        {
            ++stats.editReads;
            const AssetManifest& manifest = engine->assets().manifest();
            const InternedString name(item.row.name.c_str());
            ProjectMaterialModel model;
            model.shown = true;
            model.name = item.row.name;
            if (const auto fields = readBakedMaterialDescriptor(name, manifest))
            {
                model.values = materialValues(*fields);
            }
            else
            {
                model.error = std::string(fields.error().message);
            }
            for (const auto& [owner, entry] : manifest.getModels())
            {
                if (std::ranges::find(entry.materials, name) != entry.materials.end())
                {
                    model.models.emplace_back(owner.toView());
                }
            }
            std::ranges::sort(model.models);
            return model;
        }

        // The entities of the world the panel shows that bind the material: how many, and the first MAX_USERS by id
        void scanUsers(const std::string_view material, ProjectMaterialModel& model)
        {
            ++stats.userScans;
            materialUsersAt = control->stateGeneration();
            const InternedString wanted(material);
            const auto byId = [](const Entity a, const Entity b) { return a.id() < b.id(); };
            std::vector<Entity> listed; // a max-heap on id: the MAX_USERS smallest matches so far
            u32 count = 0;
            const World& target = world();
            auto query =
                target.createQueryBuilder().with<ecs::Read<MaterialComponent>>().includeInactive().buildQuery();
            query->forEachChunkConst(
                [&](const Chunk* chunk)
                {
                    const auto bindings = chunk->readColumn<MaterialComponent>();
                    const auto entities = chunk->entities();
                    for (sizet i = 0; i < chunk->size(); ++i)
                    {
                        if (bindings[i].materialHandle.name != wanted)
                        {
                            continue;
                        }
                        ++count;
                        if (listed.size() < ProjectMaterialModel::MAX_USERS)
                        {
                            listed.push_back(entities[i]);
                            std::ranges::push_heap(listed, byId);
                        }
                        else if (byId(entities[i], listed.front()))
                        {
                            std::ranges::pop_heap(listed, byId);
                            listed.back() = entities[i];
                            std::ranges::push_heap(listed, byId);
                        }
                    }
                });
            std::ranges::sort_heap(listed, byId);
            model.scanned = true;
            model.entityCount = count;
            model.entities.clear();
            for (const Entity user : listed)
            {
                std::string label(entityName(target, user));
                model.entities.push_back(
                    {user.id(),
                     label.empty() ? tr("editor-project.material.entity",
                                        std::vector<MessageArg>{{"id", static_cast<std::int64_t>(user.id())}})
                                   : std::move(label)});
            }
        }

        void setMaterial(ProjectMaterialModel next)
        {
            if (!(next == materialModel))
            {
                materialModel = std::move(next);
                materialDirty = true;
                detailsView = true;
            }
        }

        // base, else base_2, base_3... whichever no material of the manifest has (bake file names ignore case); a
        // base too long for the name limit is cut
        [[nodiscard]] std::string freeMaterialName(std::string base) const
        {
            const AssetManifest& manifest = engine->assets().manifest();
            const auto taken = [&manifest](const std::string& candidate)
            {
                const std::string key = AssetManifest::bakeFileKey(candidate);
                return std::ranges::any_of(manifest.getMaterials(), [&key](const auto& entry)
                                           { return AssetManifest::bakeFileKey(entry.first.toView()) == key; });
            };
            base.resize(std::min(base.size(), MAX_MATERIAL_NAME - 8));
            std::string name = base;
            for (u32 n = 2; taken(name); ++n)
            {
                name = base + "_" + std::to_string(n);
            }
            return name;
        }

        // New material and Duplicate create a material under a free name and select its row
        void createMaterialNamed(const std::string& name, const Json& fields)
        {
            Json args{{"name", name}};
            if (!fields.is_null())
            {
                args["fields"] = fields;
            }
            pending.push_back({Json{{"command", "createMaterial"}, {"args", std::move(args)}}.dump(),
                               false,
                               "editor-project.material.create.failed",
                               {},
                               [this, name](const Json&)
                               {
                                   selectName = name;
                                   selectKind = KIND_MATERIAL;
                                   useFilter(KIND_MATERIAL, name);
                               }});
        }

        void newMaterial() { createMaterialNamed(freeMaterialName(std::string(NEW_MATERIAL_NAME)), nullptr); }

        void duplicateMaterial()
        {
            if (!materialModel.shown || !materialModel.error.empty())
            {
                return;
            }
            // the whole descriptor, so a copy keeps every v2 field and slot, not just the ones the form shows
            const auto descriptor =
                readBakedMaterialDescriptor(InternedString(materialModel.name.c_str()), engine->assets().manifest());
            if (!descriptor)
            {
                return;
            }
            JsonFragmentWriter writer;
            writeValue(BakedMaterialWriteData::spiteSchema(), &*descriptor, writer, FieldSet::All);
            std::string text;
            writer.dump(text);
            const Json fields = Json::parse(text);
            std::string stem = materialModel.name;
            stem.resize(std::min(stem.size(), MAX_MATERIAL_NAME - 8 - COPY_SUFFIX.size()));
            createMaterialNamed(freeMaterialName(stem + std::string(COPY_SUFFIX)), fields);
        }

        // the textures of the manifest, by name: what a texture slot's Pick list offers
        [[nodiscard]] std::vector<std::string> textureNames() const
        {
            std::vector<std::string> names;
            for (const auto& [name, entry] : engine->assets().manifest().getTextures())
            {
                names.emplace_back(name.toView());
            }
            std::ranges::sort(names);
            return names;
        }

        // the file a Reveal shows: the baked file of an asset, the scene's or the UI document's; none for an import
        [[nodiscard]] std::filesystem::path revealPathOf(const Item& item) const
        {
            if (item.pending || item.row.path.empty())
            {
                return {};
            }
            const auto path = pathFromUtf8(item.row.path);
            const std::filesystem::path file = item.kind == KIND_SCENE ? sceneRoot() / path
                : item.kind == KIND_CONTROLLER                         ? control->root("animation") / path
                                                                       : resolveContentPath(path);
            std::error_code error;
            return std::filesystem::is_regular_file(file, error) ? file : std::filesystem::path{};
        }

        void setImport(ProjectImportModel next)
        {
            if (!(next == importModel))
            {
                importModel = std::move(next);
                importDirty = true;
                detailsView = true;
            }
        }

        // Follows the one selected row: its live facts (state, references) are compared every time the status moved,
        // its file facts are read only for a new selection or manifest. Sets detailsView when the widgets must change.
        void refreshDetails()
        {
            const std::span<const Entity> selected = table.selection();
            const Entity key = selected.size() == 1 ? selected[0] : Entity::undefined();
            if (key == detailsKey && !detailsDirty)
            {
                return;
            }
            detailsDirty = false;
            const bool moved = key != detailsKey;
            detailsKey = key;
            Item* item = key.isValid() ? findItem(key) : nullptr;
            if (!item)
            {
                if (!detailFacts.empty())
                {
                    detailFacts.clear();
                    detailLive.clear();
                    detailActions = {};
                    removeBlocked.clear();
                    previewName.clear();
                    detailsName.clear();
                    detailsView = true;
                }
                setImport({});
                setMaterial({});
                return;
            }
            readStatus(*item);
            const bool reread = fileKey != key || fileGeneration != engine->assets().manifest().generation();
            if (reread)
            {
                readFacts(*item);
            }
            const bool inUse = item->kind == KIND_MATERIAL && (fileModelUsers > 0 || item->row.refs > 1);
            if (item->kind == KIND_MATERIAL && item->token != "builtin")
            {
                // the descriptor is read again with the manifest (an edit moves it); the entities that bind the
                // material only while the Edit tab shows, when the scene's state moved
                const bool same = materialModel.shown && materialModel.name == item->row.name;
                ProjectMaterialModel next = !same || reread ? readMaterial(*item) : materialModel;
                if (editTab && (!next.scanned || materialUsersAt != control->stateGeneration()))
                {
                    scanUsers(item->row.name, next);
                }
                else if (!editTab && next.scanned)
                {
                    next.scanned = false; // the scan is read only while the tab shows: never reported stale
                    next.entityCount = 0;
                    next.entities.clear();
                }
                next.blocked = inUse;
                setMaterial(std::move(next));
            }
            else
            {
                setMaterial({});
            }
            std::vector<Fact> live{{"name", item->row.name},
                                   {"type", tr("editor-project.type." + std::string(TYPE_NAMES[displayKind(*item)]))},
                                   {"state", stateText(item->token)}};
            if (item->handle)
            {
                live.push_back({"refs", std::to_string(item->row.refs)});
            }
            if (!item->spaces.empty())
            {
                live.push_back({"spaces", item->spaces});
            }
            if (!item->row.path.empty())
            {
                live.push_back({"path", item->row.path});
            }
            if (reread || moved || importReadAt != engine->assets().importsGeneration())
            {
                importReadAt = engine->assets().importsGeneration();
                setImport(readImport(*item));
            }
            if (!reread && !moved && live == detailLive)
            {
                return;
            }
            detailLive = std::move(live);
            // the asset's own facts lead: the live ones (name, type, state, references, file) repeat the table's row
            detailFacts = fileFacts;
            for (Fact& fact : detailFacts)
            {
                if (fact.id == "submeshesCut")
                {
                    fact.value = tr("editor-project.details.firstOf",
                                    std::vector<MessageArg>{{"shown", static_cast<std::int64_t>(MAX_SUBMESHES)},
                                                            {"total", static_cast<std::int64_t>(fileSubmeshes)}});
                }
            }
            detailFacts.insert(detailFacts.end(), detailLive.begin(), detailLive.end());
            detailsName = item->row.name;
            detailsType = TYPE_NAMES[item->kind];
            revealTarget = revealPathOf(*item);
            removeLoaded = item->token == "ready" || item->token == "loading";
            removeUses = item->row.refs > removeLoaded ? item->row.refs - removeLoaded : 0;
            removeBlocked = item->token == "builtin" ? "builtin"
                : item->kind == KIND_MODEL           ? ""
                : item->kind == KIND_MATERIAL        ? (inUse ? "inUse" : "")
                                                     : "kind";
            detailActions = {.load = item->handle && item->token != "loading" && item->token != "ready",
                             .remove = removeBlocked.empty(),
                             .reveal = !revealTarget.empty()};
            if (item->handle)
            {
                loadType = item->kind == KIND_ATLAS ? "texture" : TYPE_NAMES[item->kind];
                loadName = item->handle->name.toView();
            }
            // the image is the sRGB copy: a texture held only as data (linear) shows none rather than make one
            previewName = (item->kind == KIND_TEXTURE || item->kind == KIND_ATLAS) &&
                    (item->token == "ready" || item->token == "loading") && item->spaces.starts_with("sRGB")
                ? std::string(item->handle->name.toView())
                : std::string{};
            detailsView = true;
        }

        // the bytes of `text` (whole code points, at least one) that the value cell paints within the cut width
        [[nodiscard]] sizet fittingBytes(const std::string_view text, const UiTextMeasurer* measurer,
                                         const Entity element) const
        {
            std::vector<sizet> ends;
            for (sizet i = 1; i <= text.size(); ++i)
            {
                if (i == text.size() || (static_cast<u8>(text[i]) & 0xC0) != 0x80)
                {
                    ends.push_back(i);
                }
            }
            if (!measurer || cutWidth < MIN_VALUE_WIDTH)
            {
                return ends[std::min(ends.size(), VALUE_POINTS) - 1];
            }
            sizet low = 0; // the longest prefix known to fit, in code points
            sizet high = ends.size();
            while (low < high)
            {
                const sizet mid = (low + high + 1) / 2;
                if (measurer->measureText(element, text.substr(0, ends[mid - 1])).x <= cutWidth)
                {
                    low = mid;
                }
                else
                {
                    high = mid - 1;
                }
            }
            return ends[std::max<sizet>(low, 1) - 1];
        }

        // The facts as the list shows them: a value wider than the value cell continues on rows without a key. The
        // width is the cell's as laid out now (the keys' column grows to its widest text), so a change of it cuts
        // again.
        void cutRows()
        {
            rows.clear();
            cutWidth = facts->lastCellWidth();
            const UiTextMeasurer* measurer = scope.hierarchy().textMeasurer();
            const Entity element = facts->textElement();
            for (const Fact& fact : detailFacts)
            {
                std::string_view rest = fact.value;
                std::string id = fact.id;
                do
                {
                    sizet take = rest.empty() ? 0 : fittingBytes(rest, measurer, element);
                    if (take < rest.size())
                    {
                        // a row ends at the last space (dropped) or path separator that fits: no token is split
                        for (sizet at = take; at > 0; --at)
                        {
                            if (rest[at] == ' ' || rest[at - 1] == '/' || rest[at - 1] == '\\')
                            {
                                take = at;
                                break;
                            }
                        }
                    }
                    rows.push_back({std::exchange(id, {}), std::string(rest.substr(0, take))});
                    rest.remove_prefix(take);
                    rest.remove_prefix(std::min(rest.find_first_not_of(' '), rest.size()));
                }
                while (!rest.empty());
            }
        }

        void showDetails()
        {
            if (!detailsView || !detailsSlot.isValid())
            {
                return;
            }
            detailsView = false;
            const bool any = !detailFacts.empty();
            const InternedString browser("project.browser");
            const InternedString details("project.details.panel");
            if (any && !innerDock.isOpen(details))
                innerDock.apply(UiDockOp{.kind = UiDockOpKind::Dock,
                                         .panel = details,
                                         .target = browser,
                                         .zone = UiDockZone::Right,
                                         .ratio = 1.0f - browserRatio});
            else if (!any && innerDock.isOpen(details))
                innerDock.apply(UiDockOp{.kind = UiDockOpKind::Close, .panel = details});
            const bool importOffered = importModel.shown;
            const bool importing = importOffered && importTab;
            const bool editOffered = materialModel.shown;
            const bool editing = editOffered && editTab;
            if (tabs.isVisible() != (importOffered || editOffered))
            {
                tabs.setVisible(importOffered || editOffered);
            }
            if (tabImport.isVisible() != importOffered)
            {
                tabImport.setVisible(importOffered);
            }
            if (tabEdit.isVisible() != editOffered)
            {
                tabEdit.setVisible(editOffered);
            }
            showSelected(tabDetails, !importing && !editing);
            showSelected(tabImport, importing);
            showSelected(tabEdit, editing);
            if (body.isVisible() == (importing || editing))
            {
                body.setVisible(!importing && !editing);
            }
            if (Widget pane = importView->root(); pane.isVisible() != importing)
            {
                pane.setVisible(importing);
            }
            if (Widget pane = materialView->root(); pane.isVisible() != editing)
            {
                pane.setVisible(editing);
                if (!editing)
                {
                    materialView->close();
                }
            }
            if (importDirty)
            {
                importDirty = false;
                importView->show(importModel);
            }
            if (materialDirty)
            {
                materialDirty = false;
                materialView->show(materialModel);
            }
            const bool previewing = !previewName.empty();
            if (previewFrame.isVisible() != previewing)
            {
                previewFrame.setVisible(previewing);
            }
            if (previewing && shownPreview != previewName)
            {
                shownPreview = previewName;
                editorUi(*engine).uiView().registerImage(PREVIEW_IMAGE_SLOT, InternedString(previewName.c_str()));
                const float scale = PREVIEW_SIZE / std::max({previewPixels.x, previewPixels.y, 1.0f});
                preview.setSize(previewPixels.x > 0.0f ? previewPixels * scale : Vec2{PREVIEW_SIZE, PREVIEW_SIZE});
            }
            cutRows();
            materialNames.clear();
            for (const Fact& fact : detailFacts)
                if (fact.id == "material")
                    materialNames.push_back(fact.value);
            materialLinks->setMaterials(materialNames);
            facts->setRowCount(static_cast<u32>(rows.size()));
            facts->rebindVisible();
        }

        void loadSelected()
        {
            if (detailActions.load)
            {
                pending.push_back(
                    {Json{{"command", "loadAsset"}, {"args", {{"type", loadType}, {"name", loadName}}}}.dump(), false,
                     LOAD_FAILED_KEY.data()});
            }
        }

        // Remove asks first: the commands are undoable, but a model's bake files and a material's file are deleted
        void removeSelected()
        {
            if (!detailActions.remove)
            {
                return;
            }
            const bool model = detailsType == "model";
            const std::string name = detailsName;
            const std::vector<MessageArg> args{{"name", std::string_view(name)},
                                               {"uses", static_cast<std::int64_t>(removeUses)}};
            EditorConfirm ask;
            ask.title = tr("editor-project.remove.title", args);
            // the removal leaves what is loaded in memory: a loaded asset, or one that entities hold, is told
            ask.body = tr(std::string("editor-project.remove.") + (model ? "model" : "material") +
                              (model && removeUses > 0 ? ".used"
                                   : removeLoaded      ? ".loaded"
                                                       : ""),
                          args);
            ask.confirmLabel = tr("editor-project.remove.confirm");
            ask.destructive = true;
            dialogs->confirm(std::move(ask),
                             [this, model, name](const bool confirmed)
                             {
                                 if (confirmed)
                                 {
                                     pending.push_back({Json{{"command", model ? "removeModel" : "deleteMaterial"},
                                                             {"args", {{"name", name}}}}
                                                            .dump(),
                                                        false, REMOVE_FAILED_KEY.data()});
                                 }
                             });
        }

        // The file manager shows the baked file: a window of another program, so not twice in a second
        std::string reveal(IFormatWriter& out)
        {
            catchUp();
            refreshDetails();
            if (!detailActions.reveal)
            {
                return "select one asset that has a file first (projectSelect)";
            }
            const u64 now = revealClock();
            if (hasRevealed && (now - lastRevealMs < REVEAL_GAP_MS))
            {
                return "refused: the file manager was shown a moment ago";
            }
            hasRevealed = true;
            lastRevealMs = now;
            if (!revealFile(revealTarget))
            {
                return "the file manager could not be started";
            }
            out.writeString("path", utf8Path(revealTarget));
            return {};
        }

        // Selects an asset's row: the filter takes its type and name (as Show does) and the row is selected when the
        // filter has taken effect.
        std::string selectAsset(const ControlArgs& args)
        {
            catchUp();
            const std::string name = args.string("name").value_or("");
            u8 kind = 0;
            if (const std::optional<std::string> named = args.string("type"))
            {
                kind = static_cast<u8>(std::ranges::find(TYPE_NAMES, *named) - TYPE_NAMES.begin());
                if (kind == 0 || kind >= TYPE_NAMES.size())
                {
                    return "unknown project asset type '" + *named + "'";
                }
            }
            std::vector<std::string_view> names;
            const Item* found = nullptr;
            for (const Item& item : items)
            {
                if (!matchesKind(item, kind))
                {
                    continue;
                }
                if (item.row.name == name)
                {
                    found = &item;
                    break;
                }
                names.push_back(item.row.name);
            }
            if (!found)
            {
                constexpr sizet LISTED = 12;
                std::string valid;
                for (sizet i = 0; i < std::min(names.size(), LISTED); ++i)
                {
                    valid += (i ? ", " : "") + std::string(names[i]);
                }
                return "no asset '" + name + "' in the project; " +
                    (names.empty() ? std::string("it has none of that type")
                                   : "the first " + std::to_string(std::min(names.size(), LISTED)) + " of " +
                             std::to_string(names.size()) + " are: " + valid);
            }
            selectName = found->row.name;
            selectKind = found->kind;
            useFilter(kind == KIND_VFX ? kind : found->kind, found->row.name);
            return {};
        }

        // a locale change: every text the panel wrote follows it
        void relocalize()
        {
            loadTexts();
            detailLive.clear();
            detailsDirty = true;
            for (Item& item : items)
            {
                item.row.type = typeText(displayKind(item));
                item.row.state = rowState(item);
            }
            importView->relocalize();
            importDirty = true;
            materialView->relocalize();
            materialDirty = true;
            detailsView = true;
            table.refresh(static_cast<u32>(shown.size()));
            refreshGrid();
            queue->rebindVisible();
            showType();
            chipStale = true;
            showChip();
        }

        void applyFilter()
        {
            SPITE_ZONE("EditorProject.applyFilter");
            ++stats.refreshes;
            filterDirty = false;
            shown.clear();
            for (u32 i = 0; i < items.size(); ++i)
            {
                const Item& item = items[i];
                if (matchesKind(item, type) && containsIgnoringCase(item.row.name, text))
                {
                    shown.push_back(i);
                }
            }
            table.refresh(static_cast<u32>(shown.size()));
            refreshGrid();
            std::vector<Entity> kept;
            for (const Entity key : table.selection())
            {
                if (std::ranges::any_of(shown, [&](const u32 i) { return items[i].key == key; }))
                {
                    kept.push_back(key);
                }
            }
            if (kept.size() != table.selection().size())
            {
                table.setSelection(kept);
                refreshGrid();
            }
        }

        [[nodiscard]] const Item* itemOf(const Entity key) const
        {
            const auto it = std::ranges::find(items, key, &Item::key);
            return it == items.end() ? nullptr : &*it;
        }

        // One asset type per drag. A scene drag opens one file.
        [[nodiscard]] std::optional<UiDragPayload> payloadOf(const std::span<const Entity> keys) const
        {
            const auto usable = [this, keys](const Entity key) -> const Item*
            {
                const Item* item = itemOf(key);
                return item &&
                        ((item->kind >= KIND_MODEL && item->kind <= KIND_TEXTURE) ||
                         (item->kind == KIND_SCENE && keys.size() == 1)) &&
                        !item->pending && item->token != "builtin" && item->row.name.find('\n') == std::string::npos
                    ? item
                    : nullptr;
            };
            u8 kind = 0;
            for (const Entity key : keys)
            {
                if (const Item* item = usable(key); item && (kind == 0 || item->kind == KIND_MODEL))
                {
                    kind = item->kind;
                }
            }
            std::string names;
            u64 count = 0;
            for (const Entity key : keys)
            {
                if (const Item* item = usable(key); item && item->kind == kind)
                {
                    names += (count++ ? "\n" : "") + item->row.name;
                }
            }
            if (count == 0)
            {
                return std::nullopt;
            }
            UiDragPayload payload;
            payload.type = InternedString("asset." + std::string(TYPE_NAMES[kind]));
            payload.text = names;
            payload.data = count;
            const MessageArg countArg[]{{"count", static_cast<std::int64_t>(count)}};
            payload.label = count == 1 ? names : tr("editor-project.drag." + std::string(TYPE_NAMES[kind]), countArg);
            return payload;
        }

        // -- view --------------------------------------------------------------------

        void showType()
        {
            table.setColumnVisible(InternedString("type"), type == 0);
            for (u32 i = 0; i < types.size(); ++i)
            {
                if (types[i].text() != typeTitles[i])
                {
                    types[i].setText(typeTitles[i]);
                    types[i].setAccessibleName(typeTitles[i]);
                }
                showSelected(types[i], i == type);
            }
            sizeTypes();
        }

        // A type's box fits its name; the selection is a class, so it moves no neighbour. What the widths
        // follow (a theme, font or text scale: the stylesheet; a density or padding: the token table; the locale; the
        // UI scale) is the key; follow() sizes them again only when it moved, so an idle frame measures nothing.
        // The padding is in the key as the component holds it: the layout writes it from the tokens after follow().
        [[nodiscard]] TypeKey typeKeyNow() const
        {
            return {editorUi(*engine).uiView().stylesheet().generation(), scope.layoutTokens().generation(),
                    localization ? localization->revision() : 0, scope.hierarchy().layoutScale(),
                    scope.world().getConstComponent<UiContentSizeComponent>(types[0].entity()).padding.x};
        }

        [[nodiscard]] float typeWidth(const u32 index) const
        {
            const UiTextMeasurer* measurer = scope.hierarchy().textMeasurer();
            if (!measurer)
            {
                return -1.0f;
            }
            const Entity button = types[index].entity();
            return measurer->measureText(button, typeTitles[index]).x +
                scope.world().getConstComponent<UiContentSizeComponent>(button).padding.x * 2.0f;
        }

        void sizeTypes()
        {
            typeKey = typeKeyNow();
            for (u32 i = 0; i < types.size(); ++i)
            {
                const float width = typeWidth(i);
                const Entity button = types[i].entity();
                auto& rect = scope.world().getComponent<UiRectComponent>(button);
                if (width >= 0.0f && rect.size.x != width)
                {
                    scope.world().getComponent<UiContentSizeComponent>(button).fitWidth = false;
                    rect.size.x = width;
                    scope.markLayoutDirty(button);
                }
            }
        }

        // The chip names the filters that are on (a click clears them), or says there is none. A document apply
        // rewrites its text as authored, so follow compares it every frame, against a label built when a filter or
        // the locale changed.
        void showChip()
        {
            if (!chip.isValid())
            {
                return;
            }
            if (chipStale)
            {
                chipStale = false;
                std::string parts = type != 0 ? typeTitles[type] : std::string{};
                if (!text.empty())
                {
                    parts += (parts.empty() ? "\"" : " \xC2\xB7 \"") + clipped(text, CHIP_TEXT_POINTS) + "\"";
                }
                chipLabel = parts.empty()
                    ? tr("editor-project.chip.none")
                    : tr("editor-project.chip", std::vector<MessageArg>{{"filter", std::string_view(parts)}});
            }
            chip.setVisible(type != 0 || !text.empty());
            if (chip.text() != chipLabel)
            {
                chip.setText(chipLabel);
            }
        }

        // -- document ----------------------------------------------------------------

        // The type buttons go in their slot and take its child class.
        void fillTypes(const Widget& slot)
        {
            for (Button& button : types)
            {
                putIn(scope, slot, button);
                setOwnClass(button, slotFields(scope, slot).childClass);
            }
        }

        // The table goes in the rows slot and honours its scroll and child class.
        void fillRows(Widget slot)
        {
            rowsSlot = slot;
            putIn(scope, slot, table);
            putIn(scope, slot, grid);
            Widget background = slot;
            background.onPress(
                [this, background](u8 button, Vec2)
                {
                    if (button == 1)
                        openCreate(background);
                });
            showGridMode();
            const UiSlotComponent& fields = slotFields(scope, slot);
            table.setScrollEnabled(fields.scroll);
            table.setRowClass(fields.childClass.isValid() ? fields.childClass : InternedString(ROW_CLASS));
        }

        void bindFilter(const Widget& element)
        {
            filter = element.as<TextInput>();
            bindInputPadding(filter);
            filter.setAccessibleNameKey(InternedString("editor.access.projectFilter"));
            filter.setLocalizedPlaceholder(InternedString("editor-project.search.placeholder"));
            filter.onChange(
                [this](const std::string_view typed)
                {
                    pending.push_back(
                        {Json{{"command", "projectFilter"}, {"args", {{"text", std::string(typed)}}}}.dump()});
                });
        }

        void bindClearSearch(const Widget& element)
        {
            Button button = element.as<Button>();
            button.setAccessibleNameKey(InternedString("editor-project.access.clearSearch"));
            button.onClick(
                [this] { pending.push_back({Json{{"command", "projectFilter"}, {"args", {{"text", ""}}}}.dump()}); });
        }

        void bindImport(const Widget& element)
        {
            importButton = element.as<Button>();
            importButton.onClick([this] { pickModel(); });
        }

        void openCreate(const Widget& anchor)
        {
            const std::array items{UiChoiceItem{.id = InternedString("project.create.material"),
                                                .key = InternedString("editor-project.create.material")}};
            createPopup.setItems(items);
            createPopup.open(anchor);
        }

        void openAssetMenu(Entity key, const Widget& anchor)
        {
            if (std::ranges::find(table.selection(), key) == table.selection().end())
                table.select(key);
            refreshDetails();
            refreshGrid();
            const Item* item = table.selection().size() == 1 ? findItem(key) : nullptr;
            assetMenuKey = item ? key : Entity::undefined();
            std::vector<UiChoiceItem> items;
            assetMenuActions.clear();
            const auto add = [&](int action, const char* id, const char* key, bool enabled)
            {
                items.push_back(UiChoiceItem{.id = InternedString(id), .key = InternedString(key), .enabled = enabled});
                assetMenuActions.push_back(action);
            };
            if (item && item->kind == KIND_SCENE)
                add(0, "project.asset.open", "editor-project.context.open", static_cast<bool>(openScene));
            if (item && previewable(*item))
                add(5, "project.asset.preview", "editor-project.context.preview", true);
            if (item && (item->kind == KIND_SKELETON || item->kind == KIND_CLIP))
                add(6, "project.asset.owner", "editor-project.context.owner", true);
            if (item)
            {
                add(1, "project.asset.load", "editor-project.context.load", detailActions.load);
                add(2, "project.asset.reveal", "editor-project.context.reveal", detailActions.reveal);
                if (item->kind == KIND_MATERIAL)
                    add(3, "project.asset.duplicate", "editor-project.context.duplicate",
                        materialModel.shown && materialModel.error.empty());
            }
            const bool removable = item && detailActions.remove;
            add(4, "project.asset.remove", "editor-project.context.remove", removable);
            if (!removable)
            {
                const std::string reason = item
                    ? "editor-project.menu.remove." + (removeBlocked.empty() ? "kind" : removeBlocked)
                    : "editor-project.menu.remove.multiple";
                items.push_back(UiChoiceItem{.key = InternedString(reason), .enabled = false, .header = true});
                assetMenuActions.push_back(-1);
            }
            assetPopup.setItems(items);
            assetPopup.open(anchor);
        }

        void resizeDetails(float width)
        {
            const float available = innerDock.size().x - innerSplitterSize;
            if (!std::isfinite(width) || available <= 0.0f)
                return;
            browserRatio = std::clamp(1.0f - width / available, 0.0f, 1.0f);
            innerDock.apply(UiDockOp{.kind = UiDockOpKind::Resize,
                                     .panel = InternedString("project.browser"),
                                     .target = InternedString("project.details.panel"),
                                     .ratio = browserRatio});
        }

        void bindNewMaterial(const Widget& element)
        {
            newMaterialButton = element.as<Button>();
            newMaterialButton.onClick([this] { openCreate(newMaterialButton); });
        }

        void bindChip(const Widget& element)
        {
            chip = element.as<Button>();
            chip.setAccessibleNameKey(InternedString("editor-project.access.clearFilter"));
            chip.onClick(
                [this]
                {
                    pending.push_back(
                        {Json{{"command", "projectFilter"}, {"args", {{"type", "all"}, {"text", ""}}}}.dump()});
                });
            showChip();
        }

        void bindMessage(const Widget& element)
        {
            message = element.as<Label>();
            showMessage();
        }

        // A message is ended by a success that came after it: an import that started before it does not count.
        void setMessage(std::string text)
        {
            messageText = std::move(text);
            messageAfter = 0;
            if (!messageText.empty())
            {
                for (const ModelImport& job : engine->assets().imports())
                {
                    messageAfter = std::max(messageAfter, job.id);
                }
            }
            showMessage();
        }

        // The label shows the message and is hidden without one; a document apply rewrites both, so follow compares.
        void showMessage()
        {
            if (!message.isValid())
            {
                return;
            }
            if (message.text() != messageText)
            {
                message.setText(messageText);
            }
            if (scope.world().getConstComponent<UiStateComponent>(message.entity()).visible == messageText.empty())
            {
                message.setVisible(!messageText.empty());
            }
        }

        // The queue's slot: the list goes in and honours the slot's scroll and child class.
        void fillQueue(const Widget& slot)
        {
            queueSlot = slot;
            putIn(scope, slot, queue->list);
            const UiSlotComponent& fields = slotFields(scope, slot);
            queue->list.setScrollEnabled(fields.scroll);
            queue->setRowClass(fields.childClass.isValid() ? fields.childClass : InternedString("project.dense"));
        }

        // The slot's child class styles the facts' rows.
        void fillDetails(const Widget& slot)
        {
            detailsSlot = slot;
            putIn(scope, slot, tabs);
            putIn(scope, slot, body);
            putIn(scope, slot, importView->root());
            putIn(scope, slot, materialView->root());
            const UiSlotComponent& fields = slotFields(scope, slot);
            facts->list.setScrollEnabled(fields.scroll);
            facts->setRowClass(fields.childClass.isValid() ? fields.childClass : InternedString("project.dense"));
            detailsView = true;
        }

        // The queue is shown while the session has imports.
        void showQueue()
        {
            if (queueSlot.isValid() &&
                scope.world().getConstComponent<UiStateComponent>(queueSlot.entity()).visible == jobs.empty())
            {
                queueSlot.setVisible(!jobs.empty());
            }
        }

        // The newest MAX_JOBS imports, newest first, without a failure that a later import of the same file replaced.
        void readQueue()
        {
            const std::vector<ModelImport> all = engine->assets().imports();
            jobs.clear();
            for (auto it = all.rbegin(); it != all.rend() && jobs.size() < MAX_JOBS; ++it)
            {
                const bool replaced = it->state == ModelImport::State::Failed &&
                    std::any_of(all.rbegin(), it, [it](const ModelImport& later)
                                { return later.state == ModelImport::State::Done && later.source == it->source; });
                if (!replaced)
                {
                    jobs.push_back(*it);
                }
            }
            queue->setRowCount(static_cast<u32>(jobs.size()));
            queue->rebindVisible();
        }

        static const char* jobToken(const ModelImport::State state)
        {
            switch (state)
            {
            case ModelImport::State::Queued:
                return "queued";
            case ModelImport::State::Baking:
                return "baking";
            case ModelImport::State::Done:
                return "done";
            case ModelImport::State::Failed:
                return "failed";
            default:
                return "cancelled";
            }
        }

        // A queue row: the model's name, its state (a failure with its reason) and the action its state offers.
        void readJob(const u32 row, const std::span<std::string> cells, std::string& button) const
        {
            const ModelImport& job = jobs[row];
            cells[0] = std::string(job.name.toView());
            cells[1] = stateText(job.error.empty() ? std::string(jobToken(job.state))
                                                   : std::string(jobToken(job.state)) + ": " + job.error);
            switch (job.state)
            {
            case ModelImport::State::Queued:
            case ModelImport::State::Baking:
                button = tr("editor-project.queue.cancel");
                break;
            case ModelImport::State::Done:
                button = tr("editor-project.queue.show");
                break;
            default:
                button = tr("editor-project.queue.retry");
                break;
            }
        }

        [[nodiscard]] std::string jobButtonName(const u32 row) const
        {
            const ModelImport& job = jobs[row];
            const char* key = job.state == ModelImport::State::Queued || job.state == ModelImport::State::Baking
                ? "editor-project.queue.cancel.name"
                : job.state == ModelImport::State::Done ? "editor-project.queue.show.name"
                                                        : "editor-project.queue.retry.name";
            return tr(key, std::vector<MessageArg>{{"name", job.name.toView()}});
        }

        // Cancel is removeModel (it cancels a pending import), Retry the same importModel again, Show the asset's
        // row in the table.
        void pressJob(const u32 row)
        {
            const ModelImport& job = jobs[row];
            const std::string name(job.name.toView());
            switch (job.state)
            {
            case ModelImport::State::Queued:
            case ModelImport::State::Baking:
                pending.push_back({Json{{"command", "removeModel"}, {"args", {{"name", name}}}}.dump(), false,
                                   "editor-project.cancel.failed"});
                break;
            case ModelImport::State::Done:
                selectName = name;
                selectKind = KIND_MODEL;
                pending.push_back(
                    {Json{{"command", "projectFilter"}, {"args", {{"type", "model"}, {"text", name}}}}.dump()});
                break;
            default:
                {
                    const std::string relative = importRelative(pathFromUtf8(job.source));
                    pending.push_back(
                        {Json{{"command", relative.empty() ? "importExternalModel" : "importModel"},
                              {"args", {{"path", relative.empty() ? job.source : relative}, {"name", name}}}}
                             .dump(),
                         false, IMPORT_KEY.data()});
                    break;
                }
            }
        }

        // a file as importModel takes it: relative to the import root; empty outside it
        [[nodiscard]] std::string importRelative(const std::filesystem::path& source) const
        {
            const std::filesystem::path relative = source.lexically_relative(control->root("import"));
            if (relative.empty() || *relative.begin() == "..")
            {
                return {};
            }
            std::string text = utf8Path(relative);
            std::ranges::replace(text, '\\', '/');
            return text;
        }

        // A file dropped on the window while the panel is shown imports like a typed path. The drop's text is UTF-8.
        void importDropped(const std::string& file)
        {
            const std::filesystem::path dropped = pathFromUtf8(file);
            const std::string relative = importRelative(dropped);
            std::string extension = utf8Path(dropped.extension());
            std::ranges::transform(extension, extension.begin(), lower);
            const std::string name = utf8Path(dropped.filename());
            std::string error;
            if (extension != ".gltf" && extension != ".glb")
            {
                error = tr("editor-project.drop.notModel", std::vector<MessageArg>{{"name", std::string_view(name)}});
            }
            else if (relative.empty())
            {
                error = tr("editor-project.drop.outside", std::vector<MessageArg>{{"name", std::string_view(name)}});
            }
            if (!error.empty())
            {
                setMessage(error);
                feedback->postToast(ToastLevel::Error, std::move(error));
                return;
            }
            pathText = relative;
            requestImport();
        }

        void importExternalDropped(const std::string& file, const Vec2 position)
        {
            const auto path = pathFromUtf8(file);
            std::string extension = utf8Path(path.extension());
            std::ranges::transform(extension, extension.begin(), lower);
            if (extension == ".wav" || extension == ".mp3" || extension == ".flac")
            {
                pending.push_back({Json{{"command", "importExternalAudio"}, {"args", {{"path", file}}}}.dump(),
                                   false,
                                   IMPORT_KEY.data(),
                                   {},
                                   [this](const Json& result)
                                   { audioImports.push_back(result.at("import").get<u64>()); }});
                return;
            }
            if (extension == ".hdr" || extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
                extension == ".bmp" || extension == ".tga" || extension == ".gif" || extension == ".psd" ||
                extension == ".pic" || extension == ".ppm" || extension == ".pgm" || extension == ".pnm")
            {
                pending.push_back({Json{{"command", "importExternalImage"}, {"args", {{"path", file}}}}.dump(),
                                   false,
                                   IMPORT_KEY.data(),
                                   {},
                                   [this](const Json& result)
                                   { imageImports.push_back(result.at("import").get<u64>()); }});
                return;
            }
            if (extension != ".gltf" && extension != ".glb")
            {
                const std::string name = utf8Path(path.filename());
                const std::string error =
                    tr("editor-project.drop.notModel", std::vector<MessageArg>{{"name", std::string_view(name)}});
                feedback->postToast(ToastLevel::Error, error);
                return;
            }
            Json args{{"path", file}};
            const std::string stem = utf8Path(path.stem());
            const std::string safe = AssetManifest::safeBakeName(stem);
            const std::string name = safe.empty() ? stem : safe;
            if (name != stem)
                args["name"] = name;
            const Vec2 pixel = viewPixels(engine->render().view(SCENE_VIEW), position);
            const Json point = Json::parse(
                control->handle(Json{{"query", "dropPoint"}, {"args", {{"x", pixel.x}, {"y", pixel.y}}}}.dump()),
                nullptr, false);
            if (!point.is_object() || !point.value("ok", false))
            {
                tellFailed(DROP_FAILED_KEY.data(), point.is_object() ? point.value("error", std::string{}) : "");
                return;
            }
            const auto accepted =
                [this, world = std::string(worldName()), point = point["result"]["point"]](const Json& result)
            { placements.push_back({result.at("import").get<u32>(), world, point}); };
            if (const auto existing = engine->assets().manifest().getModelEntry(InternedString(name)); existing)
            {
                std::error_code error;
                const auto droppedSource = std::filesystem::canonical(path, error);
                if (!error &&
                    droppedSource == std::filesystem::canonical(pathFromUtf8(existing->source.toView()), error) &&
                    !error)
                {
                    pending.push_back({Json{{"command", "reimportModel"}, {"args", {{"name", name}}}}.dump(),
                                       false,
                                       IMPORT_KEY.data(),
                                       {},
                                       accepted});
                    return;
                }
            }
            pending.push_back({Json{{"command", "importExternalModel"}, {"args", std::move(args)}}.dump(),
                               false,
                               IMPORT_KEY.data(),
                               {},
                               accepted});
        }

        void pickModel()
        {
            if (picking)
            {
                return;
            }
            picking = true;
            EditorPick pick;
            pick.mode = EditorPickMode::OpenFile;
            pick.title = tr("editor-project.pick.title");
            pick.roots = {"import"};
            pick.root = "import";
            pick.extensions = "glb,gltf";
            dialogs->pickFile(std::move(pick),
                              [this](const std::optional<EditorPicked> picked)
                              {
                                  picking = false;
                                  if (picked)
                                  {
                                      pathText = picked->path;
                                      requestImport();
                                  }
                              });
        }

        // A document apply writes the fields' text as authored (none: empty): the fields show what the panel holds.
        // The command and onChange mirror typing into text/pathText, so a difference is an apply, focused or not.
        void keepFields()
        {
            if (filter.value() != text)
            {
                filter.setText(text);
            }
        }

        // -- drag and drop -----------------------------------------------------------

        struct Drop
        {
            Entity parent = Entity::undefined();
            bool scene = false; // the shown scene view: placed at the point under the pointer
            std::optional<u32> layer;
        };

        // A hierarchy row (its entity the parent), the hierarchy's background
        // or the shown scene view under the pointer; none over other panels.
        [[nodiscard]] std::optional<Drop> dropAt(const Entity hit, const Vec2 position) const
        {
            const UiHierarchyManager& ui = scope.hierarchy();
            const Entity scene = dock.panelHost(InternedString("scene"));
            const HierarchyModelDrop inHierarchy = hierarchyModelDrop(*engine, ui, hierarchy, hit);
            if (inHierarchy.inside)
            {
                return inHierarchy.accepts
                    ? std::optional(Drop{.parent = inHierarchy.parent, .layer = inHierarchy.layer})
                    : std::nullopt;
            }
            bool inScene =
                false; // the hit lies inside the scene panel: the drop needs the pointer over the view's image
            for (Entity at = hit; at.isValid(); at = ui.parentOf(at))
            {
                if (scene.isValid() && at == scene)
                {
                    inScene = true;
                    break;
                }
                if (at == dock.entity())
                {
                    return std::nullopt;
                }
            }
            if (!scene.isValid() || !ui.layout().isVisible(scene) ||
                !ui.layout().absoluteRect(scene).contains(position))
            {
                return std::nullopt;
            }
            const RenderView& view = engine->render().view(SCENE_VIEW);
            const bool onImage = view.windowRect.extent.width != 0 && view.windowRect.extent.height != 0 &&
                viewPointer(view, position).has_value();
            return inScene && onImage ? std::optional(Drop{.scene = true}) : std::nullopt;
        }

        // The table reports its drag: the first step begins a UiDragDrop drag of the dragged assets, the release drops.
        void dragRows(const std::span<const Entity> dragged, const Vec2 position, const bool released)
        {
            UiDragDrop& dragDrop = scope.dragDrop();
            if (!dragging && !released)
            {
                const std::optional<UiDragPayload> payload = payloadOf(dragged);
                dragging = payload && dragDrop.begin(table.entity(), *payload, position);
                return;
            }
            if (dragging && !dragDrop.active())
            {
                dragging = false; // cancelled (Escape, a modal) while the press continues
            }
            if (dragging)
            {
                dragDrop.move(position);
                if (released)
                {
                    dragging = false;
                    dragDrop.drop();
                }
            }
        }

        // Instantiates the dropped models: onto a hierarchy row (its entity the parent), onto the hierarchy's
        // background at the origin, onto the scene view at its dropPoint (the surface or ground under the pointer).
        void dropModels(const UiDropEvent& event)
        {
            const std::optional<Drop> drop = dropAt(event.hit, event.position);
            if (!drop)
            {
                return;
            }
            Json position;
            if (drop->scene)
            {
                const Vec2 pixel = viewPixels(engine->render().view(SCENE_VIEW), event.position);
                const Json reply = Json::parse(
                    control->handle(Json{{"query", "dropPoint"}, {"args", {{"x", pixel.x}, {"y", pixel.y}}}}.dump()),
                    nullptr, false);
                if (!reply.is_object() || !reply.value("ok", false))
                {
                    tellFailed(DROP_FAILED_KEY.data(), reply.is_object() ? reply.value("error", std::string{}) : "");
                    return;
                }
                position = reply["result"]["point"];
            }
            std::vector<Json> commands;
            for (std::string_view names = event.payload.text; !names.empty();)
            {
                const sizet end = std::min(names.find('\n'), names.size());
                Json args{{"model", std::string(names.substr(0, end))}};
                names.remove_prefix(std::min(end + 1, names.size()));
                if (drop->parent.isValid())
                {
                    args["parent"] = drop->parent.id();
                }
                if (drop->layer)
                    args["layer"] = *drop->layer;
                if (drop->scene)
                {
                    args["transform"] = Json{{"position", position}};
                }
                commands.push_back(Json{{"command", "instantiateModel"}, {"args", std::move(args)}});
            }
            if (commands.empty())
            {
                return;
            }
            Json request = commands.size() == 1
                ? std::move(commands.front())
                : Json{{"command", "batch"}, {"args", {{"commands", std::move(commands)}}}};
            request["args"]["world"] = worldName();
            pending.push_back({request.dump(), true, DROP_FAILED_KEY.data()});
        }

        bool acceptsScene(const UiDragPayload& payload) const
        {
            return openScene && !engine->playWorld() && payload.type == InternedString("asset.scene") &&
                payload.data == 1 &&
                std::ranges::any_of(
                       items, [&](const Item& item)
                       { return item.kind == KIND_SCENE && !item.pending && item.row.name == payload.text; });
        }

        void dropScene(const UiDropEvent& event)
        {
            if (!acceptsScene(event.payload) || !dropAt(event.hit, event.position))
                return;
            const auto item =
                std::ranges::find_if(items, [&](const Item& item)
                                     { return item.kind == KIND_SCENE && item.row.name == event.payload.text; });
            openScene(item->row.path);
        }

        // A refusal told in a toast of the key ("error" is its arg) and in the panel's message line.
        void tellFailed(const char* key, const std::string& error)
        {
            const std::vector<MessageArg> args{{"error", std::string_view(error)}};
            feedback->postToastKey(ToastLevel::Error, key, args);
            setMessage(tr(key, args));
        }

        void openPreview(const Entity key)
        {
            const Item* item = itemOf(key);
            if (!item || !previewable(*item))
                return;
            const char* command = item->kind == KIND_MODEL ? "openModelPreview"
                : item->kind == KIND_MATERIAL              ? "openMaterialPreview"
                : item->kind == KIND_SCENE                 ? "openScenePreview"
                : item->kind == KIND_UI                    ? "openUiPreview"
                                                           : "openImagePreview";
            Json args{{"name", item->row.name}};
            if (item->kind == KIND_TEXTURE || item->kind == KIND_ATLAS)
                args["type"] = TYPE_NAMES[item->kind];
            pending.push_back({Json{{"command", command}, {"args", std::move(args)}}.dump()});
        }

        void activate(const Entity key)
        {
            const Item* item = itemOf(key);
            if (item && previewable(*item))
                openPreview(key);
        }

        // -- requests ----------------------------------------------------------------

        void dispatch()
        {
            while (!pending.empty())
            {
                std::vector<Request> requests;
                requests.swap(pending);
                for (const Request& request : requests)
                {
                    const Json reply = Json::parse(control->handle(request.line), nullptr, false);
                    if (!reply.is_object() || !reply.value("ok", false))
                    {
                        SWARNING_LOG("project: {} -> {}", request.line, reply.dump());
                        const std::string error =
                            reply.is_object() ? reply.value("error", std::string{}) : std::string{};
                        if (request.refused)
                        {
                            request.refused(error);
                        }
                        if (request.failedKey)
                        {
                            tellFailed(request.failedKey, error);
                        }
                        continue;
                    }
                    if (request.failedKey) // a request that was accepted ends the message of an earlier refusal
                    {
                        setMessage({});
                    }
                    if (request.accepted)
                    {
                        request.accepted(reply["result"]);
                    }
                    if (!request.selectCreated)
                    {
                        continue;
                    }
                    const Json& result = reply["result"];
                    Json ids = Json::array();
                    for (const Json& one : result.contains("results") ? result["results"] : Json::array({result}))
                    {
                        ids.push_back(one.value("id", u64{0}));
                    }
                    pending.push_back(
                        {Json{{"command", "select"}, {"args", {{"world", worldName()}, {"ids", ids}}}}.dump()});
                }
            }
        }

        // The path as typed, or the file dialog (glTF only) while the field is empty.
        void requestImport()
        {
            if (pathText.empty())
            {
                pickModel();
                return;
            }
            // a stem that is not a safe bake name (non-ASCII, a space, too long) imports under the name made of it
            Json args{{"path", pathText}};
            const std::string stem = utf8Path(pathFromUtf8(pathText).stem());
            if (const std::string safe = AssetManifest::safeBakeName(stem); safe != stem && !safe.empty())
            {
                args["name"] = safe;
            }
            pending.push_back(
                {Json{{"command", "importModel"}, {"args", std::move(args)}}.dump(), false, IMPORT_KEY.data()});
        }

        // -- control -----------------------------------------------------------------

        std::string setFilter(const ControlArgs& args)
        {
            u8 kind = type;
            if (const std::optional<std::string> named = args.string("type"))
            {
                kind = static_cast<u8>(std::ranges::find(TYPE_NAMES, *named) - TYPE_NAMES.begin());
            }
            useFilter(kind, args.string("text").value_or(text));
            return {};
        }

        void useFilter(const u8 kind, std::string filterText)
        {
            type = kind;
            text = std::move(filterText);
            // the field shows a filter set from elsewhere; typing owns it while focused
            if (scope.router().focused() != filter.entity() && filter.value() != text)
            {
                filter.setText(text);
            }
            showType();
            chipStale = true;
            showChip();
            filterDirty = true;
        }

        std::string setTab(const ControlArgs& args)
        {
            catchUp();
            refreshDetails();
            const std::string tab = args.string("tab").value_or("");
            if (tab == DETAILS_TABS[1] && !importModel.shown)
            {
                return "select a model first (projectSelect): only a model has import settings";
            }
            if (tab == DETAILS_TABS[2] && !materialModel.shown)
            {
                return "select a material first (projectSelect): only a material has an Edit tab";
            }
            importTab = tab == DETAILS_TABS[1];
            editTab = tab == DETAILS_TABS[2];
            materialUsersAt = ~0ull;
            detailsDirty = true;
            detailsView = true;
            refreshDetails();
            return {};
        }

        std::string tableOp(const ControlArgs& args)
        {
            const auto kind = std::ranges::find(OP_NAMES, args.string("kind").value_or(""));
            UiTableOp op{.kind = static_cast<UiTableOpKind>(kind - OP_NAMES.begin())};
            if (const std::optional<std::string> column = args.string("column"); column && !column->empty())
            {
                op.column = InternedString(*column);
            }
            op.descending = args.boolean("descending").value_or(false);
            if (const std::optional<std::string> width = args.json("width"))
            {
                op.width = std::strtof(width->c_str(), nullptr);
            }
            const bool applied = table.apply(op);
            refreshGrid();
            return applied ? std::string{}
                           : std::string("projectTable op refused: unknown column, a column that does not "
                                         "sort or a non-finite width");
        }

        void writeView(IFormatWriter& out) const
        {
            out.writeString("type", TYPE_NAMES[type]);
            out.writeString("text", text);
            out.writeString("message", messageText);
            out.beginArray("queue", static_cast<u32>(jobs.size()));
            for (const ModelImport& job : jobs)
            {
                out.beginObject({});
                out.writeU32("import", job.id);
                out.writeString("name", job.name.toView());
                out.writeString("state", jobToken(job.state));
                out.writeString("error", job.error);
                out.writeString("action",
                                job.state == ModelImport::State::Queued || job.state == ModelImport::State::Baking
                                    ? "cancel"
                                    : job.state == ModelImport::State::Done ? "show"
                                                                            : "retry");
                out.endObject();
            }
            out.endArray();
            out.beginArray("rows", static_cast<u32>(shown.size()));
            for (u32 view = 0; view < shown.size(); ++view)
            {
                const Item& item = items[shown[table.modelRow(view)]];
                out.beginObject({});
                out.writeString("name", item.row.name);
                out.writeString("type", TYPE_NAMES[item.kind]);
                if (item.vfxCard)
                    out.writeBool("vfxCard", true);
                out.writeString("state", item.token);
                out.writeU32("refs", item.row.refs);
                out.writeU32("size", item.row.size);
                out.writeString("path", item.row.path);
                out.writeBool("needsReimport", item.stale);
                out.endObject();
            }
            out.endArray();
            const std::span<const Entity> selected = table.selection();
            out.beginArray("selection", static_cast<u32>(selected.size()));
            for (const Entity key : selected)
            {
                if (const Item* item = itemOf(key))
                {
                    out.beginObject({});
                    out.writeString("name", item->row.name);
                    out.writeString("type", TYPE_NAMES[item->kind]);
                    if (item->vfxCard)
                        out.writeBool("vfxCard", true);
                    out.endObject();
                }
            }
            out.endArray();
            out.beginObject("details");
            out.writeF32("requestedWidth",
                         detailsSlot.isValid() && innerDock.isOpen(InternedString("project.details.panel"))
                             ? detailsSlot.size().x
                             : 0.0f);
            out.writeF32("resolvedWidth",
                         detailsSlot.isValid() && innerDock.isOpen(InternedString("project.details.panel"))
                             ? detailsSlot.size().x
                             : 0.0f);
            out.writeF32("rowsWidth", rowsSlot.isValid() ? rowsSlot.size().x : 0.0f);
            out.writeBool("shown", !detailFacts.empty());
            out.writeString("name", detailsName);
            out.writeString("type", detailsType);
            out.beginArray("facts", static_cast<u32>(detailFacts.size()));
            for (const Fact& fact : detailFacts)
            {
                out.beginObject({});
                out.writeString("id", fact.id);
                out.writeString("key", tr("editor-project.details." + fact.id));
                out.writeString("value", fact.value);
                out.endObject();
            }
            out.endArray();
            out.beginObject("actions");
            out.writeBool("load", detailActions.load);
            out.writeBool("remove", detailActions.remove);
            out.writeString("removeBlocked", removeBlocked);
            out.writeBool("reveal", detailActions.reveal);
            out.endObject();
            out.writeString("preview", previewName);
            out.writeString("tab",
                            importModel.shown && importTab       ? "import"
                                : materialModel.shown && editTab ? "edit"
                                                                 : "details");
            importView->write(out);
            materialView->write(out);
            out.endObject();
            out.beginObject("stats");
            out.writeU64("reads", stats.reads);
            out.writeU64("refreshes", stats.refreshes);
            out.writeU64("materialReads", stats.materialReads);
            out.writeU64("settingsReads", stats.settingsReads);
            out.writeU64("editReads", stats.editReads);
            out.writeU64("userScans", stats.userScans);
            out.endObject();
        }

        void registerCommands()
        {
            control->registerViewCommand(
                "projectFilter", OWNER, "filters the project panel's assets by type and name",
                {optionalArg("type", ArgKind::String, "a Project asset type or all", "unchanged", TYPE_NAMES),
                 optionalArg("text", ArgKind::String, "the text names contain", "unchanged")},
                [this](const ControlArgs& args, IFormatWriter&) { return setFilter(args); });
            control->registerViewCommand(
                "projectTable", OWNER, "sorts the project table or resizes one of its columns",
                {requiredArg("kind", ArgKind::String, "sort|resize", OP_NAMES),
                 optionalArg("column", ArgKind::String, "one of name, type, state, refs, size or path",
                             "none (unsorted for sort)", COLUMN_NAMES),
                 optionalArg("descending", ArgKind::Boolean, "sort descending", "false"),
                 optionalArg("width", ArgKind::Number, "the column's new width, for resize", "the minimum")},
                [this](const ControlArgs& args, IFormatWriter&) { return tableOp(args); });
            control->registerViewCommand("projectDetailsWidth", OWNER,
                                         "sets the Project details pane width for this session",
                                         {requiredArg("width", ArgKind::Number, "width in UI pixels")},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         {
                                             const auto width = args.json("width");
                                             if (!width)
                                                 return std::string("width is required");
                                             const float value = std::strtof(width->c_str(), nullptr);
                                             if (!std::isfinite(value))
                                                 return std::string("width must be finite");
                                             resizeDetails(value);
                                             return std::string{};
                                         });
            control->registerQuery(
                "projectView", OWNER,
                "the project panel's filter, shown rows, selection, the selected asset's details (facts, actions, "
                "preview), import queue, inline message and read stats. details.requestedWidth and "
                "details.resolvedWidth both report the current laid-out details pane width (zero while closed); "
                "details.rowsWidth is the browser pane width, all in UI units. Imported flipbook cards report "
                "type=model and vfxCard=true and also match the vfx filter",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    catchUp(); // live while the panel is hidden or a filter just changed
                    for (const u32 index : shown)
                    {
                        readStatus(items[index]);
                    }
                    refreshDetails();
                    writeView(out);
                    return std::string{};
                });
            control->registerViewCommand(
                "projectSelect", OWNER,
                "selects an asset's row of the project panel (the filter takes its type and name), which shows its "
                "details",
                {requiredArg("name", ArgKind::String, "the asset's name"),
                 optionalArg("type", ArgKind::String, "a Project asset type", "any type",
                             std::span<const std::string_view>(TYPE_NAMES).subspan(1))},
                [this](const ControlArgs& args, IFormatWriter&) { return selectAsset(args); });
            control->registerViewCommand(
                "projectDetailsTab", OWNER,
                "shows the details pane's Details, Import settings (a selected model's importer options, Reimport and "
                "Reset) or Edit tab (a selected material's schema fields: colors, factors, alpha mode, double sided; a "
                "texture picker per slot, Duplicate and who uses it)",
                {requiredArg("tab", ArgKind::String, "details|import|edit", DETAILS_TABS)},
                [this](const ControlArgs& args, IFormatWriter&) { return setTab(args); });
            control->registerViewCommand(
                "projectReveal", OWNER,
                "shows the selected asset's file in the file manager: the details pane's Reveal button; at most one a "
                "second",
                {}, [this](const ControlArgs&, IFormatWriter& out) { return reveal(out); });
        }

        // The imports in flight are the jobs "import"; one that settled since the last look is told in a toast, whoever
        // started it and whether or not the panel is shown. Imports that finish in one pass share one toast; a failure
        // has its own.
        void reportImports()
        {
            for (auto it = imageImports.begin(); it != imageImports.end();)
            {
                const Json reply = Json::parse(
                    control->handle(Json{{"query", "externalImageImport"}, {"args", {{"import", *it}}}}.dump()),
                    nullptr, false);
                if (reply.is_object() && reply.value("ok", false) && reply["result"].value("status", "") == "baking")
                {
                    ++it;
                    continue;
                }
                if (reply.is_object() && reply.value("ok", false) && reply["result"].value("status", "") == "ready")
                {
                    pending.push_back({Json{{"command", "publishExternalImage"}, {"args", {{"import", *it}}}}.dump(),
                                       false,
                                       IMPORT_KEY.data(),
                                       {},
                                       [this](const Json& result)
                                       {
                                           if (result.value("status", "") != "done")
                                           {
                                               tellFailed(IMPORT_KEY.data(), result.value("error", ""));
                                               return;
                                           }
                                           const std::string name = result.value("name", "");
                                           feedback->postToastKey(
                                               ToastLevel::Info, "editor-project.import.imageDone",
                                               std::vector<MessageArg>{{"name", std::string_view(name)}});
                                       }});
                    it = imageImports.erase(it);
                    continue;
                }
                if (reply.is_object() && reply.value("ok", false) && reply["result"].value("status", "") == "done")
                {
                    const std::string name = reply["result"].value("name", "");
                    feedback->postToastKey(ToastLevel::Info, "editor-project.import.imageDone",
                                           std::vector<MessageArg>{{"name", std::string_view(name)}});
                }
                else
                    tellFailed(IMPORT_KEY.data(),
                               !reply.is_object()             ? std::string{}
                                   : reply.value("ok", false) ? reply["result"].value("error", "")
                                                              : reply.value("error", ""));
                it = imageImports.erase(it);
            }
            for (auto it = audioImports.begin(); it != audioImports.end();)
            {
                const Json reply = Json::parse(
                    control->handle(Json{{"query", "externalAudioImport"}, {"args", {{"import", *it}}}}.dump()),
                    nullptr, false);
                if (reply.is_object() && reply.value("ok", false) && reply["result"].value("status", "") == "copying")
                {
                    ++it;
                    continue;
                }
                if (reply.is_object() && reply.value("ok", false) && reply["result"].value("status", "") == "done")
                {
                    const std::string name = reply["result"].value("name", "");
                    const Json& result = reply["result"];
                    feedback->postToastKey(
                        ToastLevel::Info, "editor-project.import.audioDone",
                        std::vector<MessageArg>{{"name", std::string_view(name)},
                                                {"channels", result.value("channels", int64_t{0})},
                                                {"sampleRate", result.value("sampleRate", int64_t{0})},
                                                {"frames", result.value("frames", int64_t{0})}});
                }
                else
                    tellFailed(IMPORT_KEY.data(),
                               !reply.is_object()             ? std::string{}
                                   : reply.value("ok", false) ? reply["result"].value("error", "")
                                                              : reply.value("error", ""));
                it = audioImports.erase(it);
            }
            const u64 generation = engine->assets().importsGeneration();
            if (generation == reportedGeneration)
            {
                return;
            }
            reportedGeneration = generation;
            u32 running = 0;
            u32 done = 0;
            bool laterDone = false; // a job that settled and started after the message
            InternedString doneName; // the last one that settled, and its bake time
            double doneMs = 0.0;
            bool doneReimport = false;
            for (const ModelImport& job : engine->assets().imports())
            {
                for (auto it = placements.begin(); it != placements.end();)
                {
                    if (it->import != job.id || job.state == ModelImport::State::Queued ||
                        job.state == ModelImport::State::Baking)
                    {
                        ++it;
                        continue;
                    }
                    if (job.state == ModelImport::State::Done)
                        pending.push_back({Json{{"command", "instantiateModel"},
                                                {"args",
                                                 {{"model", std::string(job.name.toView())},
                                                  {"world", it->world},
                                                  {"transform", {{"position", it->point}}}}}}
                                               .dump(),
                                           true, DROP_FAILED_KEY.data()});
                    it = placements.erase(it);
                }
                running += job.state == ModelImport::State::Queued || job.state == ModelImport::State::Baking;
                if ((job.state != ModelImport::State::Done && job.state != ModelImport::State::Failed) ||
                    !reportedImports.insert(job.id).second)
                {
                    continue;
                }
                if (job.state == ModelImport::State::Done)
                {
                    laterDone = laterDone || job.id > messageAfter;
                    ++done;
                    doneName = job.name;
                    doneMs = job.bakeMs;
                    doneReimport = job.reimport;
                    continue;
                }
                const std::vector<MessageArg> args{{"name", job.name.toView()}, {"error", std::string_view(job.error)}};
                feedback->postToastKey(ToastLevel::Error, "editor-project.import.failedNamed", args);
            }
            if (laterDone) // a success, whoever started it, ends the message of an earlier refusal
            {
                setMessage({});
            }
            if (done == 1)
            {
                const std::string ms = std::format("{:.0f}", doneMs);
                feedback->postToastKey(
                    ToastLevel::Info, doneReimport ? "editor-project.reimport.done" : "editor-project.import.done",
                    std::vector<MessageArg>{{"name", doneName.toView()}, {"ms", std::string_view(ms)}});
            }
            else if (done > 1)
            {
                feedback->postToastKey(ToastLevel::Info, "editor-project.import.doneMany",
                                       std::vector<MessageArg>{{"count", static_cast<std::int64_t>(done)}});
            }
            feedback->reportJobs("import", running);
        }

        // Re-reads the manifest only when it or the imports changed; the rows' status and references when a command
        // ran or a load settled.
        void catchUp()
        {
            const AssetRuntime& assets = engine->assets();
            const u64 manifest = assets.manifest().generation();
            const u64 imports = assets.importsGeneration();
            const u64 controllers = controllerRevision();
            if (!synced || manifest != seenManifest || imports != seenImports || controllers != seenControllers)
            {
                synced = true;
                seenManifest = manifest;
                seenImports = imports;
                seenControllers = controllers;
                seenStatus = signature();
                SPITE_ZONE("EditorProject.readModel");
                readModel();
                readQueue();
            }
            else if (const StatusSignature status = signature(); !(status == seenStatus))
            {
                const bool commandRan = status.commands != seenStatus.commands;
                seenStatus = status;
                refreshStatus(commandRan);
            }
            if (filterDirty)
            {
                applyFilter();
            }
        }

        void follow()
        {
            if (!isShown())
            {
                return; // the generations stay unseen, so showing (or a projectView query) catches up
            }
            if (localization && localization->revision() != seenLocale)
            {
                relocalize();
            }
            keepFields();
            if (typeKeyNow() != typeKey)
            {
                sizeTypes();
            }
            showChip();
            showMessage();
            catchUp();
            showQueue();
            const u32 columns = std::clamp(static_cast<u32>(std::max(1.0f, grid.size().x / gridTileWidth)), 1u, 8u);
            if (columns != gridColumns || gridWidth != grid.size().x)
            {
                gridColumns = columns;
                gridWidth = grid.size().x;
                refreshGrid();
            }
            if (!selectName.empty())
            {
                // the filter command ran before this update's follow: the asset's row is the shown one
                const auto found = std::ranges::find_if(
                    items, [this](const Item& item) { return item.kind == selectKind && item.row.name == selectName; });
                if (found != items.end())
                {
                    table.setSelection(std::array{found->key});
                    refreshGrid();
                    table.scrollTo(found->key);
                    setMessage({});
                }
                else
                {
                    setMessage(tr("editor-project.queue.gone", std::vector<MessageArg>{{"name", selectName}}));
                }
                selectName.clear();
            }
            // a document apply rewrites the slot's visibility as authored; a resize or a wider key column re-cuts
            detailsView = detailsView ||
                (detailsSlot.isValid() &&
                 (innerDock.isOpen(InternedString("project.details.panel")) == detailFacts.empty() ||
                  (!detailFacts.empty() && std::abs(facts->lastCellWidth() - cutWidth) > 0.5f)));
            refreshDetails();
            showDetails();
            if (importTab && importModel.shown)
            {
                importView->fit();
            }
            if (editTab && materialModel.shown)
            {
                materialView->tick();
                materialView->fit();
            }
        }
    };

    EditorProject::EditorProject(Engine& engine, const DockSpace& dock, const Widget& hierarchy,
                                 EditorFeedback& feedback, EditorDialogs& dialogs,
                                 std::function<void(std::string_view)> openScene) :
        m_state(std::make_unique<State>(engine, dock, hierarchy, feedback, dialogs, std::move(openScene)))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.loadTexts();
        if (engine.services().contains<ApplicationEventBus>())
        {
            state.drops = engine.services().get<ApplicationEventBus>().subscribe<FileDropEvent>(
                [&state](const FileDropEvent& event) { state.dropped.push_back(event); });
        }
        state.queue = std::make_unique<Table>(
            scope,
            TableDesc{.id = "project.queue.list",
                      .columns = {150.0f},
                      .cells = 2,
                      .buttonWidth = 74.0f,
                      .read = [&state](const u32 row, const std::span<std::string> cells, std::string& button)
                      { state.readJob(row, cells, button); },
                      .press = [&state](const u32 row) { state.pressJob(row); },
                      .buttonName = [&state](const u32 row) { return state.jobButtonName(row); }});
        state.queue->list.setLayoutItem(fillItem());

        state.tabs =
            state.density.track(stackPanel(scope, "project.tabs", Vec2{}, UiLayoutDirection::Row), HEIGHT_CONTROL);
        state.tabs.setVisible(false);
        const auto tab = [&](const char* id, const std::string_view key, const u32 target)
        {
            Button button = state.density.track(scope.createButton(id, "", fitButtonDesc()), HEIGHT_CONTROL);
            bindFitPadding(button, SPACE_MD, SPACE_NONE);
            button.setStyleClass(InternedString("project.import"));
            button.setLocalizedText(InternedString("editor-project.tab." + std::string(key)));
            button.onClick(
                [&state, target]
                {
                    state.pending.push_back(
                        {Json{{"command", "projectDetailsTab"}, {"args", {{"tab", std::string(DETAILS_TABS[target])}}}}
                             .dump()});
                });
            button.setParent(state.tabs);
            return button;
        };
        state.tabDetails = tab("project.tab.details", "details", 0);
        state.tabImport = tab("project.tab.import", "import", 1);
        state.tabImport.setVisible(false);
        state.tabEdit = tab("project.tab.edit", "edit", 2);
        state.tabEdit.setVisible(false);
        state.importView = std::make_unique<ProjectImportView>(
            scope, state.density, [&state](const std::string_view key, const std::span<const MessageArg> args)
            { return state.tr(key, args); },
            [&state](std::string line, const char* failedKey, std::function<void(const std::string&)> refused)
            {
                state.pending.push_back({std::move(line), false, failedKey,
                                         [&state, refused = std::move(refused)](const std::string& error)
                                         {
                                             refused(error);
                                             state.importDirty = true;
                                             state.detailsView = true;
                                         }});
            });
        state.materialView = std::make_unique<ProjectMaterialView>(
            scope, state.density, [&state](const std::string_view key, const std::span<const MessageArg> args)
            { return state.tr(key, args); },
            [&state](std::string line, const char* failedKey, std::function<void(const std::string&)> refused)
            {
                state.pending.push_back({std::move(line), false, failedKey,
                                         [&state, refused = std::move(refused)](const std::string& error)
                                         {
                                             refused(error);
                                             state.materialDirty = true;
                                             state.detailsView = true;
                                         }});
            },
            [&state] { return state.textureNames(); }, [&state] { state.duplicateMaterial(); });
        state.body = stackPanel(scope, "project.body", Vec2{}, UiLayoutDirection::Row, UiLayoutAlign::Stretch);
        state.body.setLayoutItem(fillItem());
        state.previewFrame = scope.createPanel("project.previewFrame", barDesc(Vec2{PREVIEW_SIZE, 0.0f}));
        state.previewFrame.setVisible(false);
        state.previewFrame.setParent(state.body);
        state.preview = scope.createImage("project.preview", topLeft({}, Vec2{PREVIEW_SIZE, PREVIEW_SIZE}));
        state.preview.setImageSlot(PREVIEW_IMAGE_SLOT);
        state.preview.setAccessibleNameKey(InternedString("editor-project.access.preview"));
        state.preview.setParent(state.previewFrame);
        Panel factsColumn =
            stackPanel(scope, "project.facts", Vec2{}, UiLayoutDirection::Column, UiLayoutAlign::Stretch);
        factsColumn.setLayoutItem(fillItem());
        bindPadding(factsColumn, SPACE_MD, SPACE_NONE, SPACE_MD, SPACE_NONE);
        factsColumn.setParent(state.body);
        state.facts = std::make_unique<Table>(
            scope,
            TableDesc{.id = "project.details.list",
                      .columns = {110.0f},
                      .cells = 2,
                      .read = [&state](const u32 row, const std::span<std::string> cells, std::string&)
                      {
                          if (row < state.rows.size())
                          {
                              const State::Fact& fact = state.rows[row];
                              cells[0] =
                                  fact.id.empty() ? std::string{} : state.tr("editor-project.details." + fact.id);
                              cells[1] = fact.value;
                          }
                      }});
        state.facts->list.setLayoutItem(fillItem());
        state.facts->list.setParent(factsColumn);
        state.materialLinks =
            std::make_unique<EditorMaterialLinks>(*state.engine, scope, "project.materialLinks", 0x53504d4c00000000ull);
        state.materialLinks->root().setParent(factsColumn);
        state.materialLinks->onOpen(
            [&state](std::string_view name)
            {
                state.pending.push_back(
                    {Json{{"command", "openMaterialPreview"}, {"args", {{"name", std::string(name)}}}}.dump()});
            });
        state.root = scope.createPanel("editor.panel", columnDesc());
        for (u32 i = 0; i < state.types.size(); ++i)
        {
            state.types[i] = state.density.track(
                scope.createButton("project.type", state.typeTitles[i], fitButtonDesc()), HEIGHT_CONTROL);
            bindFitPadding(state.types[i], SPACE_MD, SPACE_NONE);
            state.types[i].onClick(
                [&state, i]
                {
                    state.pending.push_back(
                        {Json{{"command", "projectFilter"}, {"args", {{"type", std::string(TYPE_NAMES[i])}}}}.dump()});
                });
        }

        UiTableDesc desc;
        desc.rect = topLeft();
        desc.rect.layoutItem = fillItem(); // fills the slot the document lays out
        desc.schema = &ProjectRow::spiteSchema();
        desc.rowClass = InternedString(ROW_CLASS);
        desc.columns = {column("name", 168.0f, true), column("type", 108.0f), column("state", 110.0f),
                        column("refs", 80.0f),        column("size", 112.0f), column("path", 300.0f)};
        desc.row = [&state](const u32 row) -> const void* { return state.rowOf(row); };
        desc.key = [&state](const u32 row) { return state.items[state.shown[row]].key; };
        desc.decoration = [&state](u32 row) { return state.listDecoration(row); };
        desc.rowHeight = std::max(32.0f, state.density.density().row);
        desc.headerHeight = state.density.density().control;
        desc.padding = state.density.density().xs;
        state.table = scope.createTable("project.table", std::move(desc));
        state.density.apply([&state](const Density& now)
                            { state.table.setMetrics(std::max(32.0f, now.row), now.control, now.xs); });
        UiVirtualListDesc gridDesc;
        gridDesc.rect = topLeft();
        gridDesc.rect.layoutItem = fillItem();
        gridDesc.rowHeight = 100.0f;
        gridDesc.createRow = [&state](u32 slot) { return state.createGridRow(slot); };
        gridDesc.bindRow = [&state](u32 slot, u32 row) { state.bindGridRow(slot, row); };
        state.grid = scope.createVirtualList("project.grid", std::move(gridDesc));
        state.density.apply(
            [&state](const Density& now)
            {
                state.gridTileWidth = now.control * 4.0f;
                state.gridWidth = -1.0f;
                state.grid.setRowHeight(now.control * 3.0f);
                state.refreshGrid();
            });
        state.table.onSelect([&state](std::span<const Entity>) { state.refreshGrid(); });
        state.table.onActivate(
            [&state](Entity key, InternedString column)
            {
                const auto* item = state.itemOf(key);
                if (!item)
                    return;
                if (!column.isValid() || item->kind > KIND_UI)
                    state.activate(key);
            });
        state.table.onDoubleClick([&state](Entity key) { state.openPreview(key); });
        state.table.setAccessibleNameKey(InternedString("editor.access.projectTable"));
        state.table.onContextMenu([&state](Entity key, Vec2) { state.openAssetMenu(key, state.table); });
        state.table.onRequest(
            [&state](const UiTableOp& op)
            {
                Json args{{"kind", std::string(OP_NAMES[static_cast<u8>(op.kind)])},
                          {"column", op.column.isValid() ? std::string(op.column.toView()) : std::string{}}};
                if (op.kind == UiTableOpKind::Sort)
                {
                    args["descending"] = op.descending;
                }
                else
                {
                    args["width"] = op.width;
                }
                state.pending.push_back({Json{{"command", "projectTable"}, {"args", std::move(args)}}.dump()});
            });
        state.table.onDragRows([&state](const std::span<const Entity> keys, const Vec2 position, const bool released)
                               { state.dragRows(keys, position, released); });

        // the table starts drags of the assets it lists: by pointer through dragRows, over control from its selection
        UiDragDrop& dragDrop = scope.dragDrop();
        dragDrop.addSource(state.table.entity(), [&state] { return state.payloadOf(state.table.selection()); }, false);
        // Model instantiation and scene opening share the hierarchy/Scene-view drop destinations.
        dragDrop.addTarget(
            state.dock.entity(),
            [&state](const UiDragPayload& payload, const Entity hit, const Vec2 position)
            {
                return (payload.type == InternedString(MODEL_DRAG_PAYLOAD) || state.acceptsScene(payload)) &&
                    state.dropAt(hit, position).has_value();
            },
            [&state](const UiDropEvent& event)
            {
                if (event.payload.type == InternedString("asset.scene"))
                    state.dropScene(event);
                else
                    state.dropModels(event);
            });

        state.createPopup = scope.createPopupList("project.create.popup", {.maxRows = 4});
        state.createPopup.onChoose(
            [&state](const u32 index)
            {
                if (index == 0)
                    state.newMaterial();
            });
        state.assetPopup = scope.createPopupList("project.asset.popup",
                                                 {.rect = UiRectDesc{.size = Vec2{320.0f, 0.0f}}, .maxRows = 6});
        state.assetPopup.onChoose(
            [&state](const u32 index)
            {
                if (index >= state.assetMenuActions.size())
                    return;
                const int action = state.assetMenuActions[index];
                if (state.table.selection().size() != 1 || state.table.selection()[0] != state.assetMenuKey)
                    return;
                state.refreshDetails();
                if (action == 0 && state.openScene)
                {
                    if (const auto* item = state.itemOf(state.table.selection()[0]); item && item->kind == KIND_SCENE)
                        state.openScene(item->row.path);
                }
                else if (action == 1 && state.table.selection().size() == 1)
                    state.loadSelected();
                else if (action == 2 && state.table.selection().size() == 1)
                    state.pending.push_back({R"({"command":"projectReveal"})"});
                else if (action == 3 && state.table.selection().size() == 1)
                    state.duplicateMaterial();
                else if (action == 4 && state.table.selection().size() == 1)
                    state.removeSelected();
                else if (action == 5 && state.table.selection().size() == 1)
                    state.openPreview(state.table.selection()[0]);
                else if (action == 6)
                {
                    const auto* item = state.itemOf(state.table.selection()[0]);
                    if (!item)
                        return;
                    const auto& manifest = state.engine->assets().manifest();
                    const InternedString name(item->row.name);
                    InternedString owner;
                    if (item->kind == KIND_CLIP)
                    {
                        if (const auto clip = manifest.getAnimationEntry(name))
                            owner = clip->model;
                    }
                    else if (const auto skeleton = manifest.getSkeletonEntry(name))
                    {
                        owner = skeleton->model;
                    }
                    if (owner.isValid())
                    {
                        state.selectName = owner.toView();
                        state.selectKind = KIND_MODEL;
                        state.useFilter(KIND_MODEL, state.selectName);
                    }
                }
            });
        UiDockDesc innerDesc;
        innerDesc.rect = topLeft();
        innerDesc.rect.layoutItem = fillItem();
        innerDesc.showChrome = false;
        state.innerSplitterSize = state.density.density().md;
        innerDesc.splitterSize = state.innerSplitterSize;
        state.innerDock = scope.createDock("project.dock", innerDesc);
        state.density.apply(
            [&state](const Density& now)
            {
                state.innerSplitterSize = now.md;
                state.innerDock.setSplitterSize(state.innerSplitterSize);
            });
        Panel browserHost = scope.createPanel(InternedString(ROWS_SLOT), columnDesc());
        Panel detailsHost = scope.createPanel(InternedString(DETAILS_SLOT), columnDesc());
        UiSlotComponent rowsFields;
        rowsFields.scroll = true;
        rowsFields.childClass = InternedString("project.dense");
        scope.world().addComponent<UiSlotComponent>(browserHost.entity(), rowsFields);
        UiSlotComponent detailsFields;
        detailsFields.childClass = InternedString("project.dense");
        scope.world().addComponent<UiSlotComponent>(detailsHost.entity(), detailsFields);
        state.fillRows(browserHost);
        state.fillDetails(detailsHost);
        state.innerDock.addPanel(InternedString("project.browser"), "Browser", browserHost);
        state.innerDock.addPanel(InternedString("project.details.panel"), "Details", detailsHost);
        state.innerDock.setPanelMinSize(InternedString("project.browser"), Vec2{120.0f, 0.0f});
        state.innerDock.setPanelMinSize(InternedString("project.details.panel"), Vec2{120.0f, 0.0f});
        state.innerDock.apply(UiDockOp{.kind = UiDockOpKind::Dock, .panel = InternedString("project.browser")});
        state.innerDock.onRequest(
            [&state](const UiDockOp& op)
            {
                if (op.kind != UiDockOpKind::Resize)
                    return;
                const float width =
                    (1.0f - op.ratio) * std::max(0.0f, state.innerDock.size().x - state.innerSplitterSize);
                state.pending.push_back(
                    {Json{{"command", "projectDetailsWidth"}, {"args", {{"width", width}}}}.dump()});
            });
        state.showType();
        state.registerCommands();
        scope.commit();

        // the code's contract: the slots (type buttons, table) and the bars' fields; without a readable document, the
        // fallback builds the default panel
        UiViewContract contract;
        contract.slot(InternedString(TYPES_SLOT), [&state](const Widget& slot) { state.fillTypes(slot); });
        contract.slot(InternedString("project.main"),
                      [&state](const Widget& slot) { putIn(state.scope, slot, state.innerDock); });
        contract.optional(InternedString("project.gridView"), UiWidgetKind::Button,
                          [&state](const Widget& element)
                          {
                              state.gridButton = element.as<Button>();
                              state.gridButton.setText("");
                              if (auto* fit =
                                      state.scope.world().tryGetComponent<UiContentSizeComponent>(element.entity()))
                                  fit->fitWidth = false;
                              state.gridButton.setSize(Vec2{30.0f, 30.0f});
                              state.gridButton.setIcon("squares-four");
                              state.gridButton.setAccessibleNameKey(InternedString("editor-project.view.grid"));
                              state.gridButton.setTooltip(InternedString("editor-project.view.grid"));
                              state.gridButton.onClick(
                                  [&state]
                                  {
                                      state.gridMode = true;
                                      state.showGridMode();
                                  });
                              state.gridButton.setAccessibleSelected(state.gridMode);
                          });
        contract.optional(InternedString("project.listView"), UiWidgetKind::Button,
                          [&state](const Widget& element)
                          {
                              state.listButton = element.as<Button>();
                              state.listButton.setText("");
                              if (auto* fit =
                                      state.scope.world().tryGetComponent<UiContentSizeComponent>(element.entity()))
                                  fit->fitWidth = false;
                              state.listButton.setSize(Vec2{30.0f, 30.0f});
                              state.listButton.setIcon("list");
                              state.listButton.setAccessibleNameKey(InternedString("editor-project.view.list"));
                              state.listButton.setTooltip(InternedString("editor-project.view.list"));
                              state.listButton.onClick(
                                  [&state]
                                  {
                                      state.gridMode = false;
                                      state.showGridMode();
                                  });
                              state.listButton.setAccessibleSelected(!state.gridMode);
                          });
        contract.require(InternedString(FILTER_FIELD), UiWidgetKind::TextInput,
                         [&state](const Widget& element) { state.bindFilter(element); });
        contract.optional(InternedString("project.clearSearch"), UiWidgetKind::Button,
                          [&state](const Widget& element) { state.bindClearSearch(element); });
        contract.require(InternedString(IMPORT_BUTTON), UiWidgetKind::Button,
                         [&state](const Widget& element) { state.bindImport(element); });
        contract.optional(InternedString(NEW_MATERIAL_BUTTON), UiWidgetKind::Button,
                          [&state](const Widget& element) { state.bindNewMaterial(element); });
        contract.optional(InternedString(CHIP), UiWidgetKind::Button,
                          [&state](const Widget& element) { state.bindChip(element); });
        contract.optional(InternedString(MESSAGE), UiWidgetKind::Label,
                          [&state](const Widget& element) { state.bindMessage(element); });
        contract.slot(InternedString(QUEUE_SLOT), [&state](const Widget& slot) { state.fillQueue(slot); });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                Widget panel = root;
                panel.setPivot(Vec2{0.0f, 0.0f});
                panel.setLayout(stack(UiLayoutDirection::Column, 0.0f, {}, UiLayoutAlign::Stretch));
                panel.setLayoutItem(fillItem());
                const Density density = Density::read(viewScope);
                const float fieldHeight = density.control;
                const auto bar = [&](const std::string_view id) { return toolbarPanel(viewScope, id, root, SPACE_MD); };
                const auto field = [&](const std::string_view id, const Widget& parent)
                {
                    UiRectDesc desc = topLeft({}, Vec2{0.0f, fieldHeight});
                    desc.layoutItem = fillItem();
                    TextInput input = viewScope.createTextInput(InternedString(id), desc);
                    input.setStyleClass(InternedString("project.field"));
                    input.setParent(parent);
                    return input;
                };
                auto topDesc = barDesc(
                    {},
                    stack(UiLayoutDirection::Row, density.md, Vec4{density.xs, density.xxs, density.xs, density.xxs}));
                topDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                Panel top = viewScope.createPanel("project.bar", topDesc);
                bindGap(top, SPACE_MD);
                bindPadding(top, SPACE_XS, SPACE_XXS, SPACE_XS, SPACE_XXS);
                top.setParent(root);
                auto typesLayout = stack(UiLayoutDirection::Row, density.xs);
                typesLayout.wrap = true;
                UiRectDesc typesDesc = barDesc({}, typesLayout);
                typesDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                typesDesc.layoutItem = fillItem();
                Panel types = viewScope.createPanel(InternedString(TYPES_SLOT), typesDesc);
                bindGap(types, SPACE_XS);
                types.setParent(top);
                for (const auto& [id, key] : {std::pair{"project.gridView", "editor-project.view.grid"},
                                              std::pair{"project.listView", "editor-project.view.list"}})
                {
                    Button button = viewScope.createButton(InternedString(id), "", fitButtonDesc(fieldHeight));
                    bindFitPadding(button, SPACE_MD, SPACE_NONE);
                    button.setStyleClass(InternedString("project.import"));
                    button.setLocalizedText(InternedString(key));
                    button.setParent(top);
                }
                // the filter box and its chip are one group, the import path box and its buttons another: a row of
                // two groups, set apart by the wider gap
                const Panel imports = bar("project.importBar");
                bindGap(imports, SPACE_LG);
                bindPadding(imports, SPACE_XS, SPACE_NONE, SPACE_XS, SPACE_XS); // the focus ring's room below
                const auto group = [&](const std::string_view id)
                {
                    UiRectDesc desc = barDesc(Vec2{0.0f, fieldHeight},
                                              stack(UiLayoutDirection::Row, density.xs, {}, UiLayoutAlign::Stretch));
                    desc.layoutItem = fillItem();
                    if (id == "project.importGroup")
                        desc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = false};
                    Panel panel = viewScope.createPanel(InternedString(id), desc);
                    bindGap(panel, SPACE_XS);
                    panel.setParent(imports);
                    return panel;
                };
                const Panel filterGroup = group("project.filterGroup");
                Panel importGroup = group("project.importGroup");
                importGroup.setLayoutItem(UiLayoutItemComponent{.shrink = 0.0f});
                field(FILTER_FIELD, filterGroup).setTooltip(InternedString("editor-project.tip.filter"));
                Button clearSearch = viewScope.createButton("project.clearSearch", "", fitButtonDesc(fieldHeight));
                bindFitPadding(clearSearch, SPACE_MD, SPACE_NONE);
                clearSearch.setStyleClass(InternedString("project.import"));
                clearSearch.setLocalizedText(InternedString("editor-project.search.clear"));
                clearSearch.setParent(filterGroup);
                UiRectDesc chipDesc = fitButtonDesc(fieldHeight);
                chipDesc.layoutItem->maxSize = Vec2{CHIP_MAX_WIDTH, 0.0f};
                Button chip = viewScope.createButton(InternedString(CHIP), "", chipDesc);
                bindFitPadding(chip, SPACE_MD, SPACE_NONE);
                chip.setStyleClass(InternedString("project.import"));
                chip.setLocalizedText(InternedString("editor-project.chip.none"));
                chip.setTooltip(InternedString("editor-project.tip.chip"));
                chip.setParent(filterGroup);
                Button importButton =
                    viewScope.createButton(InternedString(IMPORT_BUTTON), "", fitButtonDesc(fieldHeight));
                bindFitPadding(importButton, SPACE_MD, SPACE_NONE);
                importButton.setTooltip(InternedString("editor-project.tip.import"));
                importButton.setStyleClass(InternedString("project.import"));
                importButton.setLocalizedText(InternedString("editor-project.import"));
                importButton.setParent(importGroup);
                Button newMaterial =
                    viewScope.createButton(InternedString(NEW_MATERIAL_BUTTON), "", fitButtonDesc(fieldHeight));
                bindFitPadding(newMaterial, SPACE_MD, SPACE_NONE);
                newMaterial.setTooltip(InternedString("editor-project.tip.create"));
                newMaterial.setStyleClass(InternedString("project.import"));
                newMaterial.setLocalizedText(InternedString("editor-project.create"));
                newMaterial.setParent(importGroup);
                // hidden until there is a message and until there are imports: the code shows them
                Label message = viewScope.createLabel(InternedString(MESSAGE), "", barDesc(Vec2{0.0f, 18.0f}));
                message.setVisible(false);
                message.setParent(root);
                Panel queue =
                    viewScope.createPanel(InternedString(QUEUE_SLOT),
                                          barDesc(Vec2{0.0f, QUEUE_HEIGHT},
                                                  stack(UiLayoutDirection::Column, 0.0f, {}, UiLayoutAlign::Stretch)));
                queue.setVisible(false);
                queue.setParent(root);
                Panel main = viewScope.createPanel(InternedString("project.main"), columnDesc());
                main.setLayoutItem(fillItem());
                main.setParent(root);
            });
        state.view = std::make_unique<UiDocumentView>(editorUi(engine).ui(), std::string(VIEW), std::move(contract),
                                                      state.root.entity());
        if (!state.view->load(resolveContentPath(DOCUMENT)))
        {
            SWARNING_LOG("project: {} did not apply ({} problems): the default panel shows", DOCUMENT,
                         state.view->problems().size());
        }
    }

    EditorProject::~EditorProject()
    {
        m_state->clearGridPreviews();
        m_state->control->unregisterOwner(OWNER);
        if (!m_state->scope.isShutdown()) // the dock outlives this panel: its target calls into the state
        {
            m_state->scope.dragDrop().forget(m_state->dock.entity());
        }
    }

    Widget EditorProject::content() const { return m_state->root; }

    void EditorProject::setReveal(std::function<bool(const std::filesystem::path&)> reveal,
                                  std::function<std::uint64_t()> nowMs)
    {
        m_state->revealFile = std::move(reveal);
        if (nowMs)
        {
            m_state->revealClock = std::move(nowMs);
        }
    }

    void EditorProject::update()
    {
        if (!m_state->dropped.empty())
        {
            for (const FileDropEvent& event : std::exchange(m_state->dropped, {}))
            {
                const Window* main = m_state->engine->window();
                const Window* window =
                    main && event.windowId == main->getId() ? main : m_state->engine->secondaryWindow(event.windowId);
                if (main && (!window || main->getLogicalExtent().width == 0))
                    continue;
                if (!main && event.windowId != INVALID_WINDOW_ID)
                    continue;
                Vec2 position{event.x, event.y};
                u64 windowId = 0;
                if (main)
                {
                    const float scale = static_cast<float>(main->getFramebufferExtent().width) /
                        static_cast<float>(main->getLogicalExtent().width);
                    const auto at = window->getPosition();
                    const auto origin = main->getPosition();
                    position = Vec2{(event.x + static_cast<float>(at.x - origin.x)) * scale,
                                    (event.y + static_cast<float>(at.y - origin.y)) * scale};
                    windowId = window == main ? 0 : event.windowId;
                }
                const Entity hit = m_state->scope.hierarchy().hitTest(position, windowId);
                const auto drop = m_state->dropAt(hit, position);
                if (drop && drop->scene)
                    m_state->importExternalDropped(std::string(event.filePath.toView()), position);
                else if (m_state->isShown())
                {
                    const Entity host = m_state->dock.panelHost(InternedString("project"));
                    for (Entity at = hit; at.isValid(); at = m_state->scope.hierarchy().parentOf(at))
                        if (at == host)
                        {
                            m_state->importDropped(std::string(event.filePath.toView()));
                            break;
                        }
                }
            }
        }
        m_state->dispatch();
        m_state->reportImports();
        m_state->follow();
        m_state->updateGridPreviews();
        if (m_state->isShown())
            m_state->materialLinks->update();
    }

    namespace
    {
        // the bottom group's content keeps two bars and six rows: the Project panel's bars, a list and its details
        // settle into it at 1024x600 beside the Scene view's minimum (the density's sizes:)
        constexpr Vec2 BOTTOM_MIN_BARS_ROWS{2.0f, 6.0f};

        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "project",
             .order = 50,
             .buildOrder = 130,
             .dock = {.zone = UiDockZone::Bottom, .ratio = 0.43f, .activate = true},
             .minBarsRows = BOTTOM_MIN_BARS_ROWS,
             .zone = "EditorProject",
             .make = [](EditorPanelContext& c)
             {
                 return makeEditorPanel<EditorProject>(c.engine, c.dock, c.contentOf("hierarchy"), c.feedback,
                                                       c.dialogs, c.openScene);
             }}};
    } // namespace
} // namespace spite
