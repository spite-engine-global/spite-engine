#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include "EditorActions.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class Engine;
    class EditorModel;

    // The editor's File menu: New, Open, Open Recent, Save, Save As, Import Model and Quit, and the File
    // hotkeys. It is a view: each choice requests a flow of the editor model (EditorSceneFlows: the scene-state
    // command behind a file dialog and the unsaved-changes choice, all model state), and the menu lists the model's
    // recent scenes. Closing the window (X, Alt+F4, OS shutdown) bypasses the unsaved-changes prompt: only the Quit
    // item and its hotkey ask.
    //
    // Actions are queued and start at update(): a menu or hotkey handler never runs a flow inline. One flow runs at a
    // time; a request made meanwhile is dropped.
    class EditorFileMenu
    {
    public:
        // `model` is not const: the menu requests its flows (EditorModelAccess::flows).
        EditorFileMenu(Engine& engine, EditorModel& model, const EditorActions& actions);
        ~EditorFileMenu();
        EditorFileMenu(const EditorFileMenu&) = delete;
        EditorFileMenu& operator=(const EditorFileMenu&) = delete;

        // The menu as of now: ids file.new, file.open, file.recent (children file.recent.<n>), file.save,
        // file.saveAs, file.import, file.quit; the labels carry the hotkeys' chords. New, Open and the recent scenes
        // are disabled while a play world runs.
        [[nodiscard]] std::vector<UiMenuItem> items() const;
        // True when the item is one of the File menu's (it is queued).
        bool activate(InternedString item);
        // Open Recent for a path (the Scenes panel's Open): the same flow as the menu item.
        void openRecent(std::string_view path);
        // A File hotkey; other actions are ignored.
        void request(EditorActions::Action action);
        // Once per frame, before the dialogs update: the flows of the model start the queued action.
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
