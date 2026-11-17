#pragma once
#include <chrono>
#include <memory>
#include "asset/AssetData.hpp"
#include "asset/AssetBakerConfig.hpp"
#include "asset/BakedAssetFormat.hpp"
#include "asset/AssetAuthoringExport.hpp"
#include "asset/AssetManifest.hpp"
#include "asset/EnvironmentImport.hpp"
#include "asset/ModelImportSettings.hpp"
#include "asset/TextureMips.hpp"
#include "asset/WriteBakedFunctions.hpp"
#include "base/Expected.hpp"
#include "base/MathTypes.hpp"

namespace spite
{
    class IOManager;

    struct BakeResult
    {
        bool success = false;
        InternedString name; // Canonical name assigned by manifest
        InternedString outputPath;
        InternedString errorMessage;
        std::vector<GltfExtensionDiagnostic> diagnostics;
        bool upToDate = false; // sprite atlas: the source folder was unchanged, nothing was written
    };

    struct BakeDirectoryResult
    {
        u32 baked = 0;
        u32 failed = 0;
        u32 skipped = 0; // a bake of that source was already in flight
        u32 timedOut = 0; // still in flight when the wait ended
    };

    enum class BakeRequest : u8
    {
        Requested,
        AlreadyInFlight,
        Rejected // the IO workers are stopped
    };

    class SPITE_ASSET_AUTHORING_API AssetBaker
    {
    public:
        AssetBaker(IOManager& manager);
        ~AssetBaker();

        AssetBaker(const AssetBaker&) = delete;
        AssetBaker& operator=(const AssetBaker&) = delete;

        Expected<void> initialize(const AssetBakerConfig& config = {});
        void shutdown();
        bool saveManifest();

        // Bake operations - return canonical names via BakeResult. A texture bakes with its full mip chain,
        // averaged as `filter` says (the slot that uses it decides: bakedFilterFor); the same pixels and filter
        // give the same bytes. An image another slot baked first keeps that bake when both store it as RGBA8 (the
        // loader re-filters a chain that does not serve the space it is bound in); a compressed bake is its own
        // manifest entry per format and encoder version (TEXTURE_ENCODER_VERSION is part of its content hash).
        // `compression` is the caller's choice here and an importer's option in a model bake (`textures`); a
        // direct bake keeps RGBA8 unless asked. A compressed chain is the RGBA8 chain, encoded: the baked format
        // follows the filter's slot class (bakedTextureFormatFor).
        BakeResult bakeTexture(const InternedString& sourcePath, BakedTextureFilter filter = BakedTextureFilter::SRGB,
                               TextureCompression compression = TextureCompression::Uncompressed);

        // Bakes pre-decoded RGBA8 pixels (embedded images) under the asset's name; a texture that holds baked
        // blocks (loadBakedTexture's result) is refused.
        BakeResult bakeTexture(const TextureAsset& texture, BakedTextureFilter filter = BakedTextureFilter::SRGB,
                               TextureCompression compression = TextureCompression::Uncompressed);
        BakeResult bakeMaterial(const ParsedMaterialData& material,
                                TextureCompression compression = TextureCompression::Uncompressed);
        // sourcePath is UTF-8 (pathFromUtf8 opens it, as every external image the model names), whatever the
        // machine's ANSI code page. name: the model's manifest name, the source file's stem when invalid (a stem
        // with non-ASCII characters through AssetManifest::safeBakeName); names are bake file names, so the caller
        // keeps them to ASCII letters, digits, '_', '-' and '.'.
        BakeResult bakeModel(const InternedString& sourcePath, InternedString name = {});

        // Packs the PNGs under folder (recursively) into one atlas page plus a sprite table, named name (the
        // folder's name when invalid): <output>/textures/<name>_atlas.spitetex, <output>/atlases/<name>.spriteatlas
        // and manifest entries for both. A folder whose source hash is unchanged writes nothing (BakeResult::upToDate).
        // Invariant: the same folder contents give the same bytes; a failure leaves the manifest and bakes as they
        // were.
        BakeResult bakeSpriteAtlas(const InternedString& folder, InternedString name = {});

        // Imports an environment (importEnvironmentSource: an equirect image, a cube cross or a folder of six faces)
        // and bakes it as <output>/environments/<name>.spiteenv in the format, with its manifest entry. Baking the
        // same name again replaces the bake; the same source and format give the same bytes. name: the source stem
        // when invalid, else ASCII letters, digits, underscore, minus and dot. sourcePath is UTF-8. A failure
        // leaves the manifest and bakes as they were.
        BakeResult bakeEnvironment(const InternedString& sourcePath, InternedString name = {},
                                   EnvironmentFormat format = EnvironmentFormat::Rgba16Float);
        BakeResult bakeOceanProfile(const InternedString& sourcePath, InternedString name);
        std::string oceanProfileBakedPath(InternedString name) const;

        // Writes a material's bake from its fields under name and adds or
        // overwrites its manifest entry (the material control commands).
        Expected<void> writeMaterial(const InternedString& name, const BakedMaterialWriteData& fields);
        // Drops the material's manifest entry (models referencing it fall back
        // to the default material) and its bake file.
        Expected<void> removeMaterial(const InternedString& name);

        // Bakes every file of the extension under the directory on the IO workers and returns when each has
        // settled or the timeout elapsed.
        // Invariant: bakeDirectory returns only after every bake it started settled (baked or failed) or timed
        //   out, and reports the timed-out ones; it never reports a bake complete that is still running.
        // Invariant: ~AssetBaker cancels its queued async bakes (they settle as ErrorCode::Cancelled without
        //   touching the baker) and waits, unbounded, for the ones already running, so an IO worker never runs
        //   a bake against a destroyed baker or manifest.
        // Invariant: a source whose save is already pending is not requested again (counted skipped); a bake
        //   the IO workers did not accept (stopped) is counted failed.
        // Invariant: never wait for async bakes (bakeDirectory, ~AssetBaker) from an IO worker.
        BakeDirectoryResult bakeDirectory(const char* directory, const char* extension = ".gltf",
                                          std::chrono::milliseconds timeout = std::chrono::minutes(5));
        BakeRequest requestBakeAsync(const InternedString& sourcePath);

        // Get baked paths by name (lookup through manifest)
        InternedString getBakedTexturePath(const InternedString& name) const;
        InternedString getBakedMaterialPath(const InternedString& name) const;
        InternedString getBakedModelPath(const InternedString& name) const;

        const AssetBakerConfig& config() const { return m_config; }
        AssetManifest& getManifest() { return m_manifest; }
        const AssetManifest& getManifest() const { return m_manifest; }

    private:
        IOManager& m_ioManager;

        InternedString buildMaterialSignature(const ParsedMaterialData& material);
        static InternedString materialContentHash(const BakedMaterialWriteData& fields);
        std::string materialBakedPath(const InternedString& name) const;
        void migrateLegacyMaterials();

        // Shared body of both bakeTexture overloads; pixels are RGBA8
        // (STBI_rgb_alpha) and remain caller-owned.
        BakeResult bakeTexturePixels(const u8* pixels, int width, int height, const InternedString& baseName,
                                     BakedTextureFilter filter, TextureCompression compression);

        Expected<void> writeOutputFile(const char* path, const void* data, sizet size);
        Expected<void> writeOutputFile(const char* path, const std::vector<std::byte>& data);

        AssetManifest m_manifest;
        // m_config's name pointers point at these copies (initialize), so the config outlives the caller's strings
        AssetBakerConfig m_config;
        std::string m_outputDirectory, m_textureSubdir, m_materialSubdir, m_atlasSubdir, m_environmentSubdir,
            m_sourceRoot;
        bool m_initialized = false;

        // async bakes in flight and how they settled; shared with their completion handlers
        struct AsyncBakes;
        std::shared_ptr<AsyncBakes> m_asyncBakes;
    };
} // namespace spite
