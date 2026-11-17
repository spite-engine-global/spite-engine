#include "EditorDialogRequests.hpp"
#include "EditorFilePicker.hpp"

#include <algorithm>
#include <array>
#include <format>

#include "base/Assert.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"

namespace spite
{
    namespace
    {
        constexpr std::string_view TABLE = "editor-dialogs.";
        constexpr std::array<std::string_view, 3> POLICY_NAMES{"ask", "confirm", "cancel"};
        // indexed by the order below: confirm, cancel, alternate
        constexpr std::array<std::string_view, 3> ANSWER_NAMES{"confirm", "cancel", "alternate"};
        constexpr std::array<EditorAnswer, 3> ANSWERS{EditorAnswer::Confirm, EditorAnswer::Cancel,
                                                      EditorAnswer::Alternate};

        std::string_view nameOf(const EditorAnswer answer)
        {
            return ANSWER_NAMES[static_cast<sizet>(std::ranges::find(ANSWERS, answer) - ANSWERS.begin())];
        }
    } // namespace

    EditorDialogRequests::EditorDialogRequests(Engine& engine) : m_engine(&engine) {}

    std::string EditorDialogRequests::tr(const std::string_view key) const
    {
        const std::string full = std::string(TABLE) + std::string(key);
        if (!m_engine->services().contains<Localization>())
        {
            return full;
        }
        std::string scratch;
        return std::string(m_engine->services().get<Localization>().resolve(InternedString(full), scratch));
    }

    std::string EditorDialogRequests::confirmLabel(const EditorDialogRequest& request) const
    {
        return request.confirmLabel.empty() ? tr(request.notice ? "close" : "confirm") : request.confirmLabel;
    }

    std::string EditorDialogRequests::cancelLabel(const EditorDialogRequest& request) const
    {
        return request.cancelLabel.empty() ? tr("cancel") : request.cancelLabel;
    }

    std::string_view EditorDialogRequests::kindOf(const EditorDialogRequest& request)
    {
        return request.pick ? "pick" : request.prompt ? "prompt" : request.notice ? "notice" : "confirm";
    }

    std::string EditorDialogRequests::answerText() const
    {
        return m_view ? m_view->fieldText() : m_queue.front().value;
    }

    void EditorDialogRequests::setPolicy(const EditorDialogPolicy policy)
    {
        SASSERTM(!m_shown, "The dialog policy changes only while no request is open.");
        m_policy = policy;
    }

    void EditorDialogRequests::setView(EditorDialogView* view)
    {
        SASSERTM(!view || !m_view, "A dialog view is already set: clear it first.");
        m_view = view;
        if (m_view && m_shown)
        {
            m_view->show(m_queue.front());
        }
    }

    void EditorDialogRequests::drop()
    {
        m_shown = false;
        m_queue.clear();
        m_resolved.clear();
    }

    u64 EditorDialogRequests::push(EditorDialogRequest request)
    {
        request.id = m_nextId++;
        const u64 id = request.id;
        m_queue.push_back(std::move(request));
        return id;
    }

    u64 EditorDialogRequests::confirm(EditorConfirm request, std::function<void(bool confirmed)> done)
    {
        EditorDialogRequest queued;
        queued.destructive = request.destructive;
        queued.title = std::move(request.title);
        queued.body = std::move(request.body);
        queued.confirmLabel = std::move(request.confirmLabel);
        queued.cancelLabel = std::move(request.cancelLabel);
        queued.done = [done = std::move(done)](const EditorAnswer answer, std::string_view)
        {
            if (done)
            {
                done(answer == EditorAnswer::Confirm);
            }
        };
        return push(std::move(queued));
    }

    u64 EditorDialogRequests::choose(EditorConfirm request, std::function<void(EditorAnswer answer)> done)
    {
        EditorDialogRequest queued;
        queued.destructive = request.destructive;
        queued.title = std::move(request.title);
        queued.body = std::move(request.body);
        queued.confirmLabel = std::move(request.confirmLabel);
        queued.cancelLabel = std::move(request.cancelLabel);
        queued.alternateLabel = std::move(request.alternateLabel);
        queued.done = [done = std::move(done)](const EditorAnswer answer, std::string_view)
        {
            if (done)
            {
                done(answer);
            }
        };
        return push(std::move(queued));
    }

    u64 EditorDialogRequests::notice(EditorNotice request, std::function<void()> done)
    {
        EditorDialogRequest queued;
        queued.notice = true;
        queued.title = std::move(request.title);
        queued.body = std::move(request.body);
        queued.confirmLabel = std::move(request.closeLabel);
        queued.done = [done = std::move(done)](EditorAnswer, std::string_view)
        {
            if (done)
            {
                done();
            }
        };
        return push(std::move(queued));
    }

    u64 EditorDialogRequests::prompt(EditorPrompt request, std::function<void(std::optional<std::string> text)> done)
    {
        EditorDialogRequest queued;
        queued.prompt = true;
        queued.title = std::move(request.title);
        queued.body = std::move(request.body);
        queued.value = std::move(request.value);
        queued.confirmLabel = std::move(request.confirmLabel);
        queued.cancelLabel = std::move(request.cancelLabel);
        queued.done = [done = std::move(done)](const EditorAnswer answer, const std::string_view text)
        {
            if (done)
            {
                done(answer == EditorAnswer::Confirm ? std::optional<std::string>(std::string(text)) : std::nullopt);
            }
        };
        return push(std::move(queued));
    }

    u64 EditorDialogRequests::pickFile(EditorPick request, std::function<void(std::optional<EditorPicked> picked)> done)
    {
        EditorDialogRequest queued;
        queued.title = request.title;
        queued.body = request.body;
        queued.confirmLabel = request.confirmLabel;
        queued.cancelLabel = request.cancelLabel;
        queued.pick = std::move(request);
        queued.done = [done = std::move(done)](const EditorAnswer answer, const std::string_view text)
        {
            if (!done)
            {
                return;
            }
            if (answer != EditorAnswer::Confirm)
            {
                done(std::nullopt);
                return;
            }
            const sizet slash = text.find('/');
            done(EditorPicked{std::string(text.substr(0, slash)),
                              slash == std::string_view::npos ? std::string{} : std::string(text.substr(slash + 1))});
        };
        return push(std::move(queued));
    }

    // The request is answered: its record and its callback (run by step()) are kept.
    void EditorDialogRequests::finish(EditorDialogRequest request, EditorAnswer answer, std::string text,
                                      const std::string_view via)
    {
        if (request.notice)
        {
            answer = EditorAnswer::Confirm; // the only button; Esc and the backdrop acknowledge it
        }
        if (answer != EditorAnswer::Confirm || (!request.prompt && !request.pick))
        {
            text.clear();
        }
        if (m_log.size() == LOG_CAPACITY)
        {
            m_log.pop_front();
        }
        m_log.push_back({request.id, kindOf(request), answer, request.title, std::string(via), text});
        m_resolved.push_back({std::move(request), answer, std::move(text)});
    }

    bool EditorDialogRequests::resolve(const EditorAnswer answer, std::string text, const std::string_view via)
    {
        if (!m_shown)
        {
            return false;
        }
        m_shown = false;
        EditorDialogRequest request = std::move(m_queue.front());
        m_queue.pop_front();
        finish(std::move(request), answer, std::move(text), via);
        if (m_view)
        {
            m_view->close();
        }
        return true;
    }

    bool EditorDialogRequests::activate(const std::string_view via)
    {
        if (!m_shown)
        {
            return false;
        }
        if (m_queue.front().pick)
        {
            std::string encoded;
            if (m_view && m_view->pickCurrent(encoded).empty())
            {
                m_view->pickRemember(encoded);
                resolve(true, std::move(encoded), via);
            }
            return true;
        }
        return resolve(true, answerText(), via);
    }

    void EditorDialogRequests::show()
    {
        m_shown = true;
        if (m_view)
        {
            m_view->show(m_queue.front());
        }
    }

    void EditorDialogRequests::deliver()
    {
        std::deque<Resolved> batch;
        batch.swap(m_resolved);
        for (Resolved& answered : batch)
        {
            if (answered.request.done)
            {
                answered.request.done(answered.answer, answered.text);
            }
        }
    }

    void EditorDialogRequests::step()
    {
        if (m_resolved.empty() && m_queue.empty())
        {
            return;
        }
        deliver();
        if (m_policy == EditorDialogPolicy::Ask)
        {
            if (!m_shown && !m_queue.empty())
            {
                show();
            }
            else if (m_shown && m_queue.front().pick && m_view)
            {
                m_view->updatePick();
            }
            return;
        }
        // one pass over the requests present now: a callback that asks again is served next frame
        const bool confirmed = m_policy == EditorDialogPolicy::Confirm;
        for (sizet present = m_queue.size(); present > 0 && !m_queue.empty(); --present)
        {
            EditorDialogRequest request = std::move(m_queue.front());
            m_queue.pop_front();
            std::string text = request.value;
            bool answered = confirmed;
            if (request.pick)
            {
                text.clear();
                answered = confirmed && m_view && m_view->pickAuto(*request.pick, text).empty();
            }
            finish(std::move(request), answered ? EditorAnswer::Confirm : EditorAnswer::Cancel, std::move(text),
                   "auto");
        }
        deliver();
    }

    void EditorDialogRequests::writeButtons(IFormatWriter& out, const EditorDialogRequest& request) const
    {
        // in the order they are shown
        const bool hasAlternate = !request.alternateLabel.empty();
        out.beginArray("buttons", 1u + (request.notice ? 0u : 1u) + (hasAlternate ? 1u : 0u));
        if (hasAlternate)
        {
            out.beginObject("");
            out.writeString("id", "alternate");
            out.writeString("label", request.alternateLabel);
            out.writeString("role", "alternate");
            out.writeBool("destructive", request.destructive);
            out.endObject();
        }
        if (!request.notice)
        {
            out.beginObject("");
            out.writeString("id", "cancel");
            out.writeString("label", cancelLabel(request));
            out.writeString("role", "cancel");
            out.endObject();
        }
        out.beginObject("");
        out.writeString("id", "confirm");
        out.writeString("label", confirmLabel(request));
        out.writeString("role", "confirm");
        out.writeBool("destructive", request.destructive && !hasAlternate);
        out.endObject();
        out.endArray();
    }

    void EditorDialogRequests::write(IFormatWriter& out) const
    {
        out.writeString("policy", POLICY_NAMES[static_cast<u8>(m_policy)]);
        out.beginArray("dialogs", static_cast<u32>(m_queue.size()));
        for (sizet i = 0; i < m_queue.size(); ++i)
        {
            const EditorDialogRequest& request = m_queue[i];
            const bool open = m_shown && i == 0;
            out.beginObject("");
            out.writeU64("id", request.id);
            out.writeString("state", open ? "open" : "queued");
            out.writeString("kind", kindOf(request));
            out.writeString("role", request.destructive ? "alertdialog" : "dialog");
            out.writeString("title", request.title);
            out.writeString("body", request.body);
            out.writeBool("destructive", request.destructive);
            writeButtons(out, request);
            if (request.prompt)
            {
                out.beginObject("field");
                out.writeString("default", request.value);
                out.writeString("value", open ? answerText() : request.value);
                out.endObject();
            }
            if (request.pick)
            {
                out.beginObject("pick");
                if (open && m_view)
                {
                    m_view->writePick(out);
                }
                else
                {
                    FilePicker::writeRequest(*request.pick, out);
                }
                out.endObject();
            }
            out.endObject();
        }
        out.endArray();
        out.beginArray("log", static_cast<u32>(m_log.size()));
        for (const Record& record : m_log)
        {
            out.beginObject("");
            out.writeU64("id", record.id);
            out.writeString("kind", record.kind);
            out.writeString("title", record.title);
            out.writeString("answer", nameOf(record.answer));
            out.writeString("via", record.via);
            if ((record.kind == "prompt" || record.kind == "pick") && record.answer == EditorAnswer::Confirm)
            {
                out.writeString("text", record.text);
            }
            out.endObject();
        }
        out.endArray();
    }

    std::string EditorDialogRequests::answerCommand(const ControlArgs& args, IFormatWriter& out)
    {
        if (!m_shown)
        {
            return std::string(m_queue.empty() ? "no dialog is open"
                                               : "no dialog is open yet: the next one shows at the next frame");
        }
        const EditorDialogRequest& open = m_queue.front();
        if (const std::optional<u64> id = args.integer("id"); id && *id != open.id)
        {
            return std::format("dialog {} is not the open one ({}); the dialogs query lists them", *id, open.id);
        }
        const std::string answer = args.string("answer").value_or("");
        const auto named = std::ranges::find(ANSWER_NAMES, answer);
        if (named == ANSWER_NAMES.end())
        {
            return std::string("answerDialog answer is confirm, cancel or alternate");
        }
        const EditorAnswer given = ANSWERS[static_cast<sizet>(named - ANSWER_NAMES.begin())];
        if (given == EditorAnswer::Alternate && open.alternateLabel.empty())
        {
            return std::string("answerDialog answer=alternate: this dialog has no third button");
        }
        const std::optional<std::string> text = args.string("text");
        if (text && !open.prompt)
        {
            return std::string("answerDialog text answers a prompt; this dialog is not one");
        }
        const std::optional<std::string> path = args.string("path");
        if (path && !open.pick)
        {
            return std::string("answerDialog path answers a file dialog; this dialog is not one");
        }
        if (path && given != EditorAnswer::Confirm)
        {
            return std::string("answerDialog path goes with answer=confirm");
        }
        const u64 id = open.id;
        std::string chosen = text ? *text : answerText();
        if (open.pick && given == EditorAnswer::Confirm)
        {
            if (!m_view)
            {
                return std::string("answerDialog: a file dialog is answered through the editor's dialog view");
            }
            chosen.clear();
            const std::string error = path ? m_view->pickChoose(*path, chosen) : m_view->pickCurrent(chosen);
            if (!error.empty())
            {
                return error;
            }
            m_view->pickRemember(chosen);
        }
        resolve(given, chosen, "command");
        out.writeU64("id", id);
        out.writeString("answer", answer);
        return std::string{};
    }

    void EditorDialogRequests::registerCommands(ControlServer& control, const std::string_view owner)
    {
        control.registerQuery(
            "dialogs", owner,
            "the editor's confirm, prompt and file dialogs: the policy, the open dialog and the queued "
            "ones (title, body, buttons, prompt field, a file dialog's pick: location, name, status, "
            "table) and the last 20 answered, with how (ui, command, auto)",
            {},
            [this](const ControlArgs&, IFormatWriter& out)
            {
                write(out);
                return std::string{};
            });
        control.registerViewCommand(
            "answerDialog", owner,
            "answers the open dialog as its confirm, cancel or alternate button does; its callback runs at the "
            "next "
            "frame. A file dialog confirms path=<root>/<file or folder> (the check its button makes: a refused "
            "path leaves it open), else what it shows",
            {requiredArg("answer", ArgKind::String, "confirm, cancel, or alternate (a choice's third button)",
                         ANSWER_NAMES),
             optionalArg("text", ArgKind::String, "a prompt's answer when confirming", "the field's text"),
             optionalPath("path", ANY_ROOT,
                          "a file dialog's answer when confirming: a root name and the path under it, "
                          "<root>/<path>",
                          "what the dialog shows"),
             optionalArg("id", ArgKind::Integer, "the dialog's id, as the dialogs query gives it", "the open dialog")},
            [this](const ControlArgs& args, IFormatWriter& out) { return answerCommand(args, out); });
    }
} // namespace spite
