#include "EditorSettingsPanel.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorDialogs.hpp"
#include "EditorPanelUi.hpp"
#include "EditorSettings.hpp"
#include "ProjectSettingsModel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "engine/StyleCommands.hpp"
#include "engine/UiDisplayCommands.hpp"
#include "localization/Localization.hpp"
#include "uiview/UiStyleSheet.hpp"
#include "uiview/UiViewContext.hpp"
#include <spite/profiling/Zone.hpp>
#include "ui/UiDocument.hpp"
#include "ui/UiForm.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using namespace panelui;

        constexpr std::string_view SETTINGS_VIEW = "editor.settings";
        constexpr std::string_view SETTINGS_ROWS = "settings.rows";
        constexpr std::string_view OWNER = "editorSettingsPanel";
        constexpr std::string_view INPUT_PANEL = "input";
        constexpr std::string_view DEFAULT_LOCALE = "en";
        constexpr float BUTTON_PADDING = 8.0f;
        constexpr u32 CONFIG_FRAMES = 20; // a busy session reads the config sections this often at most
        constexpr std::string_view PROJECT_SETTINGS_PANEL = "projectSettings";
        constexpr std::string_view WINDOW_SECTION = "application.window";
        constexpr std::string_view TIMING_SECTION = "application.timing";

        // One row of a form: the editorSettings key, its kind and range, and the EditorSettings member that holds the
        // default the row's Reset restores (an Enum's default is the setting's named value in its choice list).
        struct Field
        {
            std::string_view key;
            UiFormKind kind;
            double min = 0.0;
            double max = 1.0;
            double step = 0.1;
            u8 decimals = 2;
            float EditorSettings::* number = nullptr;
            bool EditorSettings::* flag = nullptr;
            bool live = false; // read from editorSettingsState.live: a uiTheme, uiDisplay or setLocale override shows
            std::string_view section; // set: a field of this config section, edited with setConfig
            double rowStep = 0.0; // a config field's drag step and decimals; 0: the config row's
            u8 rowDecimals = 0;
        };

        constexpr double UI_MIN = UiDisplaySettings::MIN_UI_SCALE;
        constexpr double UI_MAX = UiDisplaySettings::MAX_UI_SCALE;
        constexpr std::array<Field, 4> APPEARANCE{{
            {.key = "theme", .kind = UiFormKind::Enum, .live = true},
            {"uiScale", UiFormKind::Number, UI_MIN, UI_MAX, 0.05, 2, &EditorSettings::uiScale, nullptr, true},
            {"textScale", UiFormKind::Number, UI_MIN, UI_MAX, 0.05, 2, &EditorSettings::textScale, nullptr, true},
            {"reducedMotion", UiFormKind::Bool, 0.0, 1.0, 1.0, 0, nullptr, &EditorSettings::reducedMotion, true},
        }};
        constexpr std::array<Field, 1> LANGUAGE{{{.key = "locale", .kind = UiFormKind::Enum, .live = true}}};
        constexpr std::array<Field, 11> SCENE_FIELDS{{
            {"gridEnabled", UiFormKind::Bool, 0.0, 1.0, 1.0, 0, nullptr, &EditorSettings::gridEnabled},
            {"gridOpacity", UiFormKind::Number, 0.0, 1.0, 0.005, 3, &EditorSettings::gridOpacity},
            {"gridSpacing", UiFormKind::Number, 0.001, 1.0e6, 0.01, 3, &EditorSettings::gridSpacing},
            {.key = "gridSpacingMode", .kind = UiFormKind::Enum},
            {"gridMajorEvery", UiFormKind::Number, 2.0, 16.0, 1.0, 0, &EditorSettings::gridMajorEvery},
            {.key = "gridPlane", .kind = UiFormKind::Enum},
            {"gridAxes", UiFormKind::Bool, 0.0, 1.0, 1.0, 0, nullptr, &EditorSettings::gridAxes},
            {"gridFadeDistance", UiFormKind::Number, 0.0, 1.0e7, 1.0, 1, &EditorSettings::gridFadeDistance},
            {"flySpeed", UiFormKind::Number, 0.01, 100.0, 0.01, 2, &EditorSettings::flySpeed},
            {"flyBoostRate", UiFormKind::Number, 0.0, 1000.0, 0.1, 2, &EditorSettings::flyBoostRate},
            {"flyBoostMax", UiFormKind::Number, 1.0, 1000.0, 1.0, 1, &EditorSettings::flyBoostMax},
        }};

        constexpr std::array<Field, 4> TELEMETRY_FIELDS{{
            {.key = "telemetry", .kind = UiFormKind::Enum},
            {"agentLayer", UiFormKind::Bool, 0.0, 1.0, 1.0, 0, nullptr, &EditorSettings::agentLayer},
            {"frameBudgetMs", UiFormKind::Number, 0.0, 1000.0, 0.1, 1, &EditorSettings::frameBudgetMs},
            {"systemBudgetMs", UiFormKind::Number, 0.0, 1000.0, 0.1, 1, &EditorSettings::systemBudgetMs},
        }};

        // The window and the frame pacing: rows of the config sections, which the config query describes
        constexpr std::array<Field, 5> WINDOW_FIELDS{{
            {.key = "width", .kind = UiFormKind::Number, .section = WINDOW_SECTION},
            {.key = "height", .kind = UiFormKind::Number, .section = WINDOW_SECTION},
            {.key = "fullscreen", .kind = UiFormKind::Bool, .section = WINDOW_SECTION},
            {.key = "borderless", .kind = UiFormKind::Bool, .section = WINDOW_SECTION},
            {.key = "highDpi", .kind = UiFormKind::Bool, .section = WINDOW_SECTION},
        }};
        constexpr std::array<Field, 5> TIMING_FIELDS{{
            {.key = "vsyncEnabled", .kind = UiFormKind::Bool, .section = TIMING_SECTION},
            {.key = "limitFPS", .kind = UiFormKind::Bool, .section = TIMING_SECTION},
            {.key = "targetFPS", .kind = UiFormKind::Number, .section = TIMING_SECTION},
            {.key = "fixedDeltaTime",
             .kind = UiFormKind::Number,
             .section = TIMING_SECTION,
             .rowStep = 0.0005,
             .rowDecimals = 5},
            {.key = "maxFixedSteps", .kind = UiFormKind::Number, .section = TIMING_SECTION},
        }};

        // A number as the form holds it: the setting is a float, shown rounded to the field's decimals, so a value
        // read back and a default compare equal.
        double shown(const double value, const u8 decimals)
        {
            const double scale = std::pow(10.0, decimals);
            return std::round(static_cast<double>(static_cast<float>(value)) * scale) / scale;
        }

        UiRectDesc fitButtonDesc()
        {
            UiRectDesc desc = barDesc(Vec2{});
            desc.contentSize =
                UiContentSizeComponent{.fitWidth = true, .fitHeight = false, .padding = Vec2{BUTTON_PADDING, 0.0f}};
            return desc;
        }
    } // namespace

    // The editor settings (EditorSettingsService) as sections of property-form rows, the Hotkeys link and the module
    // search paths: edits queue editorSettings and moduleSearchPath commands, the shown values follow
    // editorSettingsState whenever a command ran.
    struct EditorSettingsPanel::State
    {
        struct Group
        {
            std::string_view id;
            std::span<const Field> fields;
            std::unique_ptr<PropertyForm> form;
        };

        struct PathRow
        {
            Panel row;
            TextInput text;
            Button remove;
        };

        State(Engine& engine, EditorDialogs& dialogs, const EditorSettingsService& settings) :
            engine(&engine), control(&engine.control()), dialogs(&dialogs), service(&settings),
            localization(&engine.localization()), scope(editorUi(engine).ui())
        {
        }

        Engine* engine;
        ControlServer* control;
        EditorDialogs* dialogs;
        const EditorSettingsService* service;
        const Localization* localization;
        UiScope scope;
        DensityFollower density{scope};
        Panel root; // the docked content: the document view's root goes in it
        ScrollView scroller; // fills the rows slot
        Panel page; // the sections, as tall as they are
        std::vector<Label> headings;
        Button hotkeys;
        Label note;
        TextInput pathInput;
        Button addPath;
        Label pathError;
        Panel pathList; // the rows of the listed directories, above the add row
        Panel pathAdd;
        std::vector<PathRow> pathRows;
        std::vector<Group> groups;
        std::vector<std::string> themes;
        std::vector<std::string> locales;
        std::vector<std::string> gridPlanes{GRID_PLANES.begin(), GRID_PLANES.end()};
        std::vector<std::string> gridSpacingModes{GRID_SPACING_MODES.begin(), GRID_SPACING_MODES.end()};
        std::vector<std::string> telemetryModes;
        std::vector<projectsettings::Section> configs; // the window and timing sections as last read
        Label runtimeNote; // wraps: its class is the one the Game toolbar's hint takes
        Button projectSettings;
        bool configStale = true; // a read is due once CONFIG_FRAMES passed since the last
        u32 sinceConfig = CONFIG_FRAMES;
        std::vector<std::function<void()>> pending;
        InternedString childClass; // the slot's: restyles what the code built
        u64 seenCommand = ~0ull;
        u64 seenPaths = ~0ull;
        u64 seenLocale = 0;
        std::unique_ptr<UiDocumentView> doc; // last: destroyed first

        [[nodiscard]] std::string message(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            std::string scratch;
            return std::string(localization->resolve(InternedString(key), args, scratch));
        }

        [[nodiscard]] const std::vector<std::string>& choicesOf(const std::string_view key) const
        {
            return key == "theme"          ? themes
                : key == "telemetry"       ? telemetryModes
                : key == "gridPlane"       ? gridPlanes
                : key == "gridSpacingMode" ? gridSpacingModes
                                           : locales;
        }

        // the choice a row's Reset restores
        [[nodiscard]] static std::string_view defaultChoice(const std::string_view key)
        {
            static const EditorSettings defaults;
            return key == "theme"          ? DEFAULT_EDITOR_THEME
                : key == "telemetry"       ? std::string_view(defaults.telemetry)
                : key == "gridPlane"       ? std::string_view(defaults.gridPlane)
                : key == "gridSpacingMode" ? std::string_view(defaults.gridSpacingMode)
                                           : DEFAULT_LOCALE;
        }

        [[nodiscard]] static std::string rowKey(const Group& group, const Field& field)
        {
            return std::string(group.id) + "." + std::string(field.key);
        }

        // The config query for the window and timing sections; a read that finds fewer sections than the last (a
        // refused or missing store) keeps the last. False when there are none.
        bool readConfig()
        {
            std::vector<projectsettings::Section> read;
            for (const std::string_view id : {WINDOW_SECTION, TIMING_SECTION})
            {
                const Json reply =
                    request(*control, Json{{"query", "config"}, {"args", {{"section", std::string(id)}}}});
                for (const Json& entry : reply.value("result", Json::object()).value("sections", Json::array()))
                {
                    read.push_back(projectsettings::parseSection(entry, false));
                }
            }
            if (!read.empty() && read.size() >= configs.size())
            {
                configs = std::move(read);
            }
            return !configs.empty();
        }

        [[nodiscard]] const projectsettings::Row* configRow(const Field& field) const
        {
            const auto section = std::ranges::find(configs, field.section, &projectsettings::Section::id);
            if (section == configs.end())
            {
                return nullptr;
            }
            const auto row = std::ranges::find(section->rows, field.key, &projectsettings::Row::path);
            return row == section->rows.end() ? nullptr : &*row;
        }

        // A config field the last read does not describe: it is left as it is shown.
        [[nodiscard]] bool missing(const Field& field) const { return !field.section.empty() && !configRow(field); }

        // Whether every row of the fields is described: the section is shown whole or not at all.
        [[nodiscard]] bool hasRows(const std::span<const Field> fields) const
        {
            return std::ranges::all_of(fields, [this](const Field& field) { return configRow(field) != nullptr; });
        }

        // Runs a command now; the error ("" when accepted).
        std::string run(const std::string_view command, const Json& args)
        {
            const Json reply = request(*control, Json{{"command", command}, {"args", args}});
            if (reply.value("ok", false))
            {
                return {};
            }
            return reply.contains("error") && reply["error"].is_string() ? reply["error"].get<std::string>()
                                                                         : std::string("refused");
        }

        // A config row's edit is the command Project Settings sends for it: setConfig, or resetConfig for Reset.
        void editedConfig(Group& group, const Field& field, const UiFormEdit& edit)
        {
            const projectsettings::Row* row = configRow(field);
            if (!row)
            {
                return;
            }
            const bool reset = edit.reset;
            const Json value = reset ? Json() : projectsettings::toJson(*row, edit.value);
            pending.push_back(
                [this, &group, id = edit.field, section = std::string(field.section), path = std::string(field.key),
                 reset, value]
                {
                    const std::string error = reset
                        ? run("resetConfig", {{"section", section}, {"path", path}})
                        : run("setConfig", {{"section", section}, {"path", path}, {"value", value}});
                    group.form->setError(id, error);
                    configStale = true;
                    sinceConfig = CONFIG_FRAMES;
                    seenCommand = ~0ull; // the shown value follows what is staged, also after a refusal
                });
        }

        void edited(Group& group, const UiFormEdit& edit)
        {
            const auto field = std::ranges::find(group.fields, edit.field.toView(), &Field::key);
            if (!field->section.empty())
            {
                editedConfig(group, *field, edit);
                return;
            }
            Json value = edit.value.number[0];
            if (field->key == "uiScale" || field->key == "textScale")
            {
                value = edit.value.number[0] / 100.0;
            }
            if (field->kind == UiFormKind::Bool)
            {
                value = edit.value.number[0] != 0.0;
            }
            else if (field->kind == UiFormKind::Enum)
            {
                const std::vector<std::string>& choices = choicesOf(field->key);
                const auto index = static_cast<sizet>(edit.value.number[0]);
                value = index < choices.size() ? choices[index] : std::string{};
            }
            pending.push_back(
                [this, &group, id = edit.field, key = std::string(field->key), value]
                {
                    const std::string error = run("editorSettings", {{key, value}});
                    group.form->setError(id, error);
                    seenCommand = ~0ull; // the shown value follows what is in force, also after a refusal
                });
        }

        [[nodiscard]] UiFormField formField(const Field& field) const
        {
            if (!field.section.empty())
            {
                UiFormField out = projectsettings::fieldOf(*configRow(field));
                out.label.clear();
                out.doc.clear();
                out.labelKey = InternedString("editor-settings.runtime." + std::string(field.key));
                out.docKey = InternedString("editor-settings.runtime." + std::string(field.key) + ".doc");
                if (field.rowStep > 0.0)
                {
                    out.step = field.rowStep;
                    out.decimals = field.rowDecimals;
                }
                return out;
            }
            const EditorSettings defaults;
            UiFormField out;
            out.id = InternedString(field.key);
            out.labelKey = InternedString("editor-settings." + std::string(field.key));
            out.docKey = InternedString("editor-settings." + std::string(field.key) + ".doc");
            out.kind = field.kind;
            out.min = field.min;
            out.max = field.max;
            out.step = field.step;
            out.decimals = field.decimals;
            const bool percent = field.key == "uiScale" || field.key == "textScale";
            if (percent)
            {
                out.min *= 100.0;
                out.max *= 100.0;
                out.step = 10.0;
                out.decimals = 0;
            }
            if (field.kind == UiFormKind::Enum)
            {
                const std::vector<std::string>& choices = choicesOf(field.key);
                for (const std::string& name : choices)
                {
                    // a named choice (telemetry, grid mode) reads from the locale; a theme or a locale is its own name
                    const InternedString key(std::format("editor-settings.choice.{}.{}", field.key, name));
                    out.choices.push_back({.id = InternedString(name),
                                           .label = name,
                                           .key = localization->contains(key) ? key : InternedString{}});
                }
                const auto at = std::ranges::find(choices, defaultChoice(field.key));
                out.defaultValue =
                    UiFormValue::ofNumber(at == choices.end() ? 0.0 : static_cast<double>(at - choices.begin()));
            }
            else if (field.kind == UiFormKind::Bool)
            {
                out.defaultValue = UiFormValue::ofBool(defaults.*field.flag);
            }
            else
            {
                out.defaultValue =
                    UiFormValue::ofNumber(shown((defaults.*field.number) * (percent ? 100.0 : 1.0), out.decimals));
            }
            return out;
        }

        // A row's value as the snapshot holds it.
        [[nodiscard]] UiFormValue valueOf(const Field& field, const Json& state) const
        {
            if (!field.section.empty())
            {
                const projectsettings::Row* row = configRow(field);
                return row ? projectsettings::toForm(*row, row->value) : UiFormValue{};
            }
            const Json& value = (field.live ? state["live"] : state)[std::string(field.key)];
            if (field.kind == UiFormKind::Enum)
            {
                const std::vector<std::string>& choices = choicesOf(field.key);
                const auto at = std::ranges::find(choices, value.get<std::string>());
                return UiFormValue::ofNumber(at == choices.end() ? -1.0 : static_cast<double>(at - choices.begin()));
            }
            if (field.kind == UiFormKind::Bool)
            {
                return UiFormValue::ofBool(value.get<bool>());
            }
            return UiFormValue::ofNumber(
                shown(value.get<double>() * ((field.key == "uiScale" || field.key == "textScale") ? 100.0 : 1.0),
                      (field.key == "uiScale" || field.key == "textScale") ? 0 : field.decimals));
        }

        // The form's rows follow the snapshot; a row being edited is left alone (settingsView reads it from the
        // snapshot).
        void follow(const Json& state)
        {
            for (Group& group : groups)
            {
                for (const Field& field : group.fields)
                {
                    const InternedString id{field.key};
                    if (group.form->isEditing(id) || missing(field))
                    {
                        continue;
                    }
                    if (const UiFormValue next = valueOf(field, state); group.form->value(id) != next)
                    {
                        group.form->setValue(id, next);
                    }
                }
            }
        }

        // What is in force now, read where it lives: the stored settings, and for the appearance rows the live ones (a
        // uiTheme, uiDisplay or setLocale override differs from the stored values).
        [[nodiscard]] Json snapshot() const
        {
            const EditorSettings& s = service->settings();
            const UiDisplaySettings& display = engine->uiDisplay();
            return {{"gridEnabled", s.gridEnabled},
                    {"gridOpacity", s.gridOpacity},
                    {"gridSpacing", s.gridSpacing},
                    {"gridSpacingMode", s.gridSpacingMode},
                    {"gridMajorEvery", s.gridMajorEvery},
                    {"gridPlane", s.gridPlane},
                    {"gridAxes", s.gridAxes},
                    {"gridFadeDistance", s.gridFadeDistance},
                    {"flySpeed", s.flySpeed},
                    {"flyBoostRate", s.flyBoostRate},
                    {"flyBoostMax", s.flyBoostMax},
                    {"telemetry", s.telemetry},
                    {"agentLayer", s.agentLayer},
                    {"frameBudgetMs", s.frameBudgetMs},
                    {"systemBudgetMs", s.systemBudgetMs},
                    {"live",
                     {{"theme", std::string(editorUi(*engine).uiView().stylesheet().theme().toView())},
                      {"locale", localization->locale()},
                      {"uiScale", display.uiScale},
                      {"textScale", display.textScale},
                      {"reducedMotion", display.reducedMotion}}}};
        }

        void refresh()
        {
            SPITE_ZONE("EditorSettingsPanel.refresh");
            follow(snapshot());
        }

        void restyle()
        {
            const auto part = [this](const std::string_view suffix, const std::string_view shipped)
            {
                return childClass.isValid() ? classPart(childClass, suffix)
                                            : (shipped.empty() ? InternedString{} : InternedString(shipped));
            };
            for (Label& heading : headings)
            {
                heading.setStyleClass(part(".label", "settings.heading"));
            }
            for (Label* label : {&note, &pathError})
            {
                label->setStyleClass(part(".label", "settings.note"));
            }
            for (Button* button : {&hotkeys, &addPath, &projectSettings})
            {
                if (!button->isValid())
                {
                    continue;
                }
                button->setStyleClass(part(".toggle", ""));
            }
            pathInput.setStyleClass(part(".field", ""));
            for (PathRow& row : pathRows)
            {
                row.text.setStyleClass(part(".field", ""));
                row.remove.setStyleClass(part(".toggle", ""));
            }
        }

        void fill(const Widget& slot)
        {
            putIn(scope, slot, scroller);
            scroller.setScrollEnabled(slotFields(scope, slot).scroll);
            childClass = slotFields(scope, slot).childClass;
            restyle();
        }

        // The page is as tall as its sections; the scroll content is that or the viewport. Writes only on a change.
        void fitPage()
        {
            const Vec2 view = resolvedSize(scope, scroller);
            if (view.x <= 0.0f || view.y <= 0.0f)
            {
                return;
            }
            const float height = resolvedSize(scope, page).y + 2.0f * density.density().xs;
            const Vec2 want{view.x, std::max(view.y, height)};
            if (scroller.contentSize() != want)
            {
                scroller.setContentSize(want);
            }
        }

        PathRow makePathRow()
        {
            PathRow row;
            row.row = stackPanel(scope, "settings.path", Vec2{0.0f, density.density().control}, UiLayoutDirection::Row,
                                 UiLayoutAlign::Stretch);
            density.track(row.row, HEIGHT_CONTROL);
            row.row.setParent(pathList);
            row.text = readOnlyText(scope, InternedString("settings.path.text"), barDesc(Vec2{}));
            row.text.setLayoutItem(fillItem());
            row.text.setParent(row.row);
            row.text.setAccessibleNameKey(InternedString("editor-settings.paths.path"));
            row.remove = scope.createButton("settings.path.remove", "", fitButtonDesc());
            row.remove.setLocalizedText(InternedString("editor-settings.paths.remove"));
            row.remove.setParent(row.row);
            row.remove.onClick(
                [this, text = row.text]
                {
                    const std::string path(text.value());
                    pending.push_back(
                        [this, path]
                        {
                            const MessageArg args[]{{"path", std::string_view(path)}};
                            dialogs->confirm({.title = message("editor-settings.paths.removeTitle"),
                                              .body = message("editor-settings.paths.removeBody", args),
                                              .confirmLabel = message("editor-settings.paths.remove"),
                                              .destructive = true},
                                             [this, path](const bool confirmed)
                                             {
                                                 if (confirmed)
                                                 {
                                                     run("moduleSearchPathRemove", {{"path", path}});
                                                 }
                                             });
                        });
                });
            return row;
        }

        // The rows list service->moduleSearchPaths(): the pool grows to the list, the spare rows hide.
        void showPaths()
        {
            const std::vector<std::string>& paths = service->moduleSearchPaths();
            while (pathRows.size() < paths.size())
            {
                pathRows.push_back(makePathRow());
            }
            for (sizet i = 0; i < pathRows.size(); ++i)
            {
                const bool used = i < paths.size();
                pathRows[i].row.setVisible(used);
                if (used)
                {
                    show(pathRows[i].text, paths[i]);
                    const MessageArg args[]{{"path", std::string_view(paths[i])}};
                    pathRows[i].remove.setAccessibleName(message("editor-settings.paths.removeName", args));
                }
            }
            note.setVisible(paths.empty());
            restyle();
            seenPaths = service->moduleSearchPathRevision();
            seenLocale = localization->revision(); // the Remove names are messages
        }

        void addPathNow()
        {
            const std::string path(pathInput.value());
            pending.push_back(
                [this, path]
                {
                    const std::string error = run("moduleSearchPathAdd", {{"path", path}});
                    pathError.setText(error);
                    pathError.setVisible(!error.empty());
                    if (error.empty())
                    {
                        pathInput.setText("");
                    }
                });
        }

        void registerQuery()
        {
            control->registerQuery(
                "settingsView", OWNER,
                "the Preferences panel: per row of its forms (appearance, language, sceneView, window, timing) the "
                "field, "
                "its value (an Enum's is the choice name), whether it differs from the default, whether Reset is shown "
                "and the error under it, and the module search paths the list shows; a window or timing row is a "
                "config row (setConfig, staged value) and adds section, apply (live or startup) and restartPending",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    const Json now = snapshot();
                    readConfig(); // the window and timing rows are the staged config
                    follow(now); // also while the panel is hidden: the rows are what is in force now
                    out.beginArray("rows", 0);
                    for (const Group& group : groups)
                    {
                        for (const Field& field : group.fields)
                        {
                            UiFormRowState row = group.form->state(InternedString(field.key));
                            if (group.form->isEditing(InternedString(field.key)) && !missing(field))
                            {
                                // a focused row keeps its text: the value in force is the snapshot's
                                row.value = valueOf(field, now);
                                row.modified = row.value != formField(field).defaultValue;
                                row.resettable = row.modified;
                            }
                            out.beginObject("");
                            out.writeString("form", group.id);
                            out.writeString("field", field.key);
                            if (field.kind == UiFormKind::Enum)
                            {
                                const std::vector<std::string>& choices = choicesOf(field.key);
                                const auto index = static_cast<sizet>(row.value.number[0]);
                                out.writeString("value", index < choices.size() ? choices[index] : std::string{});
                            }
                            else if (field.kind == UiFormKind::Bool)
                            {
                                out.writeBool("value", row.value.number[0] != 0.0);
                            }
                            else
                            {
                                out.writeF64("value", row.value.number[0]);
                            }
                            out.writeBool("modified", row.modified);
                            out.writeBool("resettable", row.resettable);
                            out.writeString("error", row.error);
                            if (!field.section.empty())
                            {
                                const projectsettings::Row* config = configRow(field);
                                out.writeString("section", field.section);
                                out.writeString("apply", config ? config->apply : std::string{});
                                out.writeBool("restartPending", config && config->restartPending());
                            }
                            out.endObject();
                        }
                    }
                    out.endArray();
                    out.beginArray("paths", 0);
                    for (const std::string& path : service->moduleSearchPaths())
                    {
                        out.writeString("", path);
                    }
                    out.endArray();
                    out.writeString("pathError", pathError.text());
                    return std::string{};
                });
        }
    };

    EditorSettingsPanel::EditorSettingsPanel(Engine& engine, EditorDialogs& dialogs,
                                             const EditorSettingsService& settings) :
        m_state(std::make_unique<State>(engine, dialogs, settings))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.themes = availableUiThemes();
        std::ranges::sort(state.themes);
        state.locales = engine.localization().locales();
        std::ranges::sort(state.locales);
        state.telemetryModes.assign(TELEMETRY_MODES.begin(), TELEMETRY_MODES.end());

        state.root = scope.createPanel("editor.panel", columnDesc());
        UiRectDesc scrollDesc = topLeft();
        scrollDesc.layoutItem = fillItem();
        state.scroller = scope.createScrollView("settings.scroll", scrollDesc);
        state.scroller.setParent(state.root);
        Panel content = state.scroller.content();
        state.density.apply(
            [content](const Density& d) mutable
            {
                content.setLayout(stack(UiLayoutDirection::Column, 0.0f, Vec4{d.xs, d.xs, d.xs + THUMB, d.xs},
                                        UiLayoutAlign::Stretch));
            });
        UiRectDesc pageDesc =
            barDesc(Vec2{}, stack(UiLayoutDirection::Column, state.density.density().md, {}, UiLayoutAlign::Stretch));
        pageDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        state.page = scope.createPanel("settings.page", pageDesc);
        bindGap(state.page, SPACE_MD);
        state.page.setParent(content);

        const auto heading = [&state, &scope](const std::string_view key)
        {
            Label label =
                scope.createLabel(InternedString("settings." + std::string(key) + ".heading"), "", barDesc(Vec2{}));
            state.density.track(label, HEIGHT_ROW);
            label.setLocalizedText(InternedString("editor-settings.section." + std::string(key)));
            label.setStyleClass(InternedString("settings.heading"));
            label.setParent(state.page);
            state.headings.push_back(label);
        };
        const auto section = [&state, &scope](const std::string_view id, const std::span<const Field> fields)
        {
            State::Group group{id, fields, nullptr};
            UiFormDesc desc;
            desc.rect = formRect();
            for (const Field& field : fields)
            {
                desc.fields.push_back(state.formField(field));
            }
            group.form = std::make_unique<PropertyForm>(scope, InternedString(id), std::move(desc));
            group.form->root().setParent(state.page);
            state.groups.push_back(std::move(group));
            State::Group& stored = state.groups.back();
            stored.form->onEdit([&state, &stored](const UiFormEdit& edit) { state.edited(stored, edit); });
        };
        state.groups.reserve(6); // the edit callbacks hold a group's address
        state.readConfig();
        heading("appearance");
        section("settings.appearance", APPEARANCE);
        heading("language");
        section("settings.language", LANGUAGE);
        heading("sceneView");
        section("settings.sceneView", SCENE_FIELDS);
        heading("telemetry");
        section("settings.telemetry", TELEMETRY_FIELDS);
        // the window and the frame pacing are config rows; a run without a config store has none to show
        if (state.hasRows(WINDOW_FIELDS) && state.hasRows(TIMING_FIELDS))
        {
            heading("window");
            section("settings.window", WINDOW_FIELDS);
            heading("timing");
            section("settings.timing", TIMING_FIELDS);
            UiRectDesc noteDesc = barDesc(Vec2{});
            noteDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
            state.runtimeNote = scope.createLabel("settings.runtime.note", "", noteDesc);
            state.runtimeNote.setStyleClass(InternedString("settings.noteWrap"));
            state.runtimeNote.setLocalizedText(InternedString("editor-settings.runtime.note"));
            state.runtimeNote.setParent(state.page);
            state.projectSettings = state.density.track(
                scope.createButton("settings.runtime.projectSettings", "", fitButtonDesc()), HEIGHT_BUTTON);
            state.projectSettings.setLocalizedText(InternedString("editor-settings.runtime.open"));
            state.projectSettings.setTooltip(InternedString("editor-settings.runtime.open.tip"));
            Panel openRow = stackPanel(scope, "settings.runtime.openRow", Vec2{0.0f, state.density.density().control},
                                       UiLayoutDirection::Row);
            state.density.track(openRow, HEIGHT_BUTTON);
            openRow.setParent(state.page);
            state.projectSettings.setParent(openRow);
            state.projectSettings.onClick(
                [&state]
                {
                    state.pending.push_back(
                        [&state]
                        { state.run("dock", {{"kind", "activate"}, {"panel", std::string(PROJECT_SETTINGS_PANEL)}}); });
                });
        }

        heading("hotkeys");
        state.hotkeys = state.density.track(scope.createButton("settings.hotkeys", "", fitButtonDesc()), HEIGHT_BUTTON);
        state.hotkeys.setLocalizedText(InternedString("editor-settings.hotkeys"));
        state.hotkeys.setTooltip(InternedString("editor-settings.hotkeys.tip"));
        // the page stretches its children: a row keeps the link as wide as its text
        Panel hotkeysRow = stackPanel(scope, "settings.hotkeysRow", Vec2{0.0f, state.density.density().control},
                                      UiLayoutDirection::Row);
        state.density.track(hotkeysRow, HEIGHT_BUTTON);
        hotkeysRow.setParent(state.page);
        state.hotkeys.setParent(hotkeysRow);
        state.hotkeys.onClick(
            [&state]
            {
                state.pending.push_back(
                    [&state] { state.run("dock", {{"kind", "activate"}, {"panel", std::string(INPUT_PANEL)}}); });
            });

        heading("paths");
        state.note = scope.createLabel("settings.paths.empty", "", barDesc(Vec2{}));
        state.density.track(state.note, HEIGHT_ROW);
        state.note.setLocalizedText(InternedString("editor-settings.paths.empty"));
        state.note.setParent(state.page);
        UiRectDesc listDesc =
            barDesc(Vec2{}, stack(UiLayoutDirection::Column, state.density.density().xs, {}, UiLayoutAlign::Stretch));
        listDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        state.pathList = scope.createPanel("settings.pathList", listDesc);
        bindGap(state.pathList, SPACE_XS);
        state.pathList.setParent(state.page);
        state.pathAdd = stackPanel(scope, "settings.pathAdd", Vec2{0.0f, state.density.density().control},
                                   UiLayoutDirection::Row, UiLayoutAlign::Stretch);
        state.density.track(state.pathAdd, HEIGHT_CONTROL);
        state.pathAdd.setParent(state.page);
        state.pathInput = scope.createTextInput("settings.pathInput", barDesc(Vec2{}));
        state.pathInput.setLayoutItem(fillItem());
        state.pathInput.setAccessibleNameKey(InternedString("editor-settings.paths.input"));
        state.pathInput.setParent(state.pathAdd);
        state.pathInput.onSubmit([&state](std::string_view) { state.addPathNow(); });
        state.addPath = scope.createButton("settings.pathAdd.button", "", fitButtonDesc());
        state.addPath.setLocalizedText(InternedString("editor-settings.paths.add"));
        state.addPath.setTooltip(InternedString("editor-settings.paths.add.tip"));
        state.addPath.setParent(state.pathAdd);
        state.addPath.onClick([&state] { state.addPathNow(); });
        state.pathError = scope.createLabel("settings.pathError", "", barDesc(Vec2{}));
        state.density.track(state.pathError, HEIGHT_ROW);
        state.pathError.setVisible(false);
        state.pathError.setParent(state.page);
        state.registerQuery();
        scope.commit();

        // the document owns the containers: the scrolling page is the rows slot's content, laid out from the slot
        UiViewContract contract;
        contract.slot(InternedString(SETTINGS_ROWS), [&state](const Widget& slot) { state.fill(slot); });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                fillSlotPanel(viewScope, SETTINGS_ROWS, root);
            });
        state.doc = openView(engine, SETTINGS_VIEW, "assets/ui/editor/settings.json", std::move(contract), state.root);
    }

    EditorSettingsPanel::~EditorSettingsPanel() { m_state->control->unregisterOwner(OWNER); }

    Widget EditorSettingsPanel::content() const { return m_state->root; }

    void EditorSettingsPanel::update()
    {
        State& state = *m_state;
        if (!state.pending.empty())
        {
            std::vector<std::function<void()>> actions;
            actions.swap(state.pending);
            for (const std::function<void()>& action : actions)
            {
                action();
            }
        }
        if (!shown(state.scope, state.root))
        {
            state.seenCommand = ~0ull;
            state.configStale = true;
            state.sinceConfig = CONFIG_FRAMES;
            return;
        }
        state.fitPage();
        if (const u64 revision = state.service->moduleSearchPathRevision();
            revision != state.seenPaths || state.localization->revision() != state.seenLocale)
        {
            state.showPaths();
        }
        if (const u64 command = state.control->commandGeneration(); command != state.seenCommand)
        {
            state.seenCommand = command;
            state.configStale = !state.configs.empty();
            state.refresh();
        }
        ++state.sinceConfig;
        if (state.configStale && state.sinceConfig >= CONFIG_FRAMES)
        {
            state.configStale = false;
            state.sinceConfig = 0;
            state.readConfig();
            state.refresh();
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "settings",
             .category = "tools",
             .order = 100,
             .dock = {.target = "project"},
             .zone = "EditorSettingsPanel",
             .make = [](EditorPanelContext& c)
             { return makeEditorPanel<EditorSettingsPanel>(c.engine, c.dialogs, c.settings); }}};
    } // namespace
} // namespace spite
