#pragma once

#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/Schema.hpp"
#include "uiview/UiStyleSheet.hpp"

namespace spite
{
    class Engine;
    class IFormatWriter;

    // 2: the grid has levels and a plane; gridFadeDistance 0 means no cut-off and its default moved from
    // 100 to 1000 (migrateEditorSettings).
    inline constexpr u32 EDITOR_SETTINGS_VERSION = 2;
    inline constexpr sizet MAX_EDITOR_SETTINGS_BYTES = 64 * 1024;
    inline constexpr std::string_view DEFAULT_EDITOR_THEME = "forge-dark";
    // The Forge themes' own data colour (the ice preset): no token override.
    inline constexpr std::string_view DEFAULT_DATA_COLOR = "ice";
    // Telemetry density: quiet shows a readout only when it crosses its budget, belongs to the selection or
    // is hovered; full shows every readout.
    inline constexpr std::string_view TELEMETRY_QUIET = "quiet";
    inline constexpr std::string_view TELEMETRY_FULL = "full";
    inline constexpr std::array<std::string_view, 2> TELEMETRY_MODES{TELEMETRY_QUIET, TELEMETRY_FULL};
    // The plane the Scene grid lies in, by SceneViewOverlay::Grid::Plane: auto is the ground in a
    // perspective view and the axis plane facing the view in an orthographic one.
    inline constexpr std::string_view GRID_PLANE_AUTO = "auto";
    inline constexpr std::array<std::string_view, 4> GRID_PLANES{GRID_PLANE_AUTO, "xz", "xy", "yz"};
    // Where the grid's cell comes from: independent is gridSpacing; snap is the gizmo's translate step while the gizmo
    // snaps (gridSpacing otherwise).
    inline constexpr std::string_view GRID_SPACING_INDEPENDENT = "independent";
    inline constexpr std::string_view GRID_SPACING_SNAP = "snap";
    inline constexpr std::array<std::string_view, 2> GRID_SPACING_MODES{GRID_SPACING_INDEPENDENT, GRID_SPACING_SNAP};

    // The editor's user settings, persisted as the settings file in the user layer.
    struct EditorSettings
    {
        u32 version = EDITOR_SETTINGS_VERSION; // a file with a higher one is read for the keys known here

        bool gridEnabled = true;
        bool sceneToolbar = true; // the Scene view's toolbar shows; hidden, it is one button that brings it back
        float gridOpacity = 0.6f; // 0 hides the grid
        float gridSpacing = 1.0f; // metres between the lines of the finest level at its base; coarser levels follow
        std::string gridSpacingMode = std::string(GRID_SPACING_INDEPENDENT);
        float gridMajorEvery = 10.0f; // the ratio between a level and the next, a whole number 2 to 16
        std::string gridPlane = std::string(GRID_PLANE_AUTO);
        bool gridAxes = true; // the plane's two axes are highlighted
        float gridFadeDistance = 1000.0f; // metres from the camera at which a perspective grid is gone; 0: never

        float flySpeed = 0.6f; // fly distance per second, as a fraction of the camera's orbit distance
        float flyBoostRate = 4.5f; // fly speed multiplier gained per second Shift is held
        float flyBoostMax = 20.0f; // the multiplier's cap

        // The module browser's extra search directories, absolute paths joined by '|' (a character no Windows path
        // holds): the schema has no string list. Read through EditorSettingsService::moduleSearchPaths.
        std::string modulePaths;

        // Appearance and language: applied to the editor's UI at startup and by every change. The scales take the
        // uiDisplay limits (0.5 to 4); the locale is one the Localization has tables for.
        std::string theme = std::string(DEFAULT_EDITOR_THEME);
        // The spacing token set the editor reads over its theme (assets/ui/density): comfortable is the base sheet's.
        std::string density = std::string(DEFAULT_UI_DENSITY);
        // The colour of measurements: a preset of assets/ui/forge/tokens.json (ice, cyan, steel, bone: each
        // has a value per Forge theme) or a "#rrggbb" hex (the same in every theme), put over the themes' data token.
        std::string dataColor = std::string(DEFAULT_DATA_COLOR);
        float uiScale = 1.0f;
        float textScale = 1.0f;
        bool reducedMotion = false;
        std::string locale;
        // The agent layer: the presence chip, the AGENT and EDITED marks and the changed-field
        // provenance. Off hides every one of them; the attribution itself is recorded either way.
        bool agentLayer = true;
        // The Systems panel's cost budget: a system whose average cost per frame (ms) is over it shows its cost as a
        // warning. 0 turns the warning off.
        float systemBudgetMs = 2.0f;
        // Telemetry density: "quiet" or "full" (the status bar's readouts follow it). The frame budget (ms) is what the
        // status bar's frame time is measured against: over it by more than 5 percent the value shows as a warning,
        // within it quiet shows the FRAME OK lamp. 0 never warns.
        std::string telemetry = std::string(TELEMETRY_QUIET);
        float frameBudgetMs = 16.6f;

        SPITE_SCHEMA(EditorSettings, version, gridEnabled, sceneToolbar, gridOpacity, gridSpacing, gridSpacingMode,
                     gridMajorEvery, gridPlane, gridAxes, gridFadeDistance, flySpeed, flyBoostRate, flyBoostMax,
                     modulePaths, theme, density, dataColor, uiScale, textScale, reducedMotion, locale, agentLayer,
                     systemBudgetMs, telemetry, frameBudgetMs)
    };

    // Brings a settings file read at an older version to the current one (a file of the current version is left
    // as it is): the version 1 fade distance of 100 m was the only reach of the grid and its default.
    void migrateEditorSettings(EditorSettings& settings);

    // The cell the grid's finest level starts from: the gizmo's translate step when the settings follow the snap and
    // the gizmo snaps to a positive step, else gridSpacing.
    [[nodiscard]] float effectiveGridSpacing(const EditorSettings& settings, bool snapOn, float snapStep);

    inline constexpr sizet MAX_MODULE_SEARCH_PATHS = 16;
    inline constexpr sizet MAX_MODULE_SEARCH_PATH_BYTES = 1024;

    // The stored form is absolute, normalized UTF-8, with a trailing separator only for a filesystem root.
    // Validation is lexical. Invalid input returns an empty path and sets error.
    [[nodiscard]] std::string normalizeModuleSearchPath(std::string_view path, std::string& error);
    // Compare stored path names, ignoring case on Windows.
    [[nodiscard]] bool sameModuleSearchPath(std::string_view a, std::string_view b);

    // Loads the settings file (defaults when missing, over MAX_EDITOR_SETTINGS_BYTES or not settings, with the reason
    // in the log and the state's loadError; unknown keys are ignored, missing and malformed ones keep the default; the
    // file is not rewritten until a setting changes), applies the settings (the editor
    // UI's theme and density, the engine's UI display settings, the locale) and serves the editorSettings command (sets
    // the given fields, applies them before it replies, saves the file), the editorSettingsState query and the module
    // search path commands. A theme, density or locale the settings name that does not exist falls back (the default
    // theme and density, the current locale). An empty file persists nothing.
    class EditorSettingsService
    {
    public:
        EditorSettingsService(Engine& engine, std::filesystem::path file, const std::filesystem::path& forgeTokensFile);
        ~EditorSettingsService();
        EditorSettingsService(const EditorSettingsService&) = delete;
        EditorSettingsService& operator=(const EditorSettingsService&) = delete;

        [[nodiscard]] const EditorSettings& settings() const { return m_settings; }
        // The extra module directories (commands moduleSearchPathAdd and moduleSearchPathRemove), in the order added.
        [[nodiscard]] const std::vector<std::string>& moduleSearchPaths() const { return m_modulePaths; }
        // Bumped by every change of moduleSearchPaths.
        [[nodiscard]] u64 moduleSearchPathRevision() const { return m_modulePathRevision; }
        // Called by every editorSettings that landed, after the settings are in force and before the reply, in the
        // order added: the status bar and the inspector settle their readouts there, like the UI settles the theme and
        // the scales. The id removes it: whoever adds one removes it before what it captures goes (the service is the
        // editor model's and outlives every part of the shell).
        u32 onApplied(std::function<void(const EditorSettings&)> listener);
        void removeApplied(u32 id);

    private:
        void load();
        void loadDataPresets(const std::filesystem::path& file);
        // The UI-facing settings applyUi puts in force.
        struct UiParts
        {
            bool theme = true;
            bool density = true;
            bool dataColor = true;
            bool uiScale = true;
            bool textScale = true;
            bool reducedMotion = true;
            bool locale = true;
        };
        // The data token overrides dataColor means under a theme: none for the themes' own value (ice).
        [[nodiscard]] std::vector<UiTokenOverride> dataOverrides(const std::string& dataColor,
                                                                 const std::string& theme) const;
        // The canonical dataColor of a preset name or a #rrggbb hex; empty for anything else.
        [[nodiscard]] std::string canonicalDataColor(std::string_view text) const;
        // Applies the parts of next: the theme, density and data colour (one stylesheet generation; the only step
        // that can fail: the error, and nothing changed), then the display settings (next gets the clamped scales) and
        // the locale.
        std::string applyUi(EditorSettings& next, UiParts parts) const;
        // Writes the file; the error, empty on success.
        std::string save();
        void writePaths(IFormatWriter& out) const;
        // A list edit applied: stored, announced to the browser, saved; the error, empty on success.
        std::string changedPaths(IFormatWriter& out);

        Engine* m_engine;
        std::filesystem::path m_file;
        EditorSettings m_settings;
        std::vector<std::string> m_modulePaths;
        // assets/ui/forge/tokens.json: the data presets (name, then the hex per theme variant) and the theme of each
        // variant; empty when the file is unreadable
        std::map<std::string, std::map<std::string, std::string>> m_dataPresets;
        std::vector<std::string> m_dataPresetNames; // in the file's order: the design system's (ice, cyan, steel, bone)
        std::map<std::string, std::string> m_variantThemes;
        u64 m_modulePathRevision = 0;
        std::string m_loadError;
        std::vector<std::pair<u32, std::function<void(const EditorSettings&)>>> m_applied;
        u32 m_appliedSerial = 0;
    };
} // namespace spite
