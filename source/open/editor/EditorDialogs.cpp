#include "EditorDialogs.hpp"
#include "EditorContext.hpp"
#include "EditorFilePicker.hpp"
#include "EditorPanelUi.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

#include "control/ControlServer.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        using namespace panelui;

        constexpr std::string_view OWNER = "editorDialogs";
        constexpr std::string_view TABLE = "editor-dialogs.";

        constexpr float WIDTH = 380.0f; // the dialog's content width
        constexpr float PICKER_WIDTH = 600.0f; // a file dialog's, narrower in a window that has no room for it
        constexpr float PICKER_WIDTH_MIN = 320.0f;
        constexpr float PICKER_MARGIN = 64.0f; // the dialog's padding and the gap to the window's edge, both sides
        constexpr float PICKER_ROWS_MIN = 96.0f; // the entries table's height: what the shell leaves, within these
        constexpr float PICKER_ROWS_MAX = 240.0f;
        constexpr float PICKER_CHROME = 310.0f; // the dialog's height beside the table
        constexpr float FIELD_HEIGHT = 26.0f;
        constexpr float BUTTON_PAD = 14.0f;
        constexpr float BUTTON_MIN = 84.0f;
        constexpr u32 ESCAPE_KEY = 41; // HID
        constexpr u32 RETURN_KEY = 40;
        constexpr u32 KP_ENTER_KEY = 88;
        constexpr u8 CHORDS = UiKeyModifier::CTRL | UiKeyModifier::ALT | UiKeyModifier::SUPER;
    } // namespace

    struct EditorDialogs::State final : EditorDialogView
    {
        State(Engine& engine, EditorModel& model, const Widget& bounds) :
            control(&engine.control()), scope(editorUi(engine).ui()), bounds(bounds),
            requests(&EditorModelAccess::dialogs(model)),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
            modal = scope.createModal("editor.modal", {.closeOnEscape = false, .padding = 0.0f, .bounds = bounds});
            const Panel content = modal.content();
            Widget column = content;
            column.setLayout(stack(UiLayoutDirection::Column, 10.0f, Vec4{16.0f, 16.0f, 16.0f, 16.0f}));

            UiRectDesc wide = topLeft({}, Vec2{WIDTH, 0.0f});
            wide.layoutItem = fixedItem();
            UiContentSizeComponent fitHeightOnly;
            fitHeightOnly.fitWidth = false;
            wide.contentSize = fitHeightOnly;
            title = scope.createLabel("editor.dialog.title", "", wide);
            title.setStyleClass(InternedString("dialog.title"));
            title.setParent(content);
            body = scope.createLabel("editor.dialog.body", "", wide);
            body.setStyleClass(InternedString("dialog.body"));
            body.setParent(content);
            // a file dialog's content (EditorFilePicker.cpp) goes here, between the text and the buttons
            UiRectDesc hostDesc = topLeft({}, Vec2{WIDTH, 0.0f});
            hostDesc.layout = stack(UiLayoutDirection::Column, 6.0f, {}, UiLayoutAlign::Stretch);
            hostDesc.layoutItem = fixedItem();
            hostDesc.contentSize = fitHeightOnly;
            pickerHost = scope.createPanel("editor.dialog.pick", hostDesc);
            pickerHost.setParent(content);
            pickerHost.setVisible(false);
            field = scope.createTextInput("editor.dialog.input", barDesc(Vec2{WIDTH, FIELD_HEIGHT}));
            field.setParent(content);
            field.setVisible(false);

            UiLayoutComponent row = stack(UiLayoutDirection::Row, 8.0f, {}, UiLayoutAlign::Center);
            row.justify = UiLayoutJustify::End;
            buttons = scope.createPanel("editor.dialog.buttons", barDesc(Vec2{WIDTH, 28.0f}, row));
            buttons.setParent(content);
            const auto button = [this](const char* id)
            {
                UiRectDesc desc = barDesc(Vec2{BUTTON_MIN, 26.0f});
                UiContentSizeComponent fit;
                fit.fitHeight = false;
                fit.padding = Vec2{BUTTON_PAD, 0.0f};
                desc.contentSize = fit;
                desc.layoutItem->minSize = Vec2{BUTTON_MIN, 0.0f};
                Button created = scope.createButton(id, "", desc);
                created.setParent(buttons);
                return created;
            };
            alternate = button("editor.dialog.alternate");
            alternate.setVisible(false);
            cancel = button("editor.dialog.cancel");
            confirm = button("editor.dialog.confirm");

            alternate.onClick([this] { requests->resolve(EditorAnswer::Alternate, {}, "ui"); });
            cancel.onClick([this] { requests->resolve(false, {}, "ui"); });
            confirm.onClick([this] { requests->activate("ui"); });
            field.onSubmit([this](std::string_view) { requests->activate("ui"); });
            field.onCancel([this] { requests->resolve(false, {}, "ui"); });
            picker = std::make_unique<FilePicker>(
                scope, *control, localization, pickerHost, confirm,
                FilePicker::Hooks{.confirm = [this] { requests->activate("ui"); },
                                  .cancel = [this] { requests->resolve(false, {}, "ui"); }});
            // keys reaching the backdrop: nothing in the dialog took them. Esc cancels; Enter confirms unless a
            // button has the focus (the router clicks it after this returns false)
            Widget root = modal;
            root.onKeyDown(
                [this](const u32 scancode, const u8 modifiers, const bool repeat)
                {
                    if (repeat || (modifiers & CHORDS) != 0)
                    {
                        return false;
                    }
                    if (scancode == ESCAPE_KEY)
                    {
                        return requests->resolve(false, {}, "ui");
                    }
                    if (scancode != RETURN_KEY && scancode != KP_ENTER_KEY)
                    {
                        return false;
                    }
                    const Entity focus = scope.router().focused();
                    if (focus == alternate.entity() || focus == cancel.entity() || focus == confirm.entity())
                    {
                        return false;
                    }
                    return requests->activate("ui");
                });
            modal.onClose(
                [this]
                {
                    if (!closing)
                    {
                        requests->resolve(false, {}, "ui");
                    }
                });
            scope.commit();
        }

        Modal modal;
        Label title;
        Label body;
        Panel pickerHost;
        TextInput field;
        Panel buttons;
        Button alternate; // the choice's third button, left of Cancel
        Button cancel;
        Button confirm;
        ControlServer* control;
        UiScope scope;
        Widget bounds;
        EditorDialogRequests* requests; // the model's
        const Localization* localization;

        std::unique_ptr<FilePicker> picker; // after the scope: its widgets go first
        float width = WIDTH;
        bool closing = false; // the modal is being closed by close(): its onClose is not a cancel

        [[nodiscard]] std::string tr(const std::string_view key) const
        {
            const std::string full = std::string(TABLE) + std::string(key);
            if (!localization)
            {
                return full;
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(full), scratch));
        }

        [[nodiscard]] static UiRole roleOf(const EditorDialogRequest& request)
        {
            return request.destructive ? UiRole::AlertDialog : UiRole::Dialog;
        }

        void setWidth(const float wanted)
        {
            if (wanted == width)
            {
                return;
            }
            width = wanted;
            for (Widget text : {Widget(title), Widget(body)})
            {
                text.setSize(Vec2{wanted, text.size().y});
            }
            field.setSize(Vec2{wanted, FIELD_HEIGHT});
            buttons.setSize(Vec2{wanted, buttons.size().y});
            pickerHost.setSize(Vec2{wanted, pickerHost.size().y});
        }

        [[nodiscard]] std::string defaultPickTitle(const EditorPick& pick) const
        {
            return tr(pick.mode == EditorPickMode::OpenFile       ? "pick.title.open"
                          : pick.mode == EditorPickMode::SaveFile ? "pick.title.save"
                                                                  : "pick.title.folder");
        }

        void show(const EditorDialogRequest& request) override
        {
            const bool picking = request.pick.has_value();
            const std::string shownTitle =
                request.title.empty() && picking ? defaultPickTitle(*request.pick) : request.title;
            title.setText(shownTitle);
            body.setText(request.body);
            body.setVisible(!request.body.empty());
            field.setVisible(request.prompt);
            pickerHost.setVisible(picking);
            const Vec2 room = resolvedSize(scope, bounds);
            setWidth(!picking            ? WIDTH
                         : room.x > 0.0f ? std::clamp(room.x - PICKER_MARGIN, PICKER_WIDTH_MIN, PICKER_WIDTH)
                                         : PICKER_WIDTH);
            if (request.prompt)
            {
                field.setText(request.value);
                field.setAccessibleName(request.title);
            }
            cancel.setText(requests->cancelLabel(request));
            cancel.setVisible(!request.notice);
            alternate.setVisible(!request.alternateLabel.empty());
            alternate.setText(request.alternateLabel);
            body.setStyleClass(InternedString(request.notice ? "dialog.sheet" : "dialog.body"));
            if (picking)
            {
                const float rows = room.y - PICKER_CHROME;
                picker->open(*request.pick,
                             rows > 0.0f ? std::clamp(rows, PICKER_ROWS_MIN, PICKER_ROWS_MAX) : PICKER_ROWS_MAX);
            }
            else
            {
                const InternedString red("dialog.button.destructive");
                const bool onAlternate = !request.alternateLabel.empty();
                confirm.setText(requests->confirmLabel(request));
                confirm.setStyleClass(request.destructive && !onAlternate ? red : InternedString{});
                alternate.setStyleClass(request.destructive && onAlternate ? red : InternedString{});
            }
            Panel dialog = modal.dialog();
            dialog.setAccessibleName(shownTitle);
            dialog.setAccessibleRole(roleOf(request));
            modal.open();
            scope.router().requestFocus(picking              ? picker->initialFocus()
                                            : request.prompt ? field.entity()
                                                             : confirm.entity());
        }

        void close() override
        {
            closing = true;
            modal.close();
            closing = false;
        }

        void updatePick() override { picker->update(); }
        [[nodiscard]] std::string fieldText() const override { return std::string(field.value()); }
        [[nodiscard]] std::string pickCurrent(std::string& encoded) override { return picker->current(encoded); }
        [[nodiscard]] std::string pickChoose(const std::string_view rooted, std::string& encoded) override
        {
            return picker->choose(rooted, encoded);
        }
        [[nodiscard]] std::string pickAuto(const EditorPick& request, std::string& encoded) const override
        {
            return picker->autoChoice(request, encoded);
        }
        void pickRemember(const std::string_view encoded) override { picker->remember(encoded); }
        void writePick(IFormatWriter& out) const override { picker->write(out); }

        void registerCommands()
        {
            control->registerViewCommand(
                "pickerGo", OWNER,
                "shows another folder in the open file dialog, as a click on a breadcrumb or a folder does; the "
                "dialogs query's pick lists what it shows",
                {requiredPath("path", ANY_ROOT, "the folder: a root name and the directory under it, <root>/<dir>")},
                [this](const ControlArgs& args, IFormatWriter& out)
                {
                    const EditorDialogRequest* open = requests->open();
                    if (!open || !open->pick)
                    {
                        return std::string("no file dialog is open");
                    }
                    const std::string path = args.string("path").value_or("");
                    if (std::string error = picker->navigate(path); !error.empty())
                    {
                        return error;
                    }
                    out.writeString("path", path);
                    return std::string{};
                });
        }
    };

    EditorDialogs::EditorDialogs(Engine& engine, EditorModel& model, const Widget& bounds) :
        m_state(std::make_unique<State>(engine, model, bounds))
    {
        m_state->registerCommands();
        m_state->requests->setView(m_state.get());
    }

    void EditorDialogs::closeForShutdown() { m_state->close(); }

    EditorDialogs::~EditorDialogs()
    {
        m_state->requests->setView(nullptr);
        m_state->requests->drop();
        m_state->control->unregisterOwner(OWNER);
    }

    u64 EditorDialogs::confirm(EditorConfirm request, std::function<void(bool confirmed)> done)
    {
        return m_state->requests->confirm(std::move(request), std::move(done));
    }

    u64 EditorDialogs::choose(EditorConfirm request, std::function<void(EditorAnswer answer)> done)
    {
        return m_state->requests->choose(std::move(request), std::move(done));
    }

    u64 EditorDialogs::notice(EditorNotice request, std::function<void()> done)
    {
        return m_state->requests->notice(std::move(request), std::move(done));
    }

    u64 EditorDialogs::prompt(EditorPrompt request, std::function<void(std::optional<std::string> text)> done)
    {
        return m_state->requests->prompt(std::move(request), std::move(done));
    }

    u64 EditorDialogs::pickFile(EditorPick request, std::function<void(std::optional<EditorPicked> picked)> done)
    {
        return m_state->requests->pickFile(std::move(request), std::move(done));
    }

    void EditorDialogs::update() { m_state->requests->step(); }
} // namespace spite
