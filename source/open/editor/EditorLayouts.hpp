#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "editor/EditorExport.hpp"

namespace spite
{
    // The editor's named dock layouts: the built-in presets (Default is the registry's placements, built by
    // the shell; Debug and Profiling are assets/ui/editor/layouts/<name>.json) and the user's, one `<name>.json` per
    // layout in a folder beside the layout file (config/user/layouts). A name is validated before it becomes a path,
    // so no name reaches outside the folder. Every function answers an error text, empty for success.
    class SPITE_EDITOR_API EditorLayoutStore
    {
    public:
        static constexpr std::array<std::string_view, 3> BUILT_IN{"Default", "Debug", "Profiling"};
        static constexpr std::size_t NAME_LIMIT = 40; // bytes of UTF-8: a menu row holds the whole name

        EditorLayoutStore(std::filesystem::path userDirectory, std::filesystem::path builtInDirectory) :
            m_user(std::move(userDirectory)), m_builtIn(std::move(builtInDirectory))
        {
        }

        [[nodiscard]] const std::filesystem::path& directory() const { return m_user; }

        // Whether `name` may name a user layout: 1 to NAME_LIMIT bytes, no control character or one of \ / : * ? " < >
        // |, no leading or trailing space or dot, not a Windows device name (CON, NUL, COM1...), not a built-in's.
        [[nodiscard]] static std::string checkName(std::string_view name);
        // The built-in's own spelling for `name` (matched without case), else empty.
        [[nodiscard]] static std::string_view builtIn(std::string_view name);

        // The user layouts, sorted; files whose stem is not a valid name are skipped.
        [[nodiscard]] std::vector<std::string> names() const;
        // The text of a built-in (not Default) or a user layout.
        [[nodiscard]] std::string read(std::string_view name, std::string& text) const;
        [[nodiscard]] std::string write(std::string_view name, std::string_view text) const;
        [[nodiscard]] std::string remove(std::string_view name) const;

    private:
        [[nodiscard]] std::filesystem::path fileOf(std::string_view name) const;
        // "no layout named X" with the names that exist
        [[nodiscard]] std::string missing(std::string_view name) const;

        std::filesystem::path m_user;
        std::filesystem::path m_builtIn;
    };
} // namespace spite
