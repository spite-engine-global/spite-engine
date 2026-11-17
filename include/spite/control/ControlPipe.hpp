#pragma once

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

// Header-only: the engine (ControlServer::listenPipe) and the `spite` client, which links no engine module, derive
// the same name.
namespace spite
{
    // the pipe when an executable is not in a checkout's bin/<platform>/<config>/
    inline constexpr std::string_view CONTROL_PIPE_NAME = R"(\\.\pipe\spite-engine)";

    // a named session's pipe is the checkout's pipe plus this infix and the name
    inline constexpr std::string_view SESSION_PIPE_INFIX = "-session-";

    // The control pipe of the checkout an executable was built in (<root>/bin/<platform>/<config>/<exe>): engines from
    // several worktrees run at once, and `spite attach` reaches the one built beside it. The folder name keeps the
    // pipe readable; the hash of the whole root path keeps two checkouts of the same folder name apart.
    inline std::string controlPipeName(const std::filesystem::path& executable)
    {
        const std::filesystem::path root = executable.parent_path().parent_path().parent_path().parent_path();
        if (executable.parent_path().parent_path().parent_path().filename() != "bin" || root.filename().empty())
        {
            return std::string(CONTROL_PIPE_NAME);
        }
        std::uint64_t hash = 14695981039346656037ull;
        for (const char c : root.lexically_normal().string())
        {
            hash = (hash ^ static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(c)))) * 1099511628211ull;
        }
        return std::format(R"({}-{}-{:08x})", CONTROL_PIPE_NAME, root.filename().string(),
                           static_cast<std::uint32_t>(hash ^ (hash >> 32)));
    }
} // namespace spite
