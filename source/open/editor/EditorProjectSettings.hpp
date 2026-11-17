#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class EditorDialogs;
    class EditorFeedback;
    class Widget;

    // The Project Settings panel (the dock panel "projectSettings"): a page per config section and declared file
    // (Graphics, Timing, Window, Input, Modules, Physics, UI Navigation), built from the `config` query's schema as
    // property-form rows with units, ranges, docs, the source of the value (default, project or user file, unsaved)
    // and a Restart required badge. A client of the config commands: an edit is setConfig, a field's Reset
    // resetConfig, Save saveConfig, so the console and an LLM edit the same fields and Undo takes an edit back. The
    // query projectSettingsView and the command projectSettingsPage read and drive it also while it is hidden.
    // A search box lists the matching fields of every page (projectSettingsFilter) and the Changes page those that
    // differ from the default or the file (the editor model's projectSettingsDiff lists the same); a group of fields
    // folds (projectSettingsGroup), the state kept in the panel.
    class EditorProjectSettings
    {
    public:
        EditorProjectSettings(Engine& engine, EditorDialogs& dialogs, EditorFeedback& feedback);
        ~EditorProjectSettings();
        EditorProjectSettings(const EditorProjectSettings&) = delete;
        EditorProjectSettings& operator=(const EditorProjectSettings&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
