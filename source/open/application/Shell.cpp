#include "application/Shell.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>

namespace spite
{
    std::filesystem::path fileManagerExecutable()
    {
        wchar_t directory[MAX_PATH];
        const UINT length = GetSystemWindowsDirectoryW(directory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            return {};
        }
        return std::filesystem::path(std::wstring(directory, length)) / L"explorer.exe";
    }

    std::optional<std::wstring> fileManagerArguments(const std::filesystem::path& file)
    {
        const std::wstring& native = file.native();
        if (native.empty() || !file.is_absolute() || native.find(L'"') != std::wstring::npos ||
            native.find(L':', 2) != std::wstring::npos)
        {
            return std::nullopt;
        }
        for (const std::filesystem::path& part : file)
        {
            const std::wstring& name = part.native();
            if (!name.empty() && name != L"." && name != L".." && (name.back() == L'.' || name.back() == L' '))
            {
                return std::nullopt;
            }
        }
        return L"/select,\"" + native + L"\"";
    }

    bool revealInFileManager(const std::filesystem::path& file)
    {
        const std::optional<std::wstring> arguments = fileManagerArguments(file);
        const std::filesystem::path explorer = fileManagerExecutable();
        if (!arguments || explorer.empty())
        {
            return false;
        }
        const auto result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", explorer.c_str(), arguments->c_str(), nullptr, SW_SHOWNORMAL));
        return result > 32;
    }
} // namespace spite
#else
#include <SDL3/SDL_misc.h>

namespace spite
{
    std::filesystem::path fileManagerExecutable() { return {}; }

    std::optional<std::wstring> fileManagerArguments(const std::filesystem::path&) { return std::nullopt; }

    bool revealInFileManager(const std::filesystem::path& file)
    {
        std::error_code error;
        const auto directory = file.parent_path();
        if (!file.is_absolute() || !std::filesystem::is_directory(directory, error))
        {
            return false;
        }

        constexpr char hex[] = "0123456789ABCDEF";
        std::string url = "file://";
        for (const unsigned char byte : directory.generic_string())
        {
            if (byte == 0)
            {
                return false;
            }
            if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
                byte == '/' || byte == '-' || byte == '_' || byte == '.' || byte == '~')
            {
                url += static_cast<char>(byte);
            }
            else
            {
                url += '%';
                url += hex[byte >> 4];
                url += hex[byte & 15];
            }
        }
        return SDL_OpenURL(url.c_str());
    }
} // namespace spite
#endif
