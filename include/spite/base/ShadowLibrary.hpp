#pragma once

#include <filesystem>

#include "base/Expected.hpp"
#include "base/ImageRange.hpp"
#include "BaseExport.hpp"

namespace spite
{
    // A DLL mapped from a copy: Windows locks a mapped file, so the build keeps writing the original. The copy is
    // <directory>/<stem>.<pid>.<n>.dll, n counting the process's copies.
    struct ShadowLibrary
    {
        std::filesystem::path shadow;
        void* handle = nullptr; // HMODULE
    };

    // Refused, nothing mapped and no copy left, when the file is missing or the copy or LoadLibrary fails.
    SPITE_BASE_API Expected<ShadowLibrary> mapShadowCopy(const std::filesystem::path& file,
                                                         const std::filesystem::path& directory,
                                                         const std::filesystem::path& stem);
    // Unmaps the image and deletes its copy.
    SPITE_BASE_API void freeShadowCopy(const ShadowLibrary& library);
    [[nodiscard]] SPITE_BASE_API void* shadowSymbol(const ShadowLibrary& library, const char* name);
    [[nodiscard]] SPITE_BASE_API ImageRange shadowImage(const ShadowLibrary& library);
    // Deletes the copies of processes that no longer run; a crashed or killed process never removed its own. A live
    // process's copies stay (they are mapped).
    SPITE_BASE_API void removeDeadShadowCopies(const std::filesystem::path& directory);
    // ".dll" is optional; a relative path resolves against the executable's directory.
    [[nodiscard]] SPITE_BASE_API std::filesystem::path resolveLibraryPath(std::filesystem::path path);
} // namespace spite
