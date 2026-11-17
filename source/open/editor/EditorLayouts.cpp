#include "EditorLayouts.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <system_error>

#include "base/io/FileSystem.hpp"
#include <spite/base/Utf8.hpp>

namespace spite
{
    namespace
    {
        bool sameWithoutCase(const std::string_view a, const std::string_view b)
        {
            return std::ranges::equal(
                a, b, [](const char x, const char y)
                { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
        }
    } // namespace

    std::string_view EditorLayoutStore::builtIn(const std::string_view name)
    {
        const auto it =
            std::ranges::find_if(BUILT_IN, [name](const std::string_view own) { return sameWithoutCase(name, own); });
        return it == BUILT_IN.end() ? std::string_view{} : *it;
    }

    std::string EditorLayoutStore::checkName(const std::string_view name)
    {
        if (name.empty() || name.size() > NAME_LIMIT)
        {
            return std::format("a layout name is 1 to {} bytes", NAME_LIMIT);
        }
        if (!validUtf8(name))
        {
            return "a layout name must be valid UTF-8";
        }
        constexpr std::string_view FORBIDDEN = "\\/:*?\"<>|";
        const auto bad = [FORBIDDEN](const char c)
        { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F || FORBIDDEN.contains(c); };
        if (std::ranges::any_of(name, bad))
        {
            return "a layout name has no control character and none of \\ / : * ? \" < > |";
        }
        const auto edge = [](const char c) { return c == ' ' || c == '.'; };
        if (edge(name.front()) || edge(name.back()) || isWindowsDeviceName(name))
        {
            return "a layout name does not start or end with a space or a dot and is not a device name (CON, NUL, "
                   "COM1...)";
        }
        if (!builtIn(name).empty())
        {
            return std::format("{} is a built-in layout", builtIn(name));
        }
        return {};
    }

    std::filesystem::path EditorLayoutStore::fileOf(const std::string_view name) const
    {
        const std::string_view own = builtIn(name);
        if (!own.empty())
        {
            std::string lower(own);
            std::ranges::transform(lower, lower.begin(), [](const char c)
                                   { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            return m_builtIn / (lower + ".json");
        }
        return m_user / pathFromUtf8(std::string(name) + ".json");
    }

    std::string EditorLayoutStore::missing(const std::string_view name) const
    {
        std::string text = std::format("no layout named {}; there are", name);
        for (const std::string_view own : BUILT_IN)
        {
            text += std::format(" {},", own);
        }
        for (const std::string& saved : names())
        {
            text += std::format(" {},", saved);
        }
        text.pop_back();
        return text;
    }

    std::vector<std::string> EditorLayoutStore::names() const
    {
        std::vector<std::string> found;
        const Expected<std::vector<std::filesystem::path>> files = listDirectory(m_user);
        if (!files)
        {
            return found;
        }
        for (const std::filesystem::path& file : *files)
        {
            std::string stem;
            if (file.extension() == ".json" && isPlainEntry(file, false) && tryPathToUtf8(file.stem(), stem) &&
                checkName(stem).empty())
            {
                found.push_back(std::move(stem));
            }
        }
        std::ranges::sort(found);
        return found;
    }

    std::string EditorLayoutStore::read(const std::string_view name, std::string& text) const
    {
        if (builtIn(name).empty())
        {
            if (std::string error = checkName(name); !error.empty())
            {
                return error;
            }
        }
        const std::filesystem::path file = fileOf(name);
        if (!isPlainEntry(file, false))
        {
            return missing(name);
        }
        Expected<std::string> read = readTextFile(file);
        if (!read)
        {
            return std::string(read.error().message.c_str());
        }
        text = std::move(*read);
        return {};
    }

    std::string EditorLayoutStore::write(const std::string_view name, const std::string_view text) const
    {
        if (std::string error = checkName(name); !error.empty())
        {
            return error;
        }
        if (const Expected<void> made = ensureDirectory(m_user); !made)
        {
            return std::string(made.error().message.c_str());
        }
        if (const Expected<void> written = writeTextFileAtomically(fileOf(name), text); !written)
        {
            return std::string(written.error().message.c_str());
        }
        return {};
    }

    std::string EditorLayoutStore::remove(const std::string_view name) const
    {
        if (std::string error = checkName(name); !error.empty())
        {
            return error;
        }
        const std::filesystem::path file = fileOf(name);
        std::error_code failed;
        if (!isPlainEntry(file, false) || !std::filesystem::remove(file, failed))
        {
            return failed ? failed.message() : missing(name);
        }
        return {};
    }
} // namespace spite
