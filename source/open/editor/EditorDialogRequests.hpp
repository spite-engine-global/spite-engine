#pragma once

#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/MathTypes.hpp"
#include "editor/EditorExport.hpp"
#include "editor/EditorShell.hpp"

namespace spite
{
    class ControlArgs;
    class ControlServer;
    class Engine;
    class IFormatWriter;

    // The text is already localized, as EditorFeedback's toasts. An empty label is the table's "OK" / "Cancel"
    // (editor-dialogs.confirm, editor-dialogs.cancel).
    struct EditorConfirm
    {
        std::string title;
        std::string body;
        std::string confirmLabel;
        std::string cancelLabel;
        // The action cannot be taken back (removing a component, discarding unsaved changes): the dialog announces
        // itself as an alert dialog and the button of that action is red: the alternate button when there is one,
        // else the confirm button.
        bool destructive = false;
        // A third choice (EditorDialogs::choose), the button left of Cancel: "Discard | Cancel | Save" is alternate =
        // Discard, confirm = Save. Empty: a plain two-button confirmation.
        std::string alternateLabel;
    };

    // How a dialog was answered; a notice is always Confirm.
    enum class EditorAnswer : u8
    {
        Cancel,
        Confirm,
        Alternate,
    };

    // A message with one button (a sheet to read: the shortcuts). Esc, Enter and the button all dismiss it. The body
    // may take many lines, in the font's own spacing.
    struct EditorNotice
    {
        std::string title;
        std::string body;
        std::string closeLabel; // empty: the table's "Close"
    };

    struct EditorPrompt
    {
        std::string title;
        std::string body;
        std::string value; // the field's starting text, and what an auto-confirmed prompt returns
        std::string confirmLabel;
        std::string cancelLabel;
    };

    enum class EditorPickMode : u8
    {
        OpenFile, // an existing file
        SaveFile, // a name in a folder; an existing file is replaced (the confirm button says so)
        Folder, // the folder the dialog shows
    };

    // A file pick: the choice is a path relative to a named root of the control server, so
    // whatever a person picks is a path a control command may take. The text is already localized; empty strings
    // are the table's defaults.
    struct EditorPick
    {
        EditorPickMode mode = EditorPickMode::OpenFile;
        std::string title;
        std::string body;
        // The roots it may choose under (ControlServer::setRoot names); empty: every root set. The Places button
        // (the roots and the recent folders) shows only with several roots or a recent folder.
        std::vector<std::string> roots;
        std::string root; // where it starts; empty or not allowed: the first of roots
        std::string directory; // under root, '/' separated
        std::string name; // the file name the field starts with; also what an auto-confirmed pick answers
        // "glb,.gltf": the files listed, and what an open accepts; a save whose name has no extension gets the only
        // one. Empty: every file.
        std::string extensions;
        std::string confirmLabel;
        std::string cancelLabel;
    };

    struct EditorPicked
    {
        std::string root;
        std::string path; // relative to the root, '/' separated; empty for a folder pick of the root itself
    };

    // One dialog request of the model's queue.
    struct EditorDialogRequest
    {
        using Done = std::function<void(EditorAnswer answer, std::string_view text)>;

        u64 id = 0;
        bool prompt = false;
        bool destructive = false;
        bool notice = false; // one button
        std::optional<EditorPick> pick; // a file dialog
        std::string title;
        std::string body;
        std::string value;
        std::string confirmLabel;
        std::string cancelLabel;
        std::string alternateLabel; // non-empty: a third button
        Done done;
    };

    // The dialog that shows the open request (EditorDialogs, one per model). The queue asks it for what only the
    // widgets know; without one the requests are still answered, by answerDialog and the policy, except that a file
    // dialog (its folders, name and validation are the view's) is cancelled by the policy and refused by answerDialog.
    class EditorDialogView
    {
    public:
        virtual ~EditorDialogView() = default;
        // The front request is now the open one: put it on screen.
        virtual void show(const EditorDialogRequest& request) = 0;
        // The open request was answered: take it off the screen.
        virtual void close() = 0;
        // Per step while a file dialog is open.
        virtual void updatePick() = 0;
        // A prompt's text now.
        [[nodiscard]] virtual std::string fieldText() const = 0;
        // The open file dialog's choice, or why it is none; empty error: encoded is set.
        [[nodiscard]] virtual std::string pickCurrent(std::string& encoded) = 0;
        // A choice named by text against the open file dialog.
        [[nodiscard]] virtual std::string pickChoose(std::string_view rooted, std::string& encoded) = 0;
        // What a request answers without a person.
        [[nodiscard]] virtual std::string pickAuto(const EditorPick& request, std::string& encoded) const = 0;
        // A choice was taken.
        virtual void pickRemember(std::string_view encoded) = 0;
        // The open file dialog's state, members of an object the caller began.
        virtual void writePick(IFormatWriter& out) const = 0;
    };

    // The editor's dialog requests: the queue of confirm, choice, notice, prompt and file
    // requests, the policy that answers them without a person (EditorDialogPolicy), the last 20 answered, and the
    // dialogs query and answerDialog command (owner editorModel) over them; a state of the EditorModel
    // (EditorModelAccess::dialogs) that EditorDialogs shows. Main thread only.
    //
    // A request's callback runs exactly once, from step() (the host's between-frames point: EditorDialogs::update in
    // a shell), with the answer. Requests made while one is open, or from a callback, wait their turn in order. Under
    // Ask step() opens the next request; under Confirm and Cancel it answers every request present at once ("auto").
    // A request still waiting when the view is destroyed or the model is, is dropped without its callback: the
    // callback's owner (a panel) is gone by then.
    class SPITE_EDITOR_API EditorDialogRequests
    {
    public:
        static constexpr u32 LOG_CAPACITY = 20;

        struct Record
        {
            u64 id = 0;
            std::string_view kind; // confirm, prompt, pick, notice
            EditorAnswer answer = EditorAnswer::Cancel;
            std::string title;
            std::string via; // ui, command, auto
            std::string text;
        };

        explicit EditorDialogRequests(Engine& engine);
        EditorDialogRequests(const EditorDialogRequests&) = delete;
        EditorDialogRequests& operator=(const EditorDialogRequests&) = delete;

        // Not while a request is open: the open one was shown under the old policy.
        void setPolicy(EditorDialogPolicy policy);
        [[nodiscard]] EditorDialogPolicy policy() const { return m_policy; }

        // The dialogs query and answerDialog command, under `owner`, which the caller unregisters.
        void registerCommands(ControlServer& control, std::string_view owner);

        // One view; setting over a set one asserts, nullptr clears it. A set view shows the open request at once.
        void setView(EditorDialogView* view);
        // Forgets every request and answer not delivered, with no callback (the view's owner is going away).
        void drop();

        u64 confirm(EditorConfirm request, std::function<void(bool confirmed)> done);
        u64 choose(EditorConfirm request, std::function<void(EditorAnswer answer)> done);
        u64 notice(EditorNotice request, std::function<void()> done = {});
        u64 prompt(EditorPrompt request, std::function<void(std::optional<std::string> text)> done);
        u64 pickFile(EditorPick request, std::function<void(std::optional<EditorPicked> picked)> done);

        // Delivers the answers, then opens the next request (Ask) or answers the ones present (Confirm, Cancel).
        void step();

        // The open request (shown, or open for answerDialog), null when none.
        [[nodiscard]] const EditorDialogRequest* open() const { return m_shown ? &m_queue.front() : nullptr; }
        [[nodiscard]] bool pending() const { return !m_queue.empty(); }
        [[nodiscard]] const std::deque<Record>& log() const { return m_log; }

        // The open request is answered; false when none is open (the first resolution wins).
        bool resolve(EditorAnswer answer, std::string text, std::string_view via);
        bool resolve(bool confirmed, std::string text, std::string_view via)
        {
            return resolve(confirmed ? EditorAnswer::Confirm : EditorAnswer::Cancel, std::move(text), via);
        }
        // The confirm action of the open request (a button, Enter): a file dialog confirms only a valid choice and
        // stays open otherwise. True when the key or click was taken.
        bool activate(std::string_view via);

        // The labels and kinds the dialogs query and the view show.
        [[nodiscard]] std::string confirmLabel(const EditorDialogRequest& request) const;
        [[nodiscard]] std::string cancelLabel(const EditorDialogRequest& request) const;
        [[nodiscard]] static std::string_view kindOf(const EditorDialogRequest& request);

    private:
        struct Resolved
        {
            EditorDialogRequest request;
            EditorAnswer answer = EditorAnswer::Cancel;
            std::string text;
        };

        u64 push(EditorDialogRequest request);
        void finish(EditorDialogRequest request, EditorAnswer answer, std::string text, std::string_view via);
        void deliver();
        void show();
        [[nodiscard]] std::string tr(std::string_view key) const;
        [[nodiscard]] std::string answerText() const;
        void write(IFormatWriter& out) const;
        void writeButtons(IFormatWriter& out, const EditorDialogRequest& request) const;
        std::string answerCommand(const ControlArgs& args, IFormatWriter& out);

        Engine* m_engine;
        EditorDialogPolicy m_policy = EditorDialogPolicy::Ask;
        EditorDialogView* m_view = nullptr;
        std::deque<EditorDialogRequest> m_queue; // the front is the open one while m_shown
        std::deque<Resolved> m_resolved; // answered, callbacks not run yet
        std::deque<Record> m_log;
        u64 m_nextId = 1;
        bool m_shown = false;
    };
} // namespace spite
