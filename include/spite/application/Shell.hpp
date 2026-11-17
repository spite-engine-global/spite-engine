#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "application/ApplicationExport.hpp"

namespace spite
{
    // The file manager that revealInFileManager starts: explorer.exe in the Windows directory by its absolute path,
    // never a name the loader would search for (the application directory, the working directory and PATH come first
    // in that search, so a planted explorer.exe there would run). Empty when the Windows directory is unknown.
    SPITE_APPLICATION_API std::filesystem::path fileManagerExecutable();

    // Explorer's command line (/select) for `file`, or nothing for a path that is not safe to hand over: empty or
    // relative, a quote (it would end the argument), a colon beyond the drive letter (an alternate data stream names
    // another file than the one shown), or a name ending in a dot or a space (Win32 drops those, so the path names
    // another file than it reads).
    [[nodiscard]] SPITE_APPLICATION_API std::optional<std::wstring> fileManagerArguments(
        const std::filesystem::path& file);

    // Shows an existing file selected in the OS file manager. The path is one quoted argument and never reaches a
    // shell. False when the path is refused (fileManagerArguments) or the file manager did not start. The caller
    // decides which paths may be shown; this checks only what it needs to be safe.
    SPITE_APPLICATION_API bool revealInFileManager(const std::filesystem::path& file);
} // namespace spite
