#include "EditorSettings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <utility>

#if defined(_WIN32)
#include <cwctype>
#endif

#include "EditorContext.hpp"
#include <nlohmann/json.hpp>

#include "base/ContentRoot.hpp"
#include "base/Logging.hpp"
#include "base/StringInterner.hpp"
#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "engine/StyleCommands.hpp"
#include "engine/UiDisplayCommands.hpp"
#include "localization/Locale.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"
#include "serialization/Serializer.hpp"
#include "uiview/UiStyleSheet.hpp"
#include "uiview/UiViewContext.hpp"

namespace spite
{
    namespace
    {
        constexpr std::string_view OWNER = "editorSettings";

        // a finite number member within [min, max]; absent keeps out
        bool number(const ControlArgs& args, const std::string_view key, const float min, const float max, float& out)
        {
            const std::optional<std::string> text = args.json(key);
            if (!text)
            {
                return true;
            }
            char* end = nullptr;
            const float value = std::strtof(text->c_str(), &end);
            if (end == text->c_str() || !std::isfinite(value) || value < min || value > max)
            {
                return false;
            }
            out = value;
            return true;
        }

        // the grid and fly boost values the command accepts, and the range a stored value must lie in
        struct Range
        {
            std::string_view key;
            float EditorSettings::* member;
            float min;
            float max;
        };
        constexpr Range RANGES[] = {
            {"gridOpacity", &EditorSettings::gridOpacity, 0.0f, 1.0f},
            {"gridSpacing", &EditorSettings::gridSpacing, 0.001f, 1.0e6f},
            {"gridMajorEvery", &EditorSettings::gridMajorEvery, 2.0f, 16.0f},
            {"gridFadeDistance", &EditorSettings::gridFadeDistance, 0.0f, 1.0e7f},
            {"flySpeed", &EditorSettings::flySpeed, 0.01f, 100.0f},
            {"flyBoostRate", &EditorSettings::flyBoostRate, 0.0f, 1000.0f},
            {"flyBoostMax", &EditorSettings::flyBoostMax, 1.0f, 1000.0f},
            {"systemBudgetMs", &EditorSettings::systemBudgetMs, 0.0f, 1000.0f},
            {"frameBudgetMs", &EditorSettings::frameBudgetMs, 0.0f, 1000.0f},
        };

#if defined(_WIN32)
        std::wstring lowered(const std::string_view utf8)
        {
            std::wstring wide = pathFromUtf8(utf8).native();
            std::ranges::transform(wide, wide.begin(),
                                   [](const wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return wide;
        }
#endif

        std::string hexOf(const u32 color)
        {
            return std::format("#{:02x}{:02x}{:02x}", color & 0xFFu, (color >> 8) & 0xFFu, (color >> 16) & 0xFFu);
        }

        std::string joined(const std::vector<std::string>& paths)
        {
            std::string text;
            for (const std::string& path : paths)
            {
                text += (text.empty() ? "" : "|") + path;
            }
            return text;
        }

        // a positive finite number member; one past [min, max] clamps (as uiDisplay does); absent keeps out
        bool scale(const ControlArgs& args, const std::string_view key, const float min, const float max, float& out)
        {
            const std::optional<std::string> text = args.json(key);
            if (!text)
            {
                return true;
            }
            char* end = nullptr;
            const double value = std::strtod(text->c_str(), &end);
            if (end == text->c_str() || !std::isfinite(value) || value <= 0.0)
            {
                return false;
            }
            out = static_cast<float>(std::clamp(value, static_cast<double>(min), static_cast<double>(max)));
            return true;
        }

        void writeNames(IFormatWriter& out, const std::string_view key, std::vector<std::string> names)
        {
            std::ranges::sort(names);
            out.beginArray(key, static_cast<u32>(names.size()));
            for (const std::string& name : names)
            {
                out.writeString("", name);
            }
            out.endArray();
        }

        // the canonical form of a locale the Localization has tables for; empty for any other
        std::string loadedLocale(const Localization& localization, const std::string_view name)
        {
            std::string canonical = canonicalLocale(name);
            const std::vector<std::string> loaded = localization.locales();
            return std::ranges::find(loaded, canonical) == loaded.end() ? std::string{} : canonical;
        }

        // "#rrggbb" in lower case, empty for any other text
        std::string canonicalHex(const std::string_view text)
        {
            const auto digit = [](const char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
            if (text.size() != 7 || text[0] != '#' || !std::all_of(text.begin() + 1, text.end(), digit))
            {
                return {};
            }
            std::string hex(text);
            std::transform(hex.begin(), hex.end(), hex.begin(),
                           [](const char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
            return hex;
        }

        u32 rgba8(const std::string& hex)
        {
            const auto rgb = static_cast<u32>(std::strtoul(hex.c_str() + 1, nullptr, 16));
            return 0xFF000000u | ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) | (rgb >> 16);
        }

        std::string listed(std::vector<std::string> names)
        {
            std::ranges::sort(names);
            std::string text;
            for (const std::string& name : names)
            {
                text += (text.empty() ? "" : ", ") + name;
            }
            return text;
        }

        std::vector<std::string> telemetryModes() { return {TELEMETRY_MODES.begin(), TELEMETRY_MODES.end()}; }
        std::vector<std::string> gridPlanes() { return {GRID_PLANES.begin(), GRID_PLANES.end()}; }
        std::vector<std::string> gridSpacingModes() { return {GRID_SPACING_MODES.begin(), GRID_SPACING_MODES.end()}; }

        void write(IFormatWriter& out, const EditorSettings& settings)
        {
            out.writeBool("gridEnabled", settings.gridEnabled);
            out.writeBool("sceneToolbar", settings.sceneToolbar);
            out.writeF32("gridOpacity", settings.gridOpacity);
            out.writeF32("gridSpacing", settings.gridSpacing);
            out.writeString("gridSpacingMode", settings.gridSpacingMode);
            out.writeF32("gridMajorEvery", settings.gridMajorEvery);
            out.writeString("gridPlane", settings.gridPlane);
            out.writeBool("gridAxes", settings.gridAxes);
            out.writeF32("gridFadeDistance", settings.gridFadeDistance);
            out.writeF32("flySpeed", settings.flySpeed);
            out.writeF32("flyBoostRate", settings.flyBoostRate);
            out.writeF32("flyBoostMax", settings.flyBoostMax);
            out.writeString("theme", settings.theme);
            out.writeString("density", settings.density);
            out.writeString("dataColor", settings.dataColor);
            out.writeF32("uiScale", settings.uiScale);
            out.writeF32("textScale", settings.textScale);
            out.writeBool("reducedMotion", settings.reducedMotion);
            out.writeString("locale", settings.locale);
            out.writeBool("agentLayer", settings.agentLayer);
            out.writeF32("systemBudgetMs", settings.systemBudgetMs);
            out.writeString("telemetry", settings.telemetry);
            out.writeF32("frameBudgetMs", settings.frameBudgetMs);
        }
    } // namespace

    void migrateEditorSettings(EditorSettings& settings)
    {
        if (settings.version < 2 && settings.gridFadeDistance == 100.0f)
        {
            settings.gridFadeDistance = EditorSettings{}.gridFadeDistance;
        }
        settings.version = EDITOR_SETTINGS_VERSION;
    }

    float effectiveGridSpacing(const EditorSettings& settings, const bool snapOn, const float snapStep)
    {
        const bool follow = settings.gridSpacingMode == GRID_SPACING_SNAP && snapOn && snapStep > 0.0f;
        return follow ? snapStep : settings.gridSpacing;
    }

    std::string normalizeModuleSearchPath(const std::string_view path, std::string& error)
    {
        error.clear();
        if (path.empty() || path.size() > MAX_MODULE_SEARCH_PATH_BYTES)
        {
            error = std::format("a module directory is 1 to {} bytes", MAX_MODULE_SEARCH_PATH_BYTES);
            return {};
        }
        if (std::ranges::any_of(path, [](const char c) { return static_cast<unsigned char>(c) < 0x20 || c == '|'; }))
        {
            error = "a module directory holds no control character or '|'";
            return {};
        }
        try
        {
            const auto parsed = pathFromUtf8(path);
            if (!parsed.is_absolute())
            {
                error = std::format("{} is not an absolute path", path);
                return {};
            }
            auto normalized = parsed.lexically_normal();
            if (normalized.has_relative_path() && !normalized.has_filename())
            {
                normalized = normalized.parent_path();
            }
            std::string utf8;
            if (tryPathToUtf8(normalized, utf8))
            {
                return utf8;
            }
        }
        catch (const std::system_error&)
        {
        }
        error = "a module directory is valid UTF-8";
        return {};
    }

    bool sameModuleSearchPath(const std::string_view a, const std::string_view b)
    {
#if defined(_WIN32)
        return lowered(a) == lowered(b);
#else
        return a == b;
#endif
    }

    EditorSettingsService::EditorSettingsService(Engine& engine, std::filesystem::path file,
                                                 const std::filesystem::path& forgeTokensFile) :
        m_engine(&engine), m_file(std::move(file))
    {
        load();
        loadDataPresets(forgeTokensFile);
        // a stored list is data: what is not a usable directory name, a repeat or past the cap is dropped
        for (std::string_view rest = m_settings.modulePaths; !rest.empty();)
        {
            const sizet bar = std::min(rest.find('|'), rest.size());
            std::string error;
            const std::string path = normalizeModuleSearchPath(rest.substr(0, bar), error);
            rest.remove_prefix(std::min(bar + 1, rest.size()));
            if (!path.empty() && m_modulePaths.size() < MAX_MODULE_SEARCH_PATHS &&
                std::ranges::none_of(m_modulePaths,
                                     [&](const std::string& other) { return sameModuleSearchPath(other, path); }))
            {
                m_modulePaths.push_back(path);
            }
        }
        m_settings.modulePaths = joined(m_modulePaths);
        // a settings theme or locale that does not exist is a stale file, not an error: the editor starts
        const std::string stored =
            std::exchange(m_settings.locale, loadedLocale(engine.localization(), m_settings.locale));
        if (m_settings.locale.empty())
        {
            if (!stored.empty())
            {
                SWARNING_LOG("editor settings: locale {} has no tables: keeping {}", stored,
                             engine.localization().locale());
            }
            m_settings.locale = engine.localization().locale();
        }
        if (const std::vector<std::string> densities = availableUiDensities();
            std::ranges::find(densities, m_settings.density) == densities.end())
        {
            SWARNING_LOG("editor settings: density {} does not exist: the default", m_settings.density);
            m_settings.density = std::string(DEFAULT_UI_DENSITY);
        }
        if (m_settings.dataColor = canonicalDataColor(m_settings.dataColor); m_settings.dataColor.empty())
        {
            SWARNING_LOG("editor settings: dataColor is not a preset or a #rrggbb colour: the default");
            m_settings.dataColor = std::string(DEFAULT_DATA_COLOR);
        }
        if (std::ranges::find(TELEMETRY_MODES, m_settings.telemetry) == TELEMETRY_MODES.end())
        {
            SWARNING_LOG("editor settings: telemetry {} is not {}: the default", m_settings.telemetry,
                         listed(telemetryModes()));
            m_settings.telemetry = std::string(TELEMETRY_QUIET);
        }
        if (std::ranges::find(GRID_PLANES, m_settings.gridPlane) == GRID_PLANES.end())
        {
            SWARNING_LOG("editor settings: gridPlane {} is not {}: the default", m_settings.gridPlane,
                         listed(gridPlanes()));
            m_settings.gridPlane = std::string(GRID_PLANE_AUTO);
        }
        if (std::ranges::find(GRID_SPACING_MODES, m_settings.gridSpacingMode) == GRID_SPACING_MODES.end())
        {
            SWARNING_LOG("editor settings: gridSpacingMode {} is not {}: the default", m_settings.gridSpacingMode,
                         listed(gridSpacingModes()));
            m_settings.gridSpacingMode = std::string(GRID_SPACING_INDEPENDENT);
        }
        // without a settings file the host owns the engine's display settings: only the theme is applied
        UiParts parts;
        parts.uiScale = parts.textScale = parts.reducedMotion = !m_file.empty();
        if (const std::string error = applyUi(m_settings, parts); !error.empty())
        {
            SWARNING_LOG("editor settings: {}: the default theme", error);
            m_settings.theme = std::string(DEFAULT_EDITOR_THEME);
            if (const std::string retry = applyUi(m_settings, parts); !retry.empty())
            {
                SERROR_LOG("editor settings: {}", retry);
            }
        }
        if (m_file.empty())
        {
            const UiDisplaySettings& display = engine.uiDisplay();
            m_settings.uiScale = display.uiScale;
            m_settings.textScale = display.textScale;
            m_settings.reducedMotion = display.reducedMotion;
        }
        ControlServer& control = engine.control();
        control.registerViewCommand(
            "editorSettings", OWNER,
            "sets editor settings and saves them to the user layer: the Scene panel's grid and fly boost, the editor "
            "UI's theme, spacing density and data colour, the UI and text scale, reduced motion, the language, the "
            "agent layer (what a control client changed, shown on the hierarchy, header and inspector) and the "
            "telemetry density (the status bar's readouts). The effect has "
            "landed when the reply "
            "comes (applied: true): the UIs are laid out at the scales and the density, the theme, data colour and "
            "locale are in force and the status bar shows the telemetry density and the frame budget. An unknown "
            "theme, density, data colour, telemetry or locale is refused with the valid names "
            "(editorSettingsState lists them). A view command: "
            "preferences are not scene edits and stay out of undo",
            {optionalArg("gridEnabled", ArgKind::Boolean, "draws the grid in the Scene panel"),
             optionalArg("sceneToolbar", ArgKind::Boolean,
                         "shows the Scene panel's toolbar; false leaves only the button that shows it again"),
             optionalArg("gridOpacity", ArgKind::Number, "0 to 1; 0 hides the grid"),
             optionalArg("gridSpacing", ArgKind::Number,
                         "metres between the lines of the grid's finest level (0.001 to 1e6); the coarser levels are "
                         "gridMajorEvery times wider each, and the levels shown follow the camera's distance (or an "
                         "orthographic view's height) so no line is closer than a few pixels"),
             optionalArg("gridSpacingMode", ArgKind::String,
                         "where the grid's cell comes from: independent uses gridSpacing; snap uses the gizmo's "
                         "translate step (gizmoMode translateStep) while the gizmo snaps and gridSpacing while it "
                         "does not",
                         GRID_SPACING_INDEPENDENT, GRID_SPACING_MODES),
             optionalArg("gridMajorEvery", ArgKind::Number,
                         "the ratio between a grid level and the next, a whole number 2 to 16 (10: decimal levels)"),
             optionalArg("gridPlane", ArgKind::String,
                         "the plane the grid lies in: auto is the ground (xz) in a perspective view and the axis plane "
                         "facing an orthographic view (top and bottom xz, front and back xy, left and right yz); xz, "
                         "xy and yz force one for every view",
                         GRID_PLANE_AUTO, GRID_PLANES),
             optionalArg("gridAxes", ArgKind::Boolean,
                         "highlights the plane's two axes as coloured lines (the theme's grid-axis tokens)"),
             optionalArg("gridFadeDistance", ArgKind::Number,
                         "metres from the camera at which a perspective grid is gone, fading out from half of it; 0 "
                         "never cuts the grid off (the view angle and the pixel spacing still fade it)"),
             optionalArg("flySpeed", ArgKind::Number,
                         "fly camera speed, 0.01 to 100: distance per second as a fraction of the orbit distance"),
             optionalArg("flyBoostRate", ArgKind::Number, "fly speed multiplier gained per second Shift is held"),
             optionalArg("flyBoostMax", ArgKind::Number, "cap of the held-Shift fly multiplier (at least 1)"),
             optionalArg("systemBudgetMs", ArgKind::Number,
                         "the Systems panel's cost budget in ms per frame, 0 to 1000: a system averaging more shows "
                         "its cost as a warning; 0 turns the warning off"),
             optionalArg("frameBudgetMs", ArgKind::Number,
                         "the header's frame time budget in ms, 0 to 1000: the frame time shows as a warning "
                         "once it is more than 5 percent over; 0 never warns"),
             optionalArg("telemetry", ArgKind::String,
                         "the readout density: quiet (the default) limits telemetry readouts; full shows more "
                         "detail, including the header's frame time and history",
                         TELEMETRY_QUIET, TELEMETRY_MODES),
             optionalArg("theme", ArgKind::String, "a theme of the themes list of editorSettingsState / uiThemes"),
             optionalArg("density", ArgKind::String,
                         "a density of the densities list of editorSettingsState: the spacing and bar height tokens "
                         "every panel reads (comfortable or compact); every theme has both"),
             optionalArg("dataColor", ArgKind::String,
                         "the colour of measurements, over every Forge theme's data token: a preset of the dataColors "
                         "list of editorSettingsState (ice, the themes' own, cyan, steel, bone: each has a value per "
                         "theme) or a #rrggbb hex"),
             optionalArg("uiScale", ArgKind::Number, "multiplies the display scale into the layout scale, 0.5 to 4"),
             optionalArg("textScale", ArgKind::Number, "multiplies the text sizes only, 0.5 to 4"),
             optionalArg("reducedMotion", ArgKind::Boolean, "tweens snap to their end value"),
             optionalArg("locale", ArgKind::String, "a loaded locale of the locales list of editorSettingsState"),
             optionalArg("agentLayer", ArgKind::Boolean,
                         "shows what a control client changed: the header's presence chip, the AGENT and EDITED "
                         "marks and the inspector's provenance with Revert and Keep; off hides them all (the "
                         "attribution is recorded either way)")},
            [this](const ControlArgs& args, IFormatWriter& out)
            {
                EditorSettings next = m_settings;
                if (const std::optional<std::string> theme = args.string("theme"))
                {
                    const std::vector<std::string> themes = availableUiThemes();
                    if (std::ranges::find(themes, *theme) == themes.end())
                    {
                        return std::format("editorSettings: no theme '{}'; valid: {}", *theme, listed(themes));
                    }
                    next.theme = *theme;
                }
                if (const std::optional<std::string> density = args.string("density"))
                {
                    const std::vector<std::string> densities = availableUiDensities();
                    if (std::ranges::find(densities, *density) == densities.end())
                    {
                        return std::format("editorSettings: no density '{}'; valid: {}", *density, listed(densities));
                    }
                    next.density = *density;
                }
                if (const std::optional<std::string> telemetry = args.string("telemetry"))
                {
                    if (std::ranges::find(TELEMETRY_MODES, *telemetry) == TELEMETRY_MODES.end())
                    {
                        return std::format("editorSettings: no telemetry '{}'; valid: {}", *telemetry,
                                           listed(telemetryModes()));
                    }
                    next.telemetry = *telemetry;
                }
                // the server refuses a gridPlane or gridSpacingMode outside the declared choices
                if (const std::optional<std::string> plane = args.string("gridPlane"))
                {
                    next.gridPlane = *plane;
                }
                if (const std::optional<std::string> mode = args.string("gridSpacingMode"))
                {
                    next.gridSpacingMode = *mode;
                }
                if (const std::optional<bool> axes = args.boolean("gridAxes"))
                {
                    next.gridAxes = *axes;
                }
                if (const std::optional<std::string> color = args.string("dataColor"))
                {
                    next.dataColor = canonicalDataColor(*color);
                    if (next.dataColor.empty())
                    {
                        return std::format("editorSettings: no dataColor '{}'; valid: {} or #rrggbb", *color,
                                           listed(m_dataPresetNames));
                    }
                }
                if (const std::optional<std::string> locale = args.string("locale"))
                {
                    next.locale = loadedLocale(m_engine->localization(), *locale);
                    if (next.locale.empty())
                    {
                        return std::format("editorSettings: no locale '{}'; valid: {}", *locale,
                                           listed(m_engine->localization().locales()));
                    }
                }
                if (const std::optional<bool> reduced = args.boolean("reducedMotion"))
                {
                    next.reducedMotion = *reduced;
                }
                if (!scale(args, "uiScale", UiDisplaySettings::MIN_UI_SCALE, UiDisplaySettings::MAX_UI_SCALE,
                           next.uiScale) ||
                    !scale(args, "textScale", UiStyleSheet::MIN_TEXT_SCALE, UiStyleSheet::MAX_TEXT_SCALE,
                           next.textScale))
                {
                    return std::string("editorSettings: uiScale and textScale are finite numbers above 0 (clamped to "
                                       "0.5 to 4)");
                }
                if (const std::optional<bool> enabled = args.boolean("gridEnabled"))
                {
                    next.gridEnabled = *enabled;
                }
                if (const std::optional<bool> shown = args.boolean("sceneToolbar"))
                {
                    next.sceneToolbar = *shown;
                }
                if (const std::optional<bool> layer = args.boolean("agentLayer"))
                {
                    next.agentLayer = *layer;
                }
                for (const Range& range : RANGES)
                {
                    if (!number(args, range.key, range.min, range.max, next.*range.member))
                    {
                        return std::string("editorSettings: gridOpacity is 0 to 1, gridSpacing is a positive number, "
                                           "gridMajorEvery is 2 to 16, gridFadeDistance is 0 or more, flySpeed is "
                                           "0.01 to 100, flyBoostRate is 0 to 1000, flyBoostMax is 1 to 1000, "
                                           "systemBudgetMs and frameBudgetMs are 0 to 1000");
                    }
                }
                next.gridMajorEvery = std::round(next.gridMajorEvery);
                next.modulePaths = m_settings.modulePaths;
                // only what the request names is applied: a live override (uiTheme, uiDisplay, setLocale) of a
                // setting it leaves out stays
                const auto named = [&args](const std::string_view key) { return args.json(key).has_value(); };
                const UiParts parts{named("theme"),     named("density"),       named("dataColor"), named("uiScale"),
                                    named("textScale"), named("reducedMotion"), named("locale")};
                if (const std::string error = applyUi(next, parts); !error.empty())
                {
                    return "editorSettings: " + error;
                }
                m_settings = next;
                for (const auto& [id, listener] : m_applied)
                {
                    listener(m_settings);
                }
                write(out, m_settings);
                out.writeBool("applied", true);
                return save();
            });
        control.registerViewCommand(
            "moduleSearchPathAdd", OWNER,
            "adds a directory the module browser lists module DLLs from and saves it to the editor settings; an "
            "absolute path, at most 16, not checked for existence (the browser reports a missing one)",
            {requiredPath("path", HOST_ROOT, "an absolute directory")},
            [this](const ControlArgs& args, IFormatWriter& out)
            {
                std::string error;
                const std::string path = normalizeModuleSearchPath(args.string("path").value_or(std::string{}), error);
                if (path.empty())
                {
                    return "moduleSearchPathAdd: " + error;
                }
                if (std::ranges::any_of(m_modulePaths,
                                        [&](const std::string& other) { return sameModuleSearchPath(other, path); }))
                {
                    return "moduleSearchPathAdd: " + path + " is already listed";
                }
                if (m_modulePaths.size() >= MAX_MODULE_SEARCH_PATHS)
                {
                    return std::format("moduleSearchPathAdd: at most {} directories", MAX_MODULE_SEARCH_PATHS);
                }
                m_modulePaths.push_back(path);
                return changedPaths(out);
            });
        control.registerViewCommand(
            "moduleSearchPathRemove", OWNER, "removes a directory from the module browser's search list",
            {requiredPath("path", HOST_ROOT, "a listed directory")},
            [this](const ControlArgs& args, IFormatWriter& out)
            {
                std::string error;
                const std::string path = normalizeModuleSearchPath(args.string("path").value_or(std::string{}), error);
                const auto it = std::ranges::find_if(m_modulePaths, [&](const std::string& other)
                                                     { return !path.empty() && sameModuleSearchPath(other, path); });
                if (it == m_modulePaths.end())
                {
                    return "moduleSearchPathRemove: " + args.string("path").value_or(std::string{}) + " is not listed";
                }
                m_modulePaths.erase(it);
                return changedPaths(out);
            });
        control.registerQuery(
            "editorSettingsState", OWNER,
            "the editor settings; the file they save to (empty: not persisted) and loadError, why the file was not "
            "taken at startup (empty: taken or missing); the valid themes and locales and the scale limits; live, what "
            "is "
            "in force now",
            {},
            [this](const ControlArgs&, IFormatWriter& out)
            {
                write(out, m_settings);
                writePaths(out);
                out.writeU32("version", m_settings.version);
                out.writeString("file", m_file.string());
                out.writeString("loadError", m_loadError);
                writeNames(out, "themes", availableUiThemes());
                writeNames(out, "densities", availableUiDensities());
                out.beginArray("telemetryModes", static_cast<u32>(TELEMETRY_MODES.size()));
                for (const std::string_view mode : TELEMETRY_MODES)
                {
                    out.writeString("", mode);
                }
                out.endArray();
                out.beginArray("dataColors", static_cast<u32>(m_dataPresetNames.size())); // the file's order
                for (const std::string& name : m_dataPresetNames)
                {
                    out.writeString("", name);
                }
                out.endArray();
                writeNames(out, "locales", m_engine->localization().locales());
                out.writeF32("uiScaleMin", UiDisplaySettings::MIN_UI_SCALE);
                out.writeF32("uiScaleMax", UiDisplaySettings::MAX_UI_SCALE);
                out.writeF32("textScaleMin", UiStyleSheet::MIN_TEXT_SCALE);
                out.writeF32("textScaleMax", UiStyleSheet::MAX_TEXT_SCALE);
                // what is in force now: a uiTheme, uiDisplay or setLocale override differs from the stored values
                const UiDisplaySettings& display = m_engine->uiDisplay();
                out.beginObject("live");
                out.writeString("theme", editorUi(*m_engine).uiView().stylesheet().theme().toView());
                out.writeString("density", editorUi(*m_engine).uiView().stylesheet().density().toView());
                const std::vector<UiTokenOverride>& overrides =
                    editorUi(*m_engine).uiView().stylesheet().tokenOverrides();
                const auto data = std::ranges::find(overrides, InternedString("data"), &UiTokenOverride::name);
                out.writeString("dataColor", data == overrides.end() ? std::string{} : hexOf(data->color));
                out.writeString("locale", m_engine->localization().locale());
                out.writeF32("uiScale", display.uiScale);
                out.writeF32("textScale", display.textScale);
                out.writeBool("reducedMotion", display.reducedMotion);
                out.endObject();
                return std::string{};
            });
    }

    EditorSettingsService::~EditorSettingsService() { m_engine->control().unregisterOwner(OWNER); }

    u32 EditorSettingsService::onApplied(std::function<void(const EditorSettings&)> listener)
    {
        m_applied.emplace_back(++m_appliedSerial, std::move(listener));
        return m_appliedSerial;
    }

    void EditorSettingsService::removeApplied(const u32 id)
    {
        std::erase_if(m_applied, [id](const auto& entry) { return entry.first == id; });
    }

    void EditorSettingsService::writePaths(IFormatWriter& out) const
    {
        out.beginArray("modulePaths", static_cast<u32>(m_modulePaths.size()));
        for (const std::string& path : m_modulePaths)
        {
            out.writeString("", path);
        }
        out.endArray();
    }

    std::string EditorSettingsService::changedPaths(IFormatWriter& out)
    {
        m_settings.modulePaths = joined(m_modulePaths);
        ++m_modulePathRevision;
        writePaths(out);
        return save();
    }

    std::string EditorSettingsService::save()
    {
        if (m_file.empty())
        {
            return {};
        }
        std::string text;
        serializeJson(EditorSettings::spiteSchema(), &m_settings, text);
        if (const auto saved = writeTextFileAtomically(m_file, text); !saved)
        {
            return std::string(saved.error().message.c_str());
        }
        return {};
    }

    void EditorSettingsService::load()
    {
        if (m_file.empty() || !isRegularFile(m_file).value_or(false))
        {
            return;
        }
        const auto rejected = [this](const std::string& why)
        {
            m_settings = {};
            m_loadError = why;
            SWARNING_LOG("editor settings: {}: {}: defaults, the file stays until a setting changes", m_file.string(),
                         why);
        };
        const Expected<uintmax_t> size = fileSize(m_file);
        if (!size || *size > MAX_EDITOR_SETTINGS_BYTES)
        {
            return rejected(size ? std::format("{} bytes, at most {}", *size, MAX_EDITOR_SETTINGS_BYTES)
                                 : std::string(size.error().message.c_str()));
        }
        const Expected<std::string> text = readTextFile(m_file);
        if (!text)
        {
            return rejected(std::string(text.error().message.c_str()));
        }
        std::string error;
        std::vector<FieldIssue> issues;
        if (!deserializeJson(EditorSettings::spiteSchema(), &m_settings, *text, error, &issues))
        {
            return rejected("not settings (" + error + ")");
        }
        for (const FieldIssue& issue : issues)
        {
            SWARNING_LOG("editor settings: {}: {}: {}: the default", m_file.string(), issue.path, issue.error);
            m_loadError = std::format("{}: {}", issue.path, issue.error);
        }
        for (const Range& range : RANGES)
        {
            const float value = m_settings.*range.member;
            if (!std::isfinite(value) || value < range.min || value > range.max)
            {
                SWARNING_LOG("editor settings: {}: {} {} is outside {} to {}: the default", m_file.string(), range.key,
                             value, range.min, range.max);
                m_loadError = std::format("{}: {} is outside {} to {}", range.key, value, range.min, range.max);
                m_settings.*range.member = EditorSettings{}.*range.member;
            }
        }
        if (m_settings.version > EDITOR_SETTINGS_VERSION)
        {
            SWARNING_LOG("editor settings: {} is version {}, newer than {}: unknown keys are dropped at the next save",
                         m_file.string(), m_settings.version, EDITOR_SETTINGS_VERSION);
        }
        migrateEditorSettings(m_settings);
        m_settings.gridMajorEvery = std::round(m_settings.gridMajorEvery);
    }

    std::string EditorSettingsService::applyUi(EditorSettings& next, const UiParts parts) const
    {
        if (parts.theme || parts.density || parts.dataColor)
        {
            // what the request leaves out stays as it is live (a uiTheme override too); the data colour is the
            // setting's, resolved for whichever theme results
            UiStyleSheet& sheet = editorUi(*m_engine).uiView().stylesheet();
            const std::string theme = parts.theme ? next.theme : std::string(sheet.theme().toView());
            const std::string density = parts.density ? next.density : std::string(sheet.density().toView());
            const std::vector<UiTokenOverride> overrides = dataOverrides(next.dataColor, theme);
            if (sheet.theme().toView() != theme || sheet.density().toView() != density ||
                sheet.tokenOverrides() != overrides)
            {
                const std::filesystem::path path = uiThemeFile(theme);
                if (const auto set =
                        sheet.setTheme(InternedString(theme), std::span(&path, 1), uiThemeDensity(density), overrides);
                    !set)
                {
                    return std::format("cannot load theme '{}' at density '{}': {}", theme, density,
                                       set.error().message);
                }
                editorUi(*m_engine).uiView().settleStyle(); // a density moves the layout: in force at the reply
            }
        }
        if (parts.uiScale || parts.textScale || parts.reducedMotion)
        {
            UiDisplaySettings& display = m_engine->uiDisplay();
            if (parts.uiScale)
            {
                display.uiScale = next.uiScale;
            }
            if (parts.textScale)
            {
                display.textScale = next.textScale;
            }
            if (parts.reducedMotion)
            {
                display.reducedMotion = next.reducedMotion;
            }
            m_engine->settleUiDisplay();
            if (parts.uiScale)
            {
                next.uiScale = display.uiScale; // clamped
            }
            if (parts.textScale)
            {
                next.textScale = display.textScale;
            }
        }
        if (parts.locale && !next.locale.empty())
        {
            m_engine->localization().setLocale(next.locale);
        }
        return {};
    }

    void EditorSettingsService::loadDataPresets(const std::filesystem::path& file)
    {
        using Json = nlohmann::ordered_json;
        const Expected<std::string> text = readTextFile(file);
        const Json tokens = text ? Json::parse(*text, nullptr, false) : Json{};
        if (!tokens.is_object() || !tokens.contains("dataPresets") || !tokens["dataPresets"].is_object())
        {
            SWARNING_LOG("editor settings: {} has no dataPresets: only #rrggbb data colours", file.string());
            return;
        }
        for (const auto& [name, variants] : tokens["dataPresets"].items())
        {
            for (const auto& [variant, hex] : variants.items())
            {
                if (const std::string color =
                        variants.is_object() && hex.is_string() ? canonicalHex(hex.get<std::string>()) : std::string{};
                    !color.empty())
                {
                    m_dataPresets[name][variant] = color;
                }
            }
            if (m_dataPresets.contains(name))
            {
                m_dataPresetNames.push_back(name);
            }
        }
        if (tokens.contains("themes") && tokens["themes"].is_object())
        {
            for (const auto& [variant, theme] : tokens["themes"].items())
            {
                m_variantThemes[variant] = theme.is_string() ? theme.get<std::string>() : std::string{};
            }
        }
    }

    std::string EditorSettingsService::canonicalDataColor(const std::string_view text) const
    {
        if (text == DEFAULT_DATA_COLOR || m_dataPresets.contains(std::string(text)))
        {
            return std::string(text);
        }
        return canonicalHex(text);
    }

    std::vector<UiTokenOverride> EditorSettingsService::dataOverrides(const std::string& dataColor,
                                                                      const std::string& theme) const
    {
        std::string hex = canonicalHex(dataColor);
        if (hex.empty())
        {
            const auto preset = m_dataPresets.find(dataColor);
            if (dataColor == DEFAULT_DATA_COLOR || preset == m_dataPresets.end())
            {
                return {};
            }
            const auto variant =
                std::ranges::find_if(m_variantThemes, [&](const auto& entry) { return entry.second == theme; });
            const auto value = preset->second.find(variant == m_variantThemes.end() ? "dark" : variant->first);
            if (value == preset->second.end())
            {
                return {};
            }
            hex = value->second;
        }
        return {{InternedString("data"), rgba8(hex)}};
    }
} // namespace spite
