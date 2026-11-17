#include "control/ControlServer.hpp"

#include <algorithm>
#include <chrono>
#include <format>

#include "base/io/FileSystem.hpp"

namespace spite
{
    namespace
    {
        constexpr std::string_view SEPARATORS = "/\\";

        char lowerAscii(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

        std::string modifiedText(const std::filesystem::file_time_type time)
        {
            // by hand: std::format of a chrono time costs ~3 us, and a capped listing formats thousands
            const auto seconds =
                std::chrono::floor<std::chrono::seconds>(std::chrono::clock_cast<std::chrono::system_clock>(time));
            const auto days = std::chrono::floor<std::chrono::days>(seconds);
            const std::chrono::year_month_day date{days};
            const std::chrono::hh_mm_ss<std::chrono::seconds> clock{seconds - days};
            return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", static_cast<int>(date.year()),
                               static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                               clock.hours().count(), clock.minutes().count(), clock.seconds().count());
        }

        bool lessByName(const std::string& a, const std::string& b)
        {
            const sizet common = std::min(a.size(), b.size());
            for (sizet i = 0; i < common; ++i)
            {
                const char x = lowerAscii(a[i]);
                const char y = lowerAscii(b[i]);
                if (x != y)
                {
                    return static_cast<unsigned char>(x) < static_cast<unsigned char>(y);
                }
            }
            return a.size() < b.size() || (a.size() == b.size() && a < b);
        }
    } // namespace

    bool fileNameMatchesExtensions(const std::string_view fileName, const std::string_view extensions)
    {
        const sizet dot = fileName.rfind('.');
        const std::string_view have =
            dot == std::string_view::npos || dot == 0 ? std::string_view{} : fileName.substr(dot + 1);
        bool listed = false;
        for (sizet from = 0; from <= extensions.size();)
        {
            sizet to = extensions.find(',', from);
            to = to == std::string_view::npos ? extensions.size() : to;
            std::string_view part = extensions.substr(from, to - from);
            from = to + 1;
            while (!part.empty() && part.front() == ' ')
            {
                part.remove_prefix(1);
            }
            while (!part.empty() && part.back() == ' ')
            {
                part.remove_suffix(1);
            }
            if (!part.empty() && part.front() == '.')
            {
                part.remove_prefix(1);
            }
            if (part.empty())
            {
                continue;
            }
            listed = true;
            if (!have.empty() && have.size() == part.size() &&
                std::ranges::equal(have, part,
                                   [](const char x, const char y) { return lowerAscii(x) == lowerAscii(y); }))
            {
                return true;
            }
        }
        return !listed;
    }

    std::string ControlServer::listDirectory(const std::string_view path, const std::string_view extensions,
                                             const bool foldersOnly, DirectoryListing& out,
                                             const bool newestFirst) const
    {
        out = {};
        const std::vector<std::string> roots = rootNames();
        std::string rootList;
        for (const std::string& name : roots)
        {
            rootList += (rootList.empty() ? "" : ", ") + name;
        }

        if (path.empty())
        {
            for (const std::string& name : roots)
            {
                out.entries.push_back({.name = name, .kind = DirectoryEntryKind::Directory});
            }
            return {};
        }
        if (SEPARATORS.find(path.front()) != std::string_view::npos)
        {
            return "path '" + std::string(path) +
                "' is absolute; name a root and a directory under it, one of: " + rootList;
        }
        const sizet firstEnd = path.find_first_of(SEPARATORS);
        const std::string_view first = path.substr(0, firstEnd);
        if (!std::ranges::contains(roots, std::string(first)))
        {
            return "no '" + std::string(first) + "' root; roots: " + (rootList.empty() ? "none set" : rootList);
        }

        const std::string_view rest =
            firstEnd == std::string_view::npos ? std::string_view{} : path.substr(firstEnd + 1);
        std::filesystem::path directory;
        if (std::string error = resolveDirectory(first, rest, directory); !error.empty())
        {
            return error;
        }

        // the listed path in its one form: '/' separated, no empty or "." part (".." was refused above)
        out.root = std::string(first);
        out.path = out.root;
        for (sizet from = 0; from < rest.size();)
        {
            sizet to = rest.find_first_of(SEPARATORS, from);
            to = to == std::string_view::npos ? rest.size() : to;
            if (const std::string_view part = rest.substr(from, to - from); !part.empty() && part != ".")
            {
                out.path += "/" + std::string(part);
            }
            from = to + 1;
        }
        out.directory = utf8Path(directory);

        std::error_code error;
        if (!std::filesystem::is_directory(directory, error))
        {
            if (out.path == out.root && !std::filesystem::exists(directory, error))
            {
                return {}; // a root nothing was written to yet
            }
            return "'" + out.path + "' is not a directory";
        }

        struct Item
        {
            DirectoryEntry entry;
            std::filesystem::directory_entry source;
            std::filesystem::file_time_type time{}; // newestFirst only
        };
        std::vector<Item> items;
        std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied,
                                               error);
        for (; !error && it != std::filesystem::directory_iterator(); it.increment(error))
        {
            std::error_code statusError;
            const std::filesystem::file_type type = it->symlink_status(statusError).type();
            Item item{.source = *it};
            item.entry.kind = type == std::filesystem::file_type::regular ? DirectoryEntryKind::File
                : type == std::filesystem::file_type::directory           ? DirectoryEntryKind::Directory
                                                                          : DirectoryEntryKind::Other;
            if (foldersOnly && item.entry.kind != DirectoryEntryKind::Directory)
            {
                continue;
            }
            item.entry.name = utf8Path(it->path().filename());
            if (item.entry.kind != DirectoryEntryKind::Directory &&
                !fileNameMatchesExtensions(item.entry.name, extensions))
            {
                continue;
            }
            if (newestFirst)
            {
                item.time = it->last_write_time(statusError);
            }
            items.push_back(std::move(item));
        }
        if (error)
        {
            return "cannot read '" + out.path + "': " + error.message();
        }

        std::ranges::sort(items,
                          [newestFirst](const Item& a, const Item& b)
                          {
                              const bool aFolder = a.entry.kind == DirectoryEntryKind::Directory;
                              const bool bFolder = b.entry.kind == DirectoryEntryKind::Directory;
                              if (aFolder != bFolder)
                              {
                                  return aFolder;
                              }
                              if (newestFirst && !aFolder && a.time != b.time)
                              {
                                  return a.time > b.time;
                              }
                              return lessByName(a.entry.name, b.entry.name);
                          });
        if (items.size() > MAX_DIRECTORY_ENTRIES)
        {
            items.resize(MAX_DIRECTORY_ENTRIES);
            out.truncated = true;
        }
        out.entries.reserve(items.size());
        for (Item& item : items)
        {
            std::error_code attributeError;
            if (item.entry.kind == DirectoryEntryKind::File)
            {
                const std::uintmax_t size = item.source.file_size(attributeError);
                item.entry.size = attributeError ? 0 : size;
            }
            const std::filesystem::file_time_type time = item.source.last_write_time(attributeError);
            if (!attributeError)
            {
                item.entry.modified = modifiedText(time);
            }
            out.entries.push_back(std::move(item.entry));
        }
        return {};
    }
} // namespace spite
