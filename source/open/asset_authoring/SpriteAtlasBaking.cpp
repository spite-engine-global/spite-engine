#include "asset/TextureStorage.hpp"
#include "asset/SpriteAtlas.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <bit>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <map>
#include <type_traits>
#include <nlohmann/json.hpp>

// stb_rect_pack sorts with qsort, whose order among equal rectangles is unspecified; a stable sort keeps the packed
// layout, and so the baked bytes, identical across runtimes
static void spriteStableSort(void* base, const size_t count, const size_t size,
                             int (*compare)(const void*, const void*))
{
    auto* bytes = static_cast<unsigned char*>(base);
    unsigned char temp[64];
    for (size_t i = 1; i < count; ++i)
    {
        std::memcpy(temp, bytes + i * size, size);
        size_t j = i;
        for (; j > 0 && compare(bytes + (j - 1) * size, temp) > 0; --j)
        {
            std::memcpy(bytes + j * size, bytes + (j - 1) * size, size);
        }
        std::memcpy(bytes + j * size, temp, size);
    }
}
#define STBRP_STATIC
#define STBRP_SORT spriteStableSort
#define STB_RECT_PACK_IMPLEMENTATION
#include <stb_rect_pack.h>

static_assert(sizeof(stbrp_rect) <= 64, "spriteStableSort moves rectangles through a 64 byte buffer");

#include "asset/AssetBaker.hpp"
#include "asset/AssetImportingFunctions.hpp"
#include "base/ContentRoot.hpp"
#include "base/Logging.hpp"
#include "base/io/FileSystem.hpp"

namespace spite
{
    using json = nlohmann::json;
    namespace fs = std::filesystem;

    namespace
    {
        constexpr u64 MAX_SOURCE_FILE_BYTES = 64ull << 20;
        constexpr u32 MAX_PADDING = 8;

        Error fail(const ErrorCode code, std::string message) { return makeError(code, std::move(message)); }

        Expected<std::string> readAll(const fs::path& path)
        {
            std::error_code error;
            const u64 size = fs::file_size(path, error);
            if (error)
            {
                return std::unexpected(fail(ErrorCode::FileReadFailed,
                                            std::format("cannot read {}: {}", path.generic_string(), error.message())));
            }
            if (size > MAX_SOURCE_FILE_BYTES)
            {
                return std::unexpected(fail(ErrorCode::FileReadFailed,
                                            std::format("{} is {} bytes; the limit is {}", path.generic_string(), size,
                                                        MAX_SOURCE_FILE_BYTES)));
            }
            std::ifstream file(path, std::ios::binary);
            std::string bytes(size, '\0');
            if (!file.read(bytes.data(), static_cast<std::streamsize>(size)))
            {
                return std::unexpected(
                    fail(ErrorCode::FileReadFailed, std::format("cannot read {}", path.generic_string())));
            }
            return bytes;
        }

        struct Fnv64;

        struct Fnv64
        {
            u64 state = 14695981039346656037ull;
            void add(const void* data, const sizet size)
            {
                const auto* bytes = static_cast<const unsigned char*>(data);
                for (sizet i = 0; i < size; ++i)
                {
                    state = (state ^ bytes[i]) * 1099511628211ull;
                }
            }
            void add(const std::string_view text)
            {
                const u64 size = text.size();
                add(&size, sizeof(size));
                add(text.data(), text.size());
            }
            std::string hex() const { return std::format("{:016x}", state); }
        };

        // file bytes into the hash in chunks: no PNG is held in memory before the size checks pass
        Expected<void> hashFile(const fs::path& path, Fnv64& hash)
        {
            std::error_code error;
            const u64 size = fs::file_size(path, error);
            if (error)
            {
                return std::unexpected(fail(ErrorCode::FileReadFailed,
                                            std::format("cannot read {}: {}", path.generic_string(), error.message())));
            }
            if (size > MAX_SOURCE_FILE_BYTES)
            {
                return std::unexpected(fail(ErrorCode::FileReadFailed,
                                            std::format("{} is {} bytes; the limit is {}", path.generic_string(), size,
                                                        MAX_SOURCE_FILE_BYTES)));
            }
            std::ifstream file(path, std::ios::binary);
            char chunk[1 << 16];
            hash.add(&size, sizeof(size));
            u64 left = size;
            while (left > 0)
            {
                file.read(chunk, static_cast<std::streamsize>(std::min<u64>(left, sizeof(chunk))));
                const std::streamsize got = file.gcount();
                if (got <= 0)
                {
                    return std::unexpected(
                        fail(ErrorCode::FileReadFailed, std::format("cannot read {}", path.generic_string())));
                }
                hash.add(chunk, static_cast<sizet>(got));
                left -= static_cast<u64>(got);
            }
            return {};
        }

        bool insideRoot(const fs::path& root, const fs::path& path)
        {
            std::error_code error;
            const fs::path normalized = fs::weakly_canonical(fs::absolute(path, error), error);
            if (error)
            {
                return false;
            }
            const fs::path relative = normalized.lexically_relative(root);
            return !relative.empty() && *relative.begin() != ".." && relative != ".";
        }

        bool validSegments(const std::string_view name)
        {
            if (name.empty())
            {
                return false;
            }
            sizet segmentLength = 0;
            for (const char c : name)
            {
                if (c == '/')
                {
                    if (segmentLength == 0)
                    {
                        return false;
                    }
                    segmentLength = 0;
                }
                else if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
                {
                    ++segmentLength;
                }
                else
                {
                    return false;
                }
            }
            return segmentLength > 0;
        }

        bool reservedDeviceName(std::string name)
        {
            std::ranges::transform(name, name.begin(), [](const unsigned char c) { return std::tolower(c); });
            const bool numbered = name.size() == 4 && (name.starts_with("com") || name.starts_with("lpt")) &&
                name[3] >= '1' && name[3] <= '9';
            return numbered || name == "con" || name == "prn" || name == "aux" || name == "nul";
        }

        std::string lower(std::string text)
        {
            std::ranges::transform(text, text.begin(), [](const unsigned char c) { return std::tolower(c); });
            return text;
        }

        struct SpriteSource
        {
            std::string name;
            fs::path png;
            fs::path sidecar; // empty: none
            std::string sidecarBytes;
            int width = 0;
            int height = 0;
            SpriteInsets slice;
        };

        // {"slice": [left, top, right, bottom]} for one sprite
        Expected<SpriteInsets> parseSprite(const json& value, const int width, const int height,
                                           const std::string& file)
        {
            if (!value.is_object())
            {
                return std::unexpected(fail(ErrorCode::JsonParseFailed, file + ": a sprite entry must be an object"));
            }
            SpriteInsets insets;
            for (const auto& [key, member] : value.items())
            {
                if (key != "slice")
                {
                    return std::unexpected(
                        fail(ErrorCode::JsonParseFailed, file + ": unknown key '" + key + "' (only 'slice')"));
                }
                if (!member.is_array() || member.size() != 4 ||
                    !std::ranges::all_of(
                        member, [](const json& v)
                        { return v.is_number_integer() && v.get<long long>() >= 0 && v.get<long long>() <= 65535; }))
                {
                    return std::unexpected(
                        fail(ErrorCode::JsonParseFailed,
                             file + ": 'slice' must be [left, top, right, bottom], integers 0..65535"));
                }
                insets = {member[0].get<u16>(), member[1].get<u16>(), member[2].get<u16>(), member[3].get<u16>()};
            }
            if (insets.left + insets.right > width || insets.top + insets.bottom > height)
            {
                return std::unexpected(
                    fail(ErrorCode::JsonParseFailed,
                         std::format("{}: slice [{}, {}, {}, {}] does not fit the {}x{} sprite", file, insets.left,
                                     insets.top, insets.right, insets.bottom, width, height)));
            }
            return insets;
        }

        u32 nextPow2(u32 v)
        {
            u32 p = 1;
            while (p < v)
            {
                p <<= 1;
            }
            return p;
        }
    } // namespace

    BakeResult AssetBaker::bakeSpriteAtlas(const InternedString& folderPath, const InternedString name)
    {
        BakeResult result;
        const auto failed = [&result](const Error& error)
        {
            result.errorMessage = InternedString(error.message.c_str());
            SERROR_LOG("[AssetBaker] {}", error.message);
            return result;
        };

        const fs::path requested = resolveContentPath(std::string(folderPath.toView()));
        std::error_code ec;
        if (!fs::is_directory(requested, ec))
        {
            return failed(fail(ErrorCode::FileNotFound,
                               std::format("sprite folder {} is not a directory", requested.generic_string())));
        }
        const fs::path folder = fs::weakly_canonical(fs::absolute(requested, ec), ec);
        if (m_config.sourceRoot)
        {
            const fs::path root = fs::weakly_canonical(fs::absolute(resolveContentPath(m_config.sourceRoot), ec), ec);
            if (!insideRoot(root, folder))
            {
                return failed(fail(ErrorCode::InvalidPath,
                                   std::format("sprite folder {} is outside the source root {}",
                                               folder.generic_string(), root.generic_string())));
            }
        }

        const std::string atlasName = name.isValid() ? std::string(name.toView()) : folder.filename().generic_string();
        if (!validSegments(atlasName) || atlasName.find('/') != std::string::npos || reservedDeviceName(atlasName))
        {
            return failed(
                fail(ErrorCode::InvalidPath,
                     std::format("sprite atlas name '{}' (folder {}) must use letters, digits, '_' and '-' only, "
                                 "and not be a device name",
                                 atlasName, folder.generic_string())));
        }

        // -- collect: names, bytes, sidecars
        std::vector<SpriteSource> sprites;
        std::map<std::string, fs::path> sidecars;
        fs::path configPath;
        ec.clear();
        for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            const fs::directory_entry& entry = *it;
            std::error_code typeError;
            if (!entry.is_regular_file(typeError))
            {
                continue;
            }
            if (!insideRoot(folder, entry.path()))
            {
                return failed(fail(ErrorCode::InvalidPath,
                                   std::format("{} resolves outside the sprite folder {}",
                                               entry.path().generic_string(), folder.generic_string())));
            }
            const std::string relative = entry.path().lexically_relative(folder).generic_string();
            const std::string extension = lower(entry.path().extension().generic_string());
            const std::string stem = relative.substr(0, relative.size() - extension.size());
            if (extension == ".png" || extension == ".json")
            {
                if (relative == "atlas.json")
                {
                    configPath = entry.path();
                    continue;
                }
                if (!validSegments(stem))
                {
                    return failed(
                        fail(ErrorCode::InvalidPath,
                             std::format("{}: sprite name '{}' must use letters, digits, '_', '-' and '/' only",
                                         entry.path().generic_string(), stem)));
                }
                if (extension == ".png")
                {
                    sprites.push_back({.name = stem, .png = entry.path()});
                }
                else
                {
                    sidecars[stem] = entry.path();
                }
            }
            else
            {
                SWARNING_LOG("[AssetBaker] sprite atlas '{}' ignores {}", atlasName, entry.path().generic_string());
            }
        }
        if (ec)
        {
            return failed(fail(ErrorCode::FileReadFailed,
                               std::format("cannot walk {}: {}", folder.generic_string(), ec.message())));
        }
        if (sprites.empty())
        {
            return failed(fail(ErrorCode::AssetNotFound, std::format("{} holds no PNG", folder.generic_string())));
        }
        if (sprites.size() > SPRITE_ATLAS_MAX_SPRITES)
        {
            return failed(fail(ErrorCode::InvalidPath,
                               std::format("{} holds {} PNGs; the limit is {}", folder.generic_string(), sprites.size(),
                                           SPRITE_ATLAS_MAX_SPRITES)));
        }
        std::ranges::sort(sprites, {}, &SpriteSource::name);
        if (const auto clash = std::ranges::find(sprites, "atlas", &SpriteSource::name); clash != sprites.end())
        {
            return failed(fail(ErrorCode::InvalidPath,
                               clash->png.generic_string() +
                                   ": a sprite named 'atlas' shares its sidecar name with the folder's atlas.json; "
                                   "rename it"));
        }
        for (sizet i = 1; i < sprites.size(); ++i)
        {
            if (lower(sprites[i].name) == lower(sprites[i - 1].name))
            {
                return failed(fail(ErrorCode::InvalidPath,
                                   std::format("{} and {} name the same sprite (names ignore case)",
                                               sprites[i - 1].png.generic_string(), sprites[i].png.generic_string())));
            }
        }
        for (SpriteSource& sprite : sprites)
        {
            if (const auto sidecar = sidecars.find(sprite.name); sidecar != sidecars.end())
            {
                sprite.sidecar = sidecar->second;
                sidecars.erase(sidecar);
            }
        }
        if (!sidecars.empty())
        {
            return failed(fail(ErrorCode::AssetNotFound,
                               std::format("{} has no PNG beside it", sidecars.begin()->second.generic_string())));
        }

        // -- config and hash
        std::string configBytes;
        if (!configPath.empty())
        {
            Expected<std::string> read = readAll(configPath);
            if (!read)
            {
                return failed(read.error());
            }
            configBytes = std::move(*read);
        }
        json config = json::object();
        if (!configBytes.empty())
        {
            config = json::parse(configBytes, nullptr, false);
            if (!config.is_object())
            {
                return failed(fail(ErrorCode::JsonParseFailed,
                                   configPath.generic_string() + ": not valid JSON, or not an object"));
            }
        }
        u32 padding = 1;
        u32 maxSize = SPRITE_ATLAS_DEFAULT_MAX_SIZE;
        json spriteConfig = json::object();
        for (const auto& [key, value] : config.items())
        {
            const auto integer = [&](const char* what, const long long lo, const long long hi,
                                     u32& out) -> Expected<void>
            {
                if (!value.is_number_integer() || value.get<long long>() < lo || value.get<long long>() > hi)
                {
                    return std::unexpected(fail(
                        ErrorCode::JsonParseFailed,
                        std::format("{}: '{}' must be an integer {}..{}", configPath.generic_string(), what, lo, hi)));
                }
                out = value.get<u32>();
                return {};
            };
            Expected<void> ok;
            if (key == "padding")
            {
                ok = integer("padding", 0, MAX_PADDING, padding);
            }
            else if (key == "maxSize")
            {
                ok = integer("maxSize", 16, SPRITE_ATLAS_LIMIT_MAX_SIZE, maxSize);
            }
            else if (key == "sprites" && value.is_object())
            {
                spriteConfig = value;
            }
            else
            {
                ok = std::unexpected(fail(
                    ErrorCode::JsonParseFailed,
                    std::format("{}: unknown key '{}' (padding, maxSize, sprites)", configPath.generic_string(), key)));
            }
            if (!ok)
            {
                return failed(ok.error());
            }
        }
        if (!std::has_single_bit(maxSize))
        {
            return failed(
                fail(ErrorCode::JsonParseFailed,
                     std::format("{}: 'maxSize' {} must be a power of two", configPath.generic_string(), maxSize)));
        }
        for (const auto& [spriteName, entry] : spriteConfig.items())
        {
            if (!std::ranges::binary_search(sprites, spriteName, {}, &SpriteSource::name))
            {
                return failed(fail(ErrorCode::AssetNotFound,
                                   std::format("{}: sprite '{}' has no PNG", configPath.generic_string(), spriteName)));
            }
        }

        Fnv64 hash;
        hash.add(&SPRITE_ATLAS_VERSION, sizeof(SPRITE_ATLAS_VERSION));
        hash.add(&padding, sizeof(padding));
        hash.add(&maxSize, sizeof(maxSize));
        hash.add(configBytes);
        for (SpriteSource& sprite : sprites)
        {
            if (!sprite.sidecar.empty())
            {
                Expected<std::string> sidecar = readAll(sprite.sidecar);
                if (!sidecar)
                {
                    return failed(sidecar.error());
                }
                sprite.sidecarBytes = std::move(*sidecar);
            }
            hash.add(sprite.name);
            if (Expected<void> hashed = hashFile(sprite.png, hash); !hashed)
            {
                return failed(hashed.error());
            }
            hash.add(sprite.sidecar.empty() ? "-" : "+");
            hash.add(sprite.sidecarBytes);
        }
        const InternedString sourceHash(hash.hex().c_str());

        const InternedString atlasKey(atlasName.c_str());
        const InternedString textureName((atlasName + "_atlas").c_str());
        const std::string tablePath =
            std::string(m_config.outputDirectory) + "/" + m_config.atlasSubdir + "/" + atlasName + ".spriteatlas";
        const std::string pagePath = std::string(m_config.outputDirectory) + "/" + m_config.textureSubdir + "/" +
            std::string(textureName.toView()) + ".spitetex";

        const std::optional<AssetManifest::SpriteAtlasEntry> existing = m_manifest.getSpriteAtlasEntry(atlasKey);
        std::error_code existsError;
        if (existing && existing->contentHash == sourceHash && !m_config.forceRebake &&
            m_manifest.hasTexture(textureName) && fs::exists(resolveContentPath(pagePath), existsError) &&
            loadBakedSpriteAtlas(atlasKey, m_manifest))
        {
            result.success = true;
            result.upToDate = true;
            result.name = atlasKey;
            result.outputPath = existing->baked;
            SDEBUG_LOG("[AssetBaker] Sprite atlas '{}' unchanged", atlasName);
            return result;
        }
        if (const auto texture = m_manifest.getTextureEntry(textureName); texture && !existing)
        {
            return failed(fail(
                ErrorCode::InvalidPath,
                std::format("sprite atlas '{}' would overwrite the texture '{}'", atlasName, textureName.toView())));
        }

        // -- validate sidecars and sizes before any decode
        u64 totalArea = 0;
        for (SpriteSource& sprite : sprites)
        {
            if (const auto size = probeImageFile(sprite.png, sprite.width, sprite.height); !size)
            {
                return failed(size.error());
            }
            if (static_cast<u64>(sprite.width) + 2 * padding > maxSize ||
                static_cast<u64>(sprite.height) + 2 * padding > maxSize)
            {
                return failed(
                    fail(ErrorCode::InvalidPath,
                         std::format("{}: {}x{} plus {} px gutters exceeds the {} px page", sprite.png.generic_string(),
                                     sprite.width, sprite.height, padding, maxSize)));
            }
            totalArea += static_cast<u64>(sprite.width + 2 * padding) * (sprite.height + 2 * padding);

            const json* value = nullptr;
            json sidecarValue;
            std::string label = configPath.generic_string();
            if (!sprite.sidecar.empty())
            {
                sidecarValue = json::parse(sprite.sidecarBytes, nullptr, false);
                if (sidecarValue.is_discarded())
                {
                    return failed(
                        fail(ErrorCode::JsonParseFailed, sprite.sidecar.generic_string() + ": not valid JSON"));
                }
                value = &sidecarValue;
                label = sprite.sidecar.generic_string();
            }
            else if (spriteConfig.contains(sprite.name))
            {
                value = &spriteConfig[sprite.name];
            }
            if (value)
            {
                Expected<SpriteInsets> slice = parseSprite(*value, sprite.width, sprite.height, label);
                if (!slice)
                {
                    return failed(slice.error());
                }
                sprite.slice = *slice;
            }
        }
        if (totalArea > static_cast<u64>(maxSize) * maxSize)
        {
            return failed(fail(ErrorCode::InvalidPath,
                               std::format("{}: {} sprites need {} px^2 with gutters, over the {}x{} page limit; split "
                                           "the folder or raise maxSize in atlas.json (up to {})",
                                           folder.generic_string(), sprites.size(), totalArea, maxSize, maxSize,
                                           SPRITE_ATLAS_LIMIT_MAX_SIZE)));
        }

        // -- pack: the smallest square power-of-two page that takes every rectangle
        std::vector<stbrp_rect> rects(sprites.size());
        for (sizet i = 0; i < sprites.size(); ++i)
        {
            rects[i] = {};
            rects[i].id = static_cast<int>(i);
            rects[i].w = static_cast<stbrp_coord>(sprites[i].width + 2 * padding);
            rects[i].h = static_cast<stbrp_coord>(sprites[i].height + 2 * padding);
        }
        u32 pageSize = std::max(16u, nextPow2(static_cast<u32>(std::ceil(std::sqrt(static_cast<double>(totalArea))))));
        bool packed = false;
        for (; pageSize <= maxSize && !packed; pageSize *= 2)
        {
            std::vector<stbrp_node> nodes(pageSize);
            stbrp_context context;
            stbrp_init_target(&context, static_cast<int>(pageSize), static_cast<int>(pageSize), nodes.data(),
                              static_cast<int>(nodes.size()));
            packed = stbrp_pack_rects(&context, rects.data(), static_cast<int>(rects.size())) != 0;
            if (packed)
            {
                break;
            }
        }
        if (!packed)
        {
            return failed(fail(ErrorCode::InvalidPath,
                               std::format("{}: the sprites do not pack into a {}x{} page; split the folder or raise "
                                           "maxSize in atlas.json (up to {})",
                                           folder.generic_string(), maxSize, maxSize, SPRITE_ATLAS_LIMIT_MAX_SIZE)));
        }

        // -- compose: one page, edge pixels extruded into the gutter so filtering never reads a neighbour
        std::vector<u8> page(static_cast<sizet>(pageSize) * pageSize * 4, 0);
        SpriteAtlas atlas;
        atlas.name = atlasKey;
        atlas.texture = textureName;
        atlas.sourceHash = std::string(sourceHash.toView());
        atlas.pageWidth = atlas.pageHeight = pageSize;
        atlas.sprites.resize(sprites.size());
        for (sizet i = 0; i < sprites.size(); ++i)
        {
            const SpriteSource& sprite = sprites[i];
            const int width = sprite.width, height = sprite.height;
            Expected<std::string> png = readAll(sprite.png);
            if (!png)
            {
                return failed(png.error());
            }
            Expected<u8*> decoded = decodeImageTopFirst(std::as_bytes(std::span(*png)), sprite.width, sprite.height,
                                                        sprite.png.generic_string());
            if (!decoded)
                return failed(decoded.error());
            u8* pixels = *decoded;
            const stbrp_rect& rect = rects[i];
            for (int y = 0; y < rect.h; ++y)
            {
                for (int x = 0; x < rect.w; ++x)
                {
                    const int sx = std::clamp(x - static_cast<int>(padding), 0, width - 1);
                    const int sy = std::clamp(y - static_cast<int>(padding), 0, height - 1);
                    std::memcpy(&page[(static_cast<sizet>(rect.y + y) * pageSize + rect.x + x) * 4],
                                pixels + (static_cast<sizet>(sy) * width + sx) * 4, 4);
                }
            }
            freeTexture(pixels);
            atlas.sprites[i] = {.name = sprite.name,
                                .x = static_cast<u16>(rect.x + padding),
                                .y = static_cast<u16>(rect.y + padding),
                                .width = static_cast<u16>(width),
                                .height = static_cast<u16>(height),
                                .slice = sprite.slice};
        }

        // -- write: page, table, manifest
        json table;
        table["version"] = SPRITE_ATLAS_VERSION;
        table["texture"] = textureName.toView();
        table["sourceHash"] = sourceHash.toView();
        table["width"] = pageSize;
        table["height"] = pageSize;
        table["sprites"] = json::array();
        for (const SpriteInfo& sprite : atlas.sprites)
        {
            json row = {
                {"name", sprite.name}, {"x", sprite.x}, {"y", sprite.y}, {"w", sprite.width}, {"h", sprite.height}};
            if (sprite.slice.any())
            {
                row["slice"] = {sprite.slice.left, sprite.slice.top, sprite.slice.right, sprite.slice.bottom};
            }
            table["sprites"].push_back(std::move(row));
        }

        SpiteTextureHeader header;
        header.format = BAKED_TEXTURE_FORMAT_R8G8B8A8_SRGB;
        header.width = pageSize;
        header.height = pageSize;
        header.mipLevels = 1;
        auto pageFile = encodeBakedTexture(header, page);
        if (!pageFile)
            return failed(pageFile.error());

        for (const std::string& path : {pagePath, tablePath})
        {
            if (Expected<void> made = ensureDirectory(resolveContentPath(path).parent_path()); !made)
            {
                return failed(made.error());
            }
        }
        if (Expected<void> written = writeBinaryFileAtomically(resolveContentPath(pagePath), *pageFile); !written)
        {
            return failed(written.error());
        }
        if (Expected<void> written = writeTextFileAtomically(resolveContentPath(tablePath), table.dump(1)); !written)
        {
            return failed(written.error());
        }
        m_manifest.overwriteTexture(textureName,
                                    {.baked = InternedString(pagePath.c_str()),
                                     .contentHash = InternedString(("atlas:" + atlas.sourceHash).c_str()),
                                     .pixelBytes = page.size(),
                                     .format = BAKED_TEXTURE_FORMAT_R8G8B8A8_SRGB,
                                     .fileBytes = pageFile->size()});
        m_manifest.overwriteSpriteAtlas(atlasKey,
                                        {.baked = InternedString(tablePath.c_str()),
                                         .texture = textureName,
                                         .contentHash = sourceHash,
                                         .spriteCount = static_cast<u32>(atlas.sprites.size()),
                                         .pageWidth = pageSize,
                                         .pageHeight = pageSize});
        saveManifest();

        result.success = true;
        result.name = atlasKey;
        result.outputPath = InternedString(tablePath.c_str());
        SINFO_LOG("[AssetBaker] Baked sprite atlas '{}' ({} sprites, {}x{} page) -> {}", atlasName,
                  atlas.sprites.size(), pageSize, pageSize, tablePath);
        return result;
    }

} // namespace spite
