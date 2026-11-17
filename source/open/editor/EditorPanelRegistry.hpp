#pragma once

#include <climits>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/MathTypes.hpp"
#include "editor/EditorExport.hpp"
#include "ui/UiWidgets.hpp"

// The editor panel registry: a panel declares itself from its own file, so adding one never edits
// EditorShell.cpp. The shell reads the registry once, when it is constructed, and takes from it the Window menu, the
// tabs, the default dock layout (and with it Reset Layout), the panels' construction and their per-frame update.
//
// To add a panel "systems":
//  1. Editor<Systems>.hpp/.cpp: the class, with `Widget content() const` and `void update()` (the document view,
//     control entries and queries are the panel's own, see EditorPanelUi.hpp), constructed from the
//     EditorPanelContext it needs.
//  2. At the end of Editor<Systems>.cpp, in an anonymous namespace, one registrar:
//         const EditorPanelRegistrar REGISTER{{.name = "systems",
//                                              .order = 140,
//                                              .dock = {.target = "project"},
//                                              .zone = "EditorSystems",
//                                              .make = [](EditorPanelContext& c)
//                                              { return makeEditorPanel<EditorSystems>(c.engine, c.settings); }}};
//     `name` is the dock panel id and the Window menu item `window.systems`; `order` places the panel in the Window
//     menu and among the tabs (ascending; the default appends it); `dock` says where the default layout docks it
//     (see EditorDockPlacement); `zone` names the timed zone of its update.
//  3. Add the .cpp and .hpp to source/editor/SpiteEditor.vcxproj.
//  4. assets/loc/en/editor/systems.json holds the panel's strings, the namespace "editor-systems": the tab title is
//     its key "title" (editor-systems.title); more keys are yours ("editor-systems.empty").
//  5. Validate new generic behavior through synthetic fixtures; a product panel needs no bespoke scenario.
// Nothing else changes: not EditorShell.cpp, not the shared editor.json.
//
// Keyboard and gamepad: the dock makes every panel host a landmark, so F6, Ctrl+Tab and the shoulder
// buttons visit the panel while it is shown and holds a tab stop, entering at its first tab stop: that control, in
// paint order, is the panel's entry control, so lay the panel out with it first and name it (setAccessibleName(Key)).
// A panel with no stop of its own (a read-only list) is skipped; give it one rather than a second focus concept. A
// panel with regions marks each region's container setLandmark(true) (the Profiler's list, graph, panes, timeline).
// A test registered like SpiteEditorPanelRegistryTests' ARegisteredPanelIsALandmarkEnteredAtItsFirstStop covers it.
//
// Registration is static: a registrar object in each panel's translation unit, which runs when SpiteEditor.dll loads
// (every .cpp of the project is linked into the DLL, so none is dropped as unreferenced). Static initialisation
// order across files is unspecified, so nothing depends on it: the registry sorts by (order, name), and a panel
// reaches a service the shell owns through the EditorPanelContext, not through another panel's static. A test
// registers before it constructs the EditorShell and unregisters after.
namespace spite
{
    class Engine;
    class EditorAgentLayer;
    class EditorDialogs;
    class EditorFeedback;
    class EditorInspector;
    class EditorModel;
    class EditorSettingsService;
    class UiScope;

    // What a panel's builder may use: the engine, the editor's UI scope, the editor model (the selection and
    // the state commands change; a panel reads it and sends commands), the services the shell owns (the inspector,
    // which the shell updates before every panel, the editor settings, the feedback: toasts and running jobs, see
    // EditorFeedback.hpp, and the dialogs: confirm, prompt and file, see EditorDialogs.hpp) and the content of a panel
    // built before it.
    struct EditorPanelContext
    {
        Engine& engine;
        UiScope& scope;
        const DockSpace& dock;
        const EditorModel& model;
        EditorInspector& inspector; // its own panel's content only: the selection is the model's
        const EditorSettingsService& settings;
        EditorFeedback& feedback;
        EditorDialogs& dialogs;
        // the content of the named panel when it is built (see EditorPanelDesc::buildOrder), else an invalid widget
        std::function<Widget(std::string_view)> contentOf;
        // Adds or focuses a scene file through the File menu's Open Recent flow.
        std::function<void(std::string_view path)> openScene;
        // what control clients changed (the hierarchy's marks, the header chip): see EditorAgentLayer.hpp
        EditorAgentLayer& agent;
    };

    // A built panel: its dock content, its per-frame update (empty: none), what keeps it alive and its tab-bar tag
    // (invalid: none). The shell destroys panels in reverse build order, before the services of the
    // context.
    struct EditorPanel
    {
        Widget content;
        std::function<void()> update;
        std::shared_ptr<void> owner;
        Widget tag;
    };

    // Constructs a panel class T (`Widget content() const`, `void update()`, and `Widget tag() const` for a panel
    // that has a tab-bar tag: DockSpace::setPanelTag) and wraps it.
    template <class T, class... Args>
    EditorPanel makeEditorPanel(Args&&... args)
    {
        auto panel = std::make_shared<T>(std::forward<Args>(args)...);
        Widget tag;
        if constexpr (requires { panel->tag(); })
        {
            tag = panel->tag();
        }
        return {panel->content(), [raw = panel.get()] { raw->update(); }, panel, tag};
    }

    // Where the default layout docks a panel: in the tab group of `target` (center) or beside it (left, right, top,
    // bottom, taking `ratio` of the space); an empty target is the dock's root.
    struct EditorDockPlacement
    {
        std::string target;
        UiDockZone zone = UiDockZone::Center;
        float ratio = 0.5f;
        // docked after this panel (and its own chain) is, when the layout depends on the order of the ops: a bottom
        // panel docked before a side panel spans the side panel's height too
        std::string after;
        // the tab of its group the default layout shows
        bool activate = false;
    };

    struct EditorPanelDesc
    {
        static constexpr int AS_ORDER = INT_MIN;

        std::string
            name; // the dock panel id, the Window menu item "window.<name>" and the loc namespace "editor-<name>"
        std::string category; // empty keeps the panel at the top of the Window menu
        int order = 1000; // the Window menu and tab order, ascending; ties by name
        // The order panels are constructed and updated in; AS_ORDER: the same as `order`. Entity creation order is
        // visible in uiTree ids, so the panels that predate the registry keep theirs.
        int buildOrder = AS_ORDER;
        EditorDockPlacement dock;
        Vec2 minSize{0.0f, 0.0f}; // the least content a dock region showing the panel may have (UI pixels)
        // more height on top of minSize.y, in the density's bars and rows (size.bar, size.row), so it follows the
        // density
        Vec2 minBarsRows{0.0f, 0.0f};
        const char* zone = nullptr; // the timed zone of the update, a pointer to static storage
        // The render view whose target the panel shows as an image (the shell owns the image, the sizing of the view
        // and the Game panel's focus); then `make` is empty.
        std::optional<u32> view;
        std::function<EditorPanel(EditorPanelContext&)> make;
        bool openByDefault = true;
    };

    // The registry has no lock by design: registration runs on the main thread (static initialisation, or a test
    // before it constructs the shell) and the shell reads it from the main thread. A duplicate name logs an error
    // and keeps the first registration, in Debug and Release alike.
    SPITE_EDITOR_API void registerEditorPanel(EditorPanelDesc desc);
    SPITE_EDITOR_API bool unregisterEditorPanel(std::string_view name);
    // a copy, sorted by (order, name)
    SPITE_EDITOR_API std::vector<EditorPanelDesc> editorPanels();

    struct EditorPanelRegistrar
    {
        explicit EditorPanelRegistrar(EditorPanelDesc desc) { registerEditorPanel(std::move(desc)); }
    };
} // namespace spite
