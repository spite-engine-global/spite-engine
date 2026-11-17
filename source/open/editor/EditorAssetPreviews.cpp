#include "EditorAssetPreviews.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <exception>
#include <string_view>
#include <iomanip>
#include <sstream>
#include <utility>

#include "asset/AssetBaker.hpp"
#include "asset/TextureCompression.hpp"
#include "asset/TextureStorage.hpp"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>

namespace spite
{
    namespace
    {
        using Result = EditorAssetPreviews::Result;
        using Status = EditorAssetPreviews::Status;

        Result generate(const std::filesystem::path& cache, const std::filesystem::path& source,
                        const std::vector<std::filesystem::path>& dependencies, unsigned maxSize)
        {
            unsigned long long hash = 14695981039346656037ull;
            auto feed = [&](unsigned char c) { hash = (hash ^ c) * 1099511628211ull; };
            for (unsigned char c : std::string_view{"spite-texture-preview-v1"})
                feed(c);
            for (unsigned char c : std::to_string(maxSize))
                feed(c);
            // Validate the header and file size before retaining an untrusted baked file in memory.
            std::ifstream sourceFile(source, std::ios::binary | std::ios::ate);
            if (!sourceFile)
                return {Status::Error, {}, "Cannot read baked texture"};
            const auto sourceSize = sourceFile.tellg();
            constexpr std::streamoff MAX_SOURCE_BYTES = 512ll * 1024 * 1024;
            if (sourceSize < static_cast<std::streamoff>(sizeof(SpiteTextureHeader)) || sourceSize > MAX_SOURCE_BYTES)
                return {Status::Error, {}, "Invalid baked texture size"};
            sourceFile.seekg(0);
            SpiteTextureHeader header;
            if (!sourceFile.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
                std::memcmp(header.magic, "SPTX", 4) != 0 || header.width > 32768 || header.height > 32768 ||
                !bakedTextureMipLevels(header))
                return {Status::Error, {}, "Invalid baked texture mip chain"};
            sourceFile.close();
            std::vector<unsigned char> bytes;
            auto read = [&](const std::filesystem::path& path, bool keep)
            {
                std::ifstream file(path, std::ios::binary);
                if (!file)
                    return false;
                char buffer[65536];
                while (file.read(buffer, sizeof(buffer)) || file.gcount())
                {
                    const auto count = static_cast<size_t>(file.gcount());
                    for (size_t i = 0; i < count; ++i)
                        feed(static_cast<unsigned char>(buffer[i]));
                    if (keep)
                    {
                        if (bytes.size() + count > static_cast<size_t>(MAX_SOURCE_BYTES))
                            return false;
                        bytes.insert(bytes.end(), buffer, buffer + count);
                    }
                }
                feed(0xff);
                return file.eof();
            };
            if (!read(source, true))
                return {Status::Error, {}, "Cannot read baked texture"};
            for (const auto& dependency : dependencies)
                if (!read(dependency, false))
                    return {Status::Error, {}, "Cannot read preview dependency"};
            std::ostringstream name;
            name << std::hex << std::setfill('0') << std::setw(16) << hash << ".png";
            const auto path = cache / name.str();
            std::error_code ec;
            if (std::filesystem::is_regular_file(path, ec))
                return {Status::Ready, path, {}};
            if (bytes.size() < sizeof(SpiteTextureHeader))
                return {Status::Error, {}, "Invalid baked texture"};
            std::memcpy(&header, bytes.data(), sizeof(header));
            const u32 levels = bakedTextureMipLevels(header);
            const u8 format = bakedTextureFormat(header);
            const auto payload =
                decodeBakedTexturePayload(std::as_bytes(std::span(bytes)), static_cast<u64>(MAX_SOURCE_BYTES));
            if (!payload)
                return {Status::Error, {}, "Invalid baked texture mip chain"};
            u32 level = 0;
            size_t offset = 0;
            while (level + 1 < levels &&
                   (mipExtent(header.width, level) > maxSize || mipExtent(header.height, level) > maxSize))
            {
                offset +=
                    bakedTextureLevelBytes(format, mipExtent(header.width, level), mipExtent(header.height, level));
                ++level;
            }
            u32 width = mipExtent(header.width, level), height = mipExtent(header.height, level);
            std::vector<u8> rgba(size_t(width) * height * 4);
            if (bakedTextureFormatCompressed(format))
                decodeTextureLevel(format, payload->data() + offset, width, height, rgba.data());
            else
                std::memcpy(rgba.data(), payload->data() + offset, rgba.size());
            if (format == BAKED_TEXTURE_FORMAT_BC4_UNORM)
                for (size_t i = 0; i < rgba.size(); i += 4)
                    rgba[i + 1] = rgba[i + 2] = rgba[i];
            if (width > maxSize || height > maxSize)
            {
                auto chain =
                    buildMipChain(rgba.data(), width, height, static_cast<BakedTextureFilter>(header.mipFilter));
                size_t at = 0;
                while (width > maxSize || height > maxSize)
                {
                    at += size_t(width) * height * 4;
                    width = std::max(1u, width / 2);
                    height = std::max(1u, height / 2);
                }
                rgba.assign(chain.begin() + at, chain.begin() + at + size_t(width) * height * 4);
            }
            std::filesystem::create_directories(cache, ec);
            if (ec)
                return {Status::Error, {}, "Cannot create preview cache"};
            std::vector<unsigned char> png;
            const auto append = [](void* context, void* data, int size)
            {
                auto& out = *static_cast<std::vector<unsigned char>*>(context);
                auto* first = static_cast<unsigned char*>(data);
                out.insert(out.end(), first, first + size);
            };
            if (!stbi_write_png_to_func(append, &png, static_cast<int>(width), static_cast<int>(height), 4, rgba.data(),
                                        static_cast<int>(width * 4)))
                return {Status::Error, {}, "Cannot encode preview"};
            // Publish only a complete PNG.
            auto temp = path;
            temp += std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L".tmp";
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
            file.close();
            if (!file)
            {
                std::filesystem::remove(temp, ec);
                return {Status::Error, {}, "Cannot write preview"};
            }
            std::filesystem::rename(temp, path, ec);
            if (ec && !std::filesystem::is_regular_file(path))
            {
                std::filesystem::remove(temp, ec);
                return {Status::Error, {}, "Cannot publish preview"};
            }
            if (ec)
                std::filesystem::remove(temp, ec);
            return {Status::Ready, path, {}};
        }
    } // namespace

    EditorAssetPreviews::EditorAssetPreviews(std::filesystem::path cacheDirectory, unsigned maxSize) :
        m_cacheDirectory(std::move(cacheDirectory)), m_maxSize(maxSize)
    {
    }
    EditorAssetPreviews::~EditorAssetPreviews()
    {
        clear();
        if (m_worker.valid())
            m_worker.wait();
    }
    void EditorAssetPreviews::request(const std::string& key, const std::filesystem::path& bakedTexture,
                                      const std::vector<std::filesystem::path>& dependencies)
    {
        auto it = m_entries.find(key);
        if (it != m_entries.end() && it->second.source == bakedTexture && it->second.dependencies == dependencies)
            return;
        cancel(key);
        m_entries.emplace(key, Entry{bakedTexture, dependencies, {Status::Pending, {}, {}}, ++m_generation});
        m_queue.push_back(key);
    }
    EditorAssetPreviews::Result EditorAssetPreviews::status(const std::string& key) const
    {
        const auto it = m_entries.find(key);
        return it == m_entries.end() ? Result{} : it->second.result;
    }
    void EditorAssetPreviews::cancel(const std::string& key)
    {
        m_entries.erase(key);
        std::erase(m_queue, key);
    }
    void EditorAssetPreviews::clear()
    {
        m_entries.clear();
        m_queue.clear();
        ++m_generation;
    }
    void EditorAssetPreviews::update()
    {
        if (m_worker.valid())
        {
            if (m_worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            auto done = m_worker.get();
            auto it = m_entries.find(done.key);
            if (it != m_entries.end() && done.generation == it->second.generation)
                it->second.result = std::move(done.result);
        }
        if (m_queue.empty())
            return;
        auto key = std::move(m_queue.back());
        m_queue.pop_back();
        const auto& entry = m_entries.at(key);
        m_worker = std::async(std::launch::async,
                              [key, generation = entry.generation, cache = m_cacheDirectory, source = entry.source,
                               dependencies = entry.dependencies, maxSize = m_maxSize]
                              {
                                  try
                                  {
                                      return Completed{key, generation, generate(cache, source, dependencies, maxSize)};
                                  }
                                  catch (const std::exception& e)
                                  {
                                      return Completed{key, generation, {Status::Error, {}, e.what()}};
                                  }
                              });
    }
} // namespace spite
