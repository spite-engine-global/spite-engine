#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class EditorDialogs;
    class EditorSettingsService;
    class Widget;

    // The Preferences panel (the dock panel "settings"): the editor settings (EditorSettingsService) as property-form
    // sections (Appearance, Language, Scene View), the Hotkeys link to the Input panel and the module search paths with
    // add and remove. A client of the editorSettings and moduleSearchPath commands and the editorSettingsState query,
    // so its edits are commands and what it shows follows them; the query settingsView reads its rows.
    class EditorSettingsPanel
    {
    public:
        EditorSettingsPanel(Engine& engine, EditorDialogs& dialogs, const EditorSettingsService& settings);
        ~EditorSettingsPanel();
        EditorSettingsPanel(const EditorSettingsPanel&) = delete;
        EditorSettingsPanel& operator=(const EditorSettingsPanel&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
