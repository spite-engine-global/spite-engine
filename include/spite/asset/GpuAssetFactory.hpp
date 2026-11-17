#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "asset/AssetData.hpp"
#include "asset/AssetExport.hpp"
#include "asset/AssetHandle.hpp"
#include "asset/AssetRegistry.hpp"
#include "asset/EnvironmentAsset.hpp"
#include "asset/OceanProfile.hpp"
#include "asset/WriteBakedFunctions.hpp"
#include "base/Schema.hpp"
#include "rendering/GpuAllocationManager.hpp"
#include "rendering/GpuResourceStore.hpp"
#include "rendering/GpuDataTypes.hpp"
#include "rendering/GpuResourceHeapManager.hpp"
#include "rendering/IBindlessResourceManager.hpp"
#include "rendering/IRenderResourceManager.hpp"
#include "rendering/ITransferManager.hpp"

namespace spite
{
    struct GpuOceanProfileHeader
    {
        u32 width, height, frameCount, loop;
        float tileMetres, framesPerSecond, maxDisplacementMetres, reserved;
    };
    static_assert(sizeof(GpuOceanProfileHeader) == 32);

    struct GpuOceanProfileData
    {
        BufferHandle buffer;
        sizet bytes = 0;
        AssetRef source;
    };
    struct AssetHandle;
    class IBindlessResourceManager;
    class IRenderResourceManager;

    // Contains the allocation handles for a submesh's geometry within the global heaps.
    struct SubmeshGeometryInfo
    {
        GpuAllocationHandle vertexAllocation;
        GpuAllocationHandle indexAllocation;
        GpuAllocationHandle meshletVertexAllocation;
        GpuAllocationHandle meshletTriangleAllocation;
        GpuAllocationHandle meshletDrawIndexAllocation;
        // the submesh's rows of the SkinWeightBuffer, 8 bytes a vertex; invalid for a static submesh
        GpuAllocationHandle skinWeightAllocation;
    };

    // What a renderable of a skinned submesh needs from the submesh to draw it skinned: where its
    // vertices' skin rows are, and which skeleton joint each palette index of those rows names.
    struct SubmeshSkin
    {
        // added (wrapping) to a vertex's global index it gives the vertex's row of the SkinWeightBuffer; this is what
        // SceneObjectBounds::skinWeightDelta carries
        u32 weightDelta = 0;
        // the submesh's first row of the SkinWeightBuffer, and its row count (its vertices')
        u32 weightRow = 0;
        u32 vertexCount = 0;
        // palette index -> skeleton joint (at most 256, ascending): the renderable's palette block holds, at index i,
        // the skin matrix of skeleton joint palette[i]
        std::vector<u16> palette;
    };

    struct MeshletData;

    // CPU-side record of a registered submesh asset, owning its GPU allocations.
    // Plain data: destruction is routed through the owning GpuAssetFactory.
    struct SPITE_ASSET_API SubmeshData
    {
        const InternedString name;
        const u32 submeshIndex;
        const SubmeshGeometryInfo geometry;
        std::vector<ProcessedMeshlet> meshlets;
        UvRange uvRange; // what the vertices' unorm uvs span
        BoundingBox localBounds{};
        // null for a static submesh; shared with the renderables derived from this record
        std::shared_ptr<const SubmeshSkin> skin;
        // the live meshlet block per material id, shared by every renderable of this submesh and material;
        // GpuAssetFactory::acquireMeshlets only (main thread)
        std::vector<std::pair<u32, std::weak_ptr<MeshletData>>> meshletBlocks;

        SubmeshData(InternedString path, u32 index, const SubmeshGeometryInfo& geo,
                    std::vector<ProcessedMeshlet> meshlets, const UvRange& uvRange);
        ~SubmeshData() = default;

        SubmeshData(const SubmeshData&) = delete;
        SubmeshData& operator=(const SubmeshData&) = delete;
        SubmeshData(SubmeshData&&) = delete;
        SubmeshData& operator=(SubmeshData&&) = delete;
    };

    // A submesh's meshlets baked for one material: one block in the global
    // MeshletBuffer, shared by every renderable of that submesh and material
    // through their MeshletComponents; the last one to go frees it.
    struct SPITE_ASSET_API MeshletData
    {
        GpuAllocationHandle meshletAllocation;
        GpuAllocationManager& allocationManager;
        u32 firstMeshlet; // Index into global MeshletBuffer
        u32 meshletCount; // level-0 meshlets: the ones an object draws without LOD
        u32 lodMeshletCount; // the coarser clusters that follow them (cluster LOD)
        u32 triangleCount;
        // Triangle id this block starts at (its GPU meshlet entries'
        // globalTriangleBase values derive from it); the visibility buffer's
        // triangle id means something only next to its meshlet id, so blocks
        // need not have disjoint ranges. CPU consumers such as the validation
        // oracle resolve triangle ids through it.
        u32 firstTriangle;
        UvRange uvRange; // the submesh's: the object record carries it to the shaders that unpack a vertex uv

        MeshletData(const GpuAllocationHandle& meshletAllocation, GpuAllocationManager& allocationManager,
                    u32 firstMeshlet, u32 meshletCount, u32 lodMeshletCount, u32 triangleCount, u32 firstTriangle,
                    const UvRange& uvRange);

        ~MeshletData();
    };

    struct MaterialInfo
    {
        GpuMaterial material;
        u32 materialId;
    };

    // A texture slot the material shader skips (its INVALID_ID): the slot's own default applies.
    constexpr u32 UNBOUND_TEXTURE_INDEX = 0xFFFFFFFFu;

    struct MaterialTextureRefs
    {
        AssetRef albedoTexture;
        AssetRef normalTexture;
        AssetRef metallicRoughnessTexture;
        AssetRef occlusionTexture;
        AssetRef emissiveTexture;
        std::array<AssetRef, TERRAIN_TEXTURE_SLOT_COUNT> terrainTextures{};
    };

    // Cumulative asset-upload settle telemetry since factory creation. Wall
    // times are steady-clock totals over the matching count.
    struct GpuUploadStats
    {
        u32 uploadsSettled = 0; // assets whose uploads passed the device timeline
        u32 uploadsFailed = 0; // uploads whose worker threw (asset content is undefined)
        double uploadTotalMs = 0.0; // cumulative submit -> settle time
        SPITE_OBSERVED_SCHEMA(GpuUploadStats, uploadsSettled, uploadsFailed, uploadTotalMs)
    };

    // CPU-side record of a registered material asset. Plain data.
    struct SPITE_ASSET_API MaterialData
    {
        MaterialInfo info;
        MaterialTextureRefs textureRefs{};
        u32 terrainId = 0;

        // Non-copyable, non-movable (registry-owned)
        MaterialData(const MaterialData&) = delete;
        MaterialData& operator=(const MaterialData&) = delete;
        MaterialData(MaterialData&&) = delete;
        MaterialData& operator=(MaterialData&&) = delete;

    private:
        friend class GpuAssetFactory;
        MaterialData(MaterialInfo info) : info(info) {}
    };

    // CPU-side record of a registered texture asset. Plain data.
    struct SPITE_ASSET_API ImportedTexture
    {
        InternedString path;
        TextureHandle handle;
        ImageViewHandle view;
        BindlessHandle bindlessHandle;

        ImportedTexture(InternedString path, TextureHandle handle, ImageViewHandle view, BindlessHandle bindlessHandle);
    };

    // The asset module's single GPU write surface: one creation path per asset
    // type, one batched upload per asset, completion-based readiness and
    // deferred retirement of released assets. Owns everything the old
    // Mesh/Material/Texture managers did, without the per-type path duplication.
    //
    // Threading: create* are safe from workers; pump/isReady are main-thread.
    // The world is never touched here.
    class GpuResourceStore;

    class SPITE_ASSET_API GpuAssetFactory
    {
    public:
        GpuAssetFactory(IRenderResourceManager& resourceManager, IBindlessResourceManager& bindlessResourceManager,
                        ITransferManager& transferManager, GpuResourceStore& store, GpuResourceHeapManager& heapManager,
                        GpuAllocationManager& allocationManager, AssetRegistry& assetRegistry, u32 maxMaterialIds);
        ~GpuAssetFactory();

        GpuAssetFactory(const GpuAssetFactory&) = delete;
        GpuAssetFactory& operator=(const GpuAssetFactory&) = delete;

        // Returns the existing handle when the asset is already registered,
        // an invalid handle when GPU creation fails. The handle names the colour
        // space (textureAssetHandle): the same image in two spaces is two textures.
        AssetHandle createTexture(const TextureAsset& texture, TextureColorSpace space = TextureColorSpace::SRGB);
        // The environment's equirect chain as one sampled 2D texture (every level in one transfer), registered in the
        // bindless table through a sampler that wraps horizontally, clamps vertically and reaches every mip. The
        // handle is {name, ENVIRONMENT, 0}; the record is an ImportedTexture. Same returns as createTexture.
        AssetHandle createEnvironment(const EnvironmentAsset& environment);
        // Source must be a held CPU OCEAN_PROFILE reference. The returned subasset 1 owns a dedicated GPU buffer.
        AssetHandle createOceanProfileGpu(const AssetRef& source);
        AssetHandle createSubmesh(const ProcessedSubmesh& submesh, u32 submeshIndexInModel, InternedString modelName);
        AssetHandle createMaterial(const ParsedMaterialData& matData);

        // A registered submesh's geometry replaced by a rebake, without the gap of a release and a load: the new
        // record is built and uploaded unregistered (stageSubmesh, any thread; false when GPU creation fails);
        // once pump() settles its uploads (stagedSettled) replaceSubmesh registers it under the handle, keeping
        // the holders' reference count, and returns it (nullptr when it is not settled or the handle is no
        // longer registered). The old record frees its allocations, through the deferred free of the allocation
        // manager, once no meshlet block derived from it lives. discardStaged drops a staging that will not commit.
        // strictSkin refuses an invalid skin stream or failed skin allocation instead of staging a bind-pose fallback.
        bool stageSubmesh(const ProcessedSubmesh& submesh, u32 submeshIndexInModel, InternedString modelName,
                          bool strictSkin = false);
        bool stagedSettled(AssetHandle handle) const;
        SubmeshData* replaceSubmesh(AssetHandle handle);
        // Replace only when every distinct handle is registered and its staged upload succeeded; output is
        // untouched on refusal. Old records retire through the existing deferred-resource path.
        bool replaceSubmeshes(std::span<const AssetHandle> handles, std::span<SubmeshData*> replacements);
        // Main-thread preparation of a material-specific meshlet block for an already-settled staged submesh.
        // Hold the returned block through commit; discarded staging retires only after uploads and holders settle.
        std::shared_ptr<MeshletData> prepareStagedMeshlets(AssetHandle handle, const MaterialInfo& material);
        bool stagedMeshletsReady(AssetHandle handle) const;
        void discardStaged(AssetHandle handle);

        // Material with pre-resolved GPU data (editor writes); missing texture
        // refs fall back to the default texture's bindless slot.
        AssetHandle createMaterialFromGpuData(const InternedString& name, const GpuMaterial& gpuMaterial,
                                              MaterialTextureRefs&& textureRefs = {},
                                              const std::optional<GpuTerrainProfile>& terrain = std::nullopt);

        // Rewrites a registered material's GPU record in place: one
        // frame-ordered slot write, no rebind. The record's own upload must
        // have settled (isReady), or it could land over the edit. Texture
        // slots take the named textures when registered (in the slot's colour
        // space) and the defaults otherwise, a missing metallic-roughness slot
        // staying unbound (the caller loads them first). False when the material
        // is not registered.
        bool updateMaterial(const InternedString& name, const BakedMaterialWriteData& fields);

        // The registered materials per class, kept at registration, release and edit; the asset runtime publishes it
        // as a service for the scene upload
        MaterialClassCounts& classCounts() { return m_classCounts; }

        // The submesh's meshlet block for the material, for a renderable's
        // derived state: the live block when one exists (no allocation, no
        // upload), else a new one baked and uploaded SameFrame. Main thread.
        // Returns nullptr if inputs are invalid.
        std::shared_ptr<MeshletData> acquireMeshlets(SubmeshData& submeshData, const MaterialInfo& materialInfo);

        // True when the asset is registered and its GPU uploads have completed:
        // texture futures past the device timeline, buffer tickets past Queued
        // and Submitted (the store's tick budget may defer them for frames). A
        // buffer ticket that failed or was cancelled never becomes ready.
        bool isReady(AssetHandle handle) const;
        // True when one of the asset's uploads failed or was cancelled (stays
        // not ready until the asset is released).
        bool uploadFailed(AssetHandle handle) const;

        // Resolves submitted uploads against the device timeline and destroys
        // released records whose GPU data is safe to free. Call once per frame
        // (or per pump) on the main thread.
        void pump();

        // Demotion hook (gpu-streaming spec): when installed, refcount-zero
        // records route to it instead of the built-in retirement — the
        // ResidencyManager receives them and calls destroyReleasedRecord when
        // its policy decides. Unset: immediate built-in retirement.
        void setReleaseInterceptor(
            std::function<void(AssetHandle handle, void* data, AssetRegistry::AssetDeleter deleter, void* context)>
                interceptor)
        {
            m_releaseInterceptor = std::move(interceptor);
        }

        // The built-in retirement path, exposed as the ResidencyManager
        // backend's destroy verb: defers while uploads are in flight, then
        // destroys. Safe from the update thread.
        void destroyReleasedRecord(AssetHandle handle, void* data, AssetRegistry::AssetDeleter deleter, void* context);

        BindlessHandle getDefaultTextureBindlessHandle() const { return m_defaultTextureBindless; }

        GpuUploadStats uploadStats() const;

    private:
        struct PendingUpload
        {
            // buffer uploads go through the GpuResourceStore (tickets);
            // texture uploads still resolve raw timeline futures
            std::vector<UploadTicket> tickets;
            std::vector<std::shared_future<u64>> futures;
            std::chrono::steady_clock::time_point submittedAt = std::chrono::steady_clock::now();
            // attribution: which asset these uploads belong to, so pump can
            // name the asset when a worker throws
            AssetHandle handle;
            // a ticket failed or was cancelled: nothing (or only part) landed; the record stays pending
            bool failed = false;
        };

        struct BuiltSubmesh
        {
            SubmeshData* record = nullptr; // nullptr when GPU creation failed
            std::vector<UploadTicket> uploads;
        };
        BuiltSubmesh buildSubmesh(const ProcessedSubmesh& submesh, u32 submeshIndexInModel, InternedString modelName,
                                  bool strictSkin = false);

        // The texture, view, bindless slot and one upload of every level of `texels` (tightly packed levels of
        // blocks of `blockBytes`, each block `blockExtent` x `blockExtent` texels, largest level first), registered
        // under the handle; shared by createTexture and createEnvironment.
        AssetHandle createSampledTexture(AssetHandle assetHandle, InternedString name, const TextureDesc& desc,
                                         SamplerHandle sampler, const void* texels, u32 blockBytes,
                                         u32 blockExtent = 1);

        // static registry deleters; context is this factory
        static void destroyTexture(void* data, void* context);
        static void destroySubmesh(void* data, void* context);
        static void destroyMaterial(void* data, void* context);
        static void destroyOceanProfileGpu(void* data, void* context);

        void trackLocked(AssetHandle handle, PendingUpload&& uploads); // m_stateMutex held
        bool registerTracked(AssetHandle handle, void* data, AssetRegistry::AssetDeleter deleter,
                             PendingUpload&& uploads);
        void retireReleased(AssetHandle handle, void* data, void (*deleter)(void*, void*), void* context);
        void retireAfterUpload(AssetHandle handle, std::vector<std::shared_future<u64>>&& futures,
                               std::function<void()> destroy);
        void retireAfterUpload(AssetHandle handle, std::vector<UploadTicket>&& tickets, std::function<void()> destroy);
        u32 allocateMaterialId();
        // an id no frame has read: free now
        void releaseMaterialId(u32 materialId);
        // an id frames may still read: free once those frames retire
        void retireMaterialId(u32 materialId);

        IRenderResourceManager& m_resourceManager;
        IBindlessResourceManager& m_bindlessResourceManager;
        ITransferManager& m_transferManager;
        GpuResourceStore& m_store; // streaming buffer uploads (tickets)
        GpuResourceHeapManager& m_heapManager;
        GpuAllocationManager& m_allocations;
        AssetRegistry& m_registry;

        // startup defaults (SameFrame uploads, explicitly settled in the ctor)
        TextureHandle m_defaultTexture;
        ImageViewHandle m_defaultTextureView;
        SamplerHandle m_defaultSampler;
        SamplerHandle m_environmentSampler;
        BindlessHandle m_defaultTextureBindless{};
        // Flat (0.5, 0.5, 1) normal map: TBN * (0, 0, 1) is the geometric
        // normal, so materials without a normal texture keep their vertex
        // normals instead of normal-mapping with a white pixel.
        TextureHandle m_defaultNormalTexture;
        ImageViewHandle m_defaultNormalTextureView;
        BindlessHandle m_defaultNormalTextureBindless{};

        // creation (workers) and pump (main) both touch these
        mutable std::mutex m_stateMutex;
        std::unordered_map<AssetHandle, std::shared_ptr<PendingUpload>, AssetHandle::Hash> m_pending;
        GpuUploadStats m_uploadStats; // guarded by m_stateMutex
        struct Retired
        {
            std::shared_ptr<PendingUpload> uploads;
            std::function<void()> destroy;
        };
        std::deque<Retired> m_retired;
        // stageSubmesh records waiting for their uploads (settled set by pump), by handle; guarded by m_stateMutex
        struct StagedBlock
        {
            u32 materialId = 0;
            std::shared_ptr<MeshletData> block;
            UploadTicket upload;
        };
        struct Staged
        {
            SubmeshData* record = nullptr;
            std::shared_ptr<PendingUpload> uploads;
            bool settled = false;
            bool failed = false; // settled because a ticket failed or was cancelled: nothing good to register
            std::vector<StagedBlock> blocks;
        };
        std::unordered_map<AssetHandle, Staged, AssetHandle::Hash> m_staged;
        void retireStaged(Staged&& staged);
        std::shared_ptr<MeshletData> acquireMeshletsImpl(SubmeshData& submeshData, const MaterialInfo& materialInfo,
                                                         TransferSync sync, UploadTicket* ticket);
        // records replaceSubmesh took out of the registry, destroyed by pump once none of their meshlet blocks
        // lives (main thread, guarded by m_stateMutex)
        std::vector<SubmeshData*> m_replaced;
        struct RetiringOceanProfile
        {
            GpuOceanProfileData* data;
            u64 frame;
        };
        std::vector<RetiringOceanProfile> m_retiringOceanProfiles;
        // The textures whose upload was still in flight when a material took them (any slot): the renderable spawns on
        // the material's own upload, so the first probe bake or shadow render can sample the placeholder, and the
        // texture landing is a light edit of the material (the probe volumes rebake what it reaches), and a shadow edit
        // when it is the albedo of a material that casts through its alpha (the mask class, a dithered blended
        // caster). Noted by pump, once per material and texture. Guarded by m_stateMutex; empty on a quiescent factory.
        struct LateTexture
        {
            AssetHandle texture;
            u32 materialId;
            bool shadow;
        };
        std::vector<LateTexture> m_lateTextures;
        void watchLateTexturesLocked(const GpuMaterial& material, const MaterialTextureRefs& refs, u32 materialId);

        // release-sink demotion (gpu-streaming spec); unset = built-in retirement
        std::function<void(AssetHandle, void*, AssetRegistry::AssetDeleter, void*)> m_releaseInterceptor;

        std::mutex m_materialIdMutex;
        std::vector<u32> m_freeMaterialIds;
        struct RetiringMaterialId
        {
            u32 id;
            u64 frame; // reusable once this frame retires
        };
        std::deque<RetiringMaterialId> m_retiringMaterialIds;
        u32 m_nextMaterialId = 1; // 0 is reserved for a default material
        MaterialClassCounts m_classCounts;
        u32 m_maxMaterialIds;

        u32 m_nextTriangleId = 0; // acquireMeshlets only (main thread)
    };
} // namespace spite
