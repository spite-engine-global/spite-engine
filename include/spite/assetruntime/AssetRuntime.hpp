#pragma once

#include <array>
#include <functional>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "asset/AssetBaker.hpp"
#include "asset/AssetManifest.hpp"
#include "asset/AssetRegistry.hpp"
#include "asset/ModelImportSettings.hpp"
#include "asset/ResidencyManager.hpp"
#include "asset/SpriteAtlas.hpp"
#include "rendering/GpuBudget.hpp"
#include "assetruntime/AssetComponents.hpp"
#include "assetruntime/AssetRuntimeExport.hpp"
#include "base/Expected.hpp"
#include "base/IOManager.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    class World;
    struct ModelHierarchyDescriptor;
    // The asset module facade: baking, manifest, registry, per-type loaders,
    // entity binding/rehydration and staged model instantiation behind one API.
    // Hosts call initialize() once, update() once per frame, and
    // the binding/instantiation operations as needed; every live-world mutation
    // the module performs happens inside update() on the caller's thread, so
    // workers never touch the world and callbacks always fire there. *Sync
    // variants pump update() for hosts without a frame loop.
    class SPITE_ASSETRUNTIME_API AssetRuntime
    {
    public:
        enum class AssetStatus : u8
        {
            Loading,
            Ready,
            Failed,
            Unloaded // never loaded, or released
        };

        using LoadCallback = std::function<void(Entity root)>;
        struct SubmeshOption
        {
            u32 submeshIndex = 0;
            std::string displayName;
        };

        // Cumulative module activity since initialize() (or the last
        // resetStats). Wall times are steady-clock totals; divide by the
        // matching count for a mean.
        struct AssetRuntimeStats
        {
            u32 loadsStarted = 0; // loads that began IO+parse on workers
            u32 loadsCompleted = 0; // parsed and GPU resources created
            u32 loadsFailed = 0;
            u32 uploadsSettled = 0; // uploads that passed the device timeline
            u32 stagingsAdopted = 0; // instantiation hierarchies adopted into the world
            u32 derivationsBaked = 0; // entities that received derived meshlet state

            double loadTotalMs = 0.0; // load start -> finishLoad
            double uploadTotalMs = 0.0; // upload submit -> device settle
            double adoptionTotalMs = 0.0;
            double deriveTotalMs = 0.0;
            double updateTotalMs = 0.0; // total update() cost
            u64 updateFrames = 0;
            u64 meshletDemandWalks = 0; // full renderable walks behind the streaming-capacity check
            SPITE_OBSERVED_SCHEMA(AssetRuntimeStats, loadsStarted, loadsCompleted, loadsFailed, uploadsSettled,
                                  stagingsAdopted, derivationsBaked, loadTotalMs, uploadTotalMs, adoptionTotalMs,
                                  deriveTotalMs, updateTotalMs, updateFrames, meshletDemandWalks)
        };

        // bakerConfig overrides where the module's manifest and bake output
        // live (default: assets_baked under the content root); tests and
        // tools point it at a private directory. Manifest entries keep the path as configured; every read and write
        // resolves it against the content root (absolute stays).
        explicit AssetRuntime(const IOManagerConfig& ioConfig, const AssetBakerConfig& bakerConfig = {});
        ~AssetRuntime();

        AssetRuntime(const AssetRuntime&) = delete;
        AssetRuntime& operator=(const AssetRuntime&) = delete;

        // Requires the rendering services to be registered on the world's
        // service container; registers the module's components, constructs the
        // per-type loaders, subscribes the lifecycle hooks and publishes the
        // asset services. Must run before the first update and before any
        // binding lands in the world.
        void initialize(World& world);
        void shutdown();
        // At a frame boundary, after the renderer and its consumers have stopped submitting work.
        [[nodiscard]] std::string gpuRestartBlocker() const;
        void releaseGpuForRestart();
        void restoreGpuAfterRestart();
        // Binds a further world (a play world, a second scene): its mesh and
        // material bindings — those already in it included — load, rehydrate
        // and get derived renderable state inside update() like the first
        // world's. Instantiation and the bind API stay on the initialize()
        // world. detachWorld before destroying the world releases its binding
        // references.
        void attachWorld(World& world);
        void detachWorld(const World& world);

        // The only place the module touches the live world: binding commits,
        // rehydration, derived renderable state, upload-settling pins, staging
        // adoption and load callbacks. Idempotent; budget bounds the work.
        void update(double budgetMs = 4.0);

        // -- asset lifetime -------------------------------------------------------

        // Idempotent; starts IO + GPU upload on workers if not loaded. The
        // registry owns one reference afterwards (drop it with release()).
        void load(AssetHandle handle);
        // Streaming demand: admitted by the ResidencyManager while the budget
        // allows (gpu-streaming spec); without residency (pre-initialize) it
        // degrades to an immediate load. A model's cost is the whole model's:
        // demand its submeshes together with siblings = their count so each
        // charges an equal share.
        void prefetchAsset(AssetHandle handle, u32 priority, u32 siblings = 1);
        // aggregate policy telemetry; zeroes before initialize
        ResidencyStats residencyStats() const;
        // Visible-capacity overflow monitor: each update() compares the
        // scene's total baked meshlet count (plus the object buffer's
        // dropped-object stat) against the capacity and invokes
        // `handler(demand, capacity)` on overflow. The host grows capacity in
        // the handler (buffers + graph generation) and keeps
        // setStreamingCapacity in sync.
        void setStreamingCapacity(u32 capacity);
        [[nodiscard]] u32 streamingCapacity() const;
        void setOverflowHandler(std::function<void(u32 visibleCount, u32 capacity)> handler);
        // Installed before instantiation; called on the staging worker for each imported node.
        void setImportedNodeTranslator(std::function<void(World&, Entity, const ImportedNodeProperties&)> translator);
        // The effective device budget the residency policy admits against;
        // wired by the host from the render device. Unset = no pressure.
        void setResidencyBudgetProvider(std::function<GpuBudget()> provider);
        // releases the load() reference (binding refs release via unbind)
        void release(AssetHandle handle);
        // Blocks until the asset is Ready or Failed, driving update() and the transfer manager's processTransfers()
        // itself. Main thread, outside system execution and frames.
        Expected<void> loadSync(AssetHandle handle);
        // Removed submeshes and failed restoration report Failed; authored bindings remain unresolved.
        // A pending restoration reports Loading. Reimport the model to retry a failed restoration.
        AssetStatus status(AssetHandle handle) const;

        // A texture over its colour spaces (one GPU copy per space a slot samples it in): the furthest status of the
        // spaces (Ready, Loading, Failed, Unloaded), the references of all of them and each space's own status.
        struct TextureState
        {
            AssetStatus status = AssetStatus::Unloaded;
            u32 refs = 0;
            std::array<AssetStatus, 2> spaces{AssetStatus::Unloaded, AssetStatus::Unloaded}; // by TextureColorSpace
        };
        TextureState textureState(InternedString name) const;

        // Frame-thread only. Borrowed until the owning load is released or the asset is replaced;
        // query again when recording each frame. Empty until the texture upload has settled.
        std::optional<u32> textureIndex(InternedString name, TextureColorSpace space) const;

        // -- bindings --------------------------------------------------------------

        // Bindings are immutable components; these are remove+add commits and
        // rehydration (load, acquire, bounds, derived state) follows through the
        // lifecycle hooks inside update().
        void bindMesh(Entity entity, AssetHandle handle);
        void bindMaterial(Entity entity, AssetHandle handle);
        // Returns false if the world is not attached, the entity is not live, or the handle is not a material.
        bool bindMaterial(const World& world, Entity entity, AssetHandle handle);
        void clearMesh(Entity entity);
        void clearMaterial(Entity entity);
        bool clearMaterial(const World& world, Entity entity);

        // -- instantiation -----------------------------------------------------------

        // Parses the baked model on workers, builds the hierarchy in a staging
        // world, adopts it inside update() and fires the callback on the update
        // thread. Independent of asset readiness - bindings rehydrate after.
        void instantiateModel(InternedString name, Entity parent, LoadCallback callback = nullptr);

        struct InstantiatedModel
        {
            Entity root;
            u32 entities = 0;
        };
        // Builds the model's hierarchy into world now, on the calling thread
        // (the bake's metadata section only), under parent; bindings rehydrate
        // in update(). world is the initialize() world or an attached one, with
        // an EntityTransformManager in its services. For edits that need the
        // entities at once (the instantiateModel control command). Frame
        // thread only: a model under a disabled parent is inactive on return.
        Expected<InstantiatedModel> instantiateModelNow(const World& world, InternedString name, Entity parent);
        // instantiateModelNow for one copy per parent (an invalid parent leaves that copy a root), read and planned
        // once and adopted in one staging world: the roots in parent order.
        Expected<std::vector<Entity>> instantiateModelsNow(const World& world, InternedString name,
                                                           std::span<const Entity> parents);

        Expected<std::vector<SubmeshOption>> enumerateSubmeshes(InternedString modelName);
        // The hierarchy nodes instantiated as scene entities; rig joints and dropped skin-only nodes are excluded.
        Expected<std::vector<u8>> instantiatedNodeMask(InternedString modelName);

        // -- materials ------------------------------------------------------------------

        // Writes the material's bake and manifest entry (adding it when new). A
        // loaded material takes the fields in place - one material-slot upload,
        // no rebind - once the textures they name are loaded (a load starts
        // here when needed); one not loaded reads them when it loads. A
        // material whose load failed (a binding that named it before it
        // existed) reloads.
        Expected<void> writeMaterial(InternedString name, const BakedMaterialWriteData& fields);
        // Drops the material's bake and manifest entry; a loaded copy stays
        // registered until released. Refused while the material is loading.
        Expected<void> removeMaterial(InternedString name);

        // -- environments ---------------------------------------------------------------

        // Bakes an environment source (AssetBaker::bakeEnvironment) under name on the calling thread and saves the
        // manifest. Refused while the environment is loading (the load reads the file being replaced). A registered
        // copy keeps the old content until it is released; a copy released and held in the residency grace window is
        // discarded, so the next load reads the new bake; a load that failed retries. name must be a safe bake file
        // name, and not differ from an environment's name only by case.
        Expected<AssetManifest::EnvironmentEntry> importEnvironment(const std::string& source, InternedString name,
                                                                    EnvironmentFormat format);
        // The caller must authorize source against its import root before calling.
        Expected<AssetManifest::OceanProfileEntry> importOceanProfile(const std::string& source, InternedString name);
        Expected<AssetManifest::OceanProfileEntry> removeOceanProfile(InternedString name);
        Expected<void> restoreOceanProfile(InternedString name, const AssetManifest::OceanProfileEntry& entry);

        // -- import -------------------------------------------------------------------

        // A model file baking into the manifest.
        struct MaterialPreviewVersion
        {
            InternedString name;
            InternedString baked;
            InternedString contentHash;
        };
        struct ModelImport
        {
            enum class State : u8
            {
                Queued,
                Baking,
                Done,
                Failed,
                Cancelled
            };
            u32 id = 0;
            InternedString name;
            std::string source; // UTF-8, as importModel was given it (pathFromUtf8 opens it)
            State state = State::Queued;
            std::string error; // Failed
            std::vector<GltfExtensionDiagnostic> diagnostics;
            // what the import added besides the model (Done); materials and
            // textures the bake shared with the manifest are not listed. A
            // reimport lists what the model's earlier imports added too.
            std::vector<InternedString> materials;
            std::vector<InternedString> textures;
            double bakeMs = 0.0;
            bool generatePreviews = true;
            std::vector<MaterialPreviewVersion> previewMaterialVersions;
            bool reimport = false; // replaced the model's bake instead of adding one
            ModelImportSettings settings; // the options the bake uses, fixed when the import was queued
        };
        // Bakes the model file source under name on an IO worker, one import at
        // a time, into a staging directory beside the bakes. source and root
        // are UTF-8 (utf8Path / pathFromUtf8 convert), whatever the machine's
        // ANSI code page; name must be a safe bake file name (letters, digits,
        // '_', '-', '.': AssetManifest::safeBakeName makes one from a stem). The files the
        // model names (buffers, images) must resolve inside root (default: the
        // source's directory), else the import fails. update() commits a
        // finished bake: its files move into the bake directory and its entries
        // into the manifest (saved), only when every name it adds is still free
        // and every material and texture it shares with the manifest is still
        // there, so a failed or cancelled import leaves both as they were;
        // failed loads of those names retry. Refused while name is a model, a
        // pending import, or a model removed this session from another source
        // whose GPU copies are still loaded.
        // settings: the importer options. They are written to the source's sidecar when the import is queued
        // (a sidecar equal to the defaults is deleted) and put back, byte for byte, when the import fails or is
        // cancelled, unless something else changed the sidecar since. None: the sidecar as it is when queued
        // (an unreadable one refuses the import). The bake uses the options fixed at queue time, not the file.
        Expected<u32> importModel(std::string source, InternedString name, std::string root = {},
                                  const std::optional<ModelImportSettings>& settings = {});
        Expected<u32> importModel(std::string source, InternedString name, std::string root,
                                  const std::optional<ModelImportSettings>& settings, bool generatePreviews);
        // The same bake and commit on the calling thread, after the import
        // baking on a worker (if any) settles; the finished record.
        Expected<ModelImport> importModelNow(std::string source, InternedString name, std::string root = {},
                                             const std::optional<ModelImportSettings>& settings = {});
        Expected<ModelImport> importModelNow(std::string source, InternedString name, std::string root,
                                             const std::optional<ModelImportSettings>& settings, bool generatePreviews);
        // Bakes the model's recorded source again and replaces the model's bake and manifest entry whole or not at
        // all, like importModel (settings likewise); the materials and textures the new bake adds are added beside
        // the old ones. A pending reimport of the model is cancelled in its favour. Refused for a model not in the
        // manifest or without a recorded source. Loaded submeshes of the model take the new geometry in place, within a
        // few frames: the renderables bound to them are rewritten (bounds, meshlet block) without a release or a
        // frame without geometry, and the old GPU records free once nothing derives from them. The bake names a
        // changed material or texture anew and never rewrites a loaded one: a loaded renderable still on the material
        // its submesh had moves to the new bake's (loaded on demand) with its geometry, one the user gave another
        // material keeps it (scenes on disk keep the old material name). Node transforms of existing instances are not
        // touched; a missing submesh retains its authored binding but has no derived geometry.
        Expected<u32> reimportModel(InternedString name, std::string root = {},
                                    const std::optional<ModelImportSettings>& settings = {});
        Expected<u32> reimportModel(InternedString name, std::string root,
                                    const std::optional<ModelImportSettings>& settings, bool generatePreviews);
        Expected<ModelImport> reimportModelNow(InternedString name, std::string root = {},
                                               const std::optional<ModelImportSettings>& settings = {});
        Expected<ModelImport> reimportModelNow(InternedString name, std::string root,
                                               const std::optional<ModelImportSettings>& settings,
                                               bool generatePreviews);
        // Capture on the frame thread. The immutable result owns everything the worker needs; no live runtime is
        // accessed by prepareModel, and its IO manager is local to the worker.
        struct SPITE_ASSETRUNTIME_API PreparationSnapshot
        {
        private:
            friend class AssetRuntime;
            PreparationSnapshot() = default;
            std::string source, root, outputDirectory, textureSubdir, materialSubdir, atlasSubdir, environmentSubdir;
            InternedString name;
            ModelImportSettings settings;
            bool useSidecar = false;
            std::string settingsOverlay;
            AssetBakerConfig config;
            u64 manifestGeneration = 0;
            std::unordered_map<InternedString, AssetManifest::TextureEntry> textures;
            std::unordered_map<InternedString, AssetManifest::MaterialEntry> materials;
            std::unordered_map<InternedString, AssetManifest::ModelEntry> models;
            std::unordered_map<InternedString, AssetManifest::SkeletonEntry> skeletons;
            std::unordered_map<InternedString, AssetManifest::AnimationEntry> animations;
            std::unordered_map<InternedString, AssetManifest::SpriteAtlasEntry> atlases;
            std::unordered_map<InternedString, AssetManifest::EnvironmentEntry> environments;
            u32 materialFormat = 0, materialSchema = 0;
        };
        struct SPITE_ASSETRUNTIME_API PreparedModel
        {
            ~PreparedModel();
            void pinForRecovery() { recoveryPinned = true; }
            void unpinFromRecovery() { recoveryPinned = false; }
            PreparedModel(const PreparedModel&) = delete;
            PreparedModel& operator=(const PreparedModel&) = delete;
            std::shared_ptr<ModelHierarchyDescriptor> hierarchy;
            std::vector<u8> instantiatedNodes;
            std::vector<u32> skinnedSubmeshes; // captured during worker preparation
            std::vector<std::pair<InternedString, AssetManifest::MaterialEntry>> materials;
            std::vector<std::pair<InternedString, AssetManifest::TextureEntry>> textures;
            std::vector<std::pair<InternedString, AssetManifest::SkeletonEntry>> skeletons;
            std::vector<std::pair<InternedString, AssetManifest::AnimationEntry>> animations;
            std::vector<InternedString> sharedMaterials, sharedTextures;
            std::vector<std::pair<InternedString, AssetManifest::MaterialEntry>> sharedMaterialEntries;
            std::vector<std::pair<InternedString, AssetManifest::TextureEntry>> sharedTextureEntries;
            AssetManifest::ModelEntry model;
            std::vector<GltfExtensionDiagnostic> diagnostics;
            std::string source;
            std::string sourceRoot;
            InternedString sourceFingerprint;
            InternedString sidecarFingerprint;
            bool sidecarExisted = false;
            std::vector<std::pair<std::string, InternedString>> sourceDependencies;
            InternedString baselineModelFingerprint;
            std::vector<std::pair<std::string, InternedString>> baselineFiles;
            u64 manifestGeneration = 0;
            ModelImportSettings settings;

        private:
            friend class AssetRuntime;
            PreparedModel() = default;
            std::string staging;
            std::string ownerRoot;
            bool retained = false;
            bool recoveryPinned = false;
        };
        Expected<std::shared_ptr<const PreparationSnapshot>> captureModelPreparation(
            const std::string& source, InternedString name, const std::string& root,
            const ModelImportSettings& settings, bool useSidecar = false, std::string settingsOverlay = {}) const;
        // Perform on a worker; releasing the result removes its private staging directory.
        static Expected<std::shared_ptr<PreparedModel>> prepareModel(const PreparationSnapshot& snapshot);
        // Worker-side validation; callers must repeat after asynchronous resource preparation before publication.
        static Expected<void> verifyPreparationInputs(const PreparedModel& prepared);
        // Move a successful candidate into immutable retained storage on a worker before publication. The caller
        // owns its history/garbage-collection policy; this operation does not modify the live manifest or world.
        static Expected<void> retainPreparedModel(PreparedModel& prepared);
        // Build a complete private manifest from the captured baseline and, when supplied, a retained model version.
        static Expected<std::shared_ptr<AssetManifest>> buildPublicationManifest(const PreparationSnapshot& snapshot,
                                                                                 const PreparedModel* version);
        // Capture the published version from baked files alone, without reading the authoring source.
        static Expected<std::shared_ptr<PreparedModel>> capturePublishedModel(const PreparationSnapshot& snapshot);
        static Expected<void> verifyPublishedModel(const PreparationSnapshot& snapshot, const PreparedModel& baseline);
        static Expected<std::shared_ptr<AssetManifest>> loadRetainedModel(const std::filesystem::path& directory);
        // Call only after every history and journal reference has been released.
        static Expected<void> discardRetainedModel(const std::filesystem::path& bakeRoot,
                                                   const std::filesystem::path& directory);
        // This session's imports, oldest first; a cancelled import reads
        // cancelled at once, though its bake may still be running.
        std::vector<ModelImport> imports() const;
        // Writes source's sidecar now without a bake (a sidecar equal to the defaults is deleted). Frame thread only.
        Expected<void> saveImportSettings(const std::string& source, const ModelImportSettings& settings);
        // Bumps whenever imports() would read differently or a sidecar was written. Frame thread only.
        u64 importsGeneration() const;
        // Reads of a model's skeleton and skin files for instantiation (the rig info is kept per model until an import
        // or removal of it): N instantiations of one rigged model add one.
        u64 rigFileReads() const;
        // Skeleton files decoded to check a clip against its skeleton (kept per skeleton until any import or removal):
        // the clips of one rig add one.
        u64 skeletonCheckReads() const;

        struct RemovedModel
        {
            std::string source;
            bool cancelled = false; // a pending import was cancelled and no model removed
            std::string recoveryId;
            std::vector<InternedString> materials;
            std::vector<InternedString> textures;
        };
        // Frame thread only. Material and texture dependencies remain available for reuse.
        Expected<RemovedModel> removeModel(InternedString name);

        struct DeletedModel
        {
            std::string id;
            std::string name;
            std::string source;
            bool restored = false;
        };
        Expected<std::vector<DeletedModel>> listDeletedModels() const;
        Expected<DeletedModel> restoreRemovedModel(const std::string& id);

        // -- sprite atlases -------------------------------------------------------------

        // The sprite table of a baked atlas (sprite lookups by name: uv rect, pixel size, 9-slice insets). Read from
        // the bake file on first use and again once a rebake changed it; the page is the manifest texture
        // atlas->texture, loaded like any texture. Safe from any thread.
        Expected<std::shared_ptr<const SpriteAtlas>> spriteAtlas(InternedString name);

        // -- service accessors --------------------------------------------------------

        AssetRegistry& registry() const;
        AssetBaker& baker() const;
        AssetManifest& manifest() const;
        IOManager& io() const;

        // -- telemetry ------------------------------------------------------------------

        // Safe from any thread: returns a snapshot.
        AssetRuntimeStats stats() const;
        void resetStats();

    private:
        struct Impl;
        void initializeGpu();
        Expected<u32> queueImport(std::string source, InternedString name, std::string root, bool replace,
                                  const std::optional<ModelImportSettings>& settings, bool generatePreviews);
        Expected<ModelImport> runImport(std::string source, InternedString name, std::string root, bool replace,
                                        const std::optional<ModelImportSettings>& settings, bool generatePreviews);
        std::unique_ptr<Impl> m_impl;
    };
} // namespace spite
