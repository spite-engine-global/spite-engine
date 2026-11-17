#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/MathTypes.hpp"
#include "EditorDialogRequests.hpp"
#include "editor/EditorExport.hpp"
#include "editor/EditorShell.hpp"

namespace spite
{
    class Engine;
    class EditorModel;
    class Widget;

    // The editor's confirm, prompt and file dialogs: one modal (a dialog window with the role
    // dialog, or alertdialog for a destructive confirm) over the whole shell, shown for one request at a time; the
    // shell owns it and hands it to a panel through EditorPanelContext::dialogs. Main thread only.
    //
    // A request's callback runs exactly once, from update() (the shell's between-frames point, never inside a UI
    // handler or a control command), with whether it was confirmed and, for a prompt, the text (an empty text on
    // cancel). Requests made while one is open, or from a callback, wait their turn in order. Enter confirms
    // (the focused button, else the confirm button), Esc cancels, Tab stays inside, a prompt's field starts
    // focused. A choice (three buttons) and a notice (one) are confirmations with more or fewer buttons. A request
    // still waiting when the shell is destroyed is dropped without its callback: the callback's owner (a panel) is gone
    // by then.
    //
    // A file dialog lists a folder of a named root (the listDirectory query's data): breadcrumb, the roots and recent
    // folders, a filtered and sortable table, a name field. Its callback gets nullopt on cancel, else the root and the
    // path under it, which one function validated for every route (button, Enter, answerDialog path=, the auto
    // policy), so a refused path leaves the dialog open and reaches nobody.
    //
    // The requests, the policy and the log are the editor model's (EditorDialogRequests): this is the view
    // that shows the open request, so the confirm, choose, notice, prompt and pickFile below queue on the model.
    // Control: the model's query dialogs (the policy, the open and queued dialogs with their buttons and field, a file
    // dialog's location, name and table, and the last 20 resolved with how: ui, command or auto) and view command
    // answerDialog answer=confirm|cancel|alternate [text=] [path=] [id=] (alternate only for a choice), owner
    // editorModel; here (owner editorDialogs) pickerGo. The policy (EditorShellDesc::dialogs, `--auto-dialogs`)
    // answers requests without showing them: a choice is confirmed or cancelled like a confirmation (never the
    // alternate), a file dialog confirms with its starting folder and name when that is a valid choice (a save never
    // names an existing file: replacing is a person's or a command's choice), else cancels.
    class SPITE_EDITOR_API EditorDialogs
    {
    public:
        static constexpr u32 LOG_CAPACITY = EditorDialogRequests::LOG_CAPACITY;

        // `bounds` is the shell's root panel: the backdrop covers it and the dialog is centered in it. It becomes the
        // model's dialog view (one per model) and drops the model's pending requests when destroyed.
        EditorDialogs(Engine& engine, EditorModel& model, const Widget& bounds);
        ~EditorDialogs();
        EditorDialogs(const EditorDialogs&) = delete;
        EditorDialogs& operator=(const EditorDialogs&) = delete;

        // Asks; the request's id (as the dialogs query gives it). done(confirmed). Two buttons: a request's
        // alternateLabel is ignored (choose is the three-button form).
        u64 confirm(EditorConfirm request, std::function<void(bool confirmed)> done);
        // As confirm, with the request's alternate button when its alternateLabel is set.
        u64 choose(EditorConfirm request, std::function<void(EditorAnswer answer)> done);
        // Shows a notice; done() when it is dismissed (empty: nothing).
        u64 notice(EditorNotice request, std::function<void()> done = {});
        // done(the text) on confirm, nullopt on cancel.
        u64 prompt(EditorPrompt request, std::function<void(std::optional<std::string> text)> done);
        // done(the choice) on confirm, nullopt on cancel.
        u64 pickFile(EditorPick request, std::function<void(std::optional<EditorPicked> picked)> done);

        // Close the overlay while the shell it can restore focus to is still alive.
        void closeForShutdown();

        // Once per frame, before the panels update: delivers the answers, then shows the next dialog.
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
