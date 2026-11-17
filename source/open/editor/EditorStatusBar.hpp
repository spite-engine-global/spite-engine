#pragma once

#include <functional>
#include <memory>
#include <string>

#include "base/MathTypes.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class Engine;
    class EditorFeedback;
    class EditorModel;
    struct DockSpace;

    // Tracks scene state and diagnostic data, publishes frame windows and provides the toolbar's active jobs label.
    //
    // Toasts: at the bottom right, the newest four of EditorFeedback's, a click on one dismisses it, Esc (the
    // editor.dismissToast hotkey, while the editor is not playing) dismisses them all. A toast takes no focus and
    // carries the accessible role alert (warn, error) or status (info).
    //
    // Control (owner editorStatusBar): query statusBar (the bar's contents and the last 50 toasts). The toast commands
    // (postToast, dismissToast, clearToasts) and the toasts query belong to the editor model: the log
    // is its state, the bar only shows it and advances its clock.
    class EditorStatusBar
    {
    public:
        // `parent` is the shell's root panel: the toasts become its children.
        EditorStatusBar(Engine& engine, const Widget& parent, const DockSpace& dock, const EditorModel& editorModel,
                        EditorFeedback& feedback);
        ~EditorStatusBar();
        EditorStatusBar(const EditorStatusBar&) = delete;
        EditorStatusBar& operator=(const EditorStatusBar&) = delete;

        [[nodiscard]] Widget jobs() const;
        // The open scene as of the last update (the sceneInfo query's path, empty while untitled, and dirty): what the
        // scene tab and the File menu follow, so the editor reads sceneInfo once per change.
        [[nodiscard]] const std::string& scenePath() const;
        [[nodiscard]] bool sceneDirty() const;
        // The shell is `size`: the toasts stack above its bottom edge.
        void setSize(Vec2 size);
        // The telemetry density and the frame budget as an editorSettings left them: the readouts follow at once, so
        // the statusBar query and the widgets are not a frame behind the command's reply.
        void settle(bool fullTelemetry, float frameBudgetMs);
        // `listener` hears each closed frame window (about 4 Hz, never per frame): its mean in milliseconds and whether
        // the frame item is over its budget. The header's frame lamp and sparkline follow it.
        void onFrameWindow(std::function<void(double ms, bool over)> listener);
        // The frame item is over the budget as of the last window or settle (what the lamp's warn state follows).
        [[nodiscard]] bool frameOver() const;
        // Once per frame, after the panels reported their jobs.
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
