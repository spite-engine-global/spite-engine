#include "ui/UiColorPicker.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "ecs/World.hpp"
#include "ui/UiAccessibility.hpp"
#include "ui/UiEvents.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiLayoutTokens.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        // the messages the picker shows (assets/loc/en/engine.json)
        namespace key
        {
            constexpr const char* DIALOG = "engine.ui.color.dialog";
            constexpr const char* AREA = "engine.ui.color.area";
            constexpr const char* HEX = "engine.ui.color.hex";
            constexpr const char* PREVIEW = "engine.ui.color.preview";
            constexpr const char* HUE_BAR = "engine.ui.color.hueBar";
            constexpr const char* OPACITY_BAR = "engine.ui.color.opacityBar";
            constexpr std::array<const char*, 4> NAMES{"engine.ui.color.hue", "engine.ui.color.saturation",
                                                       "engine.ui.color.value", "engine.ui.color.opacity"};
            constexpr std::array<const char*, 4> TAGS{"engine.ui.color.tagH", "engine.ui.color.tagS",
                                                      "engine.ui.color.tagV", "engine.ui.color.tagA"};
        } // namespace key

        constexpr float MARKER_SIZE = 10.0f; // the ring on the area
        constexpr float BAR_MARKER_WIDTH = 4.0f;
        constexpr float FIELD_MIN = 24.0f;
        constexpr float HUE_MAX = 360.0f;
        constexpr float PREVIEW_MIN = 22.0f;
        constexpr float ROW_MIN = 22.0f; // until the size.control token (the density) gives the rows their height
        constexpr std::array<const char*, 6> HUE_SEGMENTS{"colorPicker.hue.0", "colorPicker.hue.1",
                                                          "colorPicker.hue.2", "colorPicker.hue.3",
                                                          "colorPicker.hue.4", "colorPicker.hue.5"};

        // A finite number clamped to [0, 1]; anything else is 0.
        float unit(const float x) { return std::isfinite(x) ? std::clamp(x, 0.0f, 1.0f) : 0.0f; }

        Vec3 hsvToRgb(const float hue, const float saturation, const float value)
        {
            const float k = hue / 60.0f;
            const auto channel = [&](const float n)
            {
                const float at = std::fmod(n + k, 6.0f);
                return value - value * saturation * std::clamp(std::min(at, 4.0f - at), 0.0f, 1.0f);
            };
            return Vec3{channel(5.0f), channel(3.0f), channel(1.0f)};
        }

        // The hue and saturation of a grey, or of black, are not in the colour: they keep what they were.
        void rgbToHsv(const Vec3& rgb, float& hue, float& saturation, float& value)
        {
            const float high = std::max({rgb.x, rgb.y, rgb.z});
            const float low = std::min({rgb.x, rgb.y, rgb.z});
            const float chroma = high - low;
            value = high;
            if (high > 0.0f)
            {
                saturation = chroma / high;
            }
            if (chroma > 1.0e-6f)
            {
                float degrees = high == rgb.x ? (rgb.y - rgb.z) / chroma
                    : high == rgb.y           ? 2.0f + (rgb.z - rgb.x) / chroma
                                              : 4.0f + (rgb.x - rgb.y) / chroma;
                degrees *= 60.0f;
                hue = degrees < 0.0f ? degrees + HUE_MAX : degrees;
            }
        }

        u32 byteOf(const float x) { return static_cast<u32>(std::lround(unit(x) * 255.0f)); }

        std::optional<u32> hexDigit(const char c)
        {
            if (c >= '0' && c <= '9')
            {
                return static_cast<u32>(c - '0');
            }
            if (c >= 'a' && c <= 'f')
            {
                return static_cast<u32>(c - 'a' + 10);
            }
            if (c >= 'A' && c <= 'F')
            {
                return static_cast<u32>(c - 'A' + 10);
            }
            return std::nullopt;
        }

        std::string_view trimmed(std::string_view text)
        {
            const auto space = [](const char c) { return c == ' ' || c == '\t'; };
            while (!text.empty() && space(text.front()))
            {
                text.remove_prefix(1);
            }
            while (!text.empty() && space(text.back()))
            {
                text.remove_suffix(1);
            }
            return text;
        }

        // Text that can still become a colour while it is typed: # and up to eight hex digits.
        bool hexPrefix(std::string_view text)
        {
            text = trimmed(text);
            if (text.starts_with('#'))
            {
                text.remove_prefix(1);
            }
            return text.size() <= 8 && std::ranges::all_of(text, [](const char c) { return hexDigit(c).has_value(); });
        }

        // #RGB, #RGBA, #RRGGBB or #RRGGBBAA (the # optional) as channels in [0, 1]; the alpha of a form without one is
        // `alpha`. A form with an alpha is refused while the colour has none.
        std::optional<Vec4> parseHex(std::string_view text, const bool hasAlpha, const float alpha)
        {
            if (text.starts_with('#'))
            {
                text.remove_prefix(1);
            }
            const bool shortForm = text.size() == 3 || text.size() == 4;
            if (!shortForm && text.size() != 6 && text.size() != 8)
            {
                return std::nullopt;
            }
            std::array<float, 4> channels{0.0f, 0.0f, 0.0f, alpha};
            const u32 count = static_cast<u32>(text.size()) / (shortForm ? 1u : 2u);
            if (count == 4 && !hasAlpha)
            {
                return std::nullopt;
            }
            for (u32 i = 0; i < count; ++i)
            {
                const std::optional<u32> high = hexDigit(text[shortForm ? i : 2 * i]);
                const std::optional<u32> low = hexDigit(text[shortForm ? i : 2 * i + 1]);
                if (!high || !low)
                {
                    return std::nullopt;
                }
                channels[i] = static_cast<float>(*high * 16 + *low) / 255.0f;
            }
            return Vec4{channels[0], channels[1], channels[2], channels[3]};
        }

        UiRectDesc placed(const Vec2 position, const Vec2 size)
        {
            UiRectDesc desc;
            desc.pivot = Vec2{0.0f, 0.0f};
            desc.offset = position;
            desc.size = size;
            return desc;
        }

        UiRectDesc marker(const Vec2 size)
        {
            UiRectDesc desc;
            desc.size = size;
            return desc;
        }

        UiLayoutItemComponent fixedItem()
        {
            UiLayoutItemComponent item;
            item.shrink = 0.0f;
            item.alignSelf = UiLayoutAlign::Start;
            return item;
        }

        UiLayoutItemComponent flexItem(const float minWidth)
        {
            UiLayoutItemComponent item;
            item.grow = 1.0f;
            item.shrink = 1.0f;
            item.basis = 0.0f;
            item.minSize = Vec2{minWidth, 0.0f};
            return item;
        }

        UiRectDesc rowDesc(const float width)
        {
            UiRectDesc desc;
            desc.pivot = Vec2{0.0f, 0.0f};
            desc.size = Vec2{width, 0.0f};
            UiLayoutComponent layout;
            layout.direction = UiLayoutDirection::Row;
            layout.align = UiLayoutAlign::Stretch;
            desc.layout = layout;
            UiContentSizeComponent fit;
            fit.fitWidth = false;
            desc.contentSize = fit;
            desc.layoutItem = fixedItem();
            desc.layoutItem->minSize = Vec2{0.0f, ROW_MIN};
            return desc;
        }

        // The callback is copied first: it may destroy the picker.
        template <typename Fn, typename... Args>
        void fire(const Fn& callback, Args&&... args)
        {
            if (callback)
            {
                const Fn local = callback;
                local(std::forward<Args>(args)...);
            }
        }
    } // namespace

    struct ColorPicker::Impl
    {
        UiScope* scope = nullptr;
        InternedString id;
        UiColorPickerDesc desc;
        Modal modal;
        Panel area;
        Panel hueBase; // the area's pure hue, tinted
        Panel areaMarker;
        Panel hueBar;
        Panel hueMarker;
        Panel opacityBar;
        Panel opacityRamp; // transparent to opaque, tinted by the colour
        Panel opacityMarker;
        Panel preview;
        TextInput hex;
        std::array<Label, 4> tags;
        std::array<NumberField, 4> fields;

        Vec3 rgb{1.0f, 1.0f,
                 1.0f}; // the colour as set or last made: exact, where hue/saturation/value are the view of it
        float hue = 0.0f; // [0, 360]
        float saturation = 0.0f;
        float value = 1.0f;
        float alpha = 1.0f;
        bool hasAlpha = true;
        bool readOnly = false;
        u32 gestures = 0;
        u32 gesture = 0; // the pointer gesture under way, 0 for none
        std::array<std::string, 5> shownText; // what the picker last put in the four fields and the hex text
        std::function<void(const UiColorEdit&)> onChange;
        std::function<void()> onClose;

        [[nodiscard]] InternedString part(const std::string_view suffix) const
        {
            return InternedString(std::string(id.toView()) + "." + std::string(suffix));
        }

        [[nodiscard]] Vec4 color() const { return Vec4{rgb.x, rgb.y, rgb.z, hasAlpha ? alpha : 1.0f}; }

        // A field being typed in or dragged keeps its own text; one that has the focus but still holds what the picker
        // last put there takes the new value (an outside set while the dialog is open).
        [[nodiscard]] bool takesValue(const TextInput& widget, const std::string& shown) const
        {
            const UiRouter& router = scope->router();
            if (router.pressed() == widget.entity())
            {
                return false;
            }
            return router.focused() != widget.entity() || shown == widget.value();
        }

        // -- build ------------------------------------------------------------------------------------------------

        Panel layer(const std::string_view suffix, const Widget& parent, const Vec2 position, const Vec2 size,
                    const char* styleClass, const bool tinted = false)
        {
            Panel out = scope->createPanel(part(suffix), placed(position, size));
            out.setParent(parent);
            out.setStyleClass(InternedString(styleClass));
            if (tinted)
            {
                scope->world().addComponent<UiVisualComponent>(out.entity(), UiVisualComponent{});
            }
            return out;
        }

        Panel markerOn(const std::string_view suffix, const Widget& parent, const Vec2 size, const char* styleClass)
        {
            Panel out = scope->createPanel(part(suffix), marker(size));
            out.setParent(parent);
            out.setStyleClass(InternedString(styleClass));
            return out;
        }

        void tokens(Widget widget, const bool padded = false)
        {
            widget.bindLayoutToken(UiLayoutField::Gap, InternedString("space.xs"));
            if (padded)
            {
                for (const UiLayoutField side : {UiLayoutField::PaddingLeft, UiLayoutField::PaddingTop,
                                                 UiLayoutField::PaddingRight, UiLayoutField::PaddingBottom})
                {
                    widget.bindLayoutToken(side, InternedString("space.md"));
                }
            }
        }

        Panel row(const std::string_view suffix, const Widget& parent)
        {
            Panel out = scope->createPanel(part(suffix), rowDesc(desc.areaSize.x));
            out.setParent(parent);
            tokens(out);
            return out;
        }

        // Presses and drags on `targets` set the value from the pointer; one press to its release is one gesture.
        void bindPointer(const std::vector<Entity>& targets, std::function<void(Vec2)> setFrom)
        {
            for (const Entity target : targets)
            {
                scope->setReaction(target, UiEventKind::PointerDown,
                                   [this, setFrom](const void* payload)
                                   {
                                       const auto* event = static_cast<const UiPointerDownEvent*>(payload);
                                       if (event->button == 0 && !readOnly)
                                       {
                                           gesture = ++gestures;
                                           setFrom(event->position);
                                       }
                                   });
                scope->setReaction(target, UiEventKind::DragStart,
                                   [this, setFrom](const void* payload)
                                   {
                                       if (gesture)
                                       {
                                           setFrom(static_cast<const UiDragStartEvent*>(payload)->position);
                                       }
                                   });
                scope->setReaction(target, UiEventKind::DragMove,
                                   [this, setFrom](const void* payload)
                                   {
                                       if (gesture)
                                       {
                                           setFrom(static_cast<const UiDragMoveEvent*>(payload)->position);
                                       }
                                   });
                scope->setReaction(target, UiEventKind::DragEnd, [this](const void*) { gesture = 0; });
                scope->setReaction(target, UiEventKind::PointerUp,
                                   [this](const void* payload)
                                   {
                                       if (static_cast<const UiPointerUpEvent*>(payload)->button == 0)
                                       {
                                           gesture = 0;
                                       }
                                   });
            }
        }

        [[nodiscard]] float fractionAlong(const Widget& surface, const Vec2 pointer, const bool horizontal) const
        {
            const UiRect rect = scope->hierarchy().absoluteRectOf(surface.entity());
            const float extent = horizontal ? rect.size.x : rect.size.y;
            if (!(extent > 0.0f))
            {
                return 0.0f;
            }
            return unit((horizontal ? pointer.x - rect.position.x : pointer.y - rect.position.y) / extent);
        }

        void build(const UiColorPickerDesc& requested)
        {
            desc = requested;
            hasAlpha = desc.alpha;
            UiModalDesc modalDesc;
            modalDesc.closeOnBackdrop = true;
            modalDesc.zOffset = desc.zOffset;
            modal = scope->createModal(id, modalDesc);
            modal.setStyleClass(InternedString("colorPicker.backdrop"));
            modal.dialog().setAccessibleNameKey(InternedString(key::DIALOG));
            const Panel content = modal.content();
            tokens(content, true);

            const Vec2 areaSize = desc.areaSize;
            UiRectDesc areaDesc = placed({}, areaSize);
            areaDesc.layoutItem = fixedItem();
            area = scope->createPanel(part("area"), areaDesc);
            area.setParent(content);
            area.setStyleClass(InternedString("colorPicker.area"));
            area.setAccessibleRole(UiRole::Image);
            area.setAccessibleNameKey(InternedString(key::AREA));
            hueBase = layer("area.hue", area, {}, areaSize, "colorPicker.base", true);
            const Panel white = layer("area.white", area, {}, areaSize, "colorPicker.white");
            const Panel black = layer("area.black", area, {}, areaSize, "colorPicker.black");
            areaMarker = markerOn("area.marker", area, Vec2{MARKER_SIZE, MARKER_SIZE}, "colorPicker.marker");
            areaMarker.setInteractable(false);
            bindPointer({area.entity(), hueBase.entity(), white.entity(), black.entity(), areaMarker.entity()},
                        [this](const Vec2 pointer)
                        {
                            setSaturationValue(fractionAlong(area, pointer, true),
                                               1.0f - fractionAlong(area, pointer, false));
                        });

            const Vec2 barSize{areaSize.x, desc.barHeight};
            UiRectDesc barDesc = placed({}, barSize);
            barDesc.layoutItem = fixedItem();
            hueBar = scope->createPanel(part("hue"), barDesc);
            hueBar.setParent(content);
            hueBar.setStyleClass(InternedString("colorPicker.bar"));
            hueBar.setAccessibleRole(UiRole::Image);
            hueBar.setAccessibleNameKey(InternedString(key::HUE_BAR));
            std::vector<Entity> hueParts{hueBar.entity()};
            const float segment = barSize.x / static_cast<float>(HUE_SEGMENTS.size());
            for (u32 i = 0; i < HUE_SEGMENTS.size(); ++i)
            {
                hueParts.push_back(layer(std::format("hue.{}", i), hueBar, Vec2{segment * static_cast<float>(i), 0.0f},
                                         Vec2{segment, barSize.y}, HUE_SEGMENTS[i])
                                       .entity());
            }
            hueMarker = markerOn("hue.marker", hueBar, Vec2{BAR_MARKER_WIDTH, barSize.y + 2.0f}, "colorPicker.knob");
            hueMarker.setInteractable(false);
            hueParts.push_back(hueMarker.entity());
            bindPointer(hueParts,
                        [this](const Vec2 pointer) { setHue(fractionAlong(hueBar, pointer, true) * HUE_MAX); });

            opacityBar = scope->createPanel(part("opacity"), barDesc);
            opacityBar.setParent(content);
            opacityBar.setStyleClass(InternedString("colorPicker.bar"));
            opacityBar.setAccessibleRole(UiRole::Image);
            opacityBar.setAccessibleNameKey(InternedString(key::OPACITY_BAR));
            opacityRamp = layer("opacity.ramp", opacityBar, {}, barSize, "colorPicker.ramp", true);
            opacityMarker =
                markerOn("opacity.marker", opacityBar, Vec2{BAR_MARKER_WIDTH, barSize.y + 2.0f}, "colorPicker.knob");
            opacityMarker.setInteractable(false);
            bindPointer({opacityBar.entity(), opacityRamp.entity(), opacityMarker.entity()},
                        [this](const Vec2 pointer) { setAlphaTo(fractionAlong(opacityBar, pointer, true)); });

            const Panel hexRow = row("hexRow", content);
            UiRectDesc previewDesc;
            previewDesc.layoutItem = fixedItem();
            previewDesc.layoutItem->alignSelf = UiLayoutAlign::Stretch;
            previewDesc.size = Vec2{PREVIEW_MIN, 0.0f};
            preview = scope->createPanel(part("preview"), previewDesc);
            preview.setParent(hexRow);
            preview.setStyleClass(InternedString("form.swatch"));
            preview.setInteractable(false);
            preview.setAccessibleRole(UiRole::Image);
            preview.setAccessibleNameKey(InternedString(key::PREVIEW));
            scope->world().addComponent<UiVisualComponent>(preview.entity(), UiVisualComponent{});
            UiRectDesc hexDesc;
            hexDesc.layoutItem = flexItem(FIELD_MIN);
            hex = scope->createTextInput(part("hex"), hexDesc);
            hex.setParent(hexRow);
            hex.setAccessibleNameKey(InternedString(key::HEX));
            hex.onChange([this](const std::string_view text) { typedHex(text); });
            hex.onCommit([this](const std::string_view text) { committedHex(text); });
            hex.onSubmit([this](std::string_view) { scope->router().clearFocus(); });
            hex.onCancel([this] { modal.close(); });

            // hue and saturation, then value and opacity: two to a row, so the fields keep room at large text
            const std::array<Panel, 2> valueRows{row("valueRow.0", content), row("valueRow.1", content)};
            static constexpr std::array<const char*, 4> LOWER{"h", "s", "v", "a"};
            for (u32 i = 0; i < 4; ++i)
            {
                const Panel& valueRow = valueRows[i / 2];
                UiRectDesc tagDesc;
                tagDesc.contentSize = UiContentSizeComponent{};
                tagDesc.layoutItem = fixedItem();
                tagDesc.layoutItem->alignSelf = UiLayoutAlign::Center;
                tags[i] = scope->createLabel(part(std::format("tag.{}", LOWER[i])), "", tagDesc);
                tags[i].setParent(valueRow);
                tags[i].setLocalizedText(InternedString(key::TAGS[i]));
                tags[i].setStyleClass(InternedString("form.axis"));
                tags[i].setAccessibleRole(UiRole::Presentation);
                UiNumberDesc numberDesc;
                numberDesc.rect.layoutItem = flexItem(FIELD_MIN);
                numberDesc.min = 0.0;
                numberDesc.max = i == 0 ? static_cast<double>(HUE_MAX) : 100.0;
                numberDesc.step = i == 0 ? 1.0 : 0.5;
                numberDesc.decimals = 0;
                fields[i] = scope->createNumberField(part(LOWER[i]), numberDesc);
                fields[i].setParent(valueRow);
                fields[i].setAccessibleNameKey(InternedString(key::NAMES[i]));
                fields[i].onNumber([this, i](const double number) { typedNumber(i, number); });
                fields[i].onCancel([this] { modal.close(); });
            }
            for (Panel line : {hexRow, valueRows[0], valueRows[1]})
            {
                line.bindLayoutToken(UiLayoutField::MinHeight, InternedString("size.control"));
            }
            tags[3].setVisible(hasAlpha);
            fields[3].setVisible(hasAlpha);
            opacityBar.setVisible(hasAlpha);
            show();
            modal.onClose(
                [this]
                {
                    gesture = 0;
                    fire(onClose);
                });
        }

        // -- editing ----------------------------------------------------------------------------------------------

        void changed(const u32 byGesture)
        {
            show();
            fire(onChange, UiColorEdit{color(), byGesture});
        }

        void setSaturationValue(const float s, const float v)
        {
            if (s != saturation || v != value)
            {
                saturation = s;
                value = v;
                rgb = hsvToRgb(hue, saturation, value);
                changed(gesture);
            }
        }

        void setHue(const float degrees)
        {
            if (degrees != hue)
            {
                hue = degrees;
                rgb = hsvToRgb(hue, saturation, value);
                changed(gesture);
            }
        }

        void setAlphaTo(const float next)
        {
            if (hasAlpha && next != alpha)
            {
                alpha = next;
                changed(gesture);
            }
        }

        void typedNumber(const u32 index, const double number)
        {
            if (readOnly || !std::isfinite(number))
            {
                return;
            }
            const float x = static_cast<float>(number);
            switch (index)
            {
            case 0:
                hue = std::clamp(x, 0.0f, HUE_MAX);
                break;
            case 1:
                saturation = unit(x / 100.0f);
                break;
            case 2:
                value = unit(x / 100.0f);
                break;
            default:
                alpha = unit(x / 100.0f);
                break;
            }
            if (index < 3)
            {
                rgb = hsvToRgb(hue, saturation, value);
            }
            changed(0);
        }

        void typedHex(const std::string_view text)
        {
            hex.setStyleClass(hexPrefix(text) ? InternedString{} : InternedString("form.field.error"));
        }

        void committedHex(const std::string_view text)
        {
            const std::optional<Vec4> parsed = readOnly ? std::nullopt : parseHex(trimmed(text), hasAlpha, alpha);
            hex.setStyleClass(InternedString{});
            if (parsed)
            {
                take(*parsed);
            }
            hex.setText(hexText());
            if (parsed)
            {
                changed(0); // last: the client's callback may destroy the picker
            }
        }

        // The colour as the picker's own hue, saturation, value and alpha
        void take(const Vec4& color)
        {
            rgb = Vec3{unit(color.x), unit(color.y), unit(color.z)};
            rgbToHsv(rgb, hue, saturation, value);
            alpha = unit(color.w);
        }

        // -- showing ----------------------------------------------------------------------------------------------

        [[nodiscard]] std::string hexText() const
        {
            const Vec4 c = color();
            char buffer[16];
            if (hasAlpha)
            {
                std::snprintf(buffer, sizeof buffer, "#%02X%02X%02X%02X", byteOf(c.x), byteOf(c.y), byteOf(c.z),
                              byteOf(c.w));
            }
            else
            {
                std::snprintf(buffer, sizeof buffer, "#%02X%02X%02X", byteOf(c.x), byteOf(c.y), byteOf(c.z));
            }
            return buffer;
        }

        void tint(const Panel& panel, const Vec4& color)
        {
            auto& visual = scope->world().getComponent<UiVisualComponent>(panel.entity());
            if (visual.tint != color)
            {
                visual.tint = color;
                scope->hierarchy().markVisualDirty(panel.entity());
            }
        }

        void anchorAt(Panel& marker, const Vec2 anchor)
        {
            if (scope->world().getConstComponent<UiRectComponent>(marker.entity()).anchor != anchor)
            {
                marker.setAnchor(anchor);
            }
        }

        void show()
        {
            const Vec4 c = color();
            const Vec3 pure = hsvToRgb(hue, 1.0f, 1.0f);
            tint(hueBase, Vec4{pure.x, pure.y, pure.z, 1.0f});
            tint(opacityRamp, Vec4{c.x, c.y, c.z, 1.0f});
            tint(preview, c);
            anchorAt(areaMarker, Vec2{saturation, 1.0f - value});
            anchorAt(hueMarker, Vec2{hue / HUE_MAX, 0.5f});
            anchorAt(opacityMarker, Vec2{alpha, 0.5f});
            const std::array<double, 4> numbers{
                std::round(static_cast<double>(hue)), std::round(static_cast<double>(saturation) * 100.0),
                std::round(static_cast<double>(value) * 100.0), std::round(static_cast<double>(alpha) * 100.0)};
            for (u32 i = 0; i < 4; ++i)
            {
                if (takesValue(fields[i], shownText[i]))
                {
                    if (fields[i].number() != numbers[i])
                    {
                        fields[i].setNumber(numbers[i]);
                    }
                    shownText[i] = std::string(fields[i].value());
                }
            }
            if (takesValue(hex, shownText[4]))
            {
                const std::string text = hexText();
                if (hex.value() != text)
                {
                    hex.setText(text);
                }
                shownText[4] = text;
            }
        }

        void setReadOnlyState(const bool on)
        {
            readOnly = on;
            for (Widget control :
                 {static_cast<Widget>(hex), static_cast<Widget>(fields[0]), static_cast<Widget>(fields[1]),
                  static_cast<Widget>(fields[2]), static_cast<Widget>(fields[3])})
            {
                control.setInteractable(!on);
            }
        }

        // -- placing ----------------------------------------------------------------------------------------------

        void place(const Widget& anchorWidget)
        {
            const UiHierarchyManager& hierarchy = scope->hierarchy();
            Entity surface = anchorWidget.entity();
            for (Entity parent = hierarchy.parentOf(surface); parent != Entity::undefined();
                 parent = hierarchy.parentOf(surface))
            {
                surface = parent;
            }
            const UiRect bounds = hierarchy.absoluteRectOf(surface);
            const UiRect anchor = hierarchy.absoluteRectOf(anchorWidget.entity());
            const Panel dialog = modal.dialog();
            const Vec2 size = hierarchy.absoluteRectOf(dialog.entity()).size;
            const bool right = anchor.position.x + anchor.size.x * 0.5f > bounds.position.x + bounds.size.x * 0.5f;
            const bool lower = anchor.position.y + anchor.size.y * 0.5f > bounds.position.y + bounds.size.y * 0.5f;
            const Vec2 at{right ? anchor.position.x - size.x : anchor.position.x + anchor.size.x,
                          lower ? anchor.position.y + anchor.size.y - size.y : anchor.position.y};
            // pivot = anchor = the share of the free room the dialog's corner sits at: inside the surface for any size,
            // so a resized window or a larger text scale cannot push it out
            const Vec2 room{bounds.size.x - size.x, bounds.size.y - size.y};
            if (room.x > 0.0f && room.y > 0.0f)
            {
                const Vec2 share{std::clamp((at.x - bounds.position.x) / room.x, 0.0f, 1.0f),
                                 std::clamp((at.y - bounds.position.y) / room.y, 0.0f, 1.0f)};
                Widget(dialog).setPivot(share);
                Widget(dialog).setAnchor(share);
            }
        }
    };

    ColorPicker::ColorPicker(UiScope& scope, const InternedString id, const UiColorPickerDesc& desc) :
        m_impl(std::make_unique<Impl>())
    {
        m_impl->scope = &scope;
        m_impl->id = id;
        m_impl->build(desc);
    }

    ColorPicker::~ColorPicker()
    {
        // every callback of the picker's widgets captures it: destroy them (UiScope::destroy purges the reactions at
        // once); the modal is a root of its own
        Impl& impl = *m_impl;
        if (!impl.modal.isValid())
        {
            return;
        }
        std::vector<Entity> parts{impl.modal.entity()};
        for (sizet at = 0; at < parts.size(); ++at)
        {
            for (const Entity child : impl.scope->hierarchy().childrenOf(parts[at]))
            {
                parts.push_back(child);
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

    void ColorPicker::setColor(const Vec4& color)
    {
        Impl& impl = *m_impl;
        if (impl.gesture)
        {
            return;
        }
        const Vec4 now{impl.rgb.x, impl.rgb.y, impl.rgb.z, impl.alpha};
        const Vec4 next{unit(color.x), unit(color.y), unit(color.z), unit(color.w)};
        if (next != now)
        {
            impl.take(next);
        }
        impl.show();
    }

    Vec4 ColorPicker::color() const { return m_impl->color(); }

    void ColorPicker::setAlpha(const bool alpha)
    {
        Impl& impl = *m_impl;
        if (alpha == impl.hasAlpha)
        {
            return;
        }
        impl.hasAlpha = alpha;
        impl.opacityBar.setVisible(alpha);
        impl.tags[3].setVisible(alpha);
        impl.fields[3].setVisible(alpha);
        impl.show();
    }

    bool ColorPicker::hasAlpha() const { return m_impl->hasAlpha; }

    void ColorPicker::setReadOnly(const bool readOnly) { m_impl->setReadOnlyState(readOnly); }

    bool ColorPicker::isReadOnly() const { return m_impl->readOnly; }

    void ColorPicker::open(const Widget& anchor)
    {
        Impl& impl = *m_impl;
        if (impl.modal.isOpen())
        {
            return;
        }
        impl.modal.open(anchor);
        impl.place(anchor);
    }

    void ColorPicker::close() { m_impl->modal.close(); }

    bool ColorPicker::isOpen() const { return m_impl->modal.isOpen(); }

    Widget ColorPicker::part(const std::string_view suffix) const
    {
        const Impl& impl = *m_impl;
        const UiHierarchyManager& hierarchy = impl.scope->hierarchy();
        const InternedString wanted = impl.part(suffix);
        std::vector<Entity> parts{impl.modal.entity()};
        for (sizet at = 0; at < parts.size(); ++at)
        {
            if (impl.scope->world().getConstComponent<UiIdentityComponent>(parts[at]).id == wanted)
            {
                return Widget(impl.scope, parts[at], impl.scope->generation());
            }
            for (const Entity child : hierarchy.childrenOf(parts[at]))
            {
                parts.push_back(child);
            }
        }
        return {};
    }

    void ColorPicker::onChange(std::function<void(const UiColorEdit&)> fn) { m_impl->onChange = std::move(fn); }

    void ColorPicker::onClose(std::function<void()> fn) { m_impl->onClose = std::move(fn); }
} // namespace spite
