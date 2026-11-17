#include "localization/Localization.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <map>
#include <mutex>
#include <ranges>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "base/Logging.hpp"
#include "base/StringInterner.hpp"
#include "LocaleRules.hpp"
#include "Message.hpp"

namespace spite
{
    namespace
    {
        struct Entry
        {
            InternedString key; // namespace.key
            std::string source;
            loc::CompiledMessage message;
        };

        struct Table
        {
            const loc::LocaleRules* rules = nullptr; // the table's language: plurals and numbers follow the text
            std::vector<Entry> entries;
        };

        struct ActiveMessage
        {
            const loc::CompiledMessage* message = nullptr;
            const loc::LocaleRules* rules = nullptr;
        };

        bool isSegmentChar(const char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        }

        bool reservedDeviceName(std::string_view name)
        {
            std::string lower;
            for (const char c : name)
                lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
            if (lower == "con" || lower == "nul" || lower == "prn" || lower == "aux")
                return true;
            return lower.size() == 4 && (lower.starts_with("com") || lower.starts_with("lpt")) && lower[3] >= '1' &&
                lower[3] <= '9';
        }

        bool validNamespace(const std::string_view name)
        {
            return !name.empty() && std::ranges::all_of(name, isSegmentChar) && !reservedDeviceName(name);
        }

        // non-empty dot-separated segments over [A-Za-z0-9_-], at most MAX_KEY_BYTES
        bool validKey(const std::string_view key)
        {
            return !key.empty() && key.size() <= MAX_KEY_BYTES &&
                std::ranges::all_of(key | std::views::split('.'), [](auto segment)
                                    { return !segment.empty() && std::ranges::all_of(segment, isSegmentChar); });
        }

        std::string printable(const std::string_view text)
        {
            std::string out;
            for (const char c : text.substr(0, 64))
                out.push_back(static_cast<u8>(c) >= 0x20 && static_cast<u8>(c) < 0x7F ? c : '?');
            return out;
        }
    } // namespace

    struct Localization::Impl
    {
        mutable std::shared_mutex mutex; // guards everything below except the atomics and the missing set
        std::string defaultLocale;
        std::string locale;
        std::vector<std::string> chain;
        std::map<std::string, std::map<std::string, Table, std::less<>>, std::less<>> tables; // locale -> namespace
        std::unordered_map<InternedString, ActiveMessage> active; // the merged fallback chain
        std::atomic<u64> revision{0};

        mutable std::mutex missingMutex;
        mutable std::unordered_set<InternedString> missing;
        mutable std::atomic<u64> missingLookups{0};

        // caller holds mutex exclusively; nearer locales are applied last and win
        void rebuildActive()
        {
            active.clear();
            for (auto locale = chain.rbegin(); locale != chain.rend(); ++locale)
            {
                const auto found = tables.find(*locale);
                if (found == tables.end())
                    continue;
                for (const Table& table : found->second | std::views::values)
                {
                    for (const Entry& entry : table.entries)
                    {
                        active.insert_or_assign(entry.key, ActiveMessage{&entry.message, table.rules});
                    }
                }
            }
        }

        [[nodiscard]] bool inChain(const std::string_view id) const
        {
            return std::ranges::find(chain, id) != chain.end();
        }
    };

    Localization::Localization(const std::string_view defaultLocale) : m_impl(std::make_unique<Impl>())
    {
        m_impl->defaultLocale = canonicalLocale(defaultLocale);
        if (m_impl->defaultLocale.empty())
        {
            m_impl->defaultLocale = "en";
        }
        m_impl->locale = m_impl->defaultLocale;
        m_impl->chain = localeFallbackChain(m_impl->locale, m_impl->defaultLocale);
    }

    Localization::~Localization() = default;

    namespace
    {
        struct BuiltTable
        {
            std::string id;
            std::string tableNamespace;
            Table table;
        };

        std::expected<BuiltTable, LocalizationError> buildTable(const std::string_view locale,
                                                                const std::string_view tableNamespace,
                                                                const std::span<const LocalizedMessage> messages)
        {
            std::string id = canonicalLocale(locale);
            if (id.empty())
            {
                return std::unexpected(
                    LocalizationError{{}, 0, std::format("malformed locale '{}'", printable(locale))});
            }
            if (!validNamespace(tableNamespace))
            {
                return std::unexpected(
                    LocalizationError{{},
                                      0,
                                      std::format("namespace '{}' is not [A-Za-z0-9_-]+ or is a reserved device name",
                                                  printable(tableNamespace))});
            }
            Table table{&loc::localeRules(id), {}};
            table.entries.reserve(messages.size());
            std::unordered_set<std::string_view> keys;
            std::string fullKey;
            for (const LocalizedMessage& message : messages)
            {
                if (!validKey(message.key))
                {
                    return std::unexpected(LocalizationError{
                        printable(message.key), 0,
                        std::format(
                            "invalid key '{}': non-empty dot-separated [A-Za-z0-9_-] segments, at most {} bytes",
                            printable(message.key), MAX_KEY_BYTES)});
                }
                if (!keys.insert(message.key).second)
                {
                    return std::unexpected(LocalizationError{std::string(message.key), 0, "duplicate key"});
                }
                auto compiled = loc::compileMessage(message.text);
                if (!compiled)
                {
                    compiled.error().key = message.key;
                    return std::unexpected(std::move(compiled.error()));
                }
                fullKey.assign(tableNamespace).append(".").append(message.key);
                table.entries.push_back({InternedString(fullKey), std::string(message.text), std::move(*compiled)});
            }
            return BuiltTable{std::move(id), std::string(tableNamespace), std::move(table)};
        }
    } // namespace

    std::expected<void, LocalizationError> Localization::setTable(const std::string_view locale,
                                                                  const std::string_view tableNamespace,
                                                                  const std::span<const LocalizedMessage> messages)
    {
        const TableSource source{locale, tableNamespace, messages};
        return setTables(std::span(&source, 1));
    }

    std::expected<void, LocalizationError> Localization::setTables(const std::span<const TableSource> sources)
    {
        std::vector<BuiltTable> built;
        built.reserve(sources.size());
        for (const TableSource& source : sources)
        {
            auto table = buildTable(source.locale, source.tableNamespace, source.messages);
            if (!table)
                return std::unexpected(std::move(table.error()));
            built.push_back(std::move(*table));
        }
        if (built.empty())
            return {};

        std::unique_lock lock(m_impl->mutex);
        bool active = false;
        for (BuiltTable& entry : built)
        {
            active |= m_impl->inChain(entry.id);
            m_impl->tables[std::move(entry.id)].insert_or_assign(std::move(entry.tableNamespace),
                                                                 std::move(entry.table));
        }
        if (active)
        {
            m_impl->rebuildActive();
        }
        m_impl->revision.fetch_add(1, std::memory_order_release);
        return {};
    }

    bool Localization::removeTable(const std::string_view locale, const std::string_view tableNamespace)
    {
        const std::string id = canonicalLocale(locale);
        std::unique_lock lock(m_impl->mutex);
        const auto found = m_impl->tables.find(id);
        if (found == m_impl->tables.end())
            return false;
        const auto table = found->second.find(tableNamespace);
        if (table == found->second.end())
            return false;
        found->second.erase(table);
        if (found->second.empty())
        {
            m_impl->tables.erase(found);
        }
        if (m_impl->inChain(id))
        {
            m_impl->rebuildActive();
        }
        m_impl->revision.fetch_add(1, std::memory_order_release);
        return true;
    }

    bool Localization::setLocale(const std::string_view locale)
    {
        std::string id = canonicalLocale(locale);
        if (id.empty())
        {
            return false;
        }
        std::unique_lock lock(m_impl->mutex);
        if (id == m_impl->locale)
        {
            return true;
        }
        if (loc::isRootRules(loc::localeRules(id)) && LogService::isInitialized())
        {
            SWARNING_LOG("Localization: no plural rules for locale {}; its own messages select 'other'", id);
        }
        m_impl->chain = localeFallbackChain(id, m_impl->defaultLocale);
        m_impl->locale = std::move(id);
        m_impl->rebuildActive();
        m_impl->revision.fetch_add(1, std::memory_order_release);
        return true;
    }

    std::string Localization::locale() const
    {
        std::shared_lock lock(m_impl->mutex);
        return m_impl->locale;
    }

    std::string Localization::defaultLocale() const
    {
        std::shared_lock lock(m_impl->mutex);
        return m_impl->defaultLocale;
    }

    std::vector<std::string> Localization::fallbackChain() const
    {
        std::shared_lock lock(m_impl->mutex);
        return m_impl->chain;
    }

    u64 Localization::revision() const noexcept { return m_impl->revision.load(std::memory_order_acquire); }

    std::string_view Localization::resolve(const InternedString key, const std::span<const MessageArg> args,
                                           std::string& scratch) const
    {
        {
            std::shared_lock lock(m_impl->mutex);
            const auto found = m_impl->active.find(key);
            if (found != m_impl->active.end())
            {
                const auto [message, rules] = found->second;
                if (message->isLiteral())
                {
                    return message->nodeCount == 0 ? std::string_view{}
                                                   : std::string_view(message->text)
                                                         .substr(message->nodes[message->firstNode].begin,
                                                                 message->nodes[message->firstNode].size);
                }
                scratch.clear();
                loc::formatMessage(*message, *rules, args, scratch);
                return scratch;
            }
        }
        m_impl->missingLookups.fetch_add(1, std::memory_order_relaxed);
        const std::string_view text = key.toView();
        {
            std::lock_guard lock(m_impl->missingMutex);
            if (m_impl->missing.insert(key).second && LogService::isInitialized())
            {
                SWARNING_LOG("Localization: missing key '{}'", text);
            }
        }
        return text;
    }

    bool Localization::contains(const InternedString key) const
    {
        std::shared_lock lock(m_impl->mutex);
        return m_impl->active.contains(key);
    }

    u64 Localization::missingLookups() const noexcept { return m_impl->missingLookups.load(std::memory_order_relaxed); }

    std::vector<InternedString> Localization::missingKeys() const
    {
        std::shared_lock lock(m_impl->mutex);
        std::lock_guard missingLock(m_impl->missingMutex);
        std::vector<InternedString> keys;
        for (const InternedString key : m_impl->missing)
        {
            if (!m_impl->active.contains(key))
                keys.push_back(key);
        }
        return keys;
    }

    std::vector<std::string> Localization::locales() const
    {
        std::shared_lock lock(m_impl->mutex);
        std::vector<std::string> ids;
        for (const std::string& id : m_impl->tables | std::views::keys)
        {
            ids.push_back(id);
        }
        return ids;
    }

    void Localization::forEachMessage(
        const std::string_view locale,
        const function_ref<void(InternedString key, std::string_view tableNamespace, std::string_view text)> visit)
        const
    {
        const std::string id = canonicalLocale(locale);
        std::shared_lock lock(m_impl->mutex);
        const auto found = m_impl->tables.find(id);
        if (found == m_impl->tables.end())
            return;
        for (const auto& [name, table] : found->second)
        {
            for (const Entry& entry : table.entries)
            {
                visit(entry.key, name, entry.source);
            }
        }
    }

    bool isValidTableNamespace(const std::string_view tableNamespace) { return validNamespace(tableNamespace); }

    bool isValidMessageKey(const std::string_view key) { return validKey(key); }
} // namespace spite
