#pragma once

#include <atomic>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>

#include "asset/AssetExport.hpp"
#include "asset/AssetHandle.hpp"
#include "asset/AssetData.hpp"
#include "asset/ModelImportSettings.hpp"
#include "rendering/GpuDataTypes.hpp"
#include "base/Expected.hpp"
#include "base/InternedString.hpp"

namespace spite
{
    // Manages the asset manifest file that tracks baked assets.
    // Uses human-readable names as primary keys with content-hash deduplication.
    // Thread-safe: every member locks (shared for reads) and reads return
    // copies, so loader threads read it while the frame thread edits it.
    // todo optimize HashedString usage
    class SPITE_ASSET_API AssetManifest
    {
    public:
        static constexpr const char* DEFAULT_PATH = "assets_baked/asset_manifest.json"; // under the content root
        static constexpr const char* DEFAULT_MATERIAL_NAME = "__default_material";

        struct TextureEntry
        {
            InternedString baked; // Baked output path
            InternedString contentHash; // For deduplication
            u64 pixelBytes = 0; // GPU upload size (every mip level of the bake), for budget estimates
            u8 format = 0; // a BAKED_TEXTURE_FORMAT_* id; 0 for an entry written before the field: RGBA8
            u64 fileBytes = 0; // stored file size including the header; 0 for a legacy manifest
        };

        struct MaterialEntry
        {
            InternedString baked; // Baked output path
            InternedString contentHash; // For deduplication
        };

        struct SpriteAtlasEntry
        {
            InternedString baked; // the sprite table file
            InternedString texture; // the page, a manifest texture
            InternedString contentHash; // over the source folder; an unchanged hash skips the bake
            u32 spriteCount = 0;
            u32 pageWidth = 0;
            u32 pageHeight = 0;
        };

        struct EnvironmentEntry
        {
            InternedString baked; // the .spiteenv
            InternedString contentHash; // over the baked bytes
            InternedString source; // the file or folder it was imported from, as the bake was given it
            u8 format = 0; // a BAKED_ENVIRONMENT_FORMAT_* id
            u32 width = 0;
            u32 height = 0;
            u32 mipLevels = 0;
            u64 pixelBytes = 0; // GPU upload size (every mip level), for budget estimates
        };

        struct SkeletonEntry
        {
            InternedString baked; // the .spiteskel
            InternedString contentHash; // over the baked bytes
            InternedString model; // the model it was baked with
            u32 jointCount = 0;
        };

        struct OceanProfileEntry
        {
            InternedString baked;
            InternedString source;
            InternedString contentHash;
            u32 width = 0, height = 0, frameCount = 0;
            float tileMetres = 0, framesPerSecond = 0, maxDisplacementMetres = 0;
            bool loop = false;
        };

        struct AnimationEntry
        {
            InternedString baked; // the .spiteanim
            InternedString contentHash;
            InternedString skeleton; // the skeleton the clip is baked against
            InternedString model;
            f32 duration = 0.0f;
            u32 trackCount = 0; // one per joint of the skeleton
        };

        // GPU-relevant geometry sizes emitted at bake time, as element counts
        // (layout-stable): the engine multiplies by its element sizes for byte
        // costs in budget admission (gpu-streaming spec).
        struct ModelGpuCosts
        {
            u64 meshletCount = 0;
            u64 vertexCount = 0;
            u64 meshletVertexIndexCount = 0; // u32 indices
            u64 meshletTriangleIndexCount = 0; // u8 indices, 3 per triangle

            // GPU slice bytes across the five geometry heaps in engine element
            // sizes; the single source of the cost formula (budget admission,
            // heap seeding, prefetch estimates all go through it)
            sizet gpuBytes() const
            {
                return sizet(meshletCount) * sizeof(Meshlet) + sizet(vertexCount) * sizeof(Vertex) +
                    sizet(meshletVertexIndexCount) * sizeof(u32) +
                    // expanded classic-path draw indices: 3 u32 per triangle
                    sizet(meshletTriangleIndexCount) * (sizeof(u8) + sizeof(u32));
            }
        };

        struct ModelEntry
        {
            InternedString baked; // Baked output path
            std::vector<InternedString> materials; // Referenced material names
            ModelGpuCosts gpuCosts{};
            InternedString source; // the generated or direct input given to the baker
            InternedString sourceRoot; // confinement root recorded by an import job
            InternedString authoringSource; // import-root-relative document identity; empty for legacy entries
            ModelImportSettings importSettings; // what the bake used; the sidecar now holding others: needs reimport
            // a model baked with a skin: its skeleton, the clips on it and the .spiteskin with the per-vertex skin
            InternedString skeleton;
            InternedString skin;
            std::vector<InternedString> clips;
        };

        AssetManifest();
        ~AssetManifest();

        AssetManifest(const AssetManifest&) = delete;
        AssetManifest& operator=(const AssetManifest&) = delete;

        // Load manifest from disk (creates empty if doesn't exist)
        Expected<void> load(const char* path = DEFAULT_PATH);
        Expected<void> loadText(std::string_view text);

        Expected<void> save(const char* path = DEFAULT_PATH);
        // Write a complete snapshot even when clean, without changing saved/dirty state.
        Expected<void> saveSnapshot(const char* path);

        // ===== Texture Operations =====

        bool hasTexture(const InternedString& name) const;
        InternedString findTextureByHash(const InternedString& contentHash) const;
        InternedString getTextureBakedPath(const InternedString& name) const;
        std::optional<TextureEntry> getTextureEntry(const InternedString& name) const;

        // Returns true if the entry was added. Returns false if name already exists (no modification).
        bool tryAddTexture(const InternedString& name, const TextureEntry& entry);

        // Overwrites the entry if present, creates if not.
        void overwriteTexture(const InternedString& name, const TextureEntry& entry);

        // ===== Material Operations =====

        bool hasMaterial(const InternedString& name) const;
        InternedString findMaterialByHash(const InternedString& contentHash) const;
        InternedString getMaterialBakedPath(const InternedString& name) const;
        std::optional<MaterialEntry> getMaterialEntry(const InternedString& name) const;

        bool tryAddMaterial(const InternedString& name, const MaterialEntry& entry);
        void overwriteMaterial(const InternedString& name, const MaterialEntry& entry);

        // ===== Model Operations =====

        bool hasModel(const InternedString& name) const;
        InternedString getModelBakedPath(const InternedString& name) const;
        std::vector<InternedString> getModelMaterials(const InternedString& name) const;
        std::optional<ModelEntry> getModelEntry(const InternedString& name) const;

        bool tryAddModel(const InternedString& name, const ModelEntry& entry);
        void overwriteModel(const InternedString& name, const ModelEntry& entry);

        // ===== Sprite Atlas Operations =====

        bool hasSpriteAtlas(const InternedString& name) const;
        std::optional<SpriteAtlasEntry> getSpriteAtlasEntry(const InternedString& name) const;
        void overwriteSpriteAtlas(const InternedString& name, const SpriteAtlasEntry& entry);
        std::unordered_map<InternedString, SpriteAtlasEntry> getSpriteAtlases() const;

        // ===== Environment Operations =====

        bool hasEnvironment(const InternedString& name) const;
        std::optional<EnvironmentEntry> getEnvironmentEntry(const InternedString& name) const;
        // Overwrites the entry if present, creates if not (a rebake of the same name replaces it).
        void overwriteEnvironment(const InternedString& name, const EnvironmentEntry& entry);
        std::unordered_map<InternedString, EnvironmentEntry> getEnvironments() const;
        std::optional<OceanProfileEntry> getOceanProfileEntry(const InternedString& name) const;
        void overwriteOceanProfile(const InternedString& name, const OceanProfileEntry& entry);
        bool removeOceanProfile(const InternedString& name);
        std::unordered_map<InternedString, OceanProfileEntry> getOceanProfiles() const;

        // ===== Skeleton and Animation Operations =====

        bool hasSkeleton(const InternedString& name) const;
        std::optional<SkeletonEntry> getSkeletonEntry(const InternedString& name) const;
        void overwriteSkeleton(const InternedString& name, const SkeletonEntry& entry);
        std::unordered_map<InternedString, SkeletonEntry> getSkeletons() const;

        bool hasAnimation(const InternedString& name) const;
        std::optional<AnimationEntry> getAnimationEntry(const InternedString& name) const;
        void overwriteAnimation(const InternedString& name, const AnimationEntry& entry);
        std::unordered_map<InternedString, AnimationEntry> getAnimations() const;

        // Drops the skeleton and clip entries the model entry lists and returns the baked paths of those and of the
        // skin file: the caller deletes the files. The model entry itself stays (overwriteModel replaces it).
        std::vector<InternedString> removeModelAnimation(const ModelEntry& model);

        // ===== Utility =====

        // The file stem of a UTF-8 path as an asset name. A stem with a non-ASCII character comes back through
        // safeBakeName, so no bake file is named outside the ANSI code page.
        static InternedString deriveNameFromPath(const InternedString& path);
        // A stem as a name a bake file can take: letters, digits, '_', '-' and '.', no "..", no trailing '.', no
        // Windows device name, at most 64 characters. A stem that is one comes back unchanged; any other has its
        // Latin-1 letters folded to ASCII and every other character replaced by '_', then "_" and eight hex digits
        // (32 bits) of the original's hash appended, so two stems practically never meet in one name; the result
        // is at most 64 characters.
        static std::string safeBakeName(std::string_view stem);
        static InternedString generateContentHash(const void* data, sizet size);
        // name as bake file names compare it: ASCII case folded
        static std::string bakeFileKey(std::string_view name);

        template <typename TEntryMap>
        static InternedString makeUniqueName(const InternedString& baseName, const TEntryMap& existing)
        {
            InternedString candidate = baseName;
            u32 counter = 2;
            while (existing.find(candidate) != existing.end())
            {
                candidate = InternedString((std::string(baseName.toView()) + "_" + std::to_string(counter)).c_str());
                ++counter;
            }
            return candidate;
        }

        // Summed GPU footprint of every manifested model, in engine element
        // sizes — the host's seed for geometry heap capacities (gpu-streaming
        // spec phase 3; growth-on-demand is phase 4).
        struct GpuHeapSeedTotals
        {
            sizet meshletBufferBytes = 0;
            sizet vertexBytes = 0;
            sizet meshletVertexIndexBytes = 0;
            sizet meshletTriangleIndexBytes = 0;
            sizet expandedDrawIndexBytes = 0;
            bool any = false;
        };
        GpuHeapSeedTotals computeGpuHeapSeedTotals() const;
        // Manifest-backed estimate for one asset (whole-model geometry costs
        // for meshes; baked pixel bytes for textures). Per sub-asset splits
        // are not tracked. Returns 0 when the manifest has no cost data.
        sizet estimateAssetGpuBytes(const AssetHandle& handle) const;

        // The .spitemat version every listed material file was brought to (0: not yet checked); AssetBaker's load
        // migration sets it, so the scan runs once.
        u32 materialFormat() const { return m_materialFormat; }
        void setMaterialFormat(const u32 version)
        {
            m_materialFormat = version;
            markChanged();
        }

        // The field list of BakedMaterialWriteData (bakedMaterialSchemaHash) the listed entries' content hashes were
        // taken under (0: not yet checked): a hash covers every field, so adding one changes every hash, and the load
        // scan brings them to the new list once.
        u32 materialSchema() const { return m_materialSchema; }
        void setMaterialSchema(const u32 hash)
        {
            m_materialSchema = hash;
            markChanged();
        }

        // Whether this process wrote the manifest file at `path` (resolved like save) with save(): the test guards'
        // attribution of a changed worktree manifest (a sibling process's write is not ours).
        static bool savedByThisProcess(const char* path);

        bool isDirty() const { return m_dirty; }
        // Bumps on every entry change and load: readers re-read only when it moved.
        u64 generation() const { return m_generation.load(std::memory_order_relaxed); }

        // snapshots
        std::unordered_map<InternedString, TextureEntry> getTextures() const;
        std::unordered_map<InternedString, MaterialEntry> getMaterials() const;
        std::unordered_map<InternedString, ModelEntry> getModels() const;

        // Exchange complete, already-prepared manifest contents under both locks; no entries are copied.
        void swapContents(AssetManifest& other);
        // Capture all entries and indexes for a later rollback.
        void copyContentsFrom(const AssetManifest& other);
        // Refuse without changing either manifest if the live generation moved since preparation.
        bool swapContentsIfGeneration(AssetManifest& other, u64 expectedGeneration);
        // Restore a prior exchange only if no later write occurred, including the original dirty state.
        bool rollbackContentsIfGeneration(AssetManifest& other, u64 expectedGeneration, bool originalDirty);

        // Removal methods
        bool removeTexture(const InternedString& name);
        bool removeMaterial(const InternedString& name);
        bool removeModel(const InternedString& name);
        bool removeEnvironment(const InternedString& name);

    private:
        Expected<void> saveInternal(const char* path, bool snapshot);
        void swapContentsLocked(AssetManifest& other);
        void markChanged()
        {
            m_dirty = true;
            m_generation.fetch_add(1, std::memory_order_relaxed);
        }

        mutable std::shared_mutex m_mutex;
        std::atomic<bool> m_dirty = false;
        std::atomic<u64> m_generation = 0;
        u32 m_materialFormat = 0;
        u32 m_materialSchema = 0;

        std::unordered_map<InternedString, TextureEntry> m_textures;
        std::unordered_map<InternedString, MaterialEntry> m_materials;
        std::unordered_map<InternedString, ModelEntry> m_models;
        std::unordered_map<InternedString, SpriteAtlasEntry> m_spriteAtlases;
        std::unordered_map<InternedString, EnvironmentEntry> m_environments;
        std::unordered_map<InternedString, OceanProfileEntry> m_oceanProfiles;
        std::unordered_map<InternedString, SkeletonEntry> m_skeletons;
        std::unordered_map<InternedString, AnimationEntry> m_animations;

        std::unordered_map<InternedString, InternedString> m_textureHashToName;
        std::unordered_map<InternedString, InternedString> m_materialHashToName;
    };
} // namespace spite
