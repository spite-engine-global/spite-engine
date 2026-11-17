#include "engine/LocalizationContent.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <tuple>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "base/Logging.hpp"
#include "base/io/FileSystem.hpp"
#include "localization/Locale.hpp"
#include "localization/Localization.hpp"

namespace spite
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr auto POLL_INTERVAL = std::chrono::milliseconds(100);
        constexpr auto SETTLE = std::chrono::milliseconds(50); // an editor's save is several writes

        struct Message
        {
            std::string key;
            std::string text;
            u32 line = 0;
        };
        using Messages = std::vector<Message>;

        // links and junctions are never followed: a content tree cannot point the loader outside itself
        bool isLink(const fs::path& path)
        {
#if defined(_WIN32)
            const DWORD attributes = GetFileAttributesW(path.c_str());
            return attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
            std::error_code error;
            const auto status = fs::symlink_status(path, error);
            return error || !isPlainEntry(path, fs::is_directory(status));
#endif
        }

        u32 lineAt(const std::string_view text, const std::size_t offset)
        {
            return 1 + static_cast<u32>(std::count(text.begin(), text.begin() + std::min(offset, text.size()), '\n'));
        }

        // A flat object of string members; builds the table without throwing.
        class TableHandler final : public nlohmann::json_sax<nlohmann::json>
        {
        public:
            Messages messages;
            std::string error;
            std::size_t errorOffset = 0;

            bool null() override { return reject("null"); }
            bool boolean(bool) override { return reject("a boolean"); }
            bool number_integer(number_integer_t) override { return reject("a number"); }
            bool number_unsigned(number_unsigned_t) override { return reject("a number"); }
            bool number_float(number_float_t, const string_t&) override { return reject("a number"); }
            bool binary(binary_t&) override { return reject("binary"); }
            bool start_array(std::size_t) override { return reject("an array"); }
            bool end_array() override { return true; }
            bool string(string_t& value) override
            {
                if (m_depth == 0)
                {
                    return reject("a string");
                }
                messages.push_back({m_key, value, 0});
                return true;
            }
            bool start_object(std::size_t) override
            {
                if (m_depth++ == 0)
                {
                    return true;
                }
                return reject("an object");
            }
            bool key(string_t& value) override
            {
                if (!m_seen.insert(value).second)
                {
                    error = std::format("duplicate key '{}'", value);
                    return false;
                }
                m_key = value;
                return true;
            }
            bool end_object() override { return true; }
            bool parse_error(const std::size_t position, const std::string&,
                             const nlohmann::detail::exception& exception) override
            {
                error = exception.what();
                errorOffset = position;
                return false;
            }

        private:
            bool reject(const char* what)
            {
                error = m_depth == 0 ? std::string("the file must be a JSON object of string messages")
                                     : std::format("the value of '{}' must be a string, not {}", m_key, what);
                return false;
            }

            u32 m_depth = 0;
            std::string m_key;
            std::set<std::string> m_seen;
        };

        // Parses text into messages with each message's line; the error carries the JSON position as a line.
        std::optional<LocFileError> parseTable(const std::string_view text, const std::string& file, Messages& out)
        {
            TableHandler handler;
            if (!nlohmann::json::sax_parse(text, &handler))
            {
                return LocFileError{file,
                                    handler.errorOffset ? lineAt(text, handler.errorOffset) : 0,
                                    {},
                                    0,
                                    handler.error.empty() ? "invalid JSON" : handler.error};
            }
            std::size_t cursor = 0;
            for (Message& message : handler.messages)
            {
                const std::size_t found = text.find('"' + message.key + '"', cursor);
                if (found != std::string_view::npos)
                {
                    message.line = lineAt(text, found);
                    cursor = found;
                }
            }
            out = std::move(handler.messages);
            return std::nullopt;
        }

        struct FileInfo
        {
            std::string id; // locale/namespace
            std::string locale;
            std::string tableNamespace;
            std::string label; // locale/namespace.json, as errors name it
            fs::path path;
            fs::file_time_type time{};
            u64 size = 0;
            bool nested = false; // <locale>/<group>/<name>.json
        };

        std::string joinId(const std::string_view locale, const std::string_view tableNamespace)
        {
            return std::string(locale) + '/' + std::string(tableNamespace);
        }
    } // namespace

    std::string LocFileError::text() const
    {
        std::string out = file;
        if (line)
        {
            out += std::format(":{}", line);
        }
        out += ": ";
        if (!key.empty())
        {
            out += std::format("message '{}' at byte {}: ", key, offset);
        }
        return out + reason;
    }

    struct LocalizationContent::Impl
    {
        struct Known
        {
            fs::file_time_type time{};
            u64 size = 0;
            bool applied = false;
        };
        struct Pending
        {
            FileInfo file;
            Messages messages;
        };

        Localization& localization;
        fs::path root;
        std::string defaultLocale;
        std::map<std::string, Known> known;
        std::map<std::string, Messages> defaultTables; // namespace -> the default locale's messages
        std::map<std::string, LocFileError> failed; // by table id
#if defined(_WIN32)
        HANDLE watch = INVALID_HANDLE_VALUE;
        std::chrono::steady_clock::time_point lastPoll{};
        std::chrono::steady_clock::time_point lastChange{};
        bool changePending = false;
#endif
        u64 published = 0;
        std::vector<std::pair<u64, ChangeFn>> listeners;
        u64 nextListener = 1;

        Impl(Localization& target, fs::path rootPath) :
            localization(target), root(std::move(rootPath)), defaultLocale(target.defaultLocale())
        {
        }

#if defined(_WIN32)
        ~Impl()
        {
            if (watch != INVALID_HANDLE_VALUE)
            {
                FindCloseChangeNotification(watch);
            }
        }

        void armWatch()
        {
            std::error_code ec;
            if (watch != INVALID_HANDLE_VALUE || !fs::is_directory(root, ec))
            {
                return;
            }
            watch = FindFirstChangeNotificationW(root.c_str(), TRUE,
                                                 FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
                                                     FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE);
            if (watch == INVALID_HANDLE_VALUE)
            {
                SWARNING_LOG("localization: cannot watch '{}'", root.string());
            }
        }

#endif

        // false when the listing is incomplete (a limit, an error or a missing root): nothing may be removed then
        bool scan(std::vector<FileInfo>& files, std::vector<LocFileError>& errors) const
        {
            std::error_code ec;
            if (!fs::is_directory(root, ec) || isLink(root))
            {
                return false;
            }
            bool complete = true;
            const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
            u32 locales = 0;
            for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
            {
                const fs::path& directory = it->path();
                std::error_code entryError;
                if (!it->is_directory(entryError))
                {
                    continue;
                }
                const std::string locale = directory.filename().string();
                if (isLink(directory))
                {
                    errors.push_back({locale, 0, {}, 0, "ignored: a link or junction"});
                    complete = false;
                    continue;
                }
                if (canonicalLocale(locale) != locale)
                {
                    errors.push_back({locale, 0, {}, 0, "ignored: the directory is not a canonical locale id"});
                    continue;
                }
                if (++locales > MAX_LOCALES)
                {
                    errors.push_back({locale, 0, {}, 0, std::format("ignored: more than {} locales", MAX_LOCALES)});
                    complete = false;
                    break;
                }
                if (fs::weakly_canonical(directory, entryError).parent_path() != canonicalRoot)
                {
                    errors.push_back({locale, 0, {}, 0, "ignored: resolves outside the localization root"});
                    complete = false;
                    continue;
                }
                std::size_t count = 0;
                // The tables of one directory: <locale>/<name>.json is the namespace <name>; one level down,
                // <locale>/<group>/<name>.json is the namespace <group>-<name> (a feature's own file).
                const auto list = [&](auto&& self, const fs::path& dir, const std::string& group) -> bool
                {
                    std::error_code listError;
                    for (fs::directory_iterator file(dir, listError), fileEnd; !listError && file != fileEnd;
                         file.increment(listError))
                    {
                        const fs::path& path = file->path();
                        const std::string name =
                            group.empty() ? path.filename().string() : group + '/' + path.filename().string();
                        const std::string label = locale + '/' + name;
                        std::error_code fileError;
                        if (group.empty() && file->is_directory(fileError))
                        {
                            if (isLink(path))
                            {
                                errors.push_back({label, 0, {}, 0, "ignored: a link or junction"});
                                complete = false;
                            }
                            else if (!isValidTableNamespace(path.filename().string()))
                            {
                                errors.push_back(
                                    {label, 0, {}, 0, "ignored: the directory name is not a valid namespace"});
                            }
                            else if (!self(self, path, path.filename().string()))
                            {
                                return false;
                            }
                            continue;
                        }
                        if (path.extension() != ".json")
                        {
                            continue;
                        }
                        if (!file->is_regular_file(fileError) || isLink(path))
                        {
                            errors.push_back({label, 0, {}, 0, "ignored: not a regular file"});
                            complete = false;
                            continue;
                        }
                        const std::string stem = path.stem().string();
                        const std::string tableNamespace = group.empty() ? stem : group + '-' + stem;
                        if (!isValidTableNamespace(tableNamespace))
                        {
                            errors.push_back({label, 0, {}, 0, "ignored: the file name is not a valid namespace"});
                            continue;
                        }
                        if (++count > MAX_FILES_PER_LOCALE)
                        {
                            errors.push_back(
                                {label,
                                 0,
                                 {},
                                 0,
                                 std::format("ignored: more than {} files in a locale", MAX_FILES_PER_LOCALE)});
                            complete = false;
                            return false;
                        }
                        const auto time = file->last_write_time(fileError);
                        const u64 size = file->file_size(fileError);
                        if (fileError)
                        {
                            errors.push_back({label, 0, {}, 0, "cannot read file attributes"});
                            complete = false;
                            continue;
                        }
                        files.push_back({joinId(locale, tableNamespace), locale, tableNamespace, label, path, time,
                                         size, !group.empty()});
                    }
                    complete = complete && !listError;
                    return true;
                };
                list(list, directory, {});
            }
            // the label breaks the tie between two nested files of one table (a/b-c.json, a-b/c.json): the
            // directory order must not pick the winner
            std::ranges::sort(files, [](const FileInfo& a, const FileInfo& b)
                              { return std::tie(a.id, a.nested, a.label) < std::tie(b.id, b.nested, b.label); });
            // a flat file and a nested one naming the same table: the flat one wins, else the first label
            for (std::size_t i = 1; i < files.size();)
            {
                if (files[i].id != files[i - 1].id)
                {
                    ++i;
                    continue;
                }
                errors.push_back({files[i].label, 0, {}, 0, "ignored: the table is also " + files[i - 1].label});
                files.erase(files.begin() + static_cast<std::ptrdiff_t>(i));
            }
            return complete && !ec;
        }

        void fail(const std::string& id, LocFileError error, LocReloadReport& report)
        {
            SWARNING_LOG("localization: {}; keeping the previous table", error.text());
            failed[id] = error;
            report.errors.push_back(std::move(error));
        }

        void succeed(const Pending& pending, LocReloadReport& report)
        {
            known[pending.file.id].applied = true;
            failed.erase(pending.file.id);
            ++report.applied;
        }

        static void withLine(const LocalizationError& error, const Messages& messages, LocFileError& out,
                             const std::string& file)
        {
            out = {file, 0, error.key, error.offset, error.reason};
            for (const Message& message : messages)
            {
                if (message.key == error.key)
                {
                    out.line = message.line;
                }
            }
        }

        // The pseudo-locale's tables from the default locale's messages.
        static std::map<std::string, Messages> pseudoTables(const std::map<std::string, Messages>& tables)
        {
            std::map<std::string, Messages> out;
            for (const auto& [name, messages] : tables)
            {
                Messages& pseudo = out[name];
                for (const Message& message : messages)
                {
                    std::string text = pseudoLocalize(message.text);
                    pseudo.push_back(
                        {message.key, validateMessage(text) ? message.text : std::move(text), message.line});
                }
            }
            return out;
        }

        struct Batch
        {
            struct Item
            {
                std::string locale;
                std::string tableNamespace;
                std::vector<LocalizedMessage> messages;
            };
            std::vector<Item> items;

            void add(const std::string_view locale, const std::string_view tableNamespace, const Messages& messages)
            {
                Item& item = items.emplace_back(std::string(locale), std::string(tableNamespace));
                for (const Message& message : messages)
                {
                    item.messages.push_back({message.key, message.text});
                }
            }

            std::expected<void, LocalizationError> apply(Localization& localization) const
            {
                std::vector<Localization::TableSource> sources;
                for (const Item& item : items)
                {
                    sources.push_back({item.locale, item.tableNamespace, item.messages});
                }
                return localization.setTables(sources);
            }
        };

        void applyPseudo(const std::map<std::string, Messages>& pseudo)
        {
            Batch batch;
            for (const auto& [name, messages] : pseudo)
            {
                batch.add(PSEUDO_LOCALE, name, messages);
            }
            if (!batch.items.empty())
            {
                if (auto applied = batch.apply(localization); !applied)
                {
                    SWARNING_LOG("localization: pseudo-locale '{}': {}", applied.error().key, applied.error().reason);
                }
            }
        }

        LocReloadReport reload(const bool force)
        {
            LocReloadReport report;
#if defined(_WIN32)
            armWatch();
#endif
            std::vector<FileInfo> files;
            const bool complete = scan(files, report.errors);
            for (const LocFileError& error : report.errors)
            {
                SWARNING_LOG("localization: {}", error.text());
            }

            std::set<std::string> present;
            std::vector<Pending> pending;
            for (FileInfo& file : files)
            {
                present.insert(file.id);
                Known& entry = known[file.id];
                if (!force && entry.time == file.time && entry.size == file.size)
                {
                    continue;
                }
                entry.time = file.time;
                entry.size = file.size; // a failure below is not retried until the file changes again
                if (file.size > MAX_FILE_BYTES)
                {
                    fail(file.id, {file.label, 0, {}, 0, std::format("larger than {} bytes", MAX_FILE_BYTES)}, report);
                    continue;
                }
                Messages messages;
                auto text = readTextFile(file.path);
                if (!text)
                {
                    fail(file.id, {file.label, 0, {}, 0, "cannot read the file"}, report);
                    continue;
                }
                if (text->size() > MAX_FILE_BYTES)
                {
                    fail(file.id, {file.label, 0, {}, 0, std::format("larger than {} bytes", MAX_FILE_BYTES)}, report);
                    continue;
                }
                if (auto error = parseTable(*text, file.label, messages))
                {
                    fail(file.id, std::move(*error), report);
                    continue;
                }
                pending.push_back({std::move(file), std::move(messages)});
            }

            bool defaultChanged = false;
            if (complete)
            {
                for (auto it = known.begin(); it != known.end();)
                {
                    if (present.contains(it->first))
                    {
                        ++it;
                        continue;
                    }
                    const std::size_t slash = it->first.find('/');
                    const std::string locale = it->first.substr(0, slash);
                    const std::string name = it->first.substr(slash + 1);
                    if (it->second.applied && localization.removeTable(locale, name))
                    {
                        ++report.removed;
                        if (locale == defaultLocale)
                        {
                            defaultTables.erase(name);
                            defaultChanged = true;
                            localization.removeTable(PSEUDO_LOCALE, name);
                        }
                    }
                    failed.erase(it->first);
                    it = known.erase(it);
                }
            }

            defaultChanged = defaultChanged ||
                std::ranges::any_of(pending, [&](const Pending& p) { return p.file.locale == defaultLocale; });
            if (pending.empty() && !defaultChanged)
            {
                return report;
            }

            std::map<std::string, Messages> pseudo;
            if (defaultChanged)
            {
                std::map<std::string, Messages> next = defaultTables;
                for (const Pending& p : pending)
                {
                    if (p.file.locale == defaultLocale)
                    {
                        next[p.file.tableNamespace] = p.messages;
                    }
                }
                pseudo = pseudoTables(next);
            }
            Batch batch;
            for (const Pending& p : pending)
            {
                batch.add(p.file.locale, p.file.tableNamespace, p.messages);
            }
            for (const auto& [name, messages] : pseudo)
            {
                batch.add(PSEUDO_LOCALE, name, messages);
            }
            if (!batch.items.empty() && batch.apply(localization))
            {
                for (Pending& p : pending)
                {
                    succeed(p, report);
                    if (p.file.locale == defaultLocale)
                    {
                        defaultTables[p.file.tableNamespace] = std::move(p.messages);
                    }
                }
                return report;
            }

            // some table is invalid: apply them one by one so only the offending files keep their previous table
            for (Pending& p : pending)
            {
                std::vector<LocalizedMessage> view;
                for (const Message& message : p.messages)
                {
                    view.push_back({message.key, message.text});
                }
                if (auto set = localization.setTable(p.file.locale, p.file.tableNamespace, view); !set)
                {
                    LocFileError error;
                    withLine(set.error(), p.messages, error, p.file.label);
                    fail(p.file.id, std::move(error), report);
                    continue;
                }
                succeed(p, report);
                if (p.file.locale == defaultLocale)
                {
                    defaultTables[p.file.tableNamespace] = std::move(p.messages);
                }
            }
            if (defaultChanged)
            {
                applyPseudo(pseudoTables(defaultTables));
            }
            return report;
        }

        void poll()
        {
#if defined(_WIN32)
            const auto now = std::chrono::steady_clock::now();
            if (now - lastPoll >= POLL_INTERVAL)
            {
                lastPoll = now;
                if (watch == INVALID_HANDLE_VALUE)
                {
                    // the root was missing at startup or was deleted: one directory check per interval until it is back
                    armWatch();
                    if (watch != INVALID_HANDLE_VALUE)
                    {
                        reload(false);
                    }
                }
                else if (WaitForSingleObject(watch, 0) == WAIT_OBJECT_0)
                {
                    changePending = true;
                    lastChange = now;
                    if (!FindNextChangeNotification(watch))
                    {
                        SWARNING_LOG("localization watch: '{}' is gone; watching again when it returns", root.string());
                        FindCloseChangeNotification(watch);
                        watch = INVALID_HANDLE_VALUE;
                    }
                }
            }
            if (changePending && now - lastChange >= SETTLE)
            {
                changePending = false;
                reload(false);
            }
#endif
            if (const u64 revision = localization.revision(); revision != published)
            {
                published = revision;
                const std::string locale = localization.locale();
                const auto snapshot = listeners; // a listener may subscribe or unsubscribe
                for (const auto& [id, listener] : snapshot)
                {
                    listener(revision, locale);
                }
            }
        }
    };

    LocalizationContent::LocalizationContent(Localization& localization, fs::path root) :
        m_impl(std::make_unique<Impl>(localization, std::move(root)))
    {
        m_impl->reload(true);
        m_impl->published = localization.revision();
    }

    LocalizationContent::~LocalizationContent() = default;

    void LocalizationContent::poll() { m_impl->poll(); }

    LocReloadReport LocalizationContent::reload(const bool force) { return m_impl->reload(force); }

    std::vector<LocFileError> LocalizationContent::errors() const
    {
        std::vector<LocFileError> out;
        for (const auto& entry : m_impl->failed)
        {
            out.push_back(entry.second);
        }
        return out;
    }

    const fs::path& LocalizationContent::root() const { return m_impl->root; }

    u64 LocalizationContent::subscribe(ChangeFn listener)
    {
        const u64 id = m_impl->nextListener++;
        m_impl->listeners.emplace_back(id, std::move(listener));
        return id;
    }

    void LocalizationContent::unsubscribe(const u64 id)
    {
        std::erase_if(m_impl->listeners, [id](const auto& entry) { return entry.first == id; });
    }

    std::string LocalizationContent::setMessage(const std::string_view locale, const std::string_view key,
                                                const std::optional<std::string_view> text)
    {
        const std::string canonical = canonicalLocale(locale);
        const std::size_t dot = key.find('.');
        if (canonical.empty())
        {
            return std::format("'{}' is not a locale id", locale);
        }
        if (dot == std::string_view::npos || !isValidTableNamespace(key.substr(0, dot)) ||
            !isValidMessageKey(key.substr(dot + 1)))
        {
            return std::format("'{}' is not a key: namespace.key with dot-separated [A-Za-z0-9_-] segments", key);
        }
        if (const auto invalid = text ? validateMessage(*text) : std::nullopt)
        {
            return std::format("invalid message at byte {}: {}", invalid->offset, invalid->reason);
        }
        const std::string tableNamespace(key.substr(0, dot));
        const std::string tableKey(key.substr(dot + 1));
        Impl& impl = *m_impl;
        const fs::path directory = impl.root / canonical;
        fs::path path = directory / (tableNamespace + ".json");
        std::string label = canonical + '/' + tableNamespace + ".json";
        std::error_code ec;
        if (fs::exists(directory, ec) && (isLink(directory) || !fs::is_directory(directory, ec)))
        {
            return "the locale directory is a link or not a directory";
        }
        // A group's own file, <locale>/<group>/<name>.json for the namespace <group>-<name>: the flat file wins when it
        // exists; else the nested file of this locale, written in place; else (a table this locale lacks) the nested
        // path when the default locale keeps the namespace nested or has the group directory.
        const auto nestedSplit = [&](const fs::path& localeDirectory, const bool orGroupDirectory)
        {
            for (std::size_t dash = tableNamespace.find('-'); dash != std::string::npos;
                 dash = tableNamespace.find('-', dash + 1))
            {
                const fs::path groupDirectory = localeDirectory / tableNamespace.substr(0, dash);
                if (!isLink(groupDirectory) &&
                    (fs::is_regular_file(groupDirectory / (tableNamespace.substr(dash + 1) + ".json"), ec) ||
                     (orGroupDirectory && fs::is_directory(groupDirectory, ec))))
                {
                    return dash;
                }
            }
            return std::string::npos;
        };
        std::size_t split = std::string::npos;
        if (!fs::exists(path, ec))
        {
            split = nestedSplit(directory, false);
            if (split == std::string::npos &&
                !fs::exists(impl.root / impl.defaultLocale / (tableNamespace + ".json"), ec))
            {
                split = nestedSplit(impl.root / impl.defaultLocale, true);
                const fs::path groupDirectory = directory / tableNamespace.substr(0, split);
                if (split != std::string::npos && fs::exists(groupDirectory, ec) &&
                    (isLink(groupDirectory) || !fs::is_directory(groupDirectory, ec)))
                {
                    split = std::string::npos;
                }
            }
        }
        if (split != std::string::npos)
        {
            const std::string group = tableNamespace.substr(0, split);
            path = directory / group / (tableNamespace.substr(split + 1) + ".json");
            label = canonical + '/' + group + '/' + path.filename().string();
        }
        const bool exists = fs::exists(path, ec);
        if (exists && (isLink(path) || !fs::is_regular_file(path, ec)))
        {
            return "the table file is a link or not a regular file";
        }

        if (exists)
        {
            // a case-variant name (Engine.JSON) is a file the scanner ignores: never rewrite it
            bool exact = false;
            for (fs::directory_iterator it(path.parent_path(), ec), end; !ec && it != end; it.increment(ec))
            {
                exact = exact || it->path().filename() == path.filename();
            }
            if (!exact)
            {
                return "a table file with a different letter case exists; rename it to " + path.filename().string();
            }
        }
        const auto stamp = [&](u64& size, fs::file_time_type& time)
        {
            std::error_code stampError;
            size = fs::file_size(path, stampError);
            time = fs::last_write_time(path, stampError);
        };
        u64 readSize = 0;
        fs::file_time_type readTime{};
        std::string previous;
        Messages messages;
        if (exists)
        {
            stamp(readSize, readTime);
            auto read = readTextFile(path);
            if (!read)
            {
                return "cannot read " + path.string();
            }
            previous = std::move(*read);
            if (auto error = parseTable(previous, label, messages))
            {
                return "the file does not load: " + error->text();
            }
        }
        const auto it = std::ranges::find(messages, tableKey, &Message::key);
        if (!text)
        {
            if (it == messages.end())
            {
                // the key lives only in memory (the derived pseudo-locale) or nowhere: there is nothing to rewrite
                return std::format("'{}' is not in the table file of '{}'", key, canonical);
            }
            messages.erase(it);
        }
        else if (it != messages.end())
        {
            it->text = *text;
        }
        else
        {
            messages.push_back({tableKey, std::string(*text), 0});
        }
        nlohmann::ordered_json object = nlohmann::ordered_json::object();
        for (const Message& message : messages)
        {
            object[message.key] = message.text;
        }
        const std::string content = object.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + '\n';
        if (content.size() > MAX_FILE_BYTES)
        {
            return std::format("the table would exceed {} bytes", MAX_FILE_BYTES);
        }
        u64 nowSize = 0;
        fs::file_time_type nowTime{};
        if (exists)
        {
            stamp(nowSize, nowTime);
        }
        if (fs::exists(path, ec) != exists || nowSize != readSize || nowTime != readTime)
        {
            return "the file changed on disk while editing; retry";
        }
        if (!ensureDirectory(directory) || !writeTextFileAtomically(path, content))
        {
            return "cannot write " + path.string();
        }
        const LocReloadReport report = impl.reload(false);
        for (const LocFileError& error : report.errors)
        {
            if (error.file == label)
            {
                if (exists)
                {
                    (void)writeTextFileAtomically(path, previous);
                }
                else
                {
                    fs::remove(path, ec);
                }
                impl.reload(false);
                impl.failed.erase(joinId(canonical, tableNamespace));
                return error.text();
            }
        }
        return {};
    }
} // namespace spite
