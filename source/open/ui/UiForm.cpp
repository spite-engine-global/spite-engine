#include "ui/UiForm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "ecs/World.hpp"
#include "localization/Localization.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        constexpr u32 NO_ROW = static_cast<u32>(-1);
        constexpr double UNBOUNDED = 1.0e300;
        constexpr float MARKER_SIZE = 6.0f;
        constexpr float RESET_WIDTH = 64.0f;
        constexpr float AXIS_MIN = 24.0f; // a vector component field
        constexpr float TAG_ALLOWANCE = 16.0f; // its axis tag
        constexpr u32 KEY_RETURN = 40;
        constexpr u32 KEY_SPACE = 44;
        constexpr u32 KEY_DOWN = 81;

        constexpr std::array<const char*, 4> AXES{"X", "Y", "Z", "W"};
        constexpr std::array<const char*, 4> CHANNELS{"R", "G", "B", "A"};

        InternedString cls(const char* name) { return InternedString(name); }

        u32 componentsOf(const UiFormField& field)
        {
            switch (field.kind)
            {
            case UiFormKind::Number:
                return 1;
            case UiFormKind::Vec2:
                return 2;
            case UiFormKind::Vec3:
                return 3;
            case UiFormKind::Vec4:
                return 4;
            case UiFormKind::Color:
                return field.alpha ? 4 : 3;
            default:
                return 0;
            }
        }

        bool sameNumber(const double a, const double b)
        {
            return std::abs(a - b) <= 1.0e-9 * std::max({1.0, std::abs(a), std::abs(b)});
        }

        // The parts of a value its kind uses: the others stay out of the comparison.
        bool sameValue(const UiFormField& field, const UiFormValue& a, const UiFormValue& b)
        {
            switch (field.kind)
            {
            case UiFormKind::String:
            case UiFormKind::Path:
                return a.text == b.text;
            case UiFormKind::Bool:
                return (a.number[0] != 0.0) == (b.number[0] != 0.0);
            case UiFormKind::Enum:
                return static_cast<std::int64_t>(a.number[0]) == static_cast<std::int64_t>(b.number[0]);
            default:
                for (u32 i = 0; i < componentsOf(field); ++i)
                {
                    if (!sameNumber(a.number[i], b.number[i]))
                    {
                        return false;
                    }
                }
                return true;
            }
        }

        UiRectDesc flexDesc(const UiLayoutDirection direction, const float gap, const UiLayoutAlign align)
        {
            UiRectDesc desc;
            desc.pivot = Vec2{0.0f, 0.0f};
            UiLayoutComponent layout;
            layout.direction = direction;
            layout.gap = gap;
            layout.align = align;
            desc.layout = layout;
            UiContentSizeComponent fit;
            fit.fitWidth = false;
            desc.contentSize = fit;
            return desc;
        }

        UiLayoutItemComponent fixed()
        {
            UiLayoutItemComponent item;
            item.shrink = 0.0f;
            return item;
        }

        UiLayoutItemComponent flex(const float grow, const float minWidth = 0.0f)
        {
            UiLayoutItemComponent item;
            item.grow = grow;
            item.shrink = 1.0f;
            item.basis = 0.0f;
            item.minSize = Vec2{minWidth, 0.0f};
            return item;
        }

        UiRectDesc sized(const Vec2 size, const UiLayoutItemComponent& item)
        {
            UiRectDesc desc;
            desc.pivot = Vec2{0.0f, 0.0f};
            desc.size = size;
            desc.layoutItem = item;
            return desc;
        }

        // A button as wide as its label (a longer locale or a larger text scale grows it), never under RESET_WIDTH.
        UiRectDesc fittedButton(const float height)
        {
            UiRectDesc desc = sized({RESET_WIDTH, height}, fixed());
            desc.layoutItem->minSize = Vec2{RESET_WIDTH, 0.0f};
            desc.contentSize =
                UiContentSizeComponent{.fitWidth = true, .fitHeight = false, .padding = Vec2{6.0f, 0.0f}};
            return desc;
        }

        // A label that fits its text in height and takes its width from the container.
        UiRectDesc textDesc(const UiLayoutItemComponent& item)
        {
            UiRectDesc desc = sized({}, item);
            UiContentSizeComponent fit;
            fit.fitWidth = false;
            desc.contentSize = fit;
            return desc;
        }

        // A label that fits its text both ways.
        UiRectDesc badgeDesc()
        {
            UiRectDesc desc = sized({}, fixed());
            desc.contentSize = UiContentSizeComponent{};
            return desc;
        }

        std::string formatNumber(const double value, const u8 decimals)
        {
            char buffer[64];
            std::snprintf(buffer, sizeof buffer, "%.*f", static_cast<int>(decimals), value);
            std::string text = buffer;
            if (text.find('.') != std::string::npos)
            {
                while (text.back() == '0')
                {
                    text.pop_back();
                }
                if (text.back() == '.')
                {
                    text.pop_back();
                }
            }
            return text;
        }
    } // namespace

    struct PropertyForm::Impl
    {
        struct Row
        {
            UiFormField field;
            Panel root; // column: the main row, then the error line
            Panel labelCell;
            Panel badges; // the row under the label: the form's badges, then a client's
            Panel controlCell;
            Panel marker;
            Label label;
            Label restart;
            Label mixedBadge;
            Label error;
            Button reset;
            Checkbox check;
            Button choice;
            TextInput input;
            Button browse;
            Button swatch; // a button so Tab reaches it and Enter or Space opens the picker
            std::array<NumberField, 4> number;
            UiFormValue value;
            u32 components = 0;
            bool mixed = false;
            bool readOnly = false;
            bool invalidText = false; // the error shown is the "not a number" one of the text being typed
            std::string errorText;
            float controlBase = 0.0f; // the control cell's own minimum width
            float controlExtra = 0.0f; // what a client parents into the cell
            std::int64_t shownChoice = -2; // the enum face's item: -1 none, -2 not built yet
            u64 namedAt = ~u64{}; // the localization revision the component names were built at
        };

        UiScope* scope = nullptr;
        InternedString id;
        UiFormDesc desc;
        Panel root;
        Tooltip tooltip;
        PopupList popup;
        std::vector<Row> rows;
        u32 popupRow = NO_ROW;
        std::function<void(const UiFormEdit&)> onEdit;
        std::function<void(InternedString, std::string_view)> onBrowse;
        std::function<void(InternedString, const Widget&)> onSwatch;

        [[nodiscard]] InternedString part(const Row& row, const std::string_view suffix) const
        {
            return InternedString(std::string(id.toView()) + "." + std::string(row.field.id.toView()) +
                                  std::string(suffix));
        }

        [[nodiscard]] u32 find(const InternedString field) const
        {
            for (u32 i = 0; i < rows.size(); ++i)
            {
                if (rows[i].field.id == field)
                {
                    return i;
                }
            }
            return NO_ROW;
        }

        // `key` resolved now with the arguments; `fallback` while it is not set or no table has it.
        [[nodiscard]] std::string resolveOr(const InternedString key, const std::string_view fallback,
                                            const std::span<const MessageArg> args = {}) const
        {
            const Localization* localization = scope->hierarchy().localization();
            if (key.isValid() && localization && localization->contains(key))
            {
                std::string scratch;
                return std::string(localization->resolve(key, args, scratch));
            }
            return std::string(fallback);
        }

        [[nodiscard]] std::string message(const char* key, const std::span<const MessageArg> args = {}) const
        {
            return resolveOr(InternedString(key), key, args);
        }

        [[nodiscard]] std::string labelText(const Row& row) const
        {
            return resolveOr(row.field.labelKey, row.field.label);
        }

        [[nodiscard]] std::string docText(const Row& row) const { return resolveOr(row.field.docKey, row.field.doc); }

        [[nodiscard]] std::string choiceText(const UiChoiceItem& item) const { return resolveOr(item.key, item.label); }

        [[nodiscard]] std::string defaultText(const Row& row) const
        {
            const UiFormField& field = row.field;
            std::string text;
            switch (field.kind)
            {
            case UiFormKind::Bool:
                text = message(field.defaultValue.number[0] != 0.0 ? uiform::ON : uiform::OFF);
                break;
            case UiFormKind::Enum:
                {
                    const auto index = static_cast<u32>(std::max(0.0, field.defaultValue.number[0]));
                    text = index < field.choices.size() ? choiceText(field.choices[index]) : std::string{};
                    break;
                }
            case UiFormKind::String:
            case UiFormKind::Path:
                text = field.defaultValue.text;
                break;
            default:
                for (u32 i = 0; i < row.components; ++i)
                {
                    text += (i ? ", " : "") + formatNumber(field.defaultValue.number[i], field.decimals);
                }
                break;
            }
            return text;
        }

        // -- build ---------------------------------------------------------------------------------------------

        void build()
        {
            UiRectDesc rootDesc = desc.rect;
            rootDesc.pivot = Vec2{0.0f, 0.0f};
            UiLayoutComponent column;
            column.direction = UiLayoutDirection::Column;
            column.gap = desc.gap;
            column.align = UiLayoutAlign::Stretch;
            rootDesc.layout = column;
            if (!rootDesc.contentSize)
            {
                UiContentSizeComponent fit;
                fit.fitWidth = false;
                rootDesc.contentSize = fit;
            }
            root = scope->createPanel(id, rootDesc);

            UiTooltipDesc tipDesc;
            tipDesc.delay = desc.tooltipDelay;
            tooltip = scope->createTooltip(InternedString(std::string(id.toView()) + ".tip"), tipDesc);
            tooltip.onShow([this](const Entity target) { fillTooltip(target); });
            UiDropdownDesc popupDesc;
            popupDesc.maxRows = 10;
            popup = scope->createPopupList(InternedString(std::string(id.toView()) + ".choices"), popupDesc);
            popup.onChoose([this](const u32 index) { chosen(index); });

            rows.reserve(desc.fields.size());
            for (const UiFormField& field : desc.fields)
            {
                Row& row = rows.emplace_back();
                row.field = field;
                row.components = componentsOf(field);
                row.value = field.defaultValue;
                row.readOnly = field.readOnly;
                buildRow(static_cast<u32>(rows.size() - 1));
            }
            for (Row& row : rows)
            {
                showValue(row);
                if (row.readOnly)
                {
                    setControlInteractable(row, false);
                }
                refreshChrome(row);
            }
        }

        void setLabelText(Label& label, const Row& row)
        {
            if (row.field.labelKey.isValid())
            {
                label.setLocalizedText(row.field.labelKey);
            }
            else
            {
                label.setText(row.field.label);
            }
        }

        void nameControl(Widget control, const Row& row)
        {
            if (row.field.labelKey.isValid())
            {
                control.setAccessibleNameKey(row.field.labelKey);
            }
            else
            {
                control.setAccessibleName(row.field.label);
            }
        }

        Label badge(const Row& row, const std::string_view suffix, const char* key, const char* styleClass)
        {
            Label out = scope->createLabel(part(row, suffix), "", badgeDesc());
            out.setLocalizedText(InternedString(key));
            out.setStyleClass(cls(styleClass));
            out.setVisible(false);
            return out;
        }

        void buildRow(const u32 index)
        {
            Row& row = rows[index];
            const UiFormField& field = row.field;
            const float height = desc.rowHeight;

            row.root =
                scope->createPanel(part(row, ""), flexDesc(UiLayoutDirection::Column, 2.0f, UiLayoutAlign::Stretch));
            row.root.setParent(root);
            Panel main = scope->createPanel(part(row, ".main"),
                                            flexDesc(UiLayoutDirection::Row, desc.gap, UiLayoutAlign::Center));
            main.setParent(row.root);

            row.marker = scope->createPanel(part(row, ".marker"), sized({MARKER_SIZE, MARKER_SIZE}, fixed()));
            row.marker.setParent(main);
            row.marker.setInteractable(false);

            // the label column: the label takes the cell's width and wraps, the badges sit under it
            UiRectDesc cellDesc = flexDesc(UiLayoutDirection::Column, 0.0f, UiLayoutAlign::Stretch);
            cellDesc.layoutItem = flex(desc.labelGrow, desc.labelMinWidth);
            row.labelCell = scope->createPanel(part(row, ".labelCell"), cellDesc);
            row.labelCell.setParent(main);
            row.label = scope->createLabel(part(row, ".label"), "", textDesc(UiLayoutItemComponent{}));
            row.label.setParent(row.labelCell);
            row.label.setStyleClass(cls("form.label"));
            setLabelText(row.label, row);
            row.badges = scope->createPanel(part(row, ".badges"),
                                            flexDesc(UiLayoutDirection::Row, desc.gap, UiLayoutAlign::Start));
            Panel& badges = row.badges;
            badges.setParent(row.labelCell);
            row.restart = badge(row, ".restart", uiform::RESTART, "form.badge");
            row.restart.setParent(badges);
            row.restart.setVisible(field.restartRequired);
            row.mixedBadge = badge(row, ".mixed", uiform::MIXED, "form.badge");
            row.mixedBadge.setParent(badges);
            tooltip.attach(row.label, -1.0f);

            UiRectDesc controlDesc = flexDesc(UiLayoutDirection::Row, 2.0f, UiLayoutAlign::Center);
            // a vector's cell never shrinks below its fields and their tags, so its parts cannot overprint one another
            const float partsWidth = row.components > 1 ? (field.kind == UiFormKind::Color ? height + 2.0f : 0.0f) +
                    static_cast<float>(row.components) * (AXIS_MIN + TAG_ALLOWANCE + 2.0f)
                                                        : 0.0f;
            row.controlBase = std::max(desc.controlMinWidth, partsWidth);
            controlDesc.layoutItem = flex(desc.controlGrow, row.controlBase);
            row.controlCell = scope->createPanel(part(row, ".control"), controlDesc);
            row.controlCell.setParent(main);
            buildControls(index, height);

            UiRectDesc tailDesc = flexDesc(UiLayoutDirection::Row, 0.0f, UiLayoutAlign::Center);
            tailDesc.layoutItem = fixed();
            tailDesc.layoutItem->minSize = Vec2{RESET_WIDTH, 0.0f};
            tailDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = false};
            Panel tail = scope->createPanel(part(row, ".tail"), tailDesc);
            tail.setParent(main);
            UiRectDesc resetDesc = fittedButton(height);
            row.reset = scope->createButton(part(row, ".reset"), "", resetDesc);
            row.reset.setParent(tail);
            row.reset.setLocalizedText(InternedString(uiform::RESET));
            row.reset.setStyleClass(cls("form.reset"));
            row.reset.setVisible(false);
            row.reset.onClick([this, index] { reset(index); });

            row.error = scope->createLabel(part(row, ".error"), "", textDesc(flex(1.0f)));
            row.error.setParent(row.root);
            row.error.setStyleClass(cls("form.error"));
            row.error.setAccessibleRole(UiRole::Alert);
            row.error.setVisible(false);
        }

        NumberField numberField(Row& row, const u32 component, const float height)
        {
            UiNumberDesc number;
            number.rect = sized({0.0f, height}, flex(1.0f, AXIS_MIN));
            number.min = -UNBOUNDED;
            number.max = UNBOUNDED;
            number.step = row.field.step;
            number.decimals = row.field.decimals;
            static constexpr const char* LOWER[4] = {"x", "y", "z", "w"};
            const std::string suffix = row.components == 1 ? ".input" : std::string(".") + LOWER[component];
            return scope->createNumberField(part(row, suffix), number);
        }

        void buildControls(const u32 index, const float height)
        {
            Row& row = rows[index];
            const UiFormField& field = row.field;
            switch (field.kind)
            {
            case UiFormKind::Bool:
                {
                    row.check = scope->createCheckbox(part(row, ".check"), "", sized({height, height}, fixed()), false);
                    // the row's label names it: the box alone, without the empty label's gap
                    row.check.label().setVisible(false);
                    row.check.setParent(row.controlCell);
                    nameControl(row.check, row);
                    row.check.onChange(
                        [this, index](const bool checked)
                        {
                            UiFormValue value = rows[index].value;
                            value.number[0] = checked ? 1.0 : 0.0;
                            edit(index, value);
                        });
                    break;
                }
            case UiFormKind::Enum:
                {
                    row.choice = scope->createButton(part(row, ".choice"), "", sized({0.0f, height}, flex(1.0f, 0.0f)));
                    row.choice.setParent(row.controlCell);
                    row.choice.setAccessibleRole(UiRole::ComboBox);
                    nameControl(row.choice, row);
                    row.choice.onPress(
                        [this, index](const u8 button, Vec2)
                        {
                            if (button == 0)
                            {
                                openChoices(index);
                            }
                        });
                    row.choice.onKeyDown(
                        [this, index](const u32 scancode, u8, bool)
                        {
                            if (scancode != KEY_RETURN && scancode != KEY_SPACE && scancode != KEY_DOWN)
                            {
                                return false;
                            }
                            openChoices(index);
                            return true;
                        });
                    break;
                }
            case UiFormKind::String:
            case UiFormKind::Path:
                {
                    row.input = scope->createTextInput(part(row, ".input"), sized({0.0f, height}, flex(1.0f, 24.0f)));
                    row.input.setParent(row.controlCell);
                    nameControl(row.input, row);
                    row.input.onCommit([this, index](const std::string_view text)
                                       { edit(index, UiFormValue::ofText(text)); });
                    if (field.kind == UiFormKind::Path)
                    {
                        row.browse = scope->createButton(part(row, ".browse"), "", fittedButton(height));
                        row.browse.setParent(row.controlCell);
                        row.browse.setLocalizedText(InternedString(uiform::BROWSE));
                        row.browse.setVisible(false);
                        row.browse.onClick(
                            [this, index]
                            {
                                if (onBrowse)
                                {
                                    onBrowse(rows[index].field.id, rows[index].value.text);
                                }
                            });
                    }
                    break;
                }
            default:
                {
                    const bool single = row.components == 1;
                    if (field.kind == UiFormKind::Color)
                    {
                        row.swatch = scope->createButton(part(row, ".swatch"), "", sized({height, height}, fixed()));
                        row.swatch.setParent(row.controlCell);
                        row.swatch.setInteractable(static_cast<bool>(onSwatch));
                        row.swatch.onPress(
                            [this, index](const u8 button, Vec2)
                            {
                                if (button == 0 && onSwatch)
                                {
                                    onSwatch(rows[index].field.id, rows[index].swatch);
                                }
                            });
                        row.swatch.setStyleClass(cls("form.swatch"));
                        scope->world().addComponent<UiVisualComponent>(row.swatch.entity(), UiVisualComponent{});
                    }
                    for (u32 i = 0; i < row.components; ++i)
                    {
                        if (!single)
                        {
                            const bool colour = field.kind == UiFormKind::Color;
                            Label tag = scope->createLabel(
                                part(row, std::string(".tag.") + (colour ? CHANNELS[i] : AXES[i])), "", badgeDesc());
                            tag.setLocalizedText(InternedString(colour ? uiform::CHANNELS[i] : uiform::AXES[i]));
                            tag.setParent(row.controlCell);
                            tag.setStyleClass(cls("form.axis"));
                            tag.setAccessibleRole(UiRole::Presentation);
                        }
                        NumberField number = numberField(row, i, height);
                        number.setParent(row.controlCell);
                        row.number[i] = number;
                        if (single)
                        {
                            nameControl(number, row);
                        }
                        number.onChange([this, index, i](std::string_view) { typed(index, i); });
                        number.onNumber(
                            [this, index, i](const double value)
                            {
                                UiFormValue next = rows[index].value;
                                next.number[i] = value;
                                edit(index, next, false, rows[index].components > 1 ? static_cast<i32>(i) : -1);
                            });
                    }
                    break;
                }
            }
        }

        // -- showing -------------------------------------------------------------------------------------------

        [[nodiscard]] bool editing(const Row& row) const
        {
            const UiRouter& router = scope->router();
            const auto busy = [&](const Widget& widget)
            {
                return widget.isValid() && (router.focused() == widget.entity() || router.pressed() == widget.entity());
            };
            if (busy(row.input) || busy(row.check) || busy(row.choice))
            {
                return true;
            }
            for (u32 i = 0; i < row.components; ++i)
            {
                if (busy(row.number[i]))
                {
                    return true;
                }
            }
            return false;
        }

        // The face shows the chosen item by its key when it has one, so a locale switch updates it.
        void showChoice(Row& row)
        {
            auto index = static_cast<std::int64_t>(row.value.number[0]);
            if (index < 0 || index >= static_cast<std::int64_t>(row.field.choices.size()))
            {
                index = -1;
            }
            if (index == row.shownChoice)
            {
                return;
            }
            row.shownChoice = index;
            if (index < 0)
            {
                row.choice.setText("");
                return;
            }
            const UiChoiceItem& item = row.field.choices[static_cast<u32>(index)];
            if (item.key.isValid())
            {
                row.choice.setLocalizedText(item.key);
            }
            else
            {
                row.choice.setText(item.label);
            }
        }

        // The controls show the row's value; a write happens only where the shown value differs.
        void showValue(Row& row)
        {
            switch (row.field.kind)
            {
            case UiFormKind::Bool:
                if (row.check.checked() != (row.value.number[0] != 0.0))
                {
                    row.check.setChecked(row.value.number[0] != 0.0);
                }
                break;
            case UiFormKind::Enum:
                showChoice(row);
                break;
            case UiFormKind::String:
            case UiFormKind::Path:
                if (row.input.value() != row.value.text)
                {
                    row.input.setText(row.value.text);
                }
                break;
            default:
                for (u32 i = 0; i < row.components; ++i)
                {
                    if (row.number[i].number() != row.value.number[i])
                    {
                        row.number[i].setNumber(row.value.number[i]);
                    }
                }
                if (row.field.kind == UiFormKind::Color)
                {
                    const Vec4 tint{static_cast<float>(row.value.number[0]), static_cast<float>(row.value.number[1]),
                                    static_cast<float>(row.value.number[2]),
                                    row.components > 3 ? static_cast<float>(row.value.number[3]) : 1.0f};
                    if (scope->world().getConstComponent<UiVisualComponent>(row.swatch.entity()).tint != tint)
                    {
                        scope->world().getComponent<UiVisualComponent>(row.swatch.entity()).tint = tint;
                        scope->hierarchy().markVisualDirty(row.swatch.entity());
                    }
                }
                break;
            }
        }

        [[nodiscard]] bool modified(const Row& row) const
        {
            return !sameValue(row.field, row.value, row.field.defaultValue);
        }

        void setControlInteractable(Row& row, const bool on)
        {
            for (Widget control : {static_cast<Widget>(row.check), static_cast<Widget>(row.choice),
                                   static_cast<Widget>(row.input), static_cast<Widget>(row.browse)})
            {
                if (control.isValid())
                {
                    control.setInteractable(on);
                }
            }
            for (u32 i = 0; i < row.components; ++i)
            {
                row.number[i].setInteractable(on);
            }
        }

        // The accessible names of a vector's fields ("<label> <tag>"), rebuilt when the tables change.
        void syncComponentNames(Row& row)
        {
            const Localization* localization = scope->hierarchy().localization();
            const u64 revision = localization ? localization->revision() : 0;
            if (row.components < 2 || row.namedAt == revision)
            {
                return;
            }
            row.namedAt = revision;
            const std::string label = labelText(row);
            const bool colour = row.field.kind == UiFormKind::Color;
            for (u32 i = 0; i < row.components; ++i)
            {
                row.number[i].setAccessibleName(label + " " + message(colour ? uiform::CHANNELS[i] : uiform::AXES[i]));
            }
            if (row.swatch.isValid() && onSwatch)
            {
                row.swatch.setAccessibleRole(UiRole::Button);
                row.swatch.setAccessibleName(label);
            }
        }

        void refreshChrome(Row& row)
        {
            syncComponentNames(row);
            const bool isModified = modified(row);
            row.marker.setStyleClass(cls(isModified ? "form.marker.modified" : "form.marker"));
            row.label.setStyleClass(cls(row.readOnly ? "form.label.readonly" : "form.label"));
            const bool resettable = !row.readOnly && (isModified || row.mixed);
            if (row.reset.isVisible() != resettable)
            {
                row.reset.setVisible(resettable);
            }
            if (row.mixedBadge.isVisible() != row.mixed)
            {
                row.mixedBadge.setVisible(row.mixed);
            }
            if (row.browse.isValid())
            {
                const bool shown = static_cast<bool>(onBrowse) && !row.readOnly;
                if (row.browse.isVisible() != shown)
                {
                    row.browse.setVisible(shown);
                }
            }
            const bool hasError = !row.errorText.empty();
            if (row.error.isVisible() != hasError)
            {
                row.error.setVisible(hasError);
            }
            if (hasError && row.error.text() != row.errorText)
            {
                row.error.setText(row.errorText);
            }
            for (Widget control : {static_cast<Widget>(row.input), static_cast<Widget>(row.choice)})
            {
                if (control.isValid())
                {
                    control.setStyleClass(hasError ? cls("form.field.error") : InternedString{});
                }
            }
            for (u32 i = 0; i < row.components; ++i)
            {
                row.number[i].setStyleClass(hasError ? cls("form.field.error") : InternedString{});
            }
        }

        void setErrorText(Row& row, const std::string& text, const bool fromTyping)
        {
            row.invalidText = fromTyping && !text.empty();
            if (row.errorText == text)
            {
                return;
            }
            row.errorText = text;
            refreshChrome(row);
        }

        // -- input ---------------------------------------------------------------------------------------------

        // Typing into a number field: text that is not a number shows its error until the text is one again.
        void typed(const u32 index, const u32 component)
        {
            Row& row = rows[index];
            const bool valid =
                scope->world().getConstComponent<UiNumericComponent>(row.number[component].entity()).valid;
            if (!valid)
            {
                setErrorText(row, message(uiform::NOT_NUMBER), true);
            }
            else if (row.invalidText)
            {
                setErrorText(row, {}, false);
            }
        }

        [[nodiscard]] std::string rangeMessage(const UiFormField& field) const
        {
            const bool low = field.min > -UNBOUNDED;
            const bool high = field.max < UNBOUNDED;
            const std::array<MessageArg, 2> args{MessageArg{"min", field.min}, MessageArg{"max", field.max}};
            if (low && high)
            {
                return message(uiform::RANGE, args);
            }
            return message(low ? uiform::AT_LEAST : uiform::AT_MOST, args);
        }

        // False (with the error shown) when a number component lies outside the field's range.
        bool validate(Row& row, const UiFormValue& value)
        {
            if (row.field.kind == UiFormKind::Enum)
            {
                const double index = value.number[0];
                if (!(index >= 0.0 && index < static_cast<double>(row.field.choices.size()) &&
                      index == std::floor(index)))
                {
                    const double last = std::max(0.0, static_cast<double>(row.field.choices.size()) - 1.0);
                    const std::array<MessageArg, 2> args{MessageArg{"min", 0.0}, MessageArg{"max", last}};
                    setErrorText(row, message(uiform::RANGE, args), false);
                    return false;
                }
                return true;
            }
            for (u32 i = 0; i < row.components; ++i)
            {
                if (!std::isfinite(value.number[i]) || value.number[i] < row.field.min ||
                    value.number[i] > row.field.max)
                {
                    setErrorText(row, rangeMessage(row.field), false);
                    return false;
                }
            }
            return true;
        }

        bool edit(const u32 index, UiFormValue value, const bool resetting = false, const i32 component = -1)
        {
            Row& row = rows[index];
            if (row.readOnly || (!resetting && !validate(row, value)))
            {
                return false;
            }
            row.value = std::move(value);
            row.mixed = false;
            row.errorText.clear();
            row.invalidText = false;
            showValue(row);
            refreshChrome(row);
            if (onEdit)
            {
                const auto fn = onEdit; // the client may replace it
                fn(UiFormEdit{row.field.id, row.value, component, resetting});
            }
            return true;
        }

        void reset(const u32 index) { edit(index, rows[index].field.defaultValue, true); }

        // -- enum popup ----------------------------------------------------------------------------------------

        void openChoices(const u32 index)
        {
            Row& row = rows[index];
            if (row.readOnly || row.field.choices.empty())
            {
                return;
            }
            popupRow = index;
            popup.setItems(row.field.choices);
            popup.setSelected(static_cast<i32>(row.value.number[0]));
            popup.open(row.choice);
        }

        void chosen(const u32 item)
        {
            if (popupRow == NO_ROW)
            {
                return;
            }
            UiFormValue value = rows[popupRow].value;
            value.number[0] = static_cast<double>(item);
            edit(popupRow, value);
        }

        // -- tooltip -------------------------------------------------------------------------------------------

        void fillTooltip(const Entity target)
        {
            for (const Row& row : rows)
            {
                if (row.label.entity() != target)
                {
                    continue;
                }
                std::string text = labelText(row);
                if (const std::string doc = docText(row); !doc.empty())
                {
                    text += "\n" + doc;
                }
                if (!row.readOnly)
                {
                    const std::string value = defaultText(row);
                    const std::array<MessageArg, 1> args{MessageArg{"value", std::string_view(value)}};
                    text += "\n" + message(uiform::DEFAULT, args);
                }
                tooltip.label().setText(text);
                return;
            }
        }
    };

    PropertyForm::PropertyForm(UiScope& scope, const InternedString id, UiFormDesc desc) :
        m_impl(std::make_unique<Impl>())
    {
        m_impl->scope = &scope;
        m_impl->id = id;
        m_impl->desc = std::move(desc);
        m_impl->build();
    }

    PropertyForm::~PropertyForm()
    {
        // every callback of the form's widgets captures it: destroy them (UiScope::destroy purges the reactions at
        // once); the overlays are roots of their own and not under root()
        Impl& impl = *m_impl;
        std::vector<Entity> parts;
        for (const Widget& top :
             {static_cast<Widget>(impl.root), static_cast<Widget>(impl.tooltip), static_cast<Widget>(impl.popup)})
        {
            if (!top.isValid())
            {
                continue;
            }
            parts.push_back(top.entity());
            for (sizet at = parts.size() - 1; at < parts.size(); ++at)
            {
                for (const Entity child : impl.scope->hierarchy().childrenOf(parts[at]))
                {
                    parts.push_back(child);
                }
            }
        }
        for (const Entity part : parts)
        {
            if (impl.scope->owns(part))
            {
                impl.scope->destroy(Widget(impl.scope, part, impl.scope->generation()));
            }
        }
    }

    Panel PropertyForm::root() const { return m_impl->root; }

    u32 PropertyForm::fieldCount() const { return static_cast<u32>(m_impl->rows.size()); }

    bool PropertyForm::has(const InternedString field) const { return m_impl->find(field) != NO_ROW; }

    Widget PropertyForm::row(const InternedString field) const
    {
        const u32 index = m_impl->find(field);
        return index == NO_ROW ? Widget{} : m_impl->rows[index].root;
    }

    Widget PropertyForm::label(const InternedString field) const
    {
        const u32 index = m_impl->find(field);
        return index == NO_ROW ? Widget{} : m_impl->rows[index].label;
    }

    Panel PropertyForm::labelCell(const InternedString field) const
    {
        const u32 index = m_impl->find(field);
        return index == NO_ROW ? Panel{} : m_impl->rows[index].labelCell;
    }

    Panel PropertyForm::badges(const InternedString field) const
    {
        const u32 index = m_impl->find(field);
        return index == NO_ROW ? Panel{} : m_impl->rows[index].badges;
    }

    void PropertyForm::setControlExtra(const InternedString field, const float width)
    {
        if (const u32 index = m_impl->find(field); index != NO_ROW)
        {
            Impl::Row& row = m_impl->rows[index];
            row.controlExtra = std::max(0.0f, width);
            const float gap = row.controlExtra > 0.0f ? 2.0f : 0.0f; // the cell's own gap
            row.controlCell.setLayoutItem(flex(m_impl->desc.controlGrow, row.controlBase + row.controlExtra + gap));
        }
    }

    Panel PropertyForm::controlCell(const InternedString field) const
    {
        const u32 index = m_impl->find(field);
        return index == NO_ROW ? Panel{} : m_impl->rows[index].controlCell;
    }

    bool PropertyForm::setValue(const InternedString field, const UiFormValue& value)
    {
        const u32 index = m_impl->find(field);
        if (index == NO_ROW || m_impl->editing(m_impl->rows[index]))
        {
            return false;
        }
        Impl::Row& row = m_impl->rows[index];
        row.value = value;
        m_impl->showValue(row);
        m_impl->refreshChrome(row);
        return true;
    }

    UiFormValue PropertyForm::value(const InternedString field) const
    {
        const u32 index = m_impl->find(field);
        return index == NO_ROW ? UiFormValue{} : m_impl->rows[index].value;
    }

    bool PropertyForm::isEditing(const InternedString field) const
    {
        const u32 index = m_impl->find(field);
        return index != NO_ROW && m_impl->editing(m_impl->rows[index]);
    }

    void PropertyForm::setMixed(const InternedString field, const bool mixed)
    {
        if (const u32 index = m_impl->find(field); index != NO_ROW && m_impl->rows[index].mixed != mixed)
        {
            m_impl->rows[index].mixed = mixed;
            m_impl->refreshChrome(m_impl->rows[index]);
        }
    }

    void PropertyForm::setError(const InternedString field, const std::string_view message)
    {
        if (const u32 index = m_impl->find(field); index != NO_ROW)
        {
            m_impl->setErrorText(m_impl->rows[index], std::string(message), false);
        }
    }

    void PropertyForm::setReadOnly(const InternedString field, const bool readOnly)
    {
        const u32 index = m_impl->find(field);
        if (index == NO_ROW || m_impl->rows[index].readOnly == readOnly)
        {
            return;
        }
        Impl::Row& row = m_impl->rows[index];
        row.readOnly = readOnly;
        m_impl->setControlInteractable(row, !readOnly);
        m_impl->refreshChrome(row);
    }

    bool PropertyForm::commit(const InternedString field, const UiFormValue& value)
    {
        const u32 index = m_impl->find(field);
        return index != NO_ROW && m_impl->edit(index, value);
    }

    void PropertyForm::onEdit(std::function<void(const UiFormEdit&)> fn) { m_impl->onEdit = std::move(fn); }

    void PropertyForm::onBrowse(std::function<void(InternedString, std::string_view)> fn)
    {
        m_impl->onBrowse = std::move(fn);
        for (Impl::Row& row : m_impl->rows)
        {
            m_impl->refreshChrome(row);
        }
    }

    void PropertyForm::onSwatch(std::function<void(InternedString, const Widget&)> fn)
    {
        m_impl->onSwatch = std::move(fn);
        for (Impl::Row& row : m_impl->rows)
        {
            if (row.swatch.isValid())
            {
                row.swatch.setInteractable(static_cast<bool>(m_impl->onSwatch));
                row.namedAt = ~u64{};
                m_impl->refreshChrome(row);
            }
        }
    }

    UiFormRowState PropertyForm::state(const InternedString field) const
    {
        UiFormRowState out;
        const u32 index = m_impl->find(field);
        if (index == NO_ROW)
        {
            return out;
        }
        const Impl::Row& row = m_impl->rows[index];
        out.field = field;
        out.value = row.value;
        out.modified = m_impl->modified(row);
        out.mixed = row.mixed;
        out.readOnly = row.readOnly;
        out.restartRequired = row.field.restartRequired;
        out.resettable = !row.readOnly && (out.modified || row.mixed);
        out.error = row.errorText;
        return out;
    }

    std::vector<UiFormRowState> PropertyForm::rows() const
    {
        std::vector<UiFormRowState> out;
        out.reserve(m_impl->rows.size());
        for (const Impl::Row& row : m_impl->rows)
        {
            out.push_back(state(row.field.id));
        }
        return out;
    }
} // namespace spite
