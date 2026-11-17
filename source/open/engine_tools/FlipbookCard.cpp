#include "FlipbookCard.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <vector>
#if defined(_WIN32)
#include "platform/ProcessId.hpp"
#define NOMINMAX
#include <Windows.h>
#elif defined(__linux__)
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>
#include <nlohmann/json.hpp>

#include "asset/AssetImportingFunctions.hpp"
#include "base/io/FileSystem.hpp"

namespace spite
{
#if defined(__linux__)
    namespace
    {
        bool syncCardFile(const int file)
        {
            int result;
            do
            {
                result = fsync(file);
            }
            while (result < 0 && errno == EINTR);
            return result == 0;
        }

        bool writeCardFile(const int file, std::span<const std::byte> bytes)
        {
            while (!bytes.empty())
            {
                const auto written = write(file, bytes.data(), std::min<size_t>(bytes.size(), 1u << 30));
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0)
                    return false;
                bytes = bytes.subspan(static_cast<size_t>(written));
            }
            return syncCardFile(file);
        }

        bool linkCardFile(const int file, const int directory, const char* name)
        {
            if (linkat(file, "", directory, name, AT_EMPTY_PATH) == 0)
                return true;
            if (errno != ENOENT && errno != EPERM)
                return false;

            // Unprivileged callers can name the same open file through procfs.
            char descriptorPath[64];
            std::snprintf(descriptorPath, sizeof(descriptorPath), "/proc/self/fd/%d", file);
            return linkat(AT_FDCWD, descriptorPath, directory, name, AT_SYMLINK_FOLLOW) == 0;
        }

        const char* publishCardFile(const std::filesystem::path& output, const std::span<const std::byte> bytes)
        {
            const auto name = output.filename();
            if (name.empty() || name == "." || name == ".." || output.native().find('\0') != std::string::npos)
                return "invalid card output path";

            const auto parent = output.parent_path();
            open_how how{};
            how.flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
            how.resolve = RESOLVE_NO_SYMLINKS;
            const int directory = static_cast<int>(
                syscall(SYS_openat2, AT_FDCWD, parent.empty() ? "." : parent.c_str(), &how, sizeof(how)));
            if (directory < 0)
                return "cannot open a plain card output directory";

            struct statx entry{};
            constexpr auto refused = STATX_ATTR_MOUNT_ROOT | STATX_ATTR_AUTOMOUNT;
            if (statx(directory, "", AT_EMPTY_PATH | AT_NO_AUTOMOUNT, STATX_TYPE, &entry) != 0 ||
                (entry.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT) == 0 || (entry.stx_attributes & refused) != 0)
            {
                close(directory);
                return "cannot verify a plain card output directory";
            }

            // An unnamed inode keeps the output hidden and gives linkat an exact source to publish.
            const int file = openat(directory, ".", O_WRONLY | O_TMPFILE | O_CLOEXEC, 0666);
            if (file < 0)
            {
                close(directory);
                return "cannot create an unnamed temporary card file on the destination filesystem";
            }

            const char* error = nullptr;
            if (!writeCardFile(file, bytes))
                error = "cannot write or flush the temporary card file";
            else if (!linkCardFile(file, directory, name.c_str()))
                error = "cannot publish card without replacing an existing file";
            else if (!syncCardFile(directory))
                error = "card was published but its output directory could not be flushed";
            close(file);
            close(directory);
            return error;
        }
    } // namespace
#endif

    FlipbookCardResult prepareFlipbookCard(const std::filesystem::path& source, const std::filesystem::path& output,
                                           const FlipbookCardRequest& request, [[maybe_unused]] const u64 jobId)
    {
        int width = 0, height = 0, channels = 0;
        const std::string sourceText = utf8Path(source);
        std::unique_ptr<u8, decltype(&freeTexture)> pixels(decodeImageFileBounded(InternedString(sourceText.c_str()),
                                                                                  width, height, channels, 16384,
                                                                                  67108864, 536870912),
                                                           &freeTexture);
        if (!pixels || width <= 0 || height <= 0)
            return {.error =
                        "cannot decode the atlas: use PNG, JPEG or TGA under 512 MiB, 16384 per side and 64M pixels"};
        if (static_cast<u32>(width) < request.columns || static_cast<u32>(height) < request.rows)
            return {.error = "atlas is smaller than the grid; each cell needs at least one pixel"};

        const size_t rowBytes = static_cast<size_t>(width) * 4;
        std::vector<u8> topFirst(rowBytes * height);
        for (int y = 0; y < height; ++y)
            std::memcpy(topFirst.data() + rowBytes * y, pixels.get() + rowBytes * (height - 1 - y), rowBytes);
        std::vector<u8> png;
        const auto append = [](void* context, void* data, int size)
        {
            auto& bytes = *static_cast<std::vector<u8>*>(context);
            const auto* first = static_cast<const u8*>(data);
            bytes.insert(bytes.end(), first, first + size);
        };
        if (!stbi_write_png_to_func(append, &png, width, height, 4, topFirst.data(), static_cast<int>(rowBytes)))
            return {.error = "cannot encode the atlas as PNG"};

        const float x0 = -request.pivot.x * request.size.x, x1 = x0 + request.size.x;
        const float y0 = -request.pivot.y * request.size.y, y1 = y0 + request.size.y;
        const float positions[] = {x0, y0, 0, x1, y0, 0, x1, y1, 0, x0, y1, 0};
        const float normals[] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
        const float uvs[] = {0, 1, 1, 1, 1, 0, 0, 0};
        const u16 indices[] = {0, 1, 2, 0, 2, 3};
        std::vector<u8> binary;
        const auto add = [&](const void* data, size_t size)
        {
            const auto* first = static_cast<const u8*>(data);
            binary.insert(binary.end(), first, first + size);
        };
        add(positions, sizeof(positions));
        add(normals, sizeof(normals));
        add(uvs, sizeof(uvs));
        add(indices, sizeof(indices));
        const size_t imageOffset = binary.size();
        add(png.data(), png.size());
        const size_t bufferLength = binary.size();
        while (binary.size() % 4)
            binary.push_back(0);
        using Json = nlohmann::json;
        Json material = {{"name", "Flipbook"},
                         {"doubleSided", true},
                         {"alphaMode", "BLEND"},
                         {"pbrMetallicRoughness",
                          {{"baseColorTexture", {{"index", 0}}}, {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}}};
        if (request.emissive)
        {
            material["emissiveTexture"] = {{"index", 0}};
            material["emissiveFactor"] = {1, 1, 1};
        }
        Json json = {
            {"asset", {{"version", "2.0"}, {"generator", "SPITE flipbook card"}}},
            {"scene", 0},
            {"scenes", Json::array({{{"nodes", Json::array({0})}}})},
            {"nodes",
             Json::array({{{"name", "FlipbookCard"}, {"mesh", 0}, {"extras", {{"spite_source_id", "card"}}}}})},
            {"meshes",
             Json::array({{{"primitives",
                            Json::array({{{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}},
                                          {"indices", 3},
                                          {"material", 0}}})}}})},
            {"materials", Json::array({material})},
            {"textures", Json::array({{{"source", 0}, {"sampler", 0}}})},
            {"samplers", Json::array({{{"magFilter", 9729}, {"minFilter", 9729}, {"wrapS", 33071}, {"wrapT", 33071}}})},
            {"images", Json::array({{{"bufferView", 4}, {"mimeType", "image/png"}}})},
            {"buffers", Json::array({{{"byteLength", bufferLength}}})},
            {"bufferViews",
             Json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", 48}},
                          {{"buffer", 0}, {"byteOffset", 48}, {"byteLength", 48}},
                          {{"buffer", 0}, {"byteOffset", 96}, {"byteLength", 32}},
                          {{"buffer", 0}, {"byteOffset", 128}, {"byteLength", 12}},
                          {{"buffer", 0}, {"byteOffset", imageOffset}, {"byteLength", png.size()}}})},
            {"accessors",
             Json::array({{{"bufferView", 0},
                           {"componentType", 5126},
                           {"count", 4},
                           {"type", "VEC3"},
                           {"min", {x0, y0, 0}},
                           {"max", {x1, y1, 0}}},
                          {{"bufferView", 1}, {"componentType", 5126}, {"count", 4}, {"type", "VEC3"}},
                          {{"bufferView", 2}, {"componentType", 5126}, {"count", 4}, {"type", "VEC2"}},
                          {{"bufferView", 3}, {"componentType", 5123}, {"count", 6}, {"type", "SCALAR"}}})}};
        std::string text = json.dump();
        while (text.size() % 4)
            text.push_back(' ');
        std::vector<std::byte> glb;
        const auto addBytes = [&](const void* data, size_t size)
        {
            const auto* first = static_cast<const std::byte*>(data);
            glb.insert(glb.end(), first, first + size);
        };
        const u32 head[] = {0x46546c67u, 2u, static_cast<u32>(12 + 8 + text.size() + 8 + binary.size()),
                            static_cast<u32>(text.size()), 0x4e4f534au};
        const u32 binHead[] = {static_cast<u32>(binary.size()), 0x004e4942u};
        addBytes(head, sizeof(head));
        addBytes(text.data(), text.size());
        addBytes(binHead, sizeof(binHead));
        addBytes(binary.data(), binary.size());
#if defined(_WIN32)
        const std::filesystem::path temp =
            output.wstring() + L".tmp-" + std::to_wstring(spite::platform::processId()) + L"-" + std::to_wstring(jobId);
        const HANDLE file =
            CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return {.error = "cannot create a new temporary card file"};
        bool wrote = true;
        size_t offset = 0;
        while (offset < glb.size())
        {
            const DWORD count = static_cast<DWORD>(std::min<size_t>(glb.size() - offset, 1u << 30));
            DWORD actual = 0;
            if (!WriteFile(file, glb.data() + offset, count, &actual, nullptr) || actual != count)
            {
                wrote = false;
                break;
            }
            offset += actual;
        }
        wrote = wrote && FlushFileBuffers(file);
        CloseHandle(file);
        if (!wrote)
        {
            DeleteFileW(temp.c_str());
            return {.error = "cannot write the temporary card file"};
        }
        if (!MoveFileExW(temp.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(temp.c_str());
            return {.error = "cannot publish card without replacing an existing file"};
        }
#elif defined(__linux__)
        if (const char* error = publishCardFile(output, glb))
            return {.error = error};
#else
        return {.error = "card publication is unavailable on this platform"};
#endif
        return {.width = static_cast<u32>(width), .height = static_cast<u32>(height)};
    }
} // namespace spite
