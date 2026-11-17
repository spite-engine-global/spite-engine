#pragma once
#include <cstddef>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/BaseExport.hpp"
#include "base/Expected.hpp"

namespace spite
{
    SPITE_BASE_API std::string getFileExtension(const std::filesystem::path& path);

    SPITE_BASE_API std::string getFilename(const std::filesystem::path& path);

    SPITE_BASE_API std::string getFilenameWithoutExtension(const std::filesystem::path& path);

    SPITE_BASE_API Expected<std::vector<std::byte>> readBinaryFile(const std::filesystem::path& path);

    SPITE_BASE_API Expected<std::string> readTextFile(const std::filesystem::path& path);

    SPITE_BASE_API Expected<void> writeBinaryFile(const std::filesystem::path& path, std::span<const std::byte> data);

    SPITE_BASE_API Expected<void> writeTextFile(const std::filesystem::path& path, std::string_view content);

    SPITE_BASE_API Expected<void> replaceFileAtomically(const std::filesystem::path& temporaryPath,
                                                        const std::filesystem::path& path);

    SPITE_BASE_API Expected<void> writeBinaryFileAtomically(const std::filesystem::path& path,
                                                            std::span<const std::byte> data);
    // Publishes only if the destination does not exist; a competing writer is never replaced.
    SPITE_BASE_API Expected<void> writeBinaryFileAtomicallyNew(const std::filesystem::path& path,
                                                               std::span<const std::byte> data);

    SPITE_BASE_API Expected<void> writeTextFileAtomically(const std::filesystem::path& path, std::string_view content);

    SPITE_BASE_API Expected<void> ensureDirectory(const std::filesystem::path& path);

    SPITE_BASE_API Expected<bool> pathExists(const std::filesystem::path& path);

    SPITE_BASE_API Expected<bool> isRegularFile(const std::filesystem::path& path);

    SPITE_BASE_API Expected<bool> isDirectory(const std::filesystem::path& path);

    SPITE_BASE_API Expected<uintmax_t> fileSize(const std::filesystem::path& path);

    SPITE_BASE_API Expected<std::filesystem::file_time_type> lastWriteTime(const std::filesystem::path& path);

    // path's text as UTF-8 in out; false for a name that is not valid UTF-16 (a lone surrogate), where path::u8string
    // throws.
    SPITE_BASE_API bool tryPathToUtf8(const std::filesystem::path& path, std::string& out);

    // path's text as UTF-8 with '/' separators: the form a control reply names a file in. A name that is not valid
    // UTF-16 gets U+FFFD, so it never throws; path::string() is the ANSI page, not UTF-8.
    SPITE_BASE_API std::string utf8Path(const std::filesystem::path& path);

    // The path a UTF-8 text names (a control arg or reply); throws std::system_error on invalid UTF-8.
    SPITE_BASE_API std::filesystem::path pathFromUtf8(std::string_view text);

    // Whether path is a plain file or directory entry, not a link, junction, mount point or cloud placeholder:
    // reading one could leave the directory or fetch it over a network. A path that cannot be queried is not.
    // directory: whether a directory is the expected kind.
    SPITE_BASE_API bool isPlainEntry(const std::filesystem::path& path, bool directory);

    // Whether a file name component (UTF-8) opens a Windows device, not a file: CON PRN AUX NUL CONIN$ CONOUT$ and
    // COM or LPT with 0-9 or the superscript digits 1-3, in any case. The part before the first dot counts, without the
    // spaces Win32 drops from its end: "CON", "nul .txt" and "COM1.log" are devices, "console" and "COM10" are not.
    SPITE_BASE_API bool isWindowsDeviceName(std::string_view name);

    SPITE_BASE_API Expected<std::vector<std::filesystem::path>> listDirectory(const std::filesystem::path& path,
                                                                              bool recursive = false);
} // namespace spite
