#include "EditorProjectImport.hpp"

#include <algorithm>
#include <format>

#include "serialization/FormatWalk.hpp"
#include "ui/UiForm.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        using namespace panelui;

        constexpr std::string_view KEYS = "editor-project.import.";

        // the option's row in the form: its labels and docs are the message keys "editor-project.import.<name>[.doc]"
        UiFormField fieldOf(const ImportOptionDesc& option)
        {
            const std::string base = std::string(KEYS) + std::string(option.name);
            UiFormField field;
            field.id = InternedString(option.name);
            field.labelKey = InternedString(base);
            field.docKey = InternedString(base + ".doc");
            if (option.type == "number")
            {
                field.kind = UiFormKind::Number;
                field.min = option.min;
                field.max = option.max;
                field.step = 0.01;
                field.decimals = 4;
                field.defaultValue = UiFormValue::ofNumber(std::stod(std::string(option.defaultValue)));
                return field;
            }
            if (option.type == "bool")
            {
                field.kind = UiFormKind::Bool;
                field.defaultValue = UiFormValue::ofBool(option.defaultValue == "true");
                return field;
            }
            field.kind = UiFormKind::Enum;
            for (const std::string_view choice : option.choices)
            {
                field.choices.push_back({.id = InternedString(choice),
                                         .label = std::string(choice),
                                         .key = InternedString(base + "." + std::string(choice))});
            }
            const auto at = std::ranges::find(option.choices, option.defaultValue);
            field.defaultValue = UiFormValue::ofNumber(static_cast<double>(at - option.choices.begin()));
            return field;
        }

        UiFormValue valueOf(const ImportOptionDesc& option, const ModelImportSettings& settings)
        {
            const ImportOptionValue current = importOptionValue(settings, option);
            if (option.type == "number")
            {
                return UiFormValue::ofNumber(current.number);
            }
            if (option.type == "bool")
            {
                return UiFormValue::ofBool(current.flag);
            }
            const auto at = std::ranges::find(option.choices, current.text);
            return UiFormValue::ofNumber(static_cast<double>(at - option.choices.begin()));
        }

        Json memberOf(const ImportOptionDesc& option, const UiFormValue& value)
        {
            if (option.type == "number")
            {
                return value.number[0];
            }
            if (option.type == "bool")
            {
                return value.number[0] != 0.0;
            }
            const sizet at = static_cast<sizet>(std::max(0.0, value.number[0]));
            return std::string(at < option.choices.size() ? option.choices[at] : option.defaultValue);
        }

        UiRectDesc fitButtonDesc()
        {
            UiRectDesc desc = barDesc(Vec2{});
            desc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = false};
            return desc;
        }

        // a text line as wide as its container and as tall as its wrapped text
        UiRectDesc textLineDesc()
        {
            UiRectDesc desc = barDesc(Vec2{});
            desc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
            return desc;
        }
    } // namespace

    struct ProjectImportView::Impl
    {
        UiScope* scope;
        DensityFollower* density;
        Tr tr;
        Send send;
        ScrollView scroller;
        Panel host; // as tall as its rows
        std::unique_ptr<PropertyForm> form;
        Panel buttons;
        Button reimport;
        Button reset;
        Label state;
        Label job;
        Label note;
        ProjectImportModel model;
        std::string refusal; // the newest refused reimport of the shown model
        std::string shownState; // the texts as written, compared before writing again
        std::string shownJob;
        std::string shownNames;
        bool shownReadOnly = false;
        bool written = false; // show() ran at least once
        bool rewrite = false; // the locale moved: the texts are written again

        [[nodiscard]] std::string text(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            return tr(std::string(KEYS) + std::string(key), args);
        }

        [[nodiscard]] bool pending() const { return model.jobState == "queued" || model.jobState == "baking"; }
        [[nodiscard]] bool editable() const { return model.shown && model.blocked.empty(); }
        [[nodiscard]] bool canReimport() const { return editable() && model.error.empty() && !pending(); }
        [[nodiscard]] bool canReset() const { return editable() && model.settings != ModelImportSettings{}; }

        [[nodiscard]] std::string stateText() const
        {
            if (model.blocked == "outside")
            {
                return text("status.outside");
            }
            if (model.blocked == "gone")
            {
                return text("status.gone");
            }
            if (!model.error.empty())
            {
                return text("status.unreadable", std::vector<MessageArg>{{"error", std::string_view(model.error)}});
            }
            return text(model.needsReimport ? "status.needs" : "status.current");
        }

        // the model's newest import job, or the refusal of the newest Reimport
        [[nodiscard]] std::string jobText() const
        {
            if (!refusal.empty())
            {
                return text("job.refused", std::vector<MessageArg>{{"error", std::string_view(refusal)}});
            }
            if (model.job == 0)
            {
                return {};
            }
            if (model.jobState == "done")
            {
                const std::string ms = std::format("{:.0f}", model.bakeMs);
                return text("job.done", std::vector<MessageArg>{{"ms", std::string_view(ms)}});
            }
            if (model.jobState == "failed")
            {
                return text("job.failed", std::vector<MessageArg>{{"error", std::string_view(model.jobError)}});
            }
            return text("job." + model.jobState);
        }

        void post(const std::string& command, const Json& args, const char* failedKey,
                  std::function<void(const std::string&)> refused = {})
        {
            send(Json{{"command", command}, {"args", args}}.dump(), failedKey, std::move(refused));
        }

        // An edit of one row: the member alone goes to the sidecar; a refusal is told under the row.
        void edited(const UiFormEdit& edit)
        {
            const auto option = std::ranges::find(modelImportOptions(), edit.field.toView(), &ImportOptionDesc::name);
            if (!model.shown || option == modelImportOptions().end())
            {
                return;
            }
            const InternedString field = edit.field;
            const Json settings{{std::string(option->name), memberOf(*option, edit.value)}};
            post("setImportSettings", {{"name", model.name}, {"settings", settings}}, nullptr,
                 [this, field](const std::string& error) { form->setError(field, error); });
        }

        void resetAll()
        {
            Json defaults = Json::object();
            for (const ImportOptionDesc& option : modelImportOptions())
            {
                defaults[std::string(option.name)] = memberOf(option, fieldOf(option).defaultValue);
            }
            post("setImportSettings", {{"name", model.name}, {"settings", defaults}}, nullptr,
                 [this](const std::string& error)
                 {
                     refusal = error;
                     rewrite = true;
                 });
        }

        void reimportNow()
        {
            refusal.clear();
            rewrite = true;
            post("reimportModel", {{"name", model.name}}, "editor-project.reimport.failed",
                 [this](const std::string& error)
                 {
                     refusal = error;
                     rewrite = true;
                 });
        }

        void follow()
        {
            for (const ImportOptionDesc& option : modelImportOptions())
            {
                const InternedString id{option.name};
                const UiFormValue value = valueOf(option, model.settings);
                if (!form->isEditing(id) && form->value(id) != value)
                {
                    form->setValue(id, value);
                }
            }
        }
    };

    ProjectImportView::ProjectImportView(UiScope& scope, DensityFollower& density, Tr tr, Send send) :
        m_impl(std::make_unique<Impl>())
    {
        Impl& impl = *m_impl;
        impl.scope = &scope;
        impl.density = &density;
        impl.tr = std::move(tr);
        impl.send = std::move(send);

        UiRectDesc scrollDesc = topLeft();
        scrollDesc.layoutItem = fillItem();
        impl.scroller = scope.createScrollView("project.import.scroll", scrollDesc);
        impl.scroller.setVisible(false);
        Panel content = impl.scroller.content();
        density.apply(
            [content](const Density& d) mutable
            {
                content.setLayout(stack(UiLayoutDirection::Column, 0.0f, Vec4{d.xs, d.xs, d.xs + THUMB, d.xs},
                                        UiLayoutAlign::Stretch));
            });
        UiRectDesc hostDesc =
            barDesc(Vec2{}, stack(UiLayoutDirection::Column, density.density().sm, {}, UiLayoutAlign::Stretch));
        hostDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        impl.host = scope.createPanel("project.import.host", hostDesc);
        bindGap(impl.host, SPACE_SM);
        impl.host.setParent(content);

        UiFormDesc desc;
        desc.rect = topLeft();
        for (const ImportOptionDesc& option : modelImportOptions())
        {
            desc.fields.push_back(fieldOf(option));
        }
        impl.form = std::make_unique<PropertyForm>(scope, InternedString("project.import.form"), std::move(desc));
        impl.form->root().setParent(impl.host);
        impl.form->onEdit([&impl](const UiFormEdit& edit) { impl.edited(edit); });

        UiRectDesc rowDesc =
            barDesc(Vec2{0.0f, density.density().control}, stack(UiLayoutDirection::Row, density.density().xs));
        impl.buttons = scope.createPanel("project.import.buttons", rowDesc);
        bindGap(impl.buttons, SPACE_XS);
        density.track(impl.buttons, HEIGHT_CONTROL);
        impl.buttons.setParent(impl.host);
        const auto button = [&](const char* id, const char* key, std::function<void()> press)
        {
            Button out = density.track(scope.createButton(id, "", fitButtonDesc()), HEIGHT_CONTROL);
            bindFitPadding(out, SPACE_MD, SPACE_NONE);
            out.setStyleClass(InternedString("project.import"));
            out.setLocalizedText(InternedString(std::string(KEYS) + key));
            out.setTooltip(InternedString(std::string("editor-project.tip.") + key));
            out.onClick(std::move(press));
            out.setInteractable(false);
            out.setParent(impl.buttons);
            return out;
        };
        impl.reimport = button("project.import.reimport", "reimport", [&impl] { impl.reimportNow(); });
        impl.reset = button("project.import.reset", "reset", [&impl] { impl.resetAll(); });

        const auto line = [&](const char* id)
        {
            Label label = scope.createLabel(id, "", textLineDesc());
            label.setStyleClass(InternedString("settings.noteWrap"));
            label.setParent(impl.host);
            return label;
        };
        impl.state = line("project.import.state");
        impl.job = line("project.import.job");
        impl.note = line("project.import.note");
        impl.note.setLocalizedText(InternedString(std::string(KEYS) + "note"));
    }

    ProjectImportView::~ProjectImportView() = default;

    Widget ProjectImportView::root() const { return m_impl->scroller; }

    void ProjectImportView::relocalize() { m_impl->rewrite = true; }

    void ProjectImportView::show(const ProjectImportModel& next)
    {
        Impl& impl = *m_impl;
        const bool other = impl.model.name != next.name;
        if (other || (next.job != impl.model.job && !impl.refusal.empty()))
        {
            impl.refusal.clear();
        }
        impl.model = next;
        if (other)
        {
            for (const ImportOptionDesc& option : modelImportOptions())
            {
                impl.form->setError(InternedString(option.name), "");
            }
        }
        impl.follow();
        const bool readOnly = !impl.editable();
        if (!impl.written || impl.shownReadOnly != readOnly)
        {
            impl.shownReadOnly = readOnly;
            for (const ImportOptionDesc& option : modelImportOptions())
            {
                impl.form->setReadOnly(InternedString(option.name), readOnly);
            }
        }
        const auto enable = [](Button button, const bool on)
        {
            if (button.isInteractable() != on)
            {
                button.setInteractable(on);
            }
        };
        enable(impl.reimport, impl.canReimport());
        enable(impl.reset, impl.canReset());
        const bool all = impl.rewrite || !impl.written;
        impl.written = true;
        impl.rewrite = false;
        if (std::string state = impl.stateText(); all || state != impl.shownState)
        {
            impl.state.setText(state);
            impl.shownState = std::move(state);
        }
        if (std::string job = impl.jobText(); all || job != impl.shownJob)
        {
            impl.job.setText(job);
            impl.job.setVisible(!job.empty());
            impl.shownJob = std::move(job);
        }
        if (const std::string names = next.name; all || names != impl.shownNames)
        {
            impl.shownNames = names;
            const std::vector<MessageArg> args{{"name", std::string_view(names)}};
            impl.reimport.setAccessibleName(impl.text("reimport.name", args));
            impl.reset.setAccessibleName(impl.text("reset.name", args));
        }
    }

    void ProjectImportView::fit()
    {
        Impl& impl = *m_impl;
        const Vec2 view = resolvedSize(*impl.scope, impl.scroller);
        if (view.x <= 0.0f || view.y <= 0.0f)
        {
            return;
        }
        const Vec2 want{view.x,
                        std::max(view.y, resolvedSize(*impl.scope, impl.host).y + 2.0f * impl.density->density().xs)};
        if (impl.scroller.contentSize() != want)
        {
            impl.scroller.setContentSize(want);
        }
    }

    void ProjectImportView::write(IFormatWriter& out) const
    {
        const Impl& impl = *m_impl;
        const ProjectImportModel& model = impl.model;
        const auto settings = [&out](const std::string_view key, const ModelImportSettings& values)
        {
            out.beginObject(key);
            for (const ImportOptionDesc& option : modelImportOptions())
            {
                const ImportOptionValue value = importOptionValue(values, option);
                if (option.type == "number")
                {
                    out.writeF64(option.name, value.number);
                }
                else if (option.type == "bool")
                {
                    out.writeBool(option.name, value.flag);
                }
                else
                {
                    out.writeString(option.name, value.text);
                }
            }
            out.endObject();
        };
        out.beginObject("import");
        out.writeBool("shown", model.shown);
        out.writeString("name", model.name);
        out.writeString("blocked", model.blocked);
        settings("settings", model.settings);
        settings("bakedWith", model.baked);
        out.writeBool("sidecarExists", model.sidecarExists);
        out.writeBool("needsReimport", model.needsReimport);
        out.writeString("error", model.error);
        out.writeString("state", impl.shownState);
        out.writeString("job", impl.shownJob);
        out.writeString("refusal", impl.refusal);
        out.writeString("note", impl.text("note"));
        out.beginObject("actions");
        out.writeBool("reimport", impl.canReimport());
        out.writeBool("reset", impl.canReset());
        out.endObject();
        const std::vector<UiFormRowState> rows = impl.form->rows();
        out.beginArray("rows", static_cast<u32>(rows.size()));
        for (const UiFormRowState& row : rows)
        {
            const auto option = std::ranges::find(modelImportOptions(), row.field.toView(), &ImportOptionDesc::name);
            out.beginObject({});
            out.writeString("field", row.field.toView());
            if (option != modelImportOptions().end() && option->type == "number")
            {
                out.writeF64("value", row.value.number[0]);
            }
            else if (option != modelImportOptions().end() && option->type == "bool")
            {
                out.writeBool("value", row.value.number[0] != 0.0);
            }
            else if (option != modelImportOptions().end())
            {
                const sizet at = static_cast<sizet>(std::max(0.0, row.value.number[0]));
                out.writeString("value", at < option->choices.size() ? option->choices[at] : std::string_view{});
            }
            out.writeBool("modified", row.modified);
            out.writeBool("readOnly", row.readOnly);
            out.writeString("error", row.error);
            out.endObject();
        }
        out.endArray();
        out.endObject();
    }
} // namespace spite
