#pragma once

#include <filesystem>
#include <string_view>

#include "base/BaseExport.hpp"

namespace spite
{
    enum class WritableFolder
    {
        UserConfig,
        Saves,
        Scenes,
        Cache,
        Shadows,
        Crashes,
        Artifacts,
        Control
    };

    // Overrides are set during startup before the host constructs its services.
    SPITE_BASE_API bool isWritableDirectoryOption(std::string_view option);
    SPITE_BASE_API void setWritableDirectoryOption(std::string_view option, const std::filesystem::path& value);
    SPITE_BASE_API std::filesystem::path writableDirectory(WritableFolder folder,
                                                           const std::filesystem::path& explicitPath = {},
                                                           bool verifyWritable = true);
    // Translate the logical user layer while preserving existing explicit path semantics.
    SPITE_BASE_API std::filesystem::path resolveUserConfigPath(const std::filesystem::path& path);
} // namespace spite
