#include "EditorProjectMaterial.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <optional>

#include "asset/WriteBakedFunctions.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiColorPicker.hpp"
#include "ui/UiForm.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        using namespace panelui;

        constexpr std::string_view KEYS = "editor-project.material.";
        constexpr sizet NAMES_SHOWN = 6; // the users a line names before "and N more"
        constexpr u32 POPUP_ROWS = 8;
        constexpr double NO_BOUND = 1.0e300; // UiFormField's open bound

        // One form row: a schema field of a kind the form edits
        struct MaterialRow
        {
            const FieldDesc* desc = nullptr;
            UiFormKind kind = UiFormKind::Number;
            u32 lanes = 1; // the numbers of a Number, vector or colour row
            bool alpha = false; // a Vec4 colour
            bool integer = false;

            [[nodiscard]] bool numeric() const
            {
                return kind != UiFormKind::Bool && kind != UiFormKind::Enum && kind != UiFormKind::Path;
            }
        };

        std::optional<MaterialRow> classify(const FieldDesc& desc)
        {
            MaterialRow row;
            row.desc = &desc;
            const bool color = (desc.flags & fieldFlags::COLOR) != 0;
            switch (desc.kind)
            {
            case FieldKind::Bool:
                row.kind = UiFormKind::Bool;
                break;
            case FieldKind::F32:
                break;
            case FieldKind::U32:
            case FieldKind::I32:
                row.kind = desc.enumNames.empty() ? UiFormKind::Number : UiFormKind::Enum;
                row.integer = true;
                break;
            case FieldKind::Vec2:
                row.kind = UiFormKind::Vec2;
                row.lanes = 2;
                break;
            case FieldKind::Vec3:
                row.kind = color ? UiFormKind::Color : UiFormKind::Vec3;
                row.lanes = 3;
                break;
            case FieldKind::Vec4:
                row.kind = color ? UiFormKind::Color : UiFormKind::Vec4;
                row.lanes = 4;
                row.alpha = true;
                break;
            case FieldKind::InternedString:
                row.kind = UiFormKind::Path;
                break;
            default:
                return std::nullopt;
            }
            return row;
        }

        const std::vector<MaterialRow>& materialRows()
        {
            static const std::vector<MaterialRow> rows = []
            {
                std::vector<MaterialRow> list;
                for (const FieldDesc& desc : BakedMaterialWriteData::spiteSchema().fields)
                {
                    if (const std::optional<MaterialRow> row = classify(desc))
                    {
                        list.push_back(*row);
                    }
                }
                return list;
            }();
            return rows;
        }

        // The descriptor's member as the form shows it: unused numbers 0 and no text outside a Path row, so two
        // values compare with ==
        UiFormValue valueOf(const MaterialRow& row, const BakedMaterialWriteData& data)
        {
            const auto* at = reinterpret_cast<const std::byte*>(&data) + row.desc->offset;
            UiFormValue value;
            switch (row.kind)
            {
            case UiFormKind::Path:
                {
                    const InternedString& name = *reinterpret_cast<const InternedString*>(at);
                    value.text = name.isValid() ? std::string(name.toView()) : std::string{};
                    break;
                }
            case UiFormKind::Bool:
                value.number[0] = *reinterpret_cast<const bool*>(at) ? 1.0 : 0.0;
                break;
            case UiFormKind::Enum:
                {
                    const std::int64_t stored = row.desc->kind == FieldKind::U32
                        ? static_cast<std::int64_t>(*reinterpret_cast<const u32*>(at))
                        : static_cast<std::int64_t>(*reinterpret_cast<const i32*>(at));
                    const auto it = std::ranges::find(row.desc->enumNames, stored, &EnumName::value);
                    value.number[0] = static_cast<double>(it - row.desc->enumNames.begin()); // past the end: unnamed
                    break;
                }
            default:
                if (row.desc->kind == FieldKind::U32)
                {
                    value.number[0] = *reinterpret_cast<const u32*>(at);
                }
                else if (row.desc->kind == FieldKind::I32)
                {
                    value.number[0] = *reinterpret_cast<const i32*>(at);
                }
                else
                {
                    for (u32 i = 0; i < row.lanes; ++i)
                    {
                        value.number[i] = reinterpret_cast<const f32*>(at)[i];
                    }
                }
                break;
            }
            return value;
        }

        const std::vector<UiFormValue>& defaultValues()
        {
            static const std::vector<UiFormValue> values = materialValues(BakedMaterialWriteData{});
            return values;
        }

        // The form's rows from the schema: range, step, enum names, the colour flag and the defaults of a default
        // descriptor. Labels and docs are the message keys "editor-project.material.<name>[.doc]".
        std::vector<UiFormField> schemaFields()
        {
            std::vector<UiFormField> fields;
            const std::vector<MaterialRow>& rows = materialRows();
            for (sizet i = 0; i < rows.size(); ++i)
            {
                const MaterialRow& row = rows[i];
                const FieldDesc& desc = *row.desc;
                const std::string base = std::string(KEYS) + std::string(desc.name);
                UiFormField field;
                field.id = InternedString(desc.name);
                field.labelKey = InternedString(base);
                field.docKey = InternedString(base + ".doc");
                field.kind = row.kind;
                field.alpha = row.alpha;
                field.defaultValue = defaultValues()[i];
                if (row.numeric())
                {
                    // an infinite bound stays open: a stored value outside the range is kept as it is
                    field.min = std::isfinite(desc.min) ? desc.min : -NO_BOUND;
                    field.max = std::isfinite(desc.max) ? desc.max : NO_BOUND;
                    field.step = desc.step > 0.0 ? desc.step : row.integer ? 1.0 : 0.1;
                    field.decimals = row.integer ? 0 : 3;
                }
                for (const EnumName& named : desc.enumNames)
                {
                    field.choices.push_back(
                        UiChoiceItem{.id = InternedString(named.name), .label = std::string(named.name)});
                }
                fields.push_back(std::move(field));
            }
            return fields;
        }

        sizet rowIndex(const InternedString field)
        {
            const std::vector<MaterialRow>& rows = materialRows();
            return static_cast<sizet>(
                std::ranges::find(rows, field.toView(), [](const MaterialRow& row) { return row.desc->name; }) -
                rows.begin());
        }

        // The value as updateMaterial takes it: a number, a bool, an enum's integer, a lane array, a texture name
        Json jsonOf(const MaterialRow& row, const UiFormValue& value)
        {
            switch (row.kind)
            {
            case UiFormKind::Path:
                return value.text;
            case UiFormKind::Bool:
                return value.number[0] != 0.0;
            case UiFormKind::Enum:
                {
                    const auto at = static_cast<sizet>(std::max(0.0, value.number[0]));
                    return at < row.desc->enumNames.size() ? row.desc->enumNames[at].value : std::int64_t{0};
                }
            default:
                if (row.lanes == 1)
                {
                    return row.integer ? Json(static_cast<std::int64_t>(value.number[0])) : Json(value.number[0]);
                }
                Json lanes = Json::array();
                for (u32 i = 0; i < row.lanes; ++i)
                {
                    lanes.push_back(value.number[i]);
                }
                return lanes;
            }
        }

        // The descriptor holds f32: a typed 0.1 equals the stored 0.1f
        bool sameValue(const MaterialRow& row, const UiFormValue& a, const UiFormValue& b)
        {
            if (row.kind == UiFormKind::Path)
            {
                return a.text == b.text;
            }
            for (u32 i = 0; i < row.lanes; ++i)
            {
                if (static_cast<float>(a.number[i]) != static_cast<float>(b.number[i]))
                {
                    return false;
                }
            }
            return true;
        }

        Vec4 colorOf(const MaterialRow& row, const UiFormValue& value)
        {
            return Vec4{static_cast<float>(value.number[0]), static_cast<float>(value.number[1]),
                        static_cast<float>(value.number[2]), row.alpha ? static_cast<float>(value.number[3]) : 1.0f};
        }

        UiFormValue colorValue(const MaterialRow& row, const Vec4& color)
        {
            return UiFormValue::ofVector(color.x, color.y, color.z, row.alpha ? color.w : 0.0);
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

    std::vector<UiFormValue> materialValues(const BakedMaterialWriteData& data)
    {
        std::vector<UiFormValue> values;
        for (const MaterialRow& row : materialRows())
        {
            values.push_back(valueOf(row, data));
        }
        return values;
    }

    struct ProjectMaterialView::Impl
    {
        UiScope* scope;
        DensityFollower* density;
        Tr tr;
        Send send;
        std::function<std::vector<std::string>()> textures;
        std::function<void()> duplicateNow;
        ScrollView scroller;
        Panel host; // as tall as its rows
        std::unique_ptr<PropertyForm> form;
        Panel buttons;
        Button duplicate;
        Label models;
        Label entities;
        Label status; // the descriptor cannot be read; why removing is refused
        PopupList popup; // the texture list a Browse button opens
        std::vector<UiChoiceItem> items;
        std::vector<std::string> choices; // the names behind items[1..]
        InternedString popupField;
        InternedString pickField; // the colour row the open picker edits
        std::unique_ptr<ColorPicker> picker;
        std::string pickName; // the material the open picker edits: a click acts on the name it was opened for
        ProjectMaterialModel model;
        std::string shownModels; // the texts as written, compared before writing again
        std::string shownEntities;
        std::string shownStatus;
        std::string shownName;
        Entity dragField = Entity::undefined(); // the number field a drag is editing
        u64 dragSerial = 0;
        u64 colorSeed = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
        bool written = false; // show() ran at least once
        bool rewrite = false; // the locale moved: the texts are written again
        bool shownReadOnly = false;

        [[nodiscard]] std::string text(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            return tr(std::string(KEYS) + std::string(key), args);
        }

        [[nodiscard]] bool editable() const { return model.shown && model.error.empty(); }

        // "A, B and 3 more": the first NAMES_SHOWN names and how many were left out
        [[nodiscard]] std::string names(const std::vector<std::string>& list, const u32 total) const
        {
            std::string out;
            for (sizet i = 0; i < std::min(list.size(), NAMES_SHOWN); ++i)
            {
                out += (i ? ", " : "") + list[i];
            }
            if (total > std::min(list.size(), NAMES_SHOWN))
            {
                const std::int64_t more = total - std::min(list.size(), NAMES_SHOWN);
                out += " " + text("usedBy.more", std::vector<MessageArg>{{"more", more}});
            }
            return out;
        }

        [[nodiscard]] std::string modelsText() const
        {
            if (!model.shown || model.models.empty())
            {
                return {};
            }
            const std::string list = names(model.models, static_cast<u32>(model.models.size()));
            return text("usedBy.models", std::vector<MessageArg>{{"names", std::string_view(list)}});
        }

        [[nodiscard]] std::string entitiesText() const
        {
            if (!model.shown)
            {
                return {};
            }
            if (!model.scanned)
            {
                return {};
            }
            if (model.entityCount == 0)
            {
                // blocked with no entity of this world and no model: another world or a loading binding holds it
                return !model.models.empty() ? std::string{} : text(model.blocked ? "usedBy.elsewhere" : "usedBy.none");
            }
            std::vector<std::string> list;
            for (const ProjectMaterialUser& user : model.entities)
            {
                list.push_back(user.name);
            }
            const std::string joined = names(list, model.entityCount);
            return text("usedBy.entities", std::vector<MessageArg>{{"names", std::string_view(joined)}});
        }

        [[nodiscard]] std::string statusText() const
        {
            if (!model.error.empty())
            {
                return text("unreadable", std::vector<MessageArg>{{"error", std::string_view(model.error)}});
            }
            return model.blocked ? text("blocked") : std::string{};
        }

        // The row's values: the descriptor's, or the defaults while no descriptor is shown
        [[nodiscard]] const std::vector<UiFormValue>& values() const
        {
            return model.values.size() == materialRows().size() ? model.values : defaultValues();
        }

        void post(const MaterialRow& row, const UiFormValue& value, const std::string& merge)
        {
            Json args{{"name", model.name}, {"fields", {{std::string(row.desc->name), jsonOf(row, value)}}}};
            if (!merge.empty())
            {
                args["merge"] = merge;
            }
            const InternedString field(row.desc->name);
            send(Json{{"command", "updateMaterial"}, {"args", std::move(args)}}.dump(), nullptr,
                 [this, field](const std::string& error) { form->setError(field, error); });
        }

        // An edit of one row: the member alone goes to updateMaterial, by the name shown; the other channels of a
        // vector are the descriptor's own (a stored value above the form's range stays as it is).
        void edited(const UiFormEdit& edit)
        {
            const sizet at = rowIndex(edit.field);
            if (!editable() || at >= materialRows().size())
            {
                return;
            }
            const MaterialRow& row = materialRows()[at];
            UiFormValue value = edit.value;
            if (row.lanes > 1 && edit.component >= 0 && edit.component < static_cast<i32>(row.lanes))
            {
                value = values()[at];
                value.number[static_cast<u32>(edit.component)] = edit.value.number[static_cast<u32>(edit.component)];
            }
            if (sameValue(row, value, values()[at]))
            {
                return;
            }
            std::string merge;
            const Entity pressed = scope->router().pressed();
            if (row.numeric() && pressed.isValid() && !edit.reset)
            {
                if (dragField != pressed)
                {
                    dragField = pressed;
                    ++dragSerial;
                }
                merge = std::format("project.material.drag.{}", dragSerial);
            }
            post(row, value, merge);
        }

        void pickedColor(const UiColorEdit& edit)
        {
            const sizet at = rowIndex(pickField);
            if (pickName.empty() || pickName != model.name || !editable() || at >= materialRows().size())
            {
                return;
            }
            const MaterialRow& row = materialRows()[at];
            const UiFormValue value = colorValue(row, edit.color);
            if (sameValue(row, value, values()[at]))
            {
                return;
            }
            Json args{{"name", pickName}, {"fields", {{std::string(row.desc->name), jsonOf(row, value)}}}};
            if (edit.gesture)
            {
                args["merge"] = std::format("project.material.color.{}.{}", colorSeed, edit.gesture);
            }
            send(Json{{"command", "updateMaterial"}, {"args", std::move(args)}}.dump(), nullptr,
                 [this, field = pickField](const std::string& error) { form->setError(field, error); });
        }

        void openPicker(const InternedString field, const Widget& swatch)
        {
            const sizet at = rowIndex(field);
            if (!model.shown || at >= materialRows().size())
            {
                return;
            }
            const MaterialRow& row = materialRows()[at];
            if (!picker)
            {
                picker = std::make_unique<ColorPicker>(*scope, InternedString("project.material.color"));
                picker->onChange([this](const UiColorEdit& edit) { pickedColor(edit); });
            }
            pickName = model.name;
            pickField = field;
            picker->setAlpha(row.alpha);
            picker->setColor(colorOf(row, values()[at]));
            picker->setReadOnly(!editable());
            picker->open(swatch);
        }

        // The Pick list: the manifest's textures at this moment, "(default texture)" first
        void openTextures(const InternedString field, const std::string_view current)
        {
            choices = textures();
            items.clear();
            items.push_back(UiChoiceItem{.id = InternedString("texture.none"),
                                         .key = InternedString(std::string(KEYS) + "texture.none")});
            i32 selected = current.empty() ? 0 : -1;
            for (u32 i = 0; i < choices.size(); ++i)
            {
                items.push_back(UiChoiceItem{.id = InternedString(std::format("texture.{}", i)), .label = choices[i]});
                if (choices[i] == current)
                {
                    selected = static_cast<i32>(i) + 1;
                }
            }
            popupField = field;
            popup.setItems(items);
            popup.setSelected(selected);
            popup.open(form->controlCell(field));
        }

        void chosen(const u32 index)
        {
            if (index < items.size() && popupField.isValid())
            {
                form->commit(popupField, UiFormValue::ofText(index == 0 ? std::string_view{} : choices[index - 1]));
            }
        }

        void closePopups()
        {
            popup.close();
            if (picker)
            {
                picker->close();
            }
            pickName.clear();
            pickField = InternedString{};
        }

        // The rows show the descriptor; a row being edited is left alone
        void follow()
        {
            const std::vector<MaterialRow>& rows = materialRows();
            for (sizet i = 0; i < rows.size(); ++i)
            {
                const InternedString id(rows[i].desc->name);
                if (!form->isEditing(id) && form->value(id) != values()[i])
                {
                    form->setValue(id, values()[i]);
                }
            }
            if (picker && picker->isOpen() && pickName == model.name)
            {
                const sizet at = rowIndex(pickField);
                if (at < rows.size() && picker->color() != colorOf(rows[at], values()[at]))
                {
                    picker->setColor(colorOf(rows[at], values()[at])); // another writer changed the colour
                }
            }
        }
    };

    ProjectMaterialView::ProjectMaterialView(UiScope& scope, DensityFollower& density, Tr tr, Send send,
                                             std::function<std::vector<std::string>()> textures,
                                             std::function<void()> duplicate) : m_impl(std::make_unique<Impl>())
    {
        Impl& impl = *m_impl;
        impl.scope = &scope;
        impl.density = &density;
        impl.tr = std::move(tr);
        impl.send = std::move(send);
        impl.textures = std::move(textures);
        impl.duplicateNow = std::move(duplicate);

        UiRectDesc scrollDesc = topLeft();
        scrollDesc.layoutItem = fillItem();
        impl.scroller = scope.createScrollView("project.material.scroll", scrollDesc);
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
        impl.host = scope.createPanel("project.material.host", hostDesc);
        bindGap(impl.host, SPACE_SM);
        impl.host.setParent(content);

        UiFormDesc desc;
        desc.rect = topLeft();
        desc.fields = schemaFields();
        impl.form = std::make_unique<PropertyForm>(scope, InternedString("project.material.form"), std::move(desc));
        impl.form->root().setParent(impl.host);
        impl.form->onEdit([&impl](const UiFormEdit& edit) { impl.edited(edit); });
        impl.form->onSwatch([&impl](const InternedString field, const Widget& swatch)
                            { impl.openPicker(field, swatch); });
        impl.form->onBrowse([&impl](const InternedString field, const std::string_view current)
                            { impl.openTextures(field, current); });
        impl.popup = scope.createPopupList("project.material.popup", {.maxRows = POPUP_ROWS, .searchable = true});
        impl.popup.onChoose([&impl](const u32 index) { impl.chosen(index); });

        UiRectDesc rowDesc =
            barDesc(Vec2{0.0f, density.density().control}, stack(UiLayoutDirection::Row, density.density().xs));
        impl.buttons = scope.createPanel("project.material.buttons", rowDesc);
        bindGap(impl.buttons, SPACE_XS);
        density.track(impl.buttons, HEIGHT_CONTROL);
        impl.buttons.setParent(impl.host);
        impl.duplicate =
            density.track(scope.createButton("project.material.duplicate", "", fitButtonDesc()), HEIGHT_CONTROL);
        bindFitPadding(impl.duplicate, SPACE_MD, SPACE_NONE);
        impl.duplicate.setStyleClass(InternedString("project.import"));
        impl.duplicate.setLocalizedText(InternedString(std::string(KEYS) + "duplicate"));
        impl.duplicate.setTooltip(InternedString("editor-project.tip.duplicate"));
        impl.duplicate.onClick([&impl] { impl.duplicateNow(); });
        impl.duplicate.setInteractable(false);
        impl.duplicate.setParent(impl.buttons);

        const auto line = [&](const char* id)
        {
            Label label = scope.createLabel(id, "", textLineDesc());
            label.setStyleClass(InternedString("settings.noteWrap"));
            label.setParent(impl.host);
            return label;
        };
        impl.models = line("project.material.models");
        impl.entities = line("project.material.entities");
        impl.status = line("project.material.status");
    }

    ProjectMaterialView::~ProjectMaterialView() = default;

    Widget ProjectMaterialView::root() const { return m_impl->scroller; }

    void ProjectMaterialView::relocalize() { m_impl->rewrite = true; }

    void ProjectMaterialView::tick()
    {
        Impl& impl = *m_impl;
        if (impl.dragField.isValid() && impl.scope->router().pressed() != impl.dragField)
        {
            impl.dragField = Entity::undefined();
        }
    }

    void ProjectMaterialView::close() { m_impl->closePopups(); }

    void ProjectMaterialView::show(const ProjectMaterialModel& next)
    {
        Impl& impl = *m_impl;
        const bool other = impl.model.name != next.name || impl.model.shown != next.shown;
        impl.model = next;
        if (other)
        {
            for (const UiFormField& field : schemaFields())
            {
                impl.form->setError(field.id, "");
            }
            impl.closePopups();
        }
        impl.follow();
        const bool readOnly = !impl.editable();
        if (!impl.written || impl.shownReadOnly != readOnly)
        {
            impl.shownReadOnly = readOnly;
            for (const UiFormField& field : schemaFields())
            {
                impl.form->setReadOnly(field.id, readOnly);
            }
        }
        if (impl.duplicate.isInteractable() != impl.editable())
        {
            impl.duplicate.setInteractable(impl.editable());
        }
        const bool all = impl.rewrite || !impl.written;
        impl.written = true;
        impl.rewrite = false;
        const auto write = [all](Label label, std::string value, std::string& shown)
        {
            if (all || value != shown)
            {
                label.setText(value);
                label.setVisible(!value.empty());
                shown = std::move(value);
            }
        };
        write(impl.models, impl.modelsText(), impl.shownModels);
        write(impl.entities, impl.entitiesText(), impl.shownEntities);
        write(impl.status, impl.statusText(), impl.shownStatus);
        if (all || next.name != impl.shownName)
        {
            impl.shownName = next.name;
            impl.duplicate.setAccessibleName(
                impl.text("duplicate.name", std::vector<MessageArg>{{"name", std::string_view(next.name)}}));
        }
    }

    void ProjectMaterialView::fit()
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

    namespace
    {
        // A row's value in the query: a lane array (a vector or colour), text (a texture name), else one number (a
        // bool 0 or 1, an enum's value)
        void writeRow(IFormatWriter& out, const std::string_view key, const MaterialRow& row, const UiFormValue& value)
        {
            if (row.kind == UiFormKind::Path)
            {
                out.writeString(key, value.text);
            }
            else if (row.lanes > 1)
            {
                out.beginArray(key, row.lanes);
                for (u32 i = 0; i < row.lanes; ++i)
                {
                    out.writeF64("", value.number[i]);
                }
                out.endArray();
            }
            else
            {
                out.writeF64(key, row.kind == UiFormKind::Enum ? jsonOf(row, value).get<double>() : value.number[0]);
            }
        }
    } // namespace

    void ProjectMaterialView::write(IFormatWriter& out) const
    {
        const Impl& impl = *m_impl;
        const ProjectMaterialModel& model = impl.model;
        out.beginObject("material");
        out.writeBool("shown", model.shown);
        out.writeString("name", model.name);
        out.writeString("error", model.error);
        out.writeBool("editable", impl.editable());
        const std::vector<MaterialRow>& rows = materialRows();
        for (sizet i = 0; i < rows.size(); ++i)
        {
            writeRow(out, rows[i].desc->name, rows[i], impl.values()[i]);
        }
        out.beginObject("usedBy");
        out.beginArray("models", static_cast<u32>(model.models.size()));
        for (const std::string& name : model.models)
        {
            out.writeString("", name);
        }
        out.endArray();
        out.writeBool("scanned", model.scanned);
        out.writeU32("entityCount", model.entityCount);
        out.writeBool("truncated", model.entityCount > model.entities.size());
        out.beginArray("entities", static_cast<u32>(model.entities.size()));
        for (const ProjectMaterialUser& user : model.entities)
        {
            out.beginObject({});
            out.writeU64("id", user.id);
            out.writeString("name", user.name);
            out.endObject();
        }
        out.endArray();
        out.endObject();
        out.writeString("status", impl.shownStatus);
        out.writeBool("duplicate", impl.duplicate.isInteractable());
        out.beginObject("picker");
        const bool open = impl.picker && impl.picker->isOpen();
        out.writeBool("open", open);
        out.writeString("name", open ? impl.pickName : std::string{});
        out.beginArray("color", open ? 4u : 0u);
        if (open)
        {
            const Vec4 color = impl.picker->color();
            for (const f32 channel : {color.x, color.y, color.z, color.w})
            {
                out.writeF64("", channel);
            }
        }
        out.endArray();
        out.endObject();
        out.writeBool("texturesOpen", impl.popup.isOpen());
        const std::vector<UiFormRowState> shown = impl.form->rows();
        out.beginArray("rows", static_cast<u32>(shown.size()));
        for (const UiFormRowState& row : shown)
        {
            out.beginObject({});
            out.writeString("field", row.field.toView());
            writeRow(out, "value", rows[rowIndex(row.field)], row.value);
            out.writeBool("modified", row.modified);
            out.writeBool("readOnly", row.readOnly);
            out.writeString("error", row.error);
            out.endObject();
        }
        out.endArray();
        out.endObject();
    }
} // namespace spite
