#pragma once
#include <filesystem>

#include "base/BaseExport.hpp"

namespace spite
{
    // The one directory engine content (shaders, assets, config, baked output) is read from, never the working
    // directory. Precedence: setContentRoot (the --content-root flag), the SPITE_CONTENT_ROOT environment variable,
    // the nearest ancestor of the executable's directory holding both `assets` and `shaders`; with none of those the
    // working directory, as before this existed. Derived once, on first use.
    SPITE_BASE_API const std::filesystem::path& contentRoot();
    // Overrides the root for the rest of the process; set before any content is read.
    SPITE_BASE_API void setContentRoot(const std::filesystem::path& root);
    // A relative path resolves against the content root; an absolute or empty path stays as is.
    SPITE_BASE_API std::filesystem::path resolveContentPath(const std::filesystem::path& path);
    // The first of `start` and its ancestors that holds `assets` and `shaders` directories; empty when none does.
    SPITE_BASE_API std::filesystem::path findContentRoot(std::filesystem::path start);
    // The configured writable save-data root for game modules.
    SPITE_BASE_API std::filesystem::path savesDirectory();
} // namespace spite
