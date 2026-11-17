#pragma once

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "ui/UiExport.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class UiScope;

    // The messages the form shows (table assets/loc/en/engine.json, namespace `engine`): every one it uses is here, so
    // a test can check each against the real tables and the pseudo-locale.
    namespace uiform
    {
        inline constexpr const char* RESET = "engine.ui.form.reset";
        inline constexpr const char* BROWSE = "engine.ui.form.browse";
        inline constexpr const char* RESTART = "engine.ui.form.restart";
        inline constexpr const char* MIXED = "engine.ui.form.mixed";
        inline constexpr const char* ON = "engine.ui.form.on";
        inline constexpr const char* OFF = "engine.ui.form.off";
        inline constexpr const char* DEFAULT = "engine.ui.form.default"; // {value}
        inline constexpr const char* NOT_NUMBER = "engine.ui.form.notNumber";
        inline constexpr const char* RANGE = "engine.ui.form.range"; // {min} {max}
        inline constexpr const char* AT_LEAST = "engine.ui.form.atLeast"; // {min}
        inline constexpr const char* AT_MOST = "engine.ui.form.atMost"; // {max}
        // The tags of a vector's fields (X Y Z W) and a colour's (R G B A)
        inline constexpr std::array<const char*, 4> AXES{"engine.ui.form.axisX", "engine.ui.form.axisY",
                                                         "engine.ui.form.axisZ", "engine.ui.form.axisW"};
        inline constexpr std::array<const char*, 4> CHANNELS{"engine.ui.form.channelR", "engine.ui.form.channelG",
                                                             "engine.ui.form.channelB", "engine.ui.form.channelA"};
        inline constexpr std::array<const char*, 19> MESSAGES{
            RESET,   BROWSE,  RESTART, MIXED,   ON,      OFF,         DEFAULT,     NOT_NUMBER,  RANGE,      AT_LEAST,
            AT_MOST, AXES[0], AXES[1], AXES[2], AXES[3], CHANNELS[0], CHANNELS[1], CHANNELS[2], CHANNELS[3]};
    } // namespace uiform

    enum class UiFormKind : u8
    {
        Number, // a NumberField over [min, max]
        Bool, // a Checkbox
        Enum, // a face button that opens the form's shared popup list of `choices`
        String, // a TextInput; an edit commits on Enter or focus loss
        Path, // a TextInput, plus a Browse button while the form has onBrowse
        Vec2, // 2, 3 or 4 labelled NumberFields on one row (X Y, X Y Z, X Y Z W)
        Vec3,
        Vec4,
        Color, // r g b a NumberFields on one row, after a swatch showing the colour
    };

    // A field's value, one shape for every kind: Number and Bool (0 or 1) use number[0], Enum the index into the
    // field's choices, Vec2..Vec4 and Color the first 2..4 numbers, String and Path `text`.
    struct UiFormValue
    {
        std::array<double, 4> number{};
        std::string text;

        [[nodiscard]] static UiFormValue ofNumber(double value)
        {
            UiFormValue out;
            out.number[0] = value;
            return out;
        }
        [[nodiscard]] static UiFormValue ofBool(bool value) { return ofNumber(value ? 1.0 : 0.0); }
        [[nodiscard]] static UiFormValue ofText(std::string_view value)
        {
            UiFormValue out;
            out.text = value;
            return out;
        }
        [[nodiscard]] static UiFormValue ofVector(double x, double y, double z = 0.0, double w = 0.0)
        {
            UiFormValue out;
            out.number = {x, y, z, w};
            return out;
        }
        friend bool operator==(const UiFormValue&, const UiFormValue&) = default;
    };

    // One row of the form. `id` is the key every call and callback names the field by, and the row's element id
    // suffix ("<form id>.<id>").
    struct UiFormField
    {
        InternedString id{};
        std::string label;
        InternedString labelKey{}; // set: a localization key that replaces `label` and follows the locale
        // The tooltip beside the label: the label, the doc and the default; `docKey` replaces `doc` likewise.
        std::string doc;
        InternedString docKey{};
        UiFormKind kind = UiFormKind::Number;
        double min = -1.0e300; // Number and the vector kinds: a value outside [min, max] is an error, not written
        double max = 1.0e300;
        double step = 0.1; // per UI pixel of horizontal drag
        u8 decimals = 3;
        bool alpha = true; // Color: false drops the fourth channel (R G B)
        std::vector<UiChoiceItem> choices; // Enum
        UiFormValue defaultValue; // the value the Reset button restores; a different value marks the row modified
        bool readOnly = false; // shown, not editable, no Reset
        bool restartRequired = false; // a badge: the change applies after a restart
    };

    struct UiFormDesc
    {
        UiRectDesc rect; // the root column; width stretches in a layout container, else rect.size.x
        std::vector<UiFormField> fields;
        float rowHeight = 24.0f; // the fields' height, unscaled UI pixels
        float gap = 4.0f; // between rows, and between the parts of a row
        float labelGrow = 2.0f; // flex weights of the label cell and the control cell
        float controlGrow = 3.0f;
        float labelMinWidth = 48.0f; // below these the cells shrink, never overlap
        float controlMinWidth = 96.0f;
        float tooltipDelay = 0.5f; // seconds; the tooltip of a label
    };

    // What an edit reports: the field, the value now, which component of a vector or colour the user edited (the
    // others are unchanged: a multi-edit writes only that one; -1 for the whole value: a Number, Bool, Enum, String
    // or Path, the Reset button, commit()), and whether the Reset button made it (the value is then the default, so a
    // client that stores overrides removes it).
    struct UiFormEdit
    {
        InternedString field{};
        UiFormValue value;
        i32 component = -1;
        bool reset = false;
    };

    // A row's state as a reader (a test, a control query) sees it.
    struct UiFormRowState
    {
        InternedString field{};
        UiFormValue value;
        bool modified = false; // the value differs from the default
        bool mixed = false;
        bool readOnly = false;
        bool restartRequired = false;
        bool resettable = false; // the Reset button is shown
        std::string error; // the message under the row; empty: none
    };

    // Schema-driven label/value rows: a flexible label column that wraps and ellipsizes (the full text and
    // the doc are the label's tooltip), the control(s) of the field's kind, a modified-from-default marker, a Reset
    // button while modified, an inline error line, a restart-required badge, a mixed-value badge for multi-edit, and
    // the read-only look. One form is one column Panel ("<id>", `root()`) a panel puts in its document slot; the
    // client owns the schema and the values: an edit is reported through onEdit after it validated (a number outside
    // [min, max] or text that is not a number shows an error and is not reported) and the form shows what the client
    // sets with setValue. Row parts are "<id>.<field>" with ".label", ".marker", ".reset", ".restart", ".mixed",
    // ".error" and the controls ".input" (".x" ".y" ".z" ".w" of a vector; ".swatch" of a colour), ".check",
    // ".choice" and ".browse". Style classes: form.label, form.label.readonly, form.marker, form.marker.modified,
    // form.reset, form.badge, form.error, form.field.error, form.swatch. Strings: engine.ui.form.*.
    // The form is a presenter object over scope-owned widgets: create it after the scope, destroy it before. Its
    // destructor destroys the row widgets, the tooltip and the popup (their callbacks hold the form), so the scope
    // may outlive it and a form with the same id can be built again. It holds the client's onEdit and onBrowse
    // functions (outside UiScope::countCallbacksIn): a client in a module that unloads destroys the form first.
    // A client extends a row through labelCell and controlCell (a unit, a source badge, a swatch button, a toggle),
    // not through new field kinds.
    class SPITE_UI_API PropertyForm
    {
    public:
        PropertyForm(UiScope& scope, InternedString id, UiFormDesc desc);
        ~PropertyForm();
        PropertyForm(const PropertyForm&) = delete;
        PropertyForm& operator=(const PropertyForm&) = delete;

        [[nodiscard]] Panel root() const;
        [[nodiscard]] u32 fieldCount() const;
        [[nodiscard]] bool has(InternedString field) const;
        // The row's panel and its label, for placement and probes; invalid for an unknown field.
        [[nodiscard]] Widget row(InternedString field) const;
        [[nodiscard]] Widget label(InternedString field) const;
        // The row's label column (the label, then its badges) and control cell (the field's controls, in a flex row
        // the cell shares with what a client parents here): invalid for an unknown field.
        [[nodiscard]] Panel labelCell(InternedString field) const;
        // The row of badges under the label (Restart, Mixed): a client's badge parented here sits on that line, after
        // them, rather than on a line of its own. Invalid for an unknown field.
        [[nodiscard]] Panel badges(InternedString field) const;
        [[nodiscard]] Panel controlCell(InternedString field) const;
        // Parent widgets in the cells; do not re-layout or destroy a cell. `width` is the room what a client parents
        // into controlCell(field) needs (its widgets' widths): it counts toward the cell's minimum width, so the
        // field's own parts and the client's never overprint one another or the Reset tail.
        void setControlExtra(InternedString field, float width);

        // Shows `value` (clamped like a field's own); fires nothing and clears neither the error nor mixed. A
        // control being edited is left alone: see isEditing. False for an unknown field.
        bool setValue(InternedString field, const UiFormValue& value);
        [[nodiscard]] UiFormValue value(InternedString field) const;
        // Whether the row's control has the focus or a press (a client refreshing from a state query skips it).
        [[nodiscard]] bool isEditing(InternedString field) const;

        // The values of a multi-edit differ: the row shows the badge; any edit of the row clears it.
        void setMixed(InternedString field, bool mixed);
        // The message under the row ("" clears) and the error look of its fields. A valid edit clears it.
        void setError(InternedString field, std::string_view message);
        void setReadOnly(InternedString field, bool readOnly);

        // As if the user entered `value` (a file picker's result, a control command): validates, shows it and
        // reports it through onEdit. False when it was refused (the error is shown) or the field is unknown.
        bool commit(InternedString field, const UiFormValue& value);

        void onEdit(std::function<void(const UiFormEdit&)> fn);
        // A Path row's Browse button opens whatever picks a file; the buttons are shown only while this is set.
        void onBrowse(std::function<void(InternedString field, std::string_view current)> fn);
        // A press on a Color row's swatch calls fn with the field and the swatch (the anchor of a colour picker); the
        // swatches take input only while this is set, and are then buttons named by their row's label.
        void onSwatch(std::function<void(InternedString field, const Widget& swatch)> fn);

        [[nodiscard]] UiFormRowState state(InternedString field) const;
        [[nodiscard]] std::vector<UiFormRowState> rows() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace spite
