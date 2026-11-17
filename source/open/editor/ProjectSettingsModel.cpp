#include "ProjectSettingsModel.hpp"
#include "EditorEntityLabel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include "control/ControlServer.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"

namespace spite::projectsettings
{
    namespace
    {
        std::string text(const Json& entry, const char* key)
        {
            const auto it = entry.find(key);
            return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string{};
        }

        Json member(const Json& entry, const char* key)
        {
            const auto it = entry.find(key);
            return it == entry.end() ? Json() : *it;
        }

        Row parseRow(const Json& entry)
        {
            Row row;
            row.path = text(entry, "path");
            row.group = text(entry, "group");
            row.leaf = row.group.empty() ? row.path : row.path.substr(row.group.size() + 1);
            row.kind = text(entry, "kind");
            row.type = text(entry, "type");
            row.doc = text(entry, "doc");
            row.unit = text(entry, "unit");
            row.apply = text(entry, "apply");
            row.source = text(entry, "source");
            row.value = member(entry, "value");
            row.running = member(entry, "running");
            row.def = member(entry, "default");
            row.saved = member(entry, "saved");
            row.min = entry.value("min", -INF);
            row.max = entry.value("max", INF);
            row.readOnly = entry.value("readOnly", false);
            for (const Json& choice : entry.value("choices", Json::array()))
            {
                row.choices.push_back(text(choice, "name"));
            }
            return row;
        }

        bool isInteger(const std::string_view type)
        {
            return type == "u8" || type == "u16" || type == "u32" || type == "u64" || type == "i32";
        }

        // The limits the type alone puts on a number, used where the row declares none.
        std::pair<double, double> typeLimits(const std::string_view type)
        {
            if (type == "u8")
            {
                return {0.0, 255.0};
            }
            if (type == "u16")
            {
                return {0.0, 65535.0};
            }
            if (type == "u32")
            {
                return {0.0, 4294967295.0};
            }
            if (type == "u64")
            {
                return {0.0, 1.8e19};
            }
            if (type == "i32")
            {
                return {-2147483648.0, 2147483647.0};
            }
            return {-3.0e38, 3.0e38};
        }

        UiFormKind formKind(const std::string_view kind)
        {
            static constexpr std::array<std::pair<std::string_view, UiFormKind>, 8> KINDS{
                {{"bool", UiFormKind::Bool},
                 {"enum", UiFormKind::Enum},
                 {"string", UiFormKind::String},
                 {"path", UiFormKind::Path},
                 {"vec2", UiFormKind::Vec2},
                 {"vec3", UiFormKind::Vec3},
                 {"vec4", UiFormKind::Vec4},
                 {"number", UiFormKind::Number}}};
            const auto it = std::ranges::find(KINDS, kind, &std::pair<std::string_view, UiFormKind>::first);
            return it == KINDS.end() ? UiFormKind::Number : it->second;
        }

        u32 componentCount(const UiFormKind kind)
        {
            return kind == UiFormKind::Vec2 ? 2 : kind == UiFormKind::Vec3 ? 3 : kind == UiFormKind::Vec4 ? 4 : 1;
        }

        bool containsIgnoringCase(const std::string_view text, const std::string_view part)
        {
            const auto lower = [](const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
            return part.empty() || std::ranges::search(text, part, {}, lower, lower).begin() != text.end();
        }

        u32 keyOf(const std::string_view section, const std::string_view path)
        {
            u32 hash = 2166136261u; // FNV-1a over section, a separator and path; 0 is no entity
            for (const std::string_view part : {section, std::string_view("|"), path})
            {
                for (const char c : part)
                {
                    hash = (hash ^ static_cast<u8>(c)) * 16777619u;
                }
            }
            return hash == 0 ? 1u : hash;
        }
    } // namespace

    Section parseSection(const Json& entry, const bool withValues)
    {
        Section section;
        section.id = text(entry, "id");
        section.kind = text(entry, "kind");
        section.doc = text(entry, "doc");
        section.layer = text(entry, "layer");
        section.file = text(entry, "file");
        section.reason = text(entry, "reason");
        section.dirty = entry.value("dirty", false);
        section.pending = entry.value("pending", false);
        if (withValues)
        {
            section.values = member(entry, "values");
        }
        for (const Json& field : entry.value("fields", Json::array()))
        {
            if (text(field, "kind") != "unsupported")
            {
                section.rows.push_back(parseRow(field));
            }
        }
        for (const Json& group : entry.value("groups", Json::array()))
        {
            section.groups.emplace_back(text(group, "path"), text(group, "doc"));
        }
        return section;
    }

    // What decides whether a page's forms still describe the section: the rows and their limits.
    std::string signatureOf(const Section& section)
    {
        std::string out = section.kind + "|" + section.doc + "|" + section.reason;
        for (const Row& row : section.rows)
        {
            out += std::format("|{}:{}:{}:{}:{}:{}:{}", row.path, row.kind, row.type, row.apply, row.readOnly, row.min,
                               row.max);
            for (const std::string& choice : row.choices)
            {
                out += "," + choice;
            }
        }
        for (const auto& [path, doc] : section.groups)
        {
            out += "|" + path + "=" + doc;
        }
        return out;
    }
    // A config value as the form holds it.
    UiFormValue toForm(const Row& row, const Json& value)
    {
        switch (formKind(row.kind))
        {
        case UiFormKind::Bool:
            return UiFormValue::ofBool(value.is_boolean() && value.get<bool>());
        case UiFormKind::Enum:
            {
                const auto at = std::ranges::find(row.choices, value.is_string() ? value.get<std::string>() : "");
                return UiFormValue::ofNumber(at == row.choices.end() ? 0.0
                                                                     : static_cast<double>(at - row.choices.begin()));
            }
        case UiFormKind::String:
        case UiFormKind::Path:
            return UiFormValue::ofText(value.is_string() ? value.get<std::string>() : std::string{});
        case UiFormKind::Vec2:
        case UiFormKind::Vec3:
        case UiFormKind::Vec4:
            {
                UiFormValue out;
                for (sizet i = 0; i < out.number.size() && value.is_array() && i < value.size(); ++i)
                {
                    out.number[i] = value[i].get<double>();
                }
                return out;
            }
        default:
            return UiFormValue::ofNumber(value.is_number() ? value.get<double>() : 0.0);
        }
    }

    // An edit as the JSON setConfig takes: an integer field an integer, an enum its choice name.
    Json toJson(const Row& row, const UiFormValue& value)
    {
        const UiFormKind kind = formKind(row.kind);
        if (kind == UiFormKind::Bool)
        {
            return value.number[0] != 0.0;
        }
        if (kind == UiFormKind::Enum)
        {
            const auto index = static_cast<sizet>(std::max(0.0, value.number[0]));
            return index < row.choices.size() ? row.choices[index] : std::string{};
        }
        if (kind == UiFormKind::String || kind == UiFormKind::Path)
        {
            return value.text;
        }
        if (componentCount(kind) > 1)
        {
            Json out = Json::array();
            for (u32 i = 0; i < componentCount(kind); ++i)
            {
                out.push_back(value.number[i]);
            }
            return out;
        }
        if (!isInteger(row.type))
        {
            return Json(value.number[0]);
        }
        // a u64 above 2^63 is sent unsigned: llround past it is undefined
        const double number = value.number[0];
        return number >= 1.8446744073709552e19 ? Json(std::numeric_limits<std::uint64_t>::max())
            : number >= 9.2233720368547758e18  ? Json(static_cast<std::uint64_t>(number))
                                               : Json(static_cast<std::int64_t>(std::llround(number)));
    }

    UiFormField fieldOf(const Row& row)
    {
        UiFormField field;
        field.id = InternedString(row.leaf);
        field.label = row.unit.empty() ? identifierWords(row.leaf) : identifierWords(row.leaf) + " (" + row.unit + ")";
        field.doc = row.doc;
        field.kind = formKind(row.kind);
        const auto [typeMin, typeMax] = typeLimits(row.type);
        field.min = std::isfinite(row.min) ? row.min : typeMin;
        field.max = std::isfinite(row.max) ? row.max : typeMax;
        field.decimals = isInteger(row.type) ? 0 : 6;
        field.step = isInteger(row.type)                       ? 1.0
            : std::isfinite(row.min) && std::isfinite(row.max) ? std::clamp((row.max - row.min) / 1000.0, 1.0e-4, 1.0)
                                                               : 0.1;
        for (const std::string& name : row.choices)
        {
            field.choices.push_back({.id = InternedString(name), .label = name});
        }
        field.defaultValue = toForm(row, row.def);
        field.readOnly = row.readOnly;
        field.restartRequired = row.apply == "startup";
        return field;
    }

    std::vector<std::string> groupPaths(const Section& section)
    {
        std::vector<std::string> order;
        if (std::ranges::any_of(section.rows, [](const Row& row) { return row.group.empty(); }))
        {
            order.emplace_back();
        }
        for (const Row& row : section.rows)
        {
            if (!std::ranges::contains(order, row.group))
            {
                order.push_back(row.group);
            }
        }
        return order;
    }

    std::string valueText(const Json& value)
    {
        if (value.is_number_float()) // a float field holds a widened float: seven digits are its own
        {
            return std::format("{:.7g}", value.get<double>());
        }
        return value.is_string() ? value.get<std::string>() : value.is_null() ? std::string("-") : value.dump();
    }

    void writeJson(IFormatWriter& out, const std::string_view name, const Json& value)
    {
        switch (value.type())
        {
        case Json::value_t::object:
            out.beginObject(name);
            for (const auto& [key, entry] : value.items())
            {
                writeJson(out, key, entry);
            }
            out.endObject();
            break;
        case Json::value_t::array:
            out.beginArray(name, static_cast<u32>(value.size()));
            for (const Json& entry : value)
            {
                writeJson(out, "", entry);
            }
            out.endArray();
            break;
        case Json::value_t::string:
            out.writeString(name, value.get<std::string>());
            break;
        case Json::value_t::boolean:
            out.writeBool(name, value.get<bool>());
            break;
        case Json::value_t::number_unsigned:
        case Json::value_t::number_integer:
        case Json::value_t::number_float:
            out.writeF64(name, value.get<double>());
            break;
        default:
            out.writeNull(name);
            break;
        }
    }

    std::string_view stemOf(const std::string_view sectionId)
    {
        const auto it = std::ranges::find(KNOWN_PAGES, sectionId, &KnownPage::id);
        return it == KNOWN_PAGES.end() ? sectionId : it->stem;
    }

    std::string pageTitle(const Localization& localization, const std::string_view sectionId)
    {
        const InternedString key(std::string(LOC_TABLE) + "page." + std::string(stemOf(sectionId)));
        std::string scratch;
        return localization.contains(key) ? std::string(localization.resolve(key, scratch)) : std::string(sectionId);
    }

    std::vector<std::string> orderedIds(const std::vector<Section>& list)
    {
        std::vector<std::string> ids;
        for (const KnownPage& page : KNOWN_PAGES)
        {
            if (std::ranges::any_of(list, [&](const Section& section) { return section.id == page.id; }))
            {
                ids.emplace_back(page.id);
            }
        }
        for (const Section& section : list)
        {
            if (!std::ranges::contains(ids, section.id))
            {
                ids.push_back(section.id);
            }
        }
        return ids;
    }

    bool readSections(ControlServer& control, const std::string_view id, std::vector<Section>& out)
    {
        Json args = Json::object();
        if (!id.empty())
        {
            args["section"] = std::string(id);
        }
        const Json reply =
            Json::parse(control.handle(Json{{"query", "config"}, {"args", args}}.dump()), nullptr, false);
        if (!reply.is_object() || !reply.value("ok", false))
        {
            return false;
        }
        out.clear();
        for (const Json& entry : reply["result"].value("sections", Json::array()))
        {
            out.push_back(parseSection(entry, !id.empty())); // the list needs ids and states, not the values
        }
        return true;
    }

    std::vector<Section> readAll(ControlServer& control, const std::vector<std::string>& ids)
    {
        std::vector<Section> out;
        for (const std::string& id : ids)
        {
            if (std::vector<Section> one; readSections(control, id, one) && !one.empty())
            {
                out.push_back(std::move(one.front()));
            }
        }
        return out;
    }

    std::vector<ListEntry> entriesOf(const Localization& localization, const std::vector<Section>& sections,
                                     const bool onlyChanged)
    {
        std::vector<ListEntry> out;
        for (const Section& section : sections)
        {
            const std::string title = pageTitle(localization, section.id);
            for (const Row& row : section.rows)
            {
                if (!onlyChanged || row.changed())
                {
                    out.push_back(
                        {.section = section.id,
                         .doc = row.doc,
                         .key = keyOf(section.id, row.path),
                         .cells = {title, row.path, valueText(row.value), valueText(row.def), valueText(row.saved)},
                         .value = row.value,
                         .def = row.def,
                         .saved = row.saved,
                         .modified = row.modified(),
                         .unsaved = row.unsaved()});
                }
            }
        }
        return out;
    }

    bool matches(const ListEntry& entry, const std::string_view needle)
    {
        return containsIgnoringCase(entry.cells.field, needle) || containsIgnoringCase(entry.doc, needle) ||
            containsIgnoringCase(entry.cells.page, needle);
    }

    std::vector<ListEntry> findEntries(ControlServer& control, const Localization& localization,
                                       const std::vector<std::string>& ids, const bool onlyChanged,
                                       const std::string_view needle)
    {
        std::vector<ListEntry> found = entriesOf(localization, readAll(control, ids), onlyChanged);
        std::erase_if(found, [&](const ListEntry& entry) { return !matches(entry, needle); });
        return found;
    }

    void writeEntry(IFormatWriter& out, const ListEntry& entry)
    {
        out.beginObject("");
        out.writeString("page", entry.section);
        out.writeString("title", entry.cells.page);
        out.writeString("path", entry.cells.field);
        writeJson(out, "value", entry.value);
        writeJson(out, "default", entry.def);
        writeJson(out, "saved", entry.saved);
        out.writeBool("modified", entry.modified);
        out.writeBool("unsaved", entry.unsaved);
        out.endObject();
    }

    void registerDiffQuery(ControlServer& control, const Localization& localization, const std::string_view owner)
    {
        static constexpr std::array<std::string_view, 3> AGAINST{"any", "default", "file"};
        control.registerQuery(
            "projectSettingsDiff", owner,
            "the fields changed from their default or from their saved file across every page, with the value, "
            "default and saved value of each and which of the two it differs from; text narrows it by field name, "
            "doc or page",
            {optionalArg("against", ArgKind::String, "which change to list", "any", AGAINST),
             optionalArg("text", ArgKind::String, "only fields whose name, doc or page contain it", "all")},
            [&control, &localization](const ControlArgs& args, IFormatWriter& out)
            {
                std::vector<Section> list;
                if (!readSections(control, {}, list))
                {
                    out.writeBool("available", false);
                    return std::string{};
                }
                out.writeBool("available", true);
                const std::string against = args.string("against").value_or("any");
                std::vector<ListEntry> found =
                    findEntries(control, localization, orderedIds(list), true, args.string("text").value_or(""));
                std::erase_if(
                    found, [&](const ListEntry& entry)
                    { return (against == "default" && !entry.modified) || (against == "file" && !entry.unsaved); });
                out.writeString("against", against);
                out.beginArray("entries", static_cast<u32>(found.size()));
                for (const ListEntry& entry : found)
                {
                    writeEntry(out, entry);
                }
                out.endArray();
                return std::string{};
            });
    }
} // namespace spite::projectsettings
