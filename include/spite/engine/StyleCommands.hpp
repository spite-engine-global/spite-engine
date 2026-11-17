#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "engine/EngineToolsExport.hpp"
#include "uiview/UiStyleSheet.hpp"

namespace spite
{
    class ControlServer;

    // the world name a request naming none acts on
    using DefaultUiWorldFn = std::function<std::string()>;

    // The theme names under assets/ui/themes, sorted (what uiThemes lists), and the layer file of one.
    SPITE_ENGINE_TOOLS_API std::vector<std::string> availableUiThemes();
    SPITE_ENGINE_TOOLS_API std::filesystem::path uiThemeFile(const std::string& name);
    // The densities (UiThemeDensity): DEFAULT_UI_DENSITY, the base sheet's own tokens, then one per
    // assets/ui/density/<name>.json token layer, sorted; and one by name (no file for the default).
    SPITE_ENGINE_TOOLS_API std::vector<std::string> availableUiDensities();
    SPITE_ENGINE_TOOLS_API UiThemeDensity uiThemeDensity(const std::string& name);

    // Registers on control (owner "engine"), acting on the UiView of the world named by the "world" arg
    // (default: defaultWorld()); the layer and class tables are the world's UiView service tables styleLayers and
    // styleDefs:
    //   uiStyle entity [state] [world] -> the element's resolved style: id, kind, styleClass, state and per field
    //     {value, level, layer, stateVariant}, level naming where it resolved (element, class, kind, defaults,
    //     builtIn) and layer the stylesheet layer that set it
    //   loadLayer name path [world] -> layers: adds the named layer over the loaded ones or reloads it in place
    //   unloadLayer name [world] -> layers: removes a layer added by loadLayer
    //   uiTheme name [density] [world] -> theme, density, layers: makes assets/ui/themes/<name>.json the theme layer
    //     in one generation, with the density's token layer over it (the current density when none is named)
    //   uiThemes [world] -> active, density, available, densities: the active theme and density, the theme files
    //     under assets/ui/themes and the densities (the default, then assets/ui/density)
    //   uiLayers [world] -> theme, layers [{name, path, theme, classes, kinds, tokens, elements}]: the stylesheet
    //     layers in order, the base sheet first
    //   uiFonts [world] -> glyphBudgetMs, glyphsGenerated, waitingElements and per font name, fallbacks, glyphs
    //     {ready, blank, pending, failed, absent} and pages {atlasPage, usedRows, version, resolved}
    // uiStyle also names the active theme and, per field, the "$token" the value was written as, the layer that
    // defines it (tokenLayer), true as tokenOverridden when a setting (editorSettings dataColor) replaced the theme
    // file's value, and, for an alias, the names followed (tokenChain).
    SPITE_ENGINE_TOOLS_API void registerStyleCommands(ControlServer& control, DefaultUiWorldFn defaultWorld);
} // namespace spite
