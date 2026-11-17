#pragma once

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/Schema.hpp"
#include "ui/UiForm.hpp"

namespace spite
{
    class ControlServer;
    class IFormatWriter;
    class Localization;

    // The Project Settings panel's model: the config query's sections and rows as the panel reads them, and the
    // mapping between a row's JSON value and the property form (internal to the editor).
    namespace projectsettings
    {
        using Json = nlohmann::json;

        constexpr double INF = std::numeric_limits<double>::infinity();

        // One leaf of a section as the config query lists it.
        struct Row
        {
            std::string path;
            std::string group; // the nested struct's path, empty for the section's own fields
            std::string leaf; // path without the group
            std::string kind; // number, bool, enum, string, path, vec2, vec3, vec4
            std::string type;
            std::string doc;
            std::string unit;
            std::string apply; // live or startup
            std::string source; // default, project, user or memory: where the saved value comes from
            Json value;
            Json running;
            Json def;
            Json saved;
            double min = -INF;
            double max = INF;
            std::vector<std::string> choices;
            bool readOnly = false;

            [[nodiscard]] bool modified() const { return value != def; }
            [[nodiscard]] bool unsaved() const { return !saved.is_null() && value != saved; }
            [[nodiscard]] bool changed() const { return modified() || unsaved(); }
            // a startup setting whose staged value is not the one in force: it applies at the next start
            [[nodiscard]] bool restartPending() const
            {
                return apply == "startup" && !running.is_null() && value != running;
            }
        };

        struct Section
        {
            std::string id;
            std::string kind; // section or file
            std::string doc;
            std::string layer;
            std::string file;
            std::string reason;
            bool dirty = false;
            bool pending = false;
            std::vector<Row> rows;
            std::vector<std::pair<std::string, std::string>> groups; // path, doc
            Json values;

            [[nodiscard]] bool listed() const { return kind == "file"; }
            [[nodiscard]] bool restartPending() const
            {
                return std::ranges::any_of(rows, [](const Row& row) { return row.restartPending(); });
            }
            [[nodiscard]] bool anyModified() const
            {
                return std::ranges::any_of(rows, [](const Row& row) { return row.modified() && !row.readOnly; });
            }
        };

        Section parseSection(const Json& entry, bool withValues);
        // What decides whether a page's forms still describe the section: the rows and their limits.
        std::string signatureOf(const Section& section);
        // A config value as the form holds it.
        UiFormValue toForm(const Row& row, const Json& value);
        // An edit as the JSON setConfig takes: an integer field an integer, an enum its choice name.
        Json toJson(const Row& row, const UiFormValue& value);
        UiFormField fieldOf(const Row& row);
        // The groups of a section's rows in page order: the section's own fields (an empty path) first, then each
        // nested struct as it appears.
        std::vector<std::string> groupPaths(const Section& section);
        // A value as a table cell shows it: a string as it is, nothing as a dash, a float to seven digits, anything
        // else as compact JSON.
        std::string valueText(const Json& value);
        void writeJson(IFormatWriter& out, std::string_view name, const Json& value);

        // The pages in the order the panel lists them; a section a module declares follows them. `stem` ends the
        // page's loc key and prefixes its element ids.
        struct KnownPage
        {
            std::string_view id;
            std::string_view stem;
        };
        inline constexpr std::array<KnownPage, 7> KNOWN_PAGES{{{"application.graphics", "graphics"},
                                                               {"application.timing", "timing"},
                                                               {"application.window", "window"},
                                                               {"application.input", "input"},
                                                               {"application.modules", "modules"},
                                                               {"physics.world", "physics"},
                                                               {"ui.navigation", "navigation"}}};
        // The prefix of the panel's loc keys, which also name the pages.
        inline constexpr std::string_view LOC_TABLE = "editor-projectSettings.";

        std::string_view stemOf(std::string_view sectionId);
        // The page's localized title (the section id where the panel has none).
        std::string pageTitle(const Localization& localization, std::string_view sectionId);
        // The sections' ids in page order.
        std::vector<std::string> orderedIds(const std::vector<Section>& list);
        // The config query: every section (empty id; ids and states only) or one with its fields and values; false
        // when the run has no config store.
        bool readSections(ControlServer& control, std::string_view id, std::vector<Section>& out);
        // The listed sections read in full, in `ids` order.
        std::vector<Section> readAll(ControlServer& control, const std::vector<std::string>& ids);

        // What the cross-page table shows of a field.
        struct ListCells
        {
            std::string page;
            std::string field;
            std::string value;
            std::string defaultValue;
            std::string saved;
            bool operator==(const ListCells&) const = default;
            SPITE_OBSERVED_SCHEMA(ListCells, page, field, value, defaultValue, saved)
        };

        // One field of one page in the cross-page list: the search finds it, the diff lists it when it changed.
        struct ListEntry
        {
            std::string section;
            std::string doc;
            u32 key = 0; // a hash of section and path: a selected row stays on its field when the list changes
            ListCells cells;
            Json value;
            Json def;
            Json saved;
            bool modified = false; // differs from the default
            bool unsaved = false; // differs from the file
            bool operator==(const ListEntry&) const = default;
        };

        std::vector<ListEntry> entriesOf(const Localization& localization, const std::vector<Section>& sections,
                                         bool onlyChanged);
        bool matches(const ListEntry& entry, std::string_view needle);
        // The fields the search text finds (all of them when it is empty), or only those changed from the default or
        // the file; read from the config query now.
        std::vector<ListEntry> findEntries(ControlServer& control, const Localization& localization,
                                           const std::vector<std::string>& ids, bool onlyChanged,
                                           std::string_view needle);
        void writeEntry(IFormatWriter& out, const ListEntry& entry);

        // The projectSettingsDiff query (owner `owner`): the fields changed from their default or their saved file
        // across every page. It reads the config query only, so it answers with the panel closed and without a shell.
        void registerDiffQuery(ControlServer& control, const Localization& localization, std::string_view owner);
    } // namespace projectsettings
} // namespace spite
