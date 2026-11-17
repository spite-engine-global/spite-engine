#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "base/FunctionRef.hpp"
#include "base/InternedString.hpp"
#include "base/Platform.hpp"
#include "localization/LocalizationExport.hpp"

namespace spite
{
    // A named message argument; numbers format with the message language's separators.
    struct MessageArg
    {
        std::string_view name;
        std::variant<std::int64_t, double, std::string_view> value;
    };

    // One entry of a string table: key (without the namespace) and its message source.
    struct LocalizedMessage
    {
        std::string_view key;
        std::string_view text;
    };

    struct LocalizationError
    {
        std::string key; // the table key the error is in; empty for a single message
        u32 offset = 0; // byte offset into the message source
        std::string reason;
    };

    // Limits setTable and validateMessage enforce (error "message too long" / "nesting too deep" at a byte offset):
    // a message source in bytes, plural nesting depth, and a table key.
    constexpr sizet MAX_MESSAGE_BYTES = 64 * 1024;
    constexpr u32 MAX_MESSAGE_NESTING = 16;
    constexpr sizet MAX_KEY_BYTES = 256;

    // Messages are an ICU MessageFormat subset: {name} (numbers locale-formatted), {name, number},
    // {name, plural, =3 {...} zero|one|two|few|many {...} other {...}} with # for the plural number, and ICU
    // apostrophe quoting ('' is a quote, '{' starts a quoted literal). Source must be valid UTF-8.
    [[nodiscard]] SPITE_LOCALIZATION_API std::optional<LocalizationError> validateMessage(std::string_view message);

    // The checks setTable applies: a namespace is a file name (loc/<locale>/<namespace>.json), a key the table key.
    [[nodiscard]] SPITE_LOCALIZATION_API bool isValidTableNamespace(std::string_view tableNamespace);
    [[nodiscard]] SPITE_LOCALIZATION_API bool isValidMessageKey(std::string_view key);

    // The pseudo-locale (en-XA): every message of the default locale accented and lengthened by 30 percent, wrapped in
    // brackets, so truncation and hard-coded text show at a glance. Arguments, plural syntax and quoting are kept, so
    // the result is a valid message in the same language; a result past MAX_MESSAGE_BYTES (or a message that does not
    // parse) is returned unchanged.
    inline constexpr std::string_view PSEUDO_LOCALE = "en-XA";
    [[nodiscard]] SPITE_LOCALIZATION_API std::string pseudoLocalize(std::string_view message);

    // Compiles and formats one message (tools and tests; tables compile once in setTable). Appends to out.
    SPITE_LOCALIZATION_API std::expected<void, LocalizationError> formatMessage(std::string_view locale,
                                                                                std::string_view message,
                                                                                std::span<const MessageArg> args,
                                                                                std::string& out);

    // The locale, the string tables per locale and namespace, and lookup with fallback. Lookups are
    // thread-safe and may run on any thread; setLocale, setTable and removeTable are the only writers and belong at
    // the frame point. A table key "menu.file" in namespace "editor" is looked up as "editor.menu.file".
    //
    // View lifetime contract: a view resolve returns for a message without arguments points into the table and
    // outlives the shared lock; it stays valid until the table that supplied it is replaced or removed, or the
    // fallback chain changes (setLocale). Writers therefore run only at the frame point, on the thread that holds
    // resolved views (a hot reload or file watcher queues the table and applies it there); a view of a different
    // table survives a setTable. Formatted results live in the caller's scratch and are never invalidated.
    class SPITE_LOCALIZATION_API Localization
    {
    public:
        static constexpr const char* SPITE_SERVICE_NAME = "localization";

        // defaultLocale ends every fallback chain and is the initial locale; a malformed id falls back to "en".
        explicit Localization(std::string_view defaultLocale = "en");
        ~Localization();
        Localization(const Localization&) = delete;
        Localization& operator=(const Localization&) = delete;

        // Replaces the (locale, namespace) table, all or nothing: on an invalid or oversized message, an invalid
        // or duplicate key, a malformed locale or an invalid namespace the previous table stays and the error names
        // the key and byte offset. A namespace is [A-Za-z0-9_-]+ and not a Windows device name (con, nul, prn, aux,
        // com1-9, lpt1-9; it becomes a file name). A key is at most MAX_KEY_BYTES of dot-separated non-empty
        // segments over [A-Za-z0-9_-]. The replaced table's resolved views end here. Bumps revision().
        std::expected<void, LocalizationError> setTable(std::string_view locale, std::string_view tableNamespace,
                                                        std::span<const LocalizedMessage> messages);
        // Several tables in one step (all or nothing, one rebuild, one revision bump): loading N namespaces with
        // setTable rebuilds the merged map N times and blocks readers each time.
        struct TableSource
        {
            std::string_view locale;
            std::string_view tableNamespace;
            std::span<const LocalizedMessage> messages;
        };
        std::expected<void, LocalizationError> setTables(std::span<const TableSource> sources);
        // false when there was no such table; bumps revision() otherwise
        bool removeTable(std::string_view locale, std::string_view tableNamespace);

        // false (and nothing changes) for a malformed id; bumps revision() only when the canonical locale changes
        bool setLocale(std::string_view locale);
        [[nodiscard]] std::string locale() const;
        [[nodiscard]] std::string defaultLocale() const;
        [[nodiscard]] std::vector<std::string> fallbackChain() const;

        // Increases once per change of the locale or any table: the locale-changed signal consumers compare
        // against the value they resolved at (hot-reload and the application event bridge build on it).
        [[nodiscard]] u64 revision() const noexcept;

        // The message for key in the nearest locale of the fallback chain, formatted with args. A message without
        // arguments returns a view of the table's text (no allocation, scratch untouched) valid until its table is
        // replaced or removed; otherwise the result is written to scratch (cleared first) and the view is scratch.
        // A missing key returns the key's text, counts the miss and logs the key once.
        [[nodiscard]] std::string_view resolve(InternedString key, std::span<const MessageArg> args,
                                               std::string& scratch) const;
        [[nodiscard]] std::string_view resolve(InternedString key, std::string& scratch) const
        {
            return resolve(key, {}, scratch);
        }
        [[nodiscard]] bool contains(InternedString key) const;

        // every lookup of a missing key, and the distinct keys missed that are still missing
        [[nodiscard]] u64 missingLookups() const noexcept;
        [[nodiscard]] std::vector<InternedString> missingKeys() const;

        // Loaded locales, and every message of one locale's tables (full key, namespace, message source). The visitor
        // runs under the shared lock: it must not call setTable/removeTable/setLocale (deadlock) nor resolve (can
        // deadlock against a pending writer).
        [[nodiscard]] std::vector<std::string> locales() const;
        void forEachMessage(
            std::string_view locale,
            function_ref<void(InternedString key, std::string_view tableNamespace, std::string_view text)> visit) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace spite
