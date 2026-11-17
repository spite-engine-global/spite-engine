#include "asset/AssetBaker.hpp"
#include "base/ContentRoot.hpp"

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_set>

#include "base/Assert.hpp"
#include "base/IOManager.hpp"
#include "asset/AssetImportingFunctions.hpp"
#include "asset/OceanProfile.hpp"
#include "base/HashCombine.hpp"
#include "base/Logging.hpp"
#include "base/StringInterner.hpp"
#include "base/io/FileSystem.hpp"
#include "asset/BakedAnimation.hpp"
#include "asset/AssetBakeOperations.hpp"
#include "asset/TextureCompression.hpp"
#include "asset/TextureStorage.hpp"
#include "asset/WriteBakedFunctions.hpp"
#include "profiling/Profiling.hpp"

namespace spite
{
    namespace
    {
        constexpr const char* SKELETON_SUBDIR = "skeletons";
        constexpr const char* ANIMATION_SUBDIR = "animations";

        // The files of a model's skeleton, clips and skin, encoded and named but not yet written.
        struct AnimationBake
        {
            std::vector<std::pair<std::string, std::vector<std::byte>>> files; // path, bytes
            std::pair<InternedString, AssetManifest::SkeletonEntry> skeleton;
            std::vector<std::pair<InternedString, AssetManifest::AnimationEntry>> clips;
            InternedString skin; // the path, invalid when no submesh is skinned
        };

        // A clip's asset name: <model>.<clip name as a file name>, an unnamed clip clip<index>, a repeat _2, _3, ...
        // (bake file names ignore case, so the repeat does too)
        InternedString clipAssetName(const InternedString& model, const InternedString& clip, const sizet index,
                                     std::unordered_set<std::string>& used)
        {
            const std::string base = clip.isValid() && !clip.toView().empty()
                ? AssetManifest::safeBakeName(clip.toView())
                : std::format("clip{}", index);
            std::string name = std::format("{}.{}", model.toView(), base);
            for (u32 repeat = 2; !used.insert(AssetManifest::bakeFileKey(name)).second; ++repeat)
            {
                name = std::format("{}.{}_{}", model.toView(), base, repeat);
            }
            return InternedString(name);
        }

        // Also bounds the model's skinned submeshes over every pose of the clips it encodes (they change in `model`).
        Expected<AnimationBake> encodeAnimation(ParsedModelData& model, const InternedString& modelName,
                                                const std::string& output, const ModelImportSettings& settings)
        {
            AnimationBake bake;
            const RawSkeleton& skeleton = model.skeleton;
            Expected<std::vector<std::byte>> skeletonBytes = encodeSkeleton(skeleton, modelName);
            if (!skeletonBytes)
            {
                return std::unexpected(skeletonBytes.error());
            }
            const std::string skeletonPath =
                std::format("{}/{}/{}.spiteskel", output, SKELETON_SUBDIR, modelName.toView());
            bake.skeleton = {
                modelName,
                {.baked = InternedString(skeletonPath.c_str()),
                 .contentHash = AssetManifest::generateContentHash(skeletonBytes->data(), skeletonBytes->size()),
                 .model = modelName,
                 .jointCount = static_cast<u32>(skeleton.joints.size())}};
            bake.files.emplace_back(skeletonPath, std::move(*skeletonBytes));

            const ClipBakeOptions options{.sampleRate = static_cast<f32>(settings.clipSampleRate),
                                          .stripScaleTracks = settings.stripScaleTracks};
            std::unordered_set<std::string> used;
            std::vector<std::vector<std::byte>> clipFiles;
            for (sizet i = 0; i < model.clips.size(); ++i)
            {
                Expected<std::vector<std::byte>> bytes = encodeClip(model.clips[i], skeleton, modelName, options);
                if (!bytes)
                {
                    return std::unexpected(bytes.error());
                }
                clipFiles.push_back(*bytes);
                const InternedString name = clipAssetName(modelName, model.clips[i].name, i, used);
                const std::string path = std::format("{}/{}/{}.spiteanim", output, ANIMATION_SUBDIR, name.toView());
                Expected<BakedClip> header = decodeClip(*bytes);
                SASSERT(header.has_value());
                bake.clips.push_back({name,
                                      {.baked = InternedString(path.c_str()),
                                       .contentHash = AssetManifest::generateContentHash(bytes->data(), bytes->size()),
                                       .skeleton = modelName,
                                       .model = modelName,
                                       .duration = header->duration,
                                       .trackCount = header->trackCount}});
                bake.files.emplace_back(path, std::move(*bytes));
            }

            if (const Expected<void> bounded = bakeSkinnedBounds(model, bake.files.front().second, clipFiles, settings);
                !bounded)
            {
                return std::unexpected(bounded.error());
            }
            BakedSkin skin;
            skin.skeleton = modelName;
            skin.skeletonIdentity = skeletonIdentity(skeleton);
            for (u32 i = 0; i < model.mesh.submeshes.size(); ++i)
            {
                if (model.mesh.submeshes[i].skin)
                {
                    skin.submeshes.push_back({i, *model.mesh.submeshes[i].skin});
                }
            }
            if (!skin.submeshes.empty())
            {
                const std::string path = std::format("{}/{}.spiteskin", output, modelName.toView());
                bake.skin = InternedString(path.c_str());
                bake.files.emplace_back(path, encodeSkin(skin));
            }
            return bake;
        }
    } // namespace

    struct AssetBaker::AsyncBakes
    {
        std::mutex mutex;
        std::condition_variable settledCv;
        u32 inFlight = 0;
        u32 started = 0;
        u32 failed = 0;
        u32 running = 0; // bakes executing on a worker, i.e. touching the baker
        bool cancelled = false; // the baker is being destroyed: queued bakes must not start

        // true when every bake settled before the deadline
        bool waitIdle(const std::chrono::steady_clock::time_point deadline)
        {
            std::unique_lock lock(mutex);
            return settledCv.wait_until(lock, deadline, [this] { return inFlight == 0; });
        }
    };

    AssetBaker::AssetBaker(IOManager& manager) : m_ioManager(manager), m_asyncBakes(std::make_shared<AsyncBakes>()) {}

    AssetBaker::~AssetBaker()
    {
        {
            std::unique_lock lock(m_asyncBakes->mutex);
            m_asyncBakes->cancelled = true;
            m_asyncBakes->settledCv.wait(lock, [this] { return m_asyncBakes->running == 0; });
        }
        if (m_initialized)
        {
            shutdown();
        }
    }

    Expected<void> AssetBaker::initialize(const AssetBakerConfig& config)
    {
        // the config names are copied: the caller's strings (or this baker's own, on a re-initialize) need not
        // outlive the baker, whose destructor saves the manifest through them
        std::string output = config.outputDirectory;
        std::string texture = config.textureSubdir;
        std::string material = config.materialSubdir;
        std::string atlas = config.atlasSubdir;
        std::string environment = config.environmentSubdir;
        std::string root = config.sourceRoot ? config.sourceRoot : "";
        m_config = config;
        m_outputDirectory = std::move(output);
        m_textureSubdir = std::move(texture);
        m_materialSubdir = std::move(material);
        m_atlasSubdir = std::move(atlas);
        m_environmentSubdir = std::move(environment);
        m_sourceRoot = std::move(root);
        m_config.outputDirectory = m_outputDirectory.c_str();
        m_config.textureSubdir = m_textureSubdir.c_str();
        m_config.materialSubdir = m_materialSubdir.c_str();
        m_config.atlasSubdir = m_atlasSubdir.c_str();
        m_config.environmentSubdir = m_environmentSubdir.c_str();
        m_config.sourceRoot = config.sourceRoot ? m_sourceRoot.c_str() : nullptr;

#ifndef SPITE_SHIPPING
        for (const std::string& directory : {std::string(m_config.outputDirectory),
                                             std::string(m_config.outputDirectory) + "/" + m_config.textureSubdir,
                                             std::string(m_config.outputDirectory) + "/" + m_config.materialSubdir})
        {
            std::error_code error;
            std::filesystem::create_directories(resolveContentPath(directory), error);
            if (error)
            {
                return std::unexpected(makeError(
                    ErrorCode::FileWriteFailed,
                    std::format("Failed to create bake directory {}: {}", directory, error.message()).c_str()));
            }
        }

#endif
        std::string manifestPath = std::string(m_config.outputDirectory) + "/asset_manifest.json";
        auto loadResult = m_manifest.load(manifestPath.c_str());
        if (!loadResult.has_value())
        {
#ifdef SPITE_SHIPPING
            return std::unexpected(loadResult.error());
#else
            SWARNING_LOG("[AssetBaker] Could not load manifest, starting fresh: {}",
                         loadResult.error().message.c_str());
#endif
        }

#ifndef SPITE_SHIPPING
        migrateLegacyMaterials();
#endif

        m_initialized = true;
        SINFO_LOG("[AssetBaker] Initialized with output directory: {}", m_config.outputDirectory);
        return {};
    }

    // Brings every material file the manifest lists to the tagged format and its entry's hash to the file's
    // fields, then records the version and the field list in the manifest so later loads skip the scan. A tagged file
    // with a stale hash (a crash between the file rewrite and the manifest save, another process, a refused rename, a
    // field added to the list since the hash was taken) is fixed here too: a stale hash would make a re-bake of
    // unchanged content take a new name.
    void AssetBaker::migrateLegacyMaterials()
    {
        const u32 schema = bakedMaterialSchemaHash();
        if (m_manifest.materialFormat() >= MATERIAL_FILE_VERSION && m_manifest.materialSchema() == schema)
        {
            return;
        }
        u32 rewrittenFiles = 0;
        bool settled = true;
        const auto materials = m_manifest.getMaterials(); // a copy: the loop rewrites entries
        for (const auto& [name, entry] : materials)
        {
            if (!entry.baked.isValid() || entry.baked.toView().empty())
            {
                continue; // the default material has no file
            }
            const auto migration = migrateBakedMaterial(entry.baked.toView());
            if (!migration)
            {
                SWARNING_LOG("[AssetBaker] Cannot migrate material '{}': {}", name.toView(),
                             migration.error().message.c_str());
                settled = settled && !std::filesystem::exists(resolveContentPath(entry.baked.toView()));
                continue;
            }
            rewrittenFiles += migration->rewritten ? 1 : 0;
            const InternedString hash = materialContentHash(migration->fields);
            if (hash != entry.contentHash)
            {
                AssetManifest::MaterialEntry updated = entry;
                updated.contentHash = hash;
                m_manifest.overwriteMaterial(name, updated);
            }
        }
        if (rewrittenFiles > 0)
        {
            SINFO_LOG("[AssetBaker] Migrated {} legacy material file(s) to the tagged format", rewrittenFiles);
        }
        if (settled)
        {
            m_manifest.setMaterialFormat(MATERIAL_FILE_VERSION);
            m_manifest.setMaterialSchema(schema);
        }
        saveManifest();
    }

    void AssetBaker::shutdown()
    {
#ifndef SPITE_SHIPPING
        saveManifest();
#endif
        m_initialized = false;
    }

    bool AssetBaker::saveManifest()
    {
        if (m_manifest.isDirty())
        {
            std::string manifestPath = std::string(m_config.outputDirectory) + "/asset_manifest.json";
            auto saveResult = m_manifest.save(manifestPath.c_str());
            if (!saveResult.has_value())
            {
                SERROR_LOG("[AssetBaker] Failed to save manifest: {}", saveResult.error().message.c_str());
                return false;
            }
        }
        return true;
    }

    InternedString AssetBaker::buildMaterialSignature(const ParsedMaterialData& material)
    {
        std::stringstream ss;

        // Include texture names (not paths)
        if (material.albedoTexture && material.albedoTexture->getName().isValid())
        {
            InternedString texName = AssetManifest::deriveNameFromPath(material.albedoTexture->getName());
            ss << texName.toView();
        }
        ss << "|";

        if (material.normalTexture && material.normalTexture->getName().isValid())
        {
            InternedString texName = AssetManifest::deriveNameFromPath(material.normalTexture->getName());
            ss << texName.toView();
        }
        ss << "|";

        if (material.metallicRoughnessTexture && material.metallicRoughnessTexture->getName().isValid())
        {
            InternedString texName = AssetManifest::deriveNameFromPath(material.metallicRoughnessTexture->getName());
            ss << texName.toView();
        }
        ss << "|";

        ss << material.albedoFactor.x << "," << material.albedoFactor.y << "," << material.albedoFactor.z << ","
           << material.albedoFactor.w;

        return InternedString(ss.str().c_str());
    }

    Expected<void> AssetBaker::writeOutputFile(const char* path, const void* data, sizet size)
    {
        std::error_code error;
        std::filesystem::create_directories(resolveContentPath(path).parent_path(), error);
        if (error)
        {
            return std::unexpected(
                makeError(ErrorCode::FileWriteFailed,
                          std::format("Failed to create the directory of {}: {}", path, error.message()).c_str()));
        }

        std::ofstream file(resolveContentPath(path), std::ios::binary);
        if (!file.is_open())
        {
            return std::unexpected(makeError(ErrorCode::FileWriteFailed,
                                             std::format("Failed to open file for writing: {}", path).c_str()));
        }

        file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        return {};
    }

    Expected<void> AssetBaker::writeOutputFile(const char* path, const std::vector<std::byte>& data)
    {
        return writeOutputFile(path, data.data(), data.size());
    }

    InternedString AssetBaker::getBakedTexturePath(const InternedString& name) const
    {
        return m_manifest.getTextureBakedPath(name);
    }

    InternedString AssetBaker::getBakedMaterialPath(const InternedString& name) const
    {
        return m_manifest.getMaterialBakedPath(name);
    }

    InternedString AssetBaker::getBakedModelPath(const InternedString& name) const
    {
        return m_manifest.getModelBakedPath(name);
    }

    BakeResult AssetBaker::bakeTexture(const InternedString& sourcePath, const BakedTextureFilter filter,
                                       const TextureCompression compression)
    {
        int width, height, channels;
        u8* pixels = decodeImageFile(sourcePath, width, height, channels);

        if (!pixels)
        {
            BakeResult result;
            result.errorMessage = InternedString(std::format("Failed to load texture: {}", sourcePath.toView()));
            SERROR_LOG("[AssetBaker] {}", result.errorMessage.toView());
            return result;
        }

        BakeResult result = bakeTexturePixels(pixels, width, height, AssetManifest::deriveNameFromPath(sourcePath),
                                              filter, compression);
        freeTexture(pixels);
        return result;
    }

    BakeResult AssetBaker::bakeTexture(const TextureAsset& texture, const BakedTextureFilter filter,
                                       const TextureCompression compression)
    {
        if (!texture.getPixels() || texture.getWidth() <= 0 || texture.getHeight() <= 0 ||
            bakedTextureFormatCompressed(texture.getFormat()))
        {
            BakeResult result;
            result.errorMessage = InternedString(
                std::format("Cannot bake texture '{}': no decoded RGBA8 pixels", texture.getName().toView()));
            SWARNING_LOG("[AssetBaker] {}", result.errorMessage.toView());
            return result;
        }
        return bakeTexturePixels(texture.getPixels(), texture.getWidth(), texture.getHeight(), texture.getName(),
                                 filter, compression);
    }

    // pixels are RGBA8 (STBI_rgb_alpha) and stay caller-owned
    BakeResult AssetBaker::bakeTexturePixels(const u8* pixels, int width, int height, const InternedString& baseName,
                                             const BakedTextureFilter filter, const TextureCompression compression)
    {
        BakeResult result;

        const sizet pixelDataSize = static_cast<sizet>(width) * height * 4;
        bool opaque = true;
        for (sizet i = 3; compression == TextureCompression::Fast && opaque && i < pixelDataSize; i += 4)
        {
            opaque = pixels[i] == 255;
        }
        const u8 format = bakedTextureFormatFor(filter, compression, opaque);
        // An RGBA8 bake keeps the hash of its pixels, so it dedupes across slots and an old manifest still matches.
        // A compressed one is its own entry per format, filter and encoder version: a change to an encoder never
        // serves a bake the older one wrote.
        InternedString contentHash = AssetManifest::generateContentHash(pixels, pixelDataSize);
        if (bakedTextureFormatCompressed(format) || filter == BakedTextureFilter::SRGB_ALPHA)
        {
            const std::string key = std::format("{}|format{}|filter{}|encoder{}", contentHash.toView(), format,
                                                static_cast<u8>(filter), TEXTURE_ENCODER_VERSION);
            contentHash = AssetManifest::generateContentHash(key.data(), key.size());
        }

        // Content-hash deduplication
        InternedString existingName = m_manifest.findTextureByHash(contentHash);
        if (existingName.isValid() && !m_config.forceRebake)
        {
            result.success = true;
            result.name = existingName;
            result.outputPath = m_manifest.getTextureBakedPath(existingName);
            SDEBUG_LOG("[AssetBaker] Texture '{}' content matches existing '{}', reusing", baseName.toView(),
                       existingName.toView());
            return result;
        }

        std::string bakedPath = std::string(m_config.outputDirectory) + "/" + m_config.textureSubdir + "/" +
            std::string(baseName.toView()) + ".spitetex";

        SpiteTextureHeader header;
        header.format = format;
        header.width = static_cast<u32>(width);
        header.height = static_cast<u32>(height);
        header.mipFilter = static_cast<u8>(filter);
        header.mipLevels = textureMipCount(header.width, header.height);
        std::vector<u8> chain = buildMipChain(pixels, header.width, header.height, filter);
        if (bakedTextureFormatCompressed(format))
        {
            chain = compressMipChain(chain.data(), header.width, header.height, header.mipLevels, format,
                                     m_config.encoderThreads);
        }

        const auto fileBytes = encodeBakedTexture(header, chain);
        if (!fileBytes)
        {
            result.errorMessage = InternedString(fileBytes.error().message.c_str());
            return result;
        }

        AssetManifest::TextureEntry texEntry;
        texEntry.baked = InternedString(bakedPath.c_str());
        texEntry.contentHash = contentHash;
        texEntry.pixelBytes = chain.size();
        texEntry.format = format;
        texEntry.fileBytes = fileBytes->size();

        InternedString finalName = baseName;
        if (!m_manifest.tryAddTexture(baseName, texEntry))
        {
            // Name collision auto-rename
            finalName = AssetManifest::makeUniqueName(baseName, m_manifest.getTextures());
            bakedPath = std::string(m_config.outputDirectory) + "/" + m_config.textureSubdir + "/" +
                std::string(finalName.toView()) + ".spitetex";
            texEntry.baked = InternedString(bakedPath.c_str());
            m_manifest.tryAddTexture(finalName, texEntry);
            SDEBUG_LOG("[AssetBaker] Texture name '{}' collision, using '{}'", baseName.toView(), finalName.toView());
        }

        // Write .spitetex binary
        std::ofstream file(resolveContentPath(bakedPath), std::ios::binary);
        if (!file.is_open())
        {
            result.errorMessage = InternedString(std::format("Failed to write texture: {}", bakedPath).c_str());
            return result;
        }

        file.write(reinterpret_cast<const char*>(fileBytes->data()), static_cast<std::streamsize>(fileBytes->size()));
        file.close();

        result.success = true;
        result.name = finalName;
        result.outputPath = InternedString(bakedPath.c_str());

        SDEBUG_LOG("[AssetBaker] Baked texture '{}' -> {} (name: '{}')", baseName.toView(), bakedPath,
                   finalName.toView());
        return result;
    }

    std::string AssetBaker::oceanProfileBakedPath(const InternedString name) const
    {
        return std::string(m_config.outputDirectory) + "/oceans/" + std::string(name.toView()) + ".spoc";
    }

    BakeResult AssetBaker::bakeOceanProfile(const InternedString& sourcePath, const InternedString name)
    {
        BakeResult result;
        const auto fail = [&](const std::string& message)
        {
            result.errorMessage = InternedString(message.c_str());
            return result;
        };
        if (!name.isValid() || name.toView().size() > 64 || AssetManifest::safeBakeName(name.toView()) != name.toView())
            return fail("invalid ocean profile name");
        for (const auto& [existing, entry] : m_manifest.getOceanProfiles())
        {
            if (AssetManifest::bakeFileKey(existing.toView()) == AssetManifest::bakeFileKey(name.toView()))
                return fail("ocean profile name already exists");
        }
        std::ifstream input(pathFromUtf8(sourcePath.toView()), std::ios::binary | std::ios::ate);
        if (!input)
            return fail("cannot open ocean profile source");
        const auto size = input.tellg();
        if (size < 33 || size > 128ll * 1024 * 1024 + 33)
            return fail("invalid ocean profile size");
        std::vector<std::byte> bytes(static_cast<sizet>(size));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), size))
            return fail("cannot read ocean profile source");
        auto profile = decodeOceanProfile(bytes);
        if (!profile)
            return fail(profile.error().message);
        const std::string baked = oceanProfileBakedPath(name);
        const auto destination = resolveContentPath(pathFromUtf8(baked));
        std::error_code error;
        const bool exists = std::filesystem::exists(destination, error);
        if (error)
            return fail("ocean profile output cannot be checked");
        if (exists)
        {
            if (!isPlainEntry(destination, false))
                return fail("ocean profile cache must be a plain file");
            std::ifstream cached(destination, std::ios::binary | std::ios::ate);
            if (!cached || cached.tellg() != size)
                return fail("ocean profile output conflicts with retained bytes");
            std::vector<std::byte> previous(bytes.size());
            cached.seekg(0);
            if (!cached.read(reinterpret_cast<char*>(previous.data()), size) || previous != bytes)
                return fail("ocean profile output conflicts with retained bytes");
            result.upToDate = true;
        }
        std::filesystem::create_directories(destination.parent_path(), error);
        if (error)
            return fail(error.message());
        if (!isPlainEntry(destination.parent_path(), true))
            return fail("ocean profile cache directory is not plain");
        if (!result.upToDate)
            if (auto written = writeBinaryFileAtomicallyNew(destination, bytes); !written)
                return fail(written.error().message);
        m_manifest.overwriteOceanProfile(name,
                                         {.baked = InternedString(baked.c_str()),
                                          .source = sourcePath,
                                          .contentHash = AssetManifest::generateContentHash(bytes.data(), bytes.size()),
                                          .width = profile->width,
                                          .height = profile->height,
                                          .frameCount = profile->frameCount,
                                          .tileMetres = profile->tileMetres,
                                          .framesPerSecond = profile->framesPerSecond,
                                          .maxDisplacementMetres = profile->maxDisplacementMetres,
                                          .loop = profile->loop});
        result.success = true;
        result.name = name;
        result.outputPath = InternedString(baked.c_str());
        return result;
    }

    BakeResult AssetBaker::bakeEnvironment(const InternedString& sourcePath, InternedString name,
                                           const EnvironmentFormat format)
    {
        BakeResult result;
        const auto fail = [&](const std::string& message)
        {
            result.errorMessage = InternedString(message.c_str());
            SERROR_LOG("[AssetBaker] {}", message);
            return result;
        };
        if (!name.isValid())
        {
            name = AssetManifest::deriveNameFromPath(sourcePath);
        }
        for (const auto& [existing, entry] : m_manifest.getEnvironments())
        {
            if (existing != name &&
                AssetManifest::bakeFileKey(existing.toView()) == AssetManifest::bakeFileKey(name.toView()))
            {
                return fail(
                    std::format("environment name '{}' clashes with environment '{}' (bake file names ignore case)",
                                name.toView(), existing.toView()));
            }
        }
        const Expected<HdrImage> image = importEnvironmentSource(std::string(sourcePath.toView()));
        if (!image)
        {
            return fail(
                std::format("Failed to import environment '{}': {}", sourcePath.toView(), image.error().message));
        }
        const Expected<std::vector<std::byte>> bytes = encodeEnvironment(*image, format);
        if (!bytes)
        {
            return fail(std::format("Failed to bake environment '{}': {}", name.toView(), bytes.error().message));
        }
        const std::string bakedPath = std::string(m_config.outputDirectory) + "/" + m_config.environmentSubdir + "/" +
            std::string(name.toView()) + ".spiteenv";
        if (const Expected<void> written = writeOutputFile(bakedPath.c_str(), *bytes); !written)
        {
            return fail(written.error().message);
        }

        SpiteEnvironmentHeader header;
        std::memcpy(&header, bytes->data(), sizeof(header));
        m_manifest.overwriteEnvironment(
            name,
            {.baked = InternedString(bakedPath.c_str()),
             .contentHash = AssetManifest::generateContentHash(bytes->data(), bytes->size()),
             .source = sourcePath,
             .format = header.format,
             .width = header.width,
             .height = header.height,
             .mipLevels = header.mipLevels,
             .pixelBytes = bytes->size() - sizeof(header)});
        result.success = true;
        result.name = name;
        result.outputPath = InternedString(bakedPath.c_str());
        SDEBUG_LOG("[AssetBaker] Baked environment '{}' -> {}", name.toView(), bakedPath);
        return result;
    }

    BakeResult AssetBaker::bakeMaterial(const ParsedMaterialData& material, const TextureCompression compression)
    {
        BakeResult result;

        InternedString materialName(material.name);
        InternedString failedTexture;

        // Process textures first bake each from source. Embedded images arrive
        // pre-decoded in the TextureAsset; external ones resolve their source
        // path at bake time.
        auto processTexture = [&](const std::shared_ptr<TextureAsset>& tex, const BakedTextureFilter filter,
                                  const TextureCompression mode) -> InternedString
        {
            if (!tex)
            {
                return {};
            }
            auto bakeResult =
                tex->getPixels() ? bakeTexture(*tex, filter, mode) : bakeTexture(tex->getName(), filter, mode);
            if (!bakeResult.success)
            {
                failedTexture = tex->getName();
                SWARNING_LOG("[AssetBaker] Failed to bake texture: {}", tex->getName().toView());
                return {};
            }
            return bakeResult.name;
        };

        BakedMaterialWriteData writeData;
        const auto terrainCount =
            std::ranges::count_if(material.terrainTextures, [](const auto& texture) { return !!texture; });
        if (terrainCount != 0 && terrainCount != TERRAIN_TEXTURE_SLOT_COUNT)
        {
            result.errorMessage = InternedString("Invalid or incomplete terrain material profile");
            return result;
        }
        writeData.albedoFactor = material.albedoFactor;
        writeData.metallicFactor = material.metallicFactor;
        writeData.roughnessFactor = material.roughnessFactor;
        writeData.emissiveFactor = material.emissiveFactor;
        writeData.emissiveStrength = material.emissiveStrength;
        writeData.occlusionStrength = material.occlusionStrength;
        writeData.normalScale = material.normalScale;
        writeData.thinLeafTransmission = material.thinLeafTransmission;
        writeData.alphaMode = material.alphaMode;
        writeData.alphaCutoff = material.alphaCutoff;
        writeData.doubleSided = material.doubleSided;
        writeData.castsShadow = material.castsShadow;
        writeData.unlit = material.unlit;
        writeData.uvOffset = material.uvOffset;
        writeData.uvScale = material.uvScale;
        writeData.uvRotation = material.uvRotation;
        writeData.textureFilter = material.textureFilter;
        for (const MaterialTextureSlot slot : MATERIAL_TEXTURE_SLOTS)
        {
            const MaterialSamplerState sampler = material.samplerState(slot);
            writeData.setWrap(slot, sampler.wrapU, sampler.wrapV);
            const bool alphaColour = slot == MaterialTextureSlot::Albedo &&
                material.space(slot) == TextureColorSpace::SRGB &&
                (material.alphaMode == MaterialAlphaMode::Mask || material.alphaMode == MaterialAlphaMode::Blend ||
                 material.alphaMode == MaterialAlphaMode::Additive);
            const BakedTextureFilter filter =
                alphaColour ? BakedTextureFilter::SRGB_ALPHA : bakedFilterFor(slot, material.space(slot));
            writeData.textureName(slot) = processTexture(material.texture(slot), filter, compression);
            writeData.textureSpace(slot) = material.space(slot);
        }
        if (material.terrainTextures[0])
        {
            writeData.terrainDetailMetres = material.terrainDetailMetres;
            writeData.terrainGrassDetailMetres = material.terrainGrassDetailMetres;
            writeData.terrainSoilDetailMetres = material.terrainSoilDetailMetres;
            writeData.terrainRockDetailMetres = material.terrainRockDetailMetres;
            for (const TerrainTextureSlot slot : TERRAIN_TEXTURE_SLOTS)
            {
                const bool rough = slot == TerrainTextureSlot::GrassRoughness ||
                    slot == TerrainTextureSlot::SoilRoughness || slot == TerrainTextureSlot::RockRoughness;
                const auto mode = rough && compression != TextureCompression::Uncompressed
                    ? TextureCompression::SingleChannel
                    : compression;
                writeData.terrainTextureName(slot) =
                    processTexture(material.terrainTextures[static_cast<sizet>(slot)], terrainFilter(slot), mode);
            }
        }
        if (!writeData.validTerrain() || (material.terrainTextures[0] && failedTexture.isValid()))
        {
            result.errorMessage = InternedString("Invalid or incomplete terrain material profile");
            return result;
        }
        if (m_config.strictModelDependencies && failedTexture.isValid())
        {
            result.errorMessage = InternedString(std::format("Failed to bake texture: {}", failedTexture.toView()));
            return result;
        }
        const InternedString contentHash = materialContentHash(writeData);

        // Content-hash dedup: reuse only when the name also matches, or is the
        // unique name an earlier bake of this material took ("Name_2"; a bake
        // again of unchanged content finds it instead of baking "Name_3").
        // Rebinding an identical bake under any other name would silently
        // rewrite what submesh TOCs reference.
        const auto isCollisionRename = [&materialName](const InternedString candidate)
        {
            const std::string_view base = materialName.toView();
            const std::string_view name = candidate.toView();
            return name.size() > base.size() + 1 && name.starts_with(base) && name[base.size()] == '_' &&
                std::ranges::all_of(name.substr(base.size() + 1), [](const char c) { return c >= '0' && c <= '9'; });
        };
        InternedString existingName = m_manifest.findMaterialByHash(contentHash);
        if (existingName.isValid() && (existingName == materialName || isCollisionRename(existingName)) &&
            !m_config.forceRebake)
        {
            result.success = true;
            result.name = existingName;
            result.outputPath = m_manifest.getMaterialBakedPath(existingName);
            SDEBUG_LOG("[AssetBaker] Material '{}' content matches existing bake, reusing", materialName.toView());
            return result;
        }

        // Same name, different content: keep the previous owner's file
        // reachable and bake under a unique name, mirroring the texture
        // collision policy.
        InternedString finalName = materialName;
        if (const auto existing = m_manifest.getMaterialEntry(materialName);
            existing && existing->contentHash != contentHash)
        {
            finalName = AssetManifest::makeUniqueName(materialName, m_manifest.getMaterials());
            SWARNING_LOG("[AssetBaker] Material '{}' already baked with different content; baking as '{}'",
                         materialName.toView(), finalName.toView());
        }

        const std::string bakedPath = materialBakedPath(finalName);
        auto writeResult = writeBakedMaterial(bakedPath.c_str(), writeData);
        if (!writeResult.has_value())
        {
            result.errorMessage = InternedString(writeResult.error().message.c_str());
            return result;
        }

        // Register in manifest overwrite if re-baking existing material
        AssetManifest::MaterialEntry matEntry;
        matEntry.baked = InternedString(bakedPath.c_str());
        matEntry.contentHash = contentHash;
        m_manifest.overwriteMaterial(finalName, matEntry);

        result.success = true;
        result.name = finalName;
        result.outputPath = InternedString(bakedPath.c_str());

        SDEBUG_LOG("[AssetBaker] Baked material: {} -> {} (name: '{}')", material.name.toView(), bakedPath,
                   finalName.toView());
        return result;
    }

    Expected<void> AssetBaker::writeMaterial(const InternedString& name, const BakedMaterialWriteData& fields)
    {
        if (!fields.validTerrain())
            return std::unexpected(makeError(ErrorCode::AssetSaveFailed, "Invalid terrain material profile"));
        const std::string bakedPath = materialBakedPath(name);
        if (Expected<void> written = writeBakedMaterial(bakedPath.c_str(), fields); !written)
        {
            return written;
        }
        m_manifest.overwriteMaterial(
            name, {.baked = InternedString(bakedPath.c_str()), .contentHash = materialContentHash(fields)});
        saveManifest();
        return {};
    }

    Expected<void> AssetBaker::removeMaterial(const InternedString& name)
    {
        const InternedString bakedPath = m_manifest.getMaterialBakedPath(name);
        if (!m_manifest.removeMaterial(name))
        {
            return std::unexpected(makeError(ErrorCode::AssetNotFound,
                                             std::format("no material '{}' in the manifest", name.toView()).c_str()));
        }
        saveManifest();
        std::error_code ignored;
        std::filesystem::remove(resolveContentPath(bakedPath.toView()), ignored);
        return {};
    }

    std::string AssetBaker::materialBakedPath(const InternedString& name) const
    {
        return std::string(m_config.outputDirectory) + "/" + m_config.materialSubdir + "/" +
            std::string(name.toView()) + ".spitemat";
    }

    InternedString AssetBaker::materialContentHash(const BakedMaterialWriteData& fields)
    {
        const std::vector<std::byte> bytes = encodeBakedMaterial(fields);
        return AssetManifest::generateContentHash(bytes.data(), bytes.size());
    }

    BakeResult AssetBaker::bakeModel(const InternedString& sourcePath, const InternedString name)
    {
        BakeResult result;

        if (m_config.sourceRoot)
        {
            std::error_code error;
            const auto root = std::filesystem::weakly_canonical(
                std::filesystem::absolute(pathFromUtf8(m_config.sourceRoot), error), error);
            const auto withinRoot = [&](const std::filesystem::path& path)
            {
                const auto resolved = std::filesystem::weakly_canonical(std::filesystem::absolute(path, error), error);
                if (error)
                    return false;
                const auto relative = resolved.lexically_relative(root);
                return !relative.empty() && relative != "." && *relative.begin() != "..";
            };
            if (error || !withinRoot(pathFromUtf8(sourcePath.toView())) ||
                (!m_config.importSettings && !withinRoot(pathFromUtf8(modelImportSidecarPath(sourcePath.toView())))))
            {
                result.errorMessage = InternedString("Model or import settings are outside the import root");
                return result;
            }
        }

        const InternedString modelName = name.isValid() ? name : AssetManifest::deriveNameFromPath(sourcePath);

        const Expected<ModelImportSettings> settings = m_config.importSettings
            ? Expected<ModelImportSettings>(*m_config.importSettings)
            : loadModelImportSettings(sourcePath.toView());
        if (!settings)
        {
            result.errorMessage = InternedString(
                std::format("Failed to import model: {} - {}", sourcePath.toView(), settings.error().message).c_str());
            SERROR_LOG("[AssetBaker] {}", result.errorMessage.toView());
            return result;
        }
        auto importResult = importModelAssimp(
            sourcePath, m_config.sourceRoot ? std::string_view(m_config.sourceRoot) : std::string_view(), *settings,
            &result.diagnostics);
        if (!importResult.has_value())
        {
            result.errorMessage = InternedString(std::format("Failed to import model: {} - {}", sourcePath.toView(),
                                                             importResult.error().message.c_str())
                                                     .c_str());
            SERROR_LOG("[AssetBaker] {}", result.errorMessage.toView());
            return result;
        }

        auto modelData = importResult.value();

        // Bake materials
        std::vector<InternedString> materialNames;
        materialNames.reserve(modelData->materials.size());

        for (const auto& material : modelData->materials)
        {
            auto matResult = bakeMaterial(material, settings->textures);
            if (!matResult.success)
            {
                if (m_config.strictModelDependencies)
                {
                    result.errorMessage = matResult.errorMessage;
                    return result;
                }
                SWARNING_LOG("[AssetBaker] Failed to bake material {} for model {}", material.name.toView(),
                             sourcePath.toView());
                materialNames.push_back(InternedString(material.name));
            }
            else
            {
                materialNames.push_back(matResult.name);
            }
        }

        const u32 meshCount = static_cast<u32>(modelData->mesh.submeshes.size());

        if (meshCount == 0)
        {
            result.success = true;
            result.name = modelName;
            SWARNING_LOG("[AssetBaker] Model has no meshes: {}", sourcePath.toView());
            return result;
        }

        // the skeleton, clips and skin of a rigged model: all encoded before anything is written, so a clip that
        // cannot bake leaves the old bake and the manifest as they were; the skinned submeshes' bounds and meshlet
        // spheres are set here, ahead of the model file's serialization
        std::optional<AnimationBake> animation;
        if (!modelData->skeleton.joints.empty())
        {
            Expected<AnimationBake> encoded =
                encodeAnimation(*modelData, modelName, m_config.outputDirectory, *settings);
            if (!encoded)
            {
                result.errorMessage = InternedString(
                    std::format("Failed to bake the rig of {}: {}", sourcePath.toView(), encoded.error().message)
                        .c_str());
                SERROR_LOG("[AssetBaker] {}", result.errorMessage.toView());
                return result;
            }
            animation = std::move(*encoded);
        }

        // ---- Mesh serialization (unchanged from original) ----
        std::vector<Vertex> allVertices;
        std::vector<u32> allMeshletVertices;
        std::vector<u8> allMeshletTriangles;
        std::vector<ProcessedMeshlet> allMeshlets;
        std::vector<SpiteAssetTOCEntry> toc(meshCount);

        sizet totalVertices = 0, totalMeshletVertices = 0, totalMeshletTriangles = 0, totalMeshlets = 0;
        for (const auto& submesh : modelData->mesh.submeshes)
        {
            totalVertices += submesh.vertices.size();
            totalMeshletVertices += submesh.meshletVertexIndices.size();
            totalMeshletTriangles += submesh.meshletTriangleIndices.size();
            totalMeshlets += submesh.meshlets.size();
        }

        {
            const prof::MemoryTagScope tagScope("ram.bake");
            allVertices.reserve(totalVertices);
            allMeshletVertices.reserve(totalMeshletVertices);
            allMeshletTriangles.reserve(totalMeshletTriangles);
            allMeshlets.reserve(totalMeshlets);
        }

        sizet currentVertexCount = 0, currentMeshletVertexCount = 0;
        sizet currentMeshletTriangleCount = 0, currentMeshletCount = 0;

        for (u32 i = 0; i < meshCount; ++i)
        {
            const auto& submesh = modelData->mesh.submeshes[i];
            auto& entry = toc[i];

            entry.subAssetId = i;
            entry.localBounds = submesh.localBounds;
            entry.materialIndex = modelData->submeshMaterialIndices[i];
            entry.meshletCount = static_cast<u32>(submesh.meshlets.size());
            entry.uvOffset = submesh.uvRange.packedOffset;
            entry.uvExtent = submesh.uvRange.packedExtent;

            // bake report: geometry fingerprint per submesh, and the material
            // resolution the runtime will see — a mismatch here is invisible
            // later because attachBinding silently falls back to the default
            // material
            if (entry.materialIndex >= materialNames.size())
            {
                SWARNING_LOG("[AssetBaker] Submesh '{}' [{}] references material index {} but only {} materials "
                             "resolved; it will bind the default material",
                             submesh.name.isValid() ? submesh.name.toView() : "", i, entry.materialIndex,
                             materialNames.size());
            }
            u32 allTriangles = 0, level0Triangles = 0;
            for (const ProcessedMeshlet& meshlet : submesh.meshlets)
            {
                allTriangles += meshlet.triangleCount;
                level0Triangles += meshlet.lodLevel == 0 ? meshlet.triangleCount : 0;
            }
            SINFO_LOG(
                "[AssetBaker]   submesh[{}] '{}' mat {}->'{}': {} meshlets ({} above level 0; {} triangles, {} at "
                "level 0), {} verts, bounds ({:.4g}, {:.4g}, "
                "{:.4g})..({:.4g}, {:.4g}, {:.4g})",
                i, submesh.name.isValid() ? submesh.name.toView() : "", entry.materialIndex,
                entry.materialIndex < materialNames.size() ? materialNames[entry.materialIndex].toView() : "<none>",
                entry.meshletCount, lodMeshletCount(submesh.meshlets), allTriangles, level0Triangles,
                submesh.vertices.size(), submesh.localBounds.min.x, submesh.localBounds.min.y,
                submesh.localBounds.min.z, submesh.localBounds.max.x, submesh.localBounds.max.y,
                submesh.localBounds.max.z);

            allVertices.insert(allVertices.end(), submesh.vertices.begin(), submesh.vertices.end());
            allMeshletVertices.insert(allMeshletVertices.end(), submesh.meshletVertexIndices.begin(),
                                      submesh.meshletVertexIndices.end());
            allMeshletTriangles.insert(allMeshletTriangles.end(), submesh.meshletTriangleIndices.begin(),
                                       submesh.meshletTriangleIndices.end());
            allMeshlets.insert(allMeshlets.end(), submesh.meshlets.begin(), submesh.meshlets.end());

            entry.vertexDataSize = submesh.vertices.size() * sizeof(Vertex);
            entry.meshletVertexDataSize = submesh.meshletVertexIndices.size() * sizeof(u32);
            entry.meshletTriangleDataSize = submesh.meshletTriangleIndices.size() * sizeof(u8);

            entry.vertexDataOffset = currentVertexCount * sizeof(Vertex);
            entry.meshletVertexDataOffset = currentMeshletVertexCount * sizeof(u32);
            entry.meshletTriangleDataOffset = currentMeshletTriangleCount * sizeof(u8);
            entry.meshletsDataOffset = currentMeshletCount * sizeof(ProcessedMeshlet);

            currentVertexCount += submesh.vertices.size();
            currentMeshletVertexCount += submesh.meshletVertexIndices.size();
            currentMeshletTriangleCount += submesh.meshletTriangleIndices.size();
            currentMeshletCount += submesh.meshlets.size();
        }

        // Serialize nodes
        const u32 nodeCount = static_cast<u32>(modelData->mesh.nodes.size());
        std::vector<SerializedNode> serializedNodes(nodeCount);
        std::vector<u32> allNodeMeshIndices;
        std::vector<u32> allNodeChildrenIndices;

        sizet totalNodeMeshIndices = 0, totalNodeChildrenIndices = 0;
        for (const auto& node : modelData->mesh.nodes)
        {
            totalNodeMeshIndices += node.meshIndices.size();
            totalNodeChildrenIndices += node.childrenIndices.size();
        }
        allNodeMeshIndices.reserve(totalNodeMeshIndices);
        allNodeChildrenIndices.reserve(totalNodeChildrenIndices);

        for (u32 i = 0; i < nodeCount; ++i)
        {
            const auto& src = modelData->mesh.nodes[i];
            auto& dst = serializedNodes[i];

            dst.localTransform = src.localTransform;
            dst.meshIndexCount = static_cast<u32>(src.meshIndices.size());
            dst.meshIndicesOffset = static_cast<u32>(allNodeMeshIndices.size() * sizeof(u32));
            dst.childrenCount = static_cast<u32>(src.childrenIndices.size());
            dst.childrenIndicesOffset = static_cast<u32>(allNodeChildrenIndices.size() * sizeof(u32));

            allNodeMeshIndices.insert(allNodeMeshIndices.end(), src.meshIndices.begin(), src.meshIndices.end());
            allNodeChildrenIndices.insert(allNodeChildrenIndices.end(), src.childrenIndices.begin(),
                                          src.childrenIndices.end());

            // bake report: the decomposed TRS the runtime hierarchy will
            // rebuild — non-uniform/negative scales and large rotations here
            // are the first suspects when a model renders displaced or scaled
            SINFO_LOG("[AssetBaker]   node[{}] '{}' T({:.4g}, {:.4g}, {:.4g}) R({:.4g}, {:.4g}, {:.4g}, {:.4g}) "
                      "S({:.4g}, {:.4g}, {:.4g}) meshes={} children={}",
                      i, src.name.isValid() ? src.name.toView() : "", src.localTransform.position.x,
                      src.localTransform.position.y, src.localTransform.position.z, src.localTransform.rotation.x,
                      src.localTransform.rotation.y, src.localTransform.rotation.z, src.localTransform.rotation.w,
                      src.localTransform.scale.x, src.localTransform.scale.y, src.localTransform.scale.z,
                      src.meshIndices.size(), src.childrenIndices.size());
        }

        // Calculate layout
        const sizet headerSize = sizeof(SpiteAssetHeader);
        const sizet tocSize = sizeof(SpiteAssetTOCEntry) * toc.size();
        const sizet serializedNodesSize = serializedNodes.size() * sizeof(SerializedNode);
        const sizet nodeMeshIndicesSize = allNodeMeshIndices.size() * sizeof(u32);
        const sizet nodeChildrenIndicesSize = allNodeChildrenIndices.size() * sizeof(u32);
        const sizet meshletsBlobSize = allMeshlets.size() * sizeof(ProcessedMeshlet);
        const sizet verticesBlobSize = allVertices.size() * sizeof(Vertex);
        const sizet meshletVerticesBlobSize = allMeshletVertices.size() * sizeof(u32);
        const sizet meshletTrianglesBlobSize = allMeshletTriangles.size() * sizeof(u8);

        const u64 tocStart = headerSize;
        const u64 nodesStart = tocStart + tocSize;
        const u64 nodeMeshIndicesStart = nodesStart + serializedNodesSize;
        const u64 nodeChildrenIndicesStart = nodeMeshIndicesStart + nodeMeshIndicesSize;
        const u64 meshletsStart = nodeChildrenIndicesStart + nodeChildrenIndicesSize;
        const u64 verticesStart = meshletsStart + meshletsBlobSize;
        const u64 meshletVerticesStart = verticesStart + verticesBlobSize;
        const u64 meshletTrianglesStart = meshletVerticesStart + meshletVerticesBlobSize;

        for (auto& node : serializedNodes)
        {
            node.meshIndicesOffset += static_cast<u32>(nodeMeshIndicesStart);
            node.childrenIndicesOffset += static_cast<u32>(nodeChildrenIndicesStart);
        }

        for (auto& entry : toc)
        {
            entry.meshletsDataOffset += meshletsStart;
            entry.vertexDataOffset += verticesStart;
            entry.meshletVertexDataOffset += meshletVerticesStart;
            entry.meshletTriangleDataOffset += meshletTrianglesStart;
        }

        // Build string table
        std::vector<char> stringTable;
        for (u32 i = 0; i < meshCount; ++i)
        {
            const auto& submesh = modelData->mesh.submeshes[i];
            auto& entry = toc[i];

            entry.nameOffset = stringTable.size();
            const char* nameStr = submesh.name.isValid() ? submesh.name.toView().data() : "";
            entry.nameLength = static_cast<u32>(strlen(nameStr));
            stringTable.insert(stringTable.end(), nameStr, nameStr + entry.nameLength + 1);
        }
        for (u32 i = 0; i < nodeCount; ++i)
        {
            const auto& node = modelData->mesh.nodes[i];
            auto& dst = serializedNodes[i];

            dst.nameOffset = stringTable.size();
            const char* nameStr = node.name.isValid() ? node.name.toView().data() : "";
            dst.nameLength = static_cast<u32>(strlen(nameStr));
            stringTable.insert(stringTable.end(), nameStr, nameStr + dst.nameLength + 1);
            if (node.sourceObject.isValid())
            {
                dst.reserved = static_cast<u32>(stringTable.size() + 1);
                const auto id = node.sourceObject.toView();
                stringTable.insert(stringTable.end(), id.begin(), id.end());
                stringTable.push_back('\0');
            }
        }

        const sizet stringTableSize = stringTable.size();
        const u64 stringTableStart = meshletTrianglesStart + meshletTrianglesBlobSize;

        // Build header
        SpiteAssetHeader header;
        header.subAssetCount = meshCount;
        header.nodeCount = nodeCount;
        header.rootNodeIndex = modelData->mesh.rootNodeIndex;
        header.nodeMeshIndicesOffset = nodeMeshIndicesStart;
        header.nodeMeshIndicesSize = nodeMeshIndicesSize;
        header.nodeChildrenIndicesOffset = nodeChildrenIndicesStart;
        header.nodeChildrenIndicesSize = nodeChildrenIndicesSize;
        header.stringTableOffset = stringTableStart;
        header.stringTableSize = stringTableSize;
        const bool hasImportedProperties = std::ranges::any_of(modelData->mesh.nodes, [](const ProcessedNode& node)
                                                               { return node.imported.camera || node.imported.light; });
        header.reserved = hasImportedProperties ? 1 : 0;

        // Build final buffer
        sizet totalFileSize = stringTableStart + stringTableSize +
            (hasImportedProperties ? nodeCount * sizeof(ImportedNodeProperties) : 0);
        std::vector<std::byte> fileBuffer;
        {
            const prof::MemoryTagScope tagScope("ram.bake");
            fileBuffer.resize(totalFileSize);
        }
        char* cursor = reinterpret_cast<char*>(fileBuffer.data());

        memcpy(cursor, &header, headerSize);
        cursor += headerSize;
        memcpy(cursor, toc.data(), tocSize);
        cursor += tocSize;
        memcpy(cursor, serializedNodes.data(), serializedNodesSize);
        cursor += serializedNodesSize;
        memcpy(cursor, allNodeMeshIndices.data(), nodeMeshIndicesSize);
        cursor += nodeMeshIndicesSize;
        memcpy(cursor, allNodeChildrenIndices.data(), nodeChildrenIndicesSize);
        cursor += nodeChildrenIndicesSize;
        memcpy(cursor, allMeshlets.data(), meshletsBlobSize);
        cursor += meshletsBlobSize;
        memcpy(cursor, allVertices.data(), verticesBlobSize);
        cursor += verticesBlobSize;
        memcpy(cursor, allMeshletVertices.data(), meshletVerticesBlobSize);
        cursor += meshletVerticesBlobSize;
        memcpy(cursor, allMeshletTriangles.data(), meshletTrianglesBlobSize);
        cursor += meshletTrianglesBlobSize;
        memcpy(cursor, stringTable.data(), stringTableSize);
        cursor += stringTableSize;
        if (hasImportedProperties)
            for (const auto& node : modelData->mesh.nodes)
            {
                memcpy(cursor, &node.imported, sizeof(node.imported));
                cursor += sizeof(node.imported);
            }

        // Write the files: each to <path>.part first and renamed into place once every one was written (the model
        // last), so a file that cannot be written (a path in the way, no space) leaves no new file behind and the
        // manifest as it was. Only the renames follow the last write.
        std::string bakedPath =
            std::string(m_config.outputDirectory) + "/" + std::string(modelName.toView()) + ".spiteasset";
        std::vector<std::pair<std::string, const std::vector<std::byte>*>> outputs;
        if (animation)
        {
            for (const auto& [path, bytes] : animation->files)
            {
                outputs.emplace_back(path, &bytes);
            }
        }
        outputs.emplace_back(bakedPath, &fileBuffer);
        std::vector<std::string> parts;
        const auto discardParts = [&parts]
        {
            for (const std::string& part : parts)
            {
                std::error_code ignored;
                std::filesystem::remove(resolveContentPath(part), ignored);
            }
        };
        for (const auto& [path, bytes] : outputs)
        {
            parts.push_back(path + ".part");
            if (const Expected<void> written = writeOutputFile(parts.back().c_str(), *bytes); !written)
            {
                discardParts();
                result.errorMessage = InternedString(written.error().message.c_str());
                return result;
            }
        }
        for (sizet i = 0; i < outputs.size(); ++i)
        {
            std::error_code error;
            std::filesystem::rename(resolveContentPath(parts[i]), resolveContentPath(outputs[i].first), error);
            if (error)
            {
                discardParts();
                result.errorMessage = InternedString(
                    std::format("Failed to move {} into place: {}", outputs[i].first, error.message()).c_str());
                return result;
            }
        }
        // the entries of an earlier bake of this model that this one no longer has (fewer clips, no skin); the files
        // are this baker's own (a staging baker's manifest holds the live entries, whose files are not its to delete)
        if (const std::optional<AssetManifest::ModelEntry> earlier = m_manifest.getModelEntry(modelName))
        {
            const std::string prefix = std::string(m_config.outputDirectory) + "/";
            for (const InternedString file : m_manifest.removeModelAnimation(*earlier))
            {
                const bool kept = animation &&
                    std::ranges::any_of(animation->files,
                                        [&file](const auto& written) { return written.first == file.toView(); });
                std::error_code ignored;
                if (!kept && file.toView().starts_with(prefix))
                {
                    std::filesystem::remove(resolveContentPath(file.toView()), ignored);
                }
            }
        }

        AssetManifest::ModelEntry modelEntry;
        modelEntry.baked = InternedString(bakedPath.c_str());
        modelEntry.materials = materialNames;
        modelEntry.source = sourcePath;
        if (const auto earlier = m_manifest.getModelEntry(modelName); earlier && earlier->source == sourcePath)
        {
            modelEntry.authoringSource = earlier->authoringSource;
            modelEntry.sourceRoot = earlier->sourceRoot;
        }
        modelEntry.importSettings = *settings;
        // geometry counts for the engine's budget math (gpu-streaming spec)
        modelEntry.gpuCosts = {.meshletCount = static_cast<u64>(totalMeshlets),
                               .vertexCount = static_cast<u64>(totalVertices),
                               .meshletVertexIndexCount = static_cast<u64>(totalMeshletVertices),
                               .meshletTriangleIndexCount = static_cast<u64>(totalMeshletTriangles)};
        if (animation)
        {
            m_manifest.overwriteSkeleton(animation->skeleton.first, animation->skeleton.second);
            modelEntry.skeleton = animation->skeleton.first;
            modelEntry.skin = animation->skin;
            for (const auto& [name, entry] : animation->clips)
            {
                m_manifest.overwriteAnimation(name, entry);
                modelEntry.clips.push_back(name);
            }
        }
        m_manifest.overwriteModel(modelName, modelEntry);

        result.success = true;
        result.name = modelName;
        result.outputPath = InternedString(bakedPath.c_str());

        SINFO_LOG("[AssetBaker] Baked model: {} -> {}, name {} ({} meshes, {} nodes, {} materials)",
                  sourcePath.toView(), bakedPath, modelName.toView(), meshCount, nodeCount, materialNames.size());
        if (animation)
        {
            SINFO_LOG("[AssetBaker]   rig: {} joints, {} clips, {} skinned submeshes",
                      modelData->skeleton.joints.size(), animation->clips.size(),
                      std::ranges::count_if(modelData->mesh.submeshes,
                                            [](const auto& submesh) { return submesh.skin.has_value(); }));
        }

        saveManifest();
        return result;
    }

    BakeDirectoryResult AssetBaker::bakeDirectory(const char* directory, const char* extension,
                                                  const std::chrono::milliseconds timeout)
    {
        SINFO_LOG("[AssetBaker] Baking all {} files in {}", extension, directory);

        BakeDirectoryResult result;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        u32 firstStarted;
        u32 firstFailed;
        u32 rejected = 0;
        {
            std::lock_guard lock(m_asyncBakes->mutex);
            firstStarted = m_asyncBakes->started;
            firstFailed = m_asyncBakes->failed;
        }

        std::error_code error;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 directory, std::filesystem::directory_options::skip_permission_denied, error);
             !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error))
        {
            const auto& entry = *it;
            std::error_code typeError;
            if (entry.is_regular_file(typeError) && entry.path().extension() == extension)
            {
                InternedString path = InternedString(utf8Path(entry.path()).c_str());
                switch (requestBakeAsync(path))
                {
                case BakeRequest::Requested:
                    break;
                case BakeRequest::AlreadyInFlight:
                    ++result.skipped;
                    break;
                case BakeRequest::Rejected:
                    ++rejected;
                    break;
                }
            }
        }
        if (error)
        {
            SERROR_LOG("[AssetBaker] Could not walk {}: {}", directory, error.message());
        }

        // the wait covers every async bake of this baker, a concurrent import's included
        const bool idle = m_asyncBakes->waitIdle(deadline);
        {
            std::lock_guard lock(m_asyncBakes->mutex);
            const u32 requested = m_asyncBakes->started - firstStarted;
            result.failed = m_asyncBakes->failed - firstFailed;
            result.timedOut = idle ? 0 : std::min(requested, m_asyncBakes->inFlight);
            result.baked = requested - result.failed - result.timedOut;
            result.failed += rejected;
        }
        saveManifest();
        return result;
    }

    BakeRequest AssetBaker::requestBakeAsync(const InternedString& sourcePath)
    {
        // in flight before the submit: a worker may settle the bake before requestSave returns
        {
            std::lock_guard lock(m_asyncBakes->mutex);
            ++m_asyncBakes->inFlight;
        }
        const SaveRequest request = m_ioManager.requestSave(
            sourcePath,
            [this, sourcePath, bakes = m_asyncBakes]() -> Expected<void>
            {
                {
                    std::lock_guard lock(bakes->mutex);
                    if (bakes->cancelled)
                    {
                        return std::unexpected(makeError(ErrorCode::Cancelled, "the baker was destroyed"));
                    }
                    ++bakes->running;
                }
                const BakeResult result = bakeModel(sourcePath);
                {
                    // the destructor may free the baker from here on
                    std::lock_guard lock(bakes->mutex);
                    --bakes->running;
                    bakes->settledCv.notify_all();
                }
                if (!result.success)
                {
                    return std::unexpected(
                        makeError(ErrorCode::AssetSaveFailed, std::string(result.errorMessage.toView())));
                }
                return {};
            },
            [bakes = m_asyncBakes](const IOCompletionEvent& event)
            {
                std::lock_guard lock(bakes->mutex);
                --bakes->inFlight;
                bakes->failed += event.isSuccess() ? 0 : 1;
                bakes->settledCv.notify_all();
            });
        {
            std::lock_guard lock(m_asyncBakes->mutex);
            if (request.submitted)
            {
                ++m_asyncBakes->started;
                return BakeRequest::Requested;
            }
            // requestSave dropped the task and its handler: nothing else will settle this count
            --m_asyncBakes->inFlight;
            m_asyncBakes->settledCv.notify_all();
        }
        if (request.taskId.isValid())
        {
            return BakeRequest::AlreadyInFlight;
        }
        SERROR_LOG("[AssetBaker] Could not request the bake of {}: the IO workers are stopped", sourcePath.toView());
        return BakeRequest::Rejected;
    }
} // namespace spite
