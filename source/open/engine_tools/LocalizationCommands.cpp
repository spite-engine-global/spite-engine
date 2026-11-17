#include "engine/LocalizationCommands.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/StringInterner.hpp"
#include "control/ControlServer.hpp"
#include "engine/LocalizationContent.hpp"
#include "localization/Locale.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"

namespace spite
{
    namespace
    {
        constexpr std::string_view OWNER = "engine";
        constexpr u64 MAX_LIMIT = 5000;
        constexpr u64 DEFAULT_LIMIT = 200;

        const ArgDesc LOCALE_ARG =
            optionalArg("locale", ArgKind::String, "a locale id, e.g. de-AT", "the current locale");

        void writeError(IFormatWriter& out, const LocFileError& error)
        {
            out.beginObject("");
            out.writeString("file", error.file);
            out.writeU32("line", error.line);
            out.writeString("key", error.key);
            out.writeU32("offset", error.offset);
            out.writeString("reason", error.reason);
            out.endObject();
        }

        void writeErrors(IFormatWriter& out, const std::vector<LocFileError>& errors)
        {
            out.beginArray("errors", static_cast<u32>(errors.size()));
            for (const LocFileError& error : errors)
            {
                writeError(out, error);
            }
            out.endArray();
        }

        void writeStrings(IFormatWriter& out, const std::string_view name, const std::vector<std::string>& values)
        {
            out.beginArray(name, static_cast<u32>(values.size()));
            for (const std::string& value : values)
            {
                out.writeString("", value);
            }
            out.endArray();
        }

        // the locale a request names, canonical; empty plus an error when malformed
        std::string requestLocale(const Localization& localization, const ControlArgs& args, std::string& error)
        {
            const std::optional<std::string> named = args.string("locale");
            if (!named)
            {
                return localization.locale();
            }
            std::string canonical = canonicalLocale(*named);
            if (canonical.empty())
            {
                error = std::format("'{}' is not a locale id (BCP 47, e.g. en, de-AT)", *named);
            }
            return canonical;
        }

        struct Row
        {
            std::string key;
            std::string tableNamespace;
            std::string text;
        };

        std::vector<Row> rowsOf(const Localization& localization, const std::string& locale)
        {
            std::vector<Row> rows;
            localization.forEachMessage(
                locale,
                [&](const InternedString key, const std::string_view tableNamespace, const std::string_view text)
                { rows.push_back({std::string(key.toView()), std::string(tableNamespace), std::string(text)}); });
            std::ranges::sort(rows, {}, &Row::key);
            return rows;
        }

        // The text the locale's own table holds for key, when it defines it.
        std::optional<std::string> ownText(const Localization& localization, const std::string& locale,
                                           const std::string& key)
        {
            std::optional<std::string> found;
            const InternedString interned(key);
            localization.forEachMessage(locale,
                                        [&](const InternedString k, std::string_view, const std::string_view text)
                                        {
                                            if (k == interned)
                                            {
                                                found = std::string(text);
                                            }
                                        });
            return found;
        }

        // setLocString and unsetLocString: write (text) or remove the message, then record how to put it back.
        std::string editMessage(ControlServer& control, const Localization& localization, LocalizationContent& content,
                                const ControlArgs& args, const std::optional<std::string_view> text, IFormatWriter& out)
        {
            const std::string locale = canonicalLocale(args.string("locale").value_or(""));
            const std::string key = args.string("key").value_or("");
            const std::optional<std::string> previous =
                locale.empty() ? std::nullopt : ownText(localization, locale, key);
            if (!text && !previous && !locale.empty())
            {
                return std::format("locale '{}' does not define '{}'", locale, key);
            }
            if (std::string error =
                    content.setMessage(locale.empty() ? args.string("locale").value_or("") : locale, key, text);
                !error.empty())
            {
                return error;
            }
            if (previous)
            {
                control.recordInverse("setLocString",
                                      nlohmann::json{{"locale", locale}, {"key", key}, {"text", *previous}}.dump());
            }
            else
            {
                control.recordInverse("unsetLocString", nlohmann::json{{"locale", locale}, {"key", key}}.dump());
            }
            out.writeString("locale", locale);
            out.writeString("key", key);
            return std::string{};
        }
    } // namespace

    void registerLocalizationCommands(ControlServer& control, Localization& localization, LocalizationContent& content)
    {
        control.registerQuery(
            "locales", OWNER,
            "the localization state: current and default locale, fallback chain, revision (the locale-changed "
            "counter), "
            "missing-key lookups, per loaded locale its tables and message counts, and the files that failed to load; "
            "en-XA is the pseudo-locale derived from the default locale",
            {},
            [&localization, &content](const ControlArgs&, IFormatWriter& out)
            {
                out.writeString("locale", localization.locale());
                out.writeString("defaultLocale", localization.defaultLocale());
                writeStrings(out, "fallbackChain", localization.fallbackChain());
                out.writeU64("revision", localization.revision());
                out.writeU64("missingLookups", localization.missingLookups());
                out.writeString("root", content.root().generic_string());
                const std::vector<std::string> locales = localization.locales();
                out.beginArray("locales", static_cast<u32>(locales.size()));
                for (const std::string& locale : locales)
                {
                    std::map<std::string, u32> tables;
                    u32 total = 0;
                    localization.forEachMessage(
                        locale,
                        [&](InternedString, const std::string_view tableNamespace, std::string_view)
                        {
                            ++tables[std::string(tableNamespace)];
                            ++total;
                        });
                    out.beginObject("");
                    out.writeString("locale", locale);
                    out.writeU32("messages", total);
                    out.beginArray("tables", static_cast<u32>(tables.size()));
                    for (const auto& [name, count] : tables)
                    {
                        out.beginObject("");
                        out.writeString("namespace", name);
                        out.writeU32("messages", count);
                        out.endObject();
                    }
                    out.endArray();
                    out.endObject();
                }
                out.endArray();
                writeErrors(out, content.errors());
                return std::string{};
            });

        control.registerViewCommand(
            "setLocale", OWNER,
            "switches the locale: every table's merged view is rebuilt and the revision bumps once, so every consumer "
            "re-resolves in that frame; a malformed id is refused",
            {requiredArg("locale", ArgKind::String, "a locale id, e.g. de-AT, or en-XA for the pseudo-locale")},
            [&localization](const ControlArgs& args, IFormatWriter& out)
            {
                const std::string requested = args.string("locale").value_or("");
                if (!localization.setLocale(requested))
                {
                    std::string known;
                    for (const std::string& locale : localization.locales())
                    {
                        known += (known.empty() ? "" : ", ") + locale;
                    }
                    return std::format("'{}' is not a locale id (BCP 47, e.g. en, de-AT); loaded locales: {}",
                                       requested, known.empty() ? "none" : known);
                }
                const std::vector<std::string> chain = localization.fallbackChain();
                const std::vector<std::string> loaded = localization.locales();
                out.writeString("locale", localization.locale());
                out.writeBool("loaded",
                              std::ranges::any_of(chain, [&](const std::string& locale)
                                                  { return std::ranges::find(loaded, locale) != loaded.end(); }));
                out.writeU64("revision", localization.revision());
                writeStrings(out, "fallbackChain", chain);
                return std::string{};
            });

        control.suggestArgValues("setLocale", "locale", [&localization] { return localization.locales(); });

        control.registerQuery(
            "locStrings", OWNER,
            "the messages of one locale's own tables (no fallback), sorted by key: key, namespace and message source",
            {LOCALE_ARG, optionalArg("namespace", ArgKind::String, "only this namespace", "all"),
             optionalArg("prefix", ArgKind::String, "only keys starting with this text", "all"),
             optionalArg("limit", ArgKind::Integer, "at most this many messages (max 5000)", "200"),
             optionalArg("offset", ArgKind::Integer, "skip this many first", "0")},
            [&localization](const ControlArgs& args, IFormatWriter& out)
            {
                std::string error;
                const std::string locale = requestLocale(localization, args, error);
                if (!error.empty())
                {
                    return error;
                }
                const std::string tableNamespace = args.string("namespace").value_or("");
                const std::string prefix = args.string("prefix").value_or("");
                const u64 limit = std::min(args.integer("limit").value_or(DEFAULT_LIMIT), MAX_LIMIT);
                const u64 offset = args.integer("offset").value_or(0);
                std::vector<Row> rows = rowsOf(localization, locale);
                std::erase_if(rows,
                              [&](const Row& row)
                              {
                                  return (!tableNamespace.empty() && row.tableNamespace != tableNamespace) ||
                                      !row.key.starts_with(prefix);
                              });
                const u64 first = std::min<u64>(offset, rows.size());
                const u64 count = std::min<u64>(limit, rows.size() - first);
                out.writeString("locale", locale);
                out.writeU64("total", rows.size());
                out.beginArray("messages", static_cast<u32>(count));
                for (u64 i = first; i < first + count; ++i)
                {
                    out.beginObject("");
                    out.writeString("key", rows[i].key);
                    out.writeString("namespace", rows[i].tableNamespace);
                    out.writeString("text", rows[i].text);
                    out.endObject();
                }
                out.endArray();
                return std::string{};
            });

        control.registerQuery(
            "locMissing", OWNER,
            "translation gaps per loaded locale (the pseudo-locale is excluded): the keys another locale defines that "
            "this locale's own tables do not (up to 500 per locale), plus the keys lookups asked for that no table "
            "answers",
            {optionalArg("locale", ArgKind::String, "only this locale", "every loaded locale")},
            [&localization](const ControlArgs& args, IFormatWriter& out)
            {
                std::vector<std::string> locales = localization.locales();
                std::erase(locales, std::string(PSEUDO_LOCALE));
                std::vector<std::string> shown = locales;
                if (args.string("locale"))
                {
                    std::string error;
                    const std::string locale = requestLocale(localization, args, error);
                    if (!error.empty())
                    {
                        return error;
                    }
                    shown = {locale};
                }
                std::unordered_set<InternedString> all;
                std::vector<std::unordered_set<InternedString>> own(locales.size());
                for (sizet i = 0; i < locales.size(); ++i)
                {
                    localization.forEachMessage(locales[i],
                                                [&](const InternedString key, std::string_view, std::string_view)
                                                {
                                                    own[i].insert(key);
                                                    all.insert(key);
                                                });
                }
                constexpr sizet LIST_CAP = 500;
                out.beginArray("locales", static_cast<u32>(shown.size()));
                for (const std::string& locale : shown)
                {
                    const auto at = std::ranges::find(locales, locale);
                    std::vector<std::string> missing;
                    for (const InternedString key : all)
                    {
                        if (at == locales.end() || !own[at - locales.begin()].contains(key))
                        {
                            missing.emplace_back(key.toView());
                        }
                    }
                    std::ranges::sort(missing);
                    const sizet count = missing.size();
                    missing.resize(std::min(count, LIST_CAP));
                    out.beginObject("");
                    out.writeString("locale", locale);
                    out.writeU64("count", count);
                    writeStrings(out, "keys", missing);
                    out.writeBool("truncated", count > missing.size());
                    out.endObject();
                }
                out.endArray();
                std::vector<std::string> lookups;
                for (const InternedString key : localization.missingKeys())
                {
                    lookups.emplace_back(key.toView());
                }
                std::ranges::sort(lookups);
                out.beginObject("lookups");
                out.writeU64("total", localization.missingLookups());
                out.writeU64("distinct", lookups.size());
                lookups.resize(std::min(lookups.size(), LIST_CAP));
                writeStrings(out, "keys", lookups);
                out.endObject();
                return std::string{};
            });

        control.registerQuery(
            "locResolve", OWNER,
            "the active locale's message for a key, formatted with args; found is false for a key no table answers "
            "(the key itself is returned, and the lookup is not counted as missing)",
            {requiredArg("key", ArgKind::String, "namespace.key"),
             optionalArg("args", ArgKind::Json, "a JSON object of argument names to integers, numbers or strings",
                         "none")},
            [&localization](const ControlArgs& args, IFormatWriter& out)
            {
                const std::string key = args.string("key").value_or("");
                if (!isValidMessageKey(key))
                {
                    return std::format("'{}' is not a key", key);
                }
                struct Named
                {
                    std::string name;
                    std::variant<std::int64_t, double, std::string> value;
                };
                std::vector<Named> named;
                if (const std::optional<std::string> argsJson = args.json("args"))
                {
                    const nlohmann::json parsed = nlohmann::json::parse(*argsJson, nullptr, false);
                    if (!parsed.is_object())
                    {
                        return std::string("args must be a JSON object of names to integers, numbers or strings");
                    }
                    for (const auto& [name, value] : parsed.items())
                    {
                        if (value.is_number_integer() && (!value.is_number_unsigned() || value.get<u64>() <= INT64_MAX))
                        {
                            named.push_back({name, value.get<std::int64_t>()});
                        }
                        else if (value.is_number())
                        {
                            named.push_back({name, value.get<double>()});
                        }
                        else if (value.is_string())
                        {
                            named.push_back({name, value.get<std::string>()});
                        }
                        else
                        {
                            return std::format("argument '{}' must be an integer, a number or a string", name);
                        }
                    }
                }
                std::vector<MessageArg> messageArgs;
                for (const Named& arg : named)
                {
                    MessageArg& added = messageArgs.emplace_back();
                    added.name = arg.name;
                    if (const auto* integer = std::get_if<std::int64_t>(&arg.value))
                    {
                        added.value = *integer;
                    }
                    else if (const auto* number = std::get_if<double>(&arg.value))
                    {
                        added.value = *number;
                    }
                    else
                    {
                        added.value = std::string_view(std::get<std::string>(arg.value));
                    }
                }
                const InternedString interned(key);
                const bool found = localization.contains(interned);
                std::string scratch;
                out.writeString("key", key);
                out.writeBool("found", found);
                out.writeString("text", found ? localization.resolve(interned, messageArgs, scratch) : key);
                return std::string{};
            });

        control.registerViewCommand(
            "locReload", OWNER,
            "re-reads the string tables under assets/loc now (they are also watched and reload on save); an invalid "
            "file keeps its previous table and is reported with its line and key",
            {optionalArg("force", ArgKind::Boolean, "re-read files whose timestamp and size did not change", "false")},
            [&content](const ControlArgs& args, IFormatWriter& out)
            {
                const LocReloadReport report = content.reload(args.boolean("force").value_or(false));
                out.writeU32("applied", report.applied);
                out.writeU32("removed", report.removed);
                writeErrors(out, report.errors);
                return std::string{};
            });

        // inverse: setLocString with the message the locale's own table held, else unsetLocString
        control.registerUndoableViewCommand(
            "setLocString", OWNER,
            "writes one message into assets/loc/<locale>/<namespace>.json (the key's first segment is the namespace; "
            "the file is created) and reloads it; a namespace <group>-<name> goes to <locale>/<group>/<name>.json when "
            "that file exists or the default locale keeps the namespace in a <group> directory (a flat file wins); "
            "an invalid message or key is refused and the file left as it was; undo writes the previous text back "
            "(or removes the message)",
            {requiredArg("locale", ArgKind::String, "the locale to edit, e.g. de"),
             requiredArg("key", ArgKind::String, "namespace.key"),
             requiredArg("text", ArgKind::String, "the message, ICU subset: {name}, {n, plural, one {..} other {..}}")},
            [&control, &localization, &content](const ControlArgs& args, IFormatWriter& out)
            { return editMessage(control, localization, content, args, args.string("text").value_or(""), out); });
        control.registerUndoableViewCommand(
            "unsetLocString", OWNER,
            "removes one message from the locale's own table file (the key then resolves through the fallback chain "
            "and shows as missing); a key the locale does not define is refused",
            {requiredArg("locale", ArgKind::String, "the locale to edit, e.g. de"),
             requiredArg("key", ArgKind::String, "namespace.key")},
            [&control, &localization, &content](const ControlArgs& args, IFormatWriter& out)
            { return editMessage(control, localization, content, args, std::nullopt, out); });
    }
} // namespace spite
