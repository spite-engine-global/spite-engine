#pragma once

#include <functional>
#include <memory>
#include <string_view>

#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "ui/UiExport.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class UiScope;

    // Sizes in unscaled UI pixels; the rows take their height and the gaps the layout tokens (size.control,
    // space.xs, space.md), so the density applies.
    struct UiColorPickerDesc
    {
        Vec2 areaSize{196.0f, 96.0f}; // the saturation and value area; the bars are as wide
        float barHeight = 14.0f; // the hue and opacity bars
        bool alpha = true; // the opacity bar and field (a Vec4 colour); false for an rgb colour
        i32 zOffset = 500; // the dialog's root zOffset (a Modal's)
    };

    // What an edit reports: the colour now (rgb and alpha in [0, 1] as stored; alpha is 1 while the picker has none)
    // and the gesture it belongs to: one press and drag on the area or a bar is one gesture (the same non-zero number
    // for every step), anything else (a field, the hex text) is its own edit with gesture 0, so a client that folds a
    // drag into one undo step merges on the number.
    struct UiColorEdit
    {
        Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
        u32 gesture = 0;
    };

    // A colour picker: a dialog over the surface with the saturation and value area, a hue bar, an
    // opacity bar when the colour has alpha, the hex text (#RRGGBB or #RRGGBBAA) and the H S V A fields. It is a
    // presenter over scope-owned widgets (a Modal and its parts "<id>.area", ".hue", ".opacity", ".hex", ".h", ".s",
    // ".v", ".a", ".preview"): the area and bars take a press and a drag, the fields and the hex text take typing, so
    // everything is reachable by keys and by `uiActivate setValue` too. The colour is the picker's own until
    // setColor; a client shows what it stores, the picker shows what the user last made: hue and saturation survive a
    // grey. The numbers are not converted (no transfer function): the hex text is the stored value times 255.
    // Idle: no per-frame work at all; a closed picker is a hidden surface.
    // Create it after the scope and destroy it before; the destructor destroys its widgets (the client's callbacks
    // captured by them go with it).
    class SPITE_UI_API ColorPicker
    {
    public:
        ColorPicker(UiScope& scope, InternedString id, const UiColorPickerDesc& desc = {});
        ~ColorPicker();
        ColorPicker(const ColorPicker&) = delete;
        ColorPicker& operator=(const ColorPicker&) = delete;

        // Shows `color` (a component that is not a finite number is 0, the others clamp to [0, 1]); fires nothing.
        // While a pointer gesture is under way the picker keeps its own colour.
        void setColor(const Vec4& color);
        [[nodiscard]] Vec4 color() const;
        // Whether the colour has an alpha (the opacity bar and field); a Vec3 colour has none.
        void setAlpha(bool alpha);
        [[nodiscard]] bool hasAlpha() const;
        // Shown, not editable: the area, the bars and the fields take no input.
        void setReadOnly(bool readOnly);
        [[nodiscard]] bool isReadOnly() const;

        // Opens the dialog beside `anchor` (the swatch that asked for it), inside the anchor's surface; on the side
        // with more room, moved to stay inside. Escape or a press outside closes it and gives the focus back.
        void open(const Widget& anchor);
        void close();
        [[nodiscard]] bool isOpen() const;
        // The dialog's widgets by suffix ("dialog", "area", "hue", "opacity", "hex", "h", "s", "v", "a", "preview");
        // invalid for an unknown one.
        [[nodiscard]] Widget part(std::string_view suffix) const;

        // Each change the user makes (pointer, field, hex text); not setColor's.
        void onChange(std::function<void(const UiColorEdit&)> fn);
        // After the dialog closed.
        void onClose(std::function<void()> fn);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace spite
