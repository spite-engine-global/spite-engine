#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class EditorDialogs;
    class EditorFeedback;
    class Widget;

    // The Input panel: the input actions of one context (a selector over the game, ui and editor
    // contexts) as a table with one row per binding (action, type, binding, live value) and a filter, and a Devices
    // page with the keyboard, the mouse and each connected gamepad: the keys and buttons down and the axis values
    // with whether they sit inside the deadzone. A line under the table says which file the context was read from
    // (the user layer's copy or the project's) and where a save writes.
    //
    // Nothing is read while hidden. The rows come from the `inputActions` query when shown and the page, context,
    // locale or a command changed; the live values are read from the input system every LIVE_FRAMES frames and only
    // a changed text is written. Query `inputView` (owner editorInput): page, context, rows, devices and the
    // counters; view commands `inputTab` and `inputContext`.
    class EditorInput
    {
    public:
        EditorInput(Engine& engine, EditorDialogs& dialogs, EditorFeedback& feedback);
        ~EditorInput();
        EditorInput(const EditorInput&) = delete;
        EditorInput& operator=(const EditorInput&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
