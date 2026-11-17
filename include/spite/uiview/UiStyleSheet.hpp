#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/Expected.hpp"
#include "base/InternedString.hpp"
#include "base/Platform.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiTween.hpp"
#include "uiview/UiText.hpp"
#include "uiview/UiViewExport.hpp"

namespace spite
{
    // At most one state variant applies per element; uiInteractionState()
    // picks it by priority.
    enum class UiInteractionState : u8
    {
        Normal,
        Focused,
        Hover,
        Pressed,
        Disabled,
    };

    [[nodiscard]] constexpr UiInteractionState uiInteractionState(const bool interactable, const bool pressed,
                                                                  const bool hovered, const bool focused)
    {
        if (!interactable)
        {
            return UiInteractionState::Disabled;
        }
        if (pressed)
        {
            return UiInteractionState::Pressed;
        }
        if (hovered)
        {
            return UiInteractionState::Hover;
        }
        return focused ? UiInteractionState::Focused : UiInteractionState::Normal;
    }

    // The style fields a transition animates; the order is UiStyleAnimComponent's field order.
    enum class UiTransitionProperty : u8
    {
        BackgroundColor,
        TextColor,
        BorderColor,
        ShadowColor,
        BorderWidth,
        ShadowOffset,
        ShadowBlur,
        Opacity,
        OutlineColor,
        OutlineWidth,
        OutlineOffset,
        CornerRadius,
        ShadowSpread,
        Count,
    };

    inline constexpr std::array<std::string_view, static_cast<sizet>(UiTransitionProperty::Count)>
        UI_TRANSITION_PROPERTY_NAMES = {"backgroundColor", "textColor",    "borderColor",   "shadowColor",
                                        "borderWidth",     "shadowOffset", "shadowBlur",    "opacity",
                                        "outlineColor",    "outlineWidth", "outlineOffset", "cornerRadius",
                                        "shadowSpread"};

    // One entry of a style's shadow stack (UI_MAX_SHADOWS of them, each its own record); lengths are UI
    // pixels before layout scale. An outer shadow is the rect moved by `offset`, grown by `spread` on every side (each
    // non-zero corner radius grows with it) and blurred over `blur`, drawn under the element; an inset one is the
    // element's own shape minus that rect shrunk by `spread` and moved by `offset`, blurred, drawn over the fill. A
    // color with alpha 0 is no shadow.
    struct UiShadow
    {
        u32 color = 0; // rgba8, red in the low byte
        Vec2 offset{};
        float blur = 0.0f;
        float spread = 0.0f;
        bool inset = false;
        bool operator==(const UiShadow&) const = default;
    };

    // How one property moves when its resolved value changes: no duration and no delay snaps.
    struct UiTransitionSpec
    {
        float duration = 0.0f; // seconds
        float delay = 0.0f;
        UiEase ease = UiEase::OutCubic;
        bool operator==(const UiTransitionSpec&) const = default;
    };
    using UiTransitionSet = std::array<UiTransitionSpec, static_cast<sizet>(UiTransitionProperty::Count)>;

    // A fully resolved element style. Colors are rgba8 with red in the low
    // byte (R8G8B8A8 memory order); lengths are UI pixels before layout scale.
    struct UiStyle
    {
        u32 backgroundColor = 0; // fully transparent: no fill
        InternedString backgroundImage{}; // baked texture asset name or "atlas/sprite"; invalid = none
        u32 backgroundTint = 0xFFFFFFFFu; // multiplies the background image and image widgets
        // Per corner: top-left, top-right, bottom-right, bottom-left; a stylesheet's one number sets all
        // four. A packed (differing) set paints in whole pixels, 0..UI_CORNER_RADIUS_MAX.
        Vec4 cornerRadius{};
        InternedString font{}; // invalid = the view's default font
        // 0: the font as named; else the face the stylesheet's fontWeights declares for `font` nearest to this weight
        // (fontFaceFor), so a state switches an icon from Regular (400) to Fill (900) with no code.
        u16 fontWeight = 0;
        // A Phosphor icon name (UiIcons.hpp, validated at load; invalid = none): an element whose label sets no icon
        // of its own draws this icon in the UI_ICON_FONT family in place of its text.
        InternedString icon{};
        float textSize = 14.0f;
        u32 textColor = 0xFFFFFFFFu;
        UiTextAlign textAlign = UiTextAlign::Left; // each line within the rect width
        // Multi-line text of a label. The defaults are v1's single centered line. maxLines 0: no limit;
        // lineHeight multiplies the font's line height; a text input never wraps.
        UiTextWrap textWrap = UiTextWrap::None;
        UiTextOverflow textOverflow = UiTextOverflow::Clip;
        UiTextVAlign textVAlign = UiTextVAlign::Center;
        u32 textMaxLines = 0;
        float textLineHeight = 1.0f;
        // px added after each glyph's advance (negative tightens; a glyph's advance never goes below 0), before layout
        // scale. Measure, wrap, ellipsis, paint, caret and hit test lay out with it; resting style only, like the
        // lines.
        float textLetterSpacing = 0.0f;
        // The case the text lays out and paints in (UiTextTransform); resting style only. Rich-text labels keep their
        // case: their spans index the source text.
        UiTextTransform textTransform = UiTextTransform::None;
        u32 caretColor = 0xFFFFFFFFu; // text inputs being edited
        u32 selectionColor = 0x80CC6633u;
        u32 placeholderColor =
            0x99FFFFFFu; // a text input's placeholder (TextInput::setPlaceholder), while its value is empty
        // Box decorations. The border is inside the rect; the gradient runs from backgroundColor to
        // gradientColor along gradientAngle (degrees, CSS: 0 is to the top, 90 to the right); each shadow entry is the
        // rect moved by its offset, blurred, drawn under it (none when its alpha is 0), in index order; opacity scales
        // every color of the element's own primitives.
        float borderWidth = 0.0f;
        u32 borderColor = 0xFFFFFFFFu;
        std::optional<u32> gradientColor; // none: a flat fill
        float gradientAngle = 180.0f;
        std::array<UiShadow, UI_MAX_SHADOWS> shadows{};
        float opacity = 1.0f;
        // The outline is a ring around the border box, drawn outside it: its inner edge is `outlineOffset`
        // px from the rect (negative: inside), `outlineWidth` thick, following cornerRadius corner by corner. It takes
        // no layout space and is clipped by ancestors only. Nothing is drawn when its width is 0 or its alpha is 0.
        float outlineWidth = 0.0f;
        u32 outlineColor = 0xFFFFFFFFu;
        float outlineOffset = 0.0f;
        // 0: no transition; else one plus the index of the set in the sheet (UiStyleSheet::transitionSet), valid
        // until the sheet's next generation.
        u16 transition = 0;

        bool operator==(const UiStyle&) const = default;
    };

    [[nodiscard]] SPITE_UIVIEW_API std::string_view uiWidgetKindName(UiWidgetKind kind);
    // "normal", "focused", "hover", "pressed" or "disabled".
    [[nodiscard]] SPITE_UIVIEW_API std::optional<UiInteractionState> parseUiInteractionState(std::string_view name);

    // The resolution level a style field came from, most specific first.
    enum class UiStyleLevel : u8
    {
        Element,
        Class,
        Kind,
        Defaults,
        BuiltIn, // no layer set it: the UiStyle value
    };

    // The UiStyle fields, in resolution and report order.
    enum class UiStyleField : u8
    {
        BackgroundColor,
        BackgroundImage,
        CornerRadius,
        Font,
        TextSize,
        TextColor,
        TextAlign,
        CaretColor,
        SelectionColor,
        BorderWidth,
        BorderColor,
        GradientColor,
        GradientAngle,
        ShadowColor,
        ShadowOffset,
        ShadowBlur,
        Opacity,
        Transition,
        TextWrap,
        TextOverflow,
        TextVAlign,
        TextMaxLines,
        TextLineHeight,
        BackgroundTint,
        TextLetterSpacing,
        FontWeight,
        Icon,
        OutlineWidth,
        OutlineColor,
        OutlineOffset,
        ShadowSpread, // ShadowColor..ShadowSpread and ShadowInset are entry 0 of UiStyle::shadows
        ShadowInset,
        ShadowLayers, // entries 1..3 as one field: the topmost level that sets the array form wins as a whole
        TextTransform,
        PlaceholderColor,
        Count,
    };

    inline constexpr std::array<std::string_view, static_cast<sizet>(UiStyleField::Count)> UI_STYLE_FIELD_NAMES = {
        "backgroundColor",   "backgroundImage", "cornerRadius",   "font",          "textSize",        "textColor",
        "textAlign",         "caretColor",      "selectionColor", "borderWidth",   "borderColor",     "gradientColor",
        "gradientAngle",     "shadowColor",     "shadowOffset",   "shadowBlur",    "opacity",         "transition",
        "textWrap",          "textOverflow",    "textVAlign",     "textMaxLines",  "textLineHeight",  "backgroundTint",
        "textLetterSpacing", "fontWeight",      "icon",           "outlineWidth",  "outlineColor",    "outlineOffset",
        "shadowSpread",      "shadowInset",     "shadowLayers",   "textTransform", "placeholderColor"};

    // Where one UiStyle field resolved: the level, the layer (0 is the base sheet) and whether the level's state
    // variant (not its base block) set it. Indexed by UiStyleField.
    struct UiStyleOrigin
    {
        UiStyleLevel level = UiStyleLevel::BuiltIn;
        u8 layer = 0;
        bool stateVariant = false;
        InternedString token{}; // the "$name" variable the value was written as; invalid = a literal
    };
    using UiStyleOrigins = std::array<UiStyleOrigin, UI_STYLE_FIELD_NAMES.size()>;

    // Where a token a field was written as took its value (UiStyleSheet::tokenOrigin).
    struct UiStyleTokenOrigin
    {
        InternedString layer{}; // the layer whose var holds the value ("base" for the base sheet)
        std::vector<InternedString> chain; // the written name first, then each alias, ending at the var with the value
        bool overridden = false; // the var's value is a UiTokenOverride's, not its theme file's own
    };

    // One loaded layer as layers() reports it; order is its index (0 is the base sheet, named "base").
    struct UiStyleLayerInfo
    {
        InternedString name{};
        std::filesystem::path path; // empty: never loaded from a file
        std::vector<InternedString> classes; // sorted by name
        std::vector<std::string_view> kinds; // the kinds that set any field
        std::vector<InternedString> tokens; // the variables its "vars" block defines, sorted by name
        bool theme = false; // added by setTheme
        u32 elementCount = 0;
    };

    // The density of a theme: the name of a set of layout tokens ("space.*", "size.*") and the layer file
    // that overrides the base sheet's. The base sheet's own values are the DEFAULT_UI_DENSITY: no file, no layer.
    struct UiThemeDensity
    {
        InternedString name{};
        std::filesystem::path file;
    };
    inline constexpr std::string_view DEFAULT_UI_DENSITY = "comfortable";

    // A color a setting gives a token in place of the value the theme files define (rgba8, red in the low
    // byte). It replaces the var in each theme layer that defines it, so every rule and alias reading the token takes
    // it.
    struct UiTokenOverride
    {
        InternedString name{};
        u32 color = 0;
        bool operator==(const UiTokenOverride&) const = default;
    };

    // The view's style registry (Style system): the only owner
    // of visual parameters. Every field resolves through element state
    // variant, element, style class state variant, style class, kind state
    // variant, kind, "defaults", then the built-in UiStyle value; at each of
    // those levels a later layer wins over an earlier one (the base sheet is
    // the first layer). Unknown fields are ignored (inside "border", "outline", "gradient" and "shadow" an unknown
    // key fails the load); wrong value types, a negative border or outline width, a negative blur and an opacity
    // outside 0..1 fail the load. "shadow" is one object (entry 0 of the stack: each key resolves on its own, "color",
    // "offset", "blur", "spread", "inset") or an array of at most four such objects (UI_MAX_SHADOWS; a fifth fails the
    // load), which replaces the whole stack of the levels below, a missing key being its default.
    // A layer's "vars" object defines tokens (names A-Z a-z 0-9 _ . -); any field value written as the string
    // "$name" takes the token's value at load, so resolution never looks a token up. A token reads the
    // layer's own vars, then the active theme layers' (the topmost first), then the base sheet's; the base sheet
    // itself reads the themes first and its own vars last, so its vars are defaults a theme overrides. A var may
    // be another "$name" (an alias), followed by the same lookup; an unknown name or a cycle fails the load
    // (the message names the token path and the defined names). A layer that read a token through the themes or
    // the base sheet is re-parsed when one of those layers changes (load, setTheme, a watched reload), in the same
    // generation; the change is refused when it leaves a layer unresolved.
    // Any string of "$" plus at least one character is a token; there is no escape ("$$x" is an unknown token).
    // "transition" (a block field, resolved as a whole: the topmost level that sets it wins) maps property names
    // (UI_TRANSITION_PROPERTY_NAMES, or "all" for those not named) to {"duration": seconds, "delay": seconds,
    // "ease": a UiEase name}; an unknown property, key or ease name and a negative or non-finite time fail the load.
    // The set of the style a change moves to governs the change; an empty block ends the transitions of a lower level.
    // Resolved styles are cached per (id, class, kind, state) and dropped on a new generation; resolve and
    // explain are not thread-safe.
    class SPITE_UIVIEW_API UiStyleSheet
    {
    public:
        UiStyleSheet();
        ~UiStyleSheet();
        UiStyleSheet(const UiStyleSheet&) = delete;
        UiStyleSheet& operator=(const UiStyleSheet&) = delete;

        static constexpr u32 MAX_LAYERS = 8; // the base sheet included

        // Replaces the base sheet; a failed load keeps the previous content.
        Expected<void> load(const std::filesystem::path& path);
        // Clears the base sheet only; layers stay. False (nothing changed) when a theme layer reads a token of the
        // base sheet.
        bool unload();

        // Adds a named layer over every earlier one, or replaces the named
        // layer's content in place; a failed load keeps the previous content.
        Expected<void> loadLayer(InternedString name, const std::filesystem::path& path);
        // An in-memory layer for previews. It resolves through the same parser and stays outside file watching.
        Expected<void> loadLayerText(InternedString name, std::string_view text);
        // Returns false when no such layer is loaded, or when a layer reads a token of the one removed.
        bool unloadLayer(InternedString name);
        [[nodiscard]] bool hasLayer(InternedString name) const;

        // Makes the named theme (an ordered set of layer files, at most MAX_LAYERS minus the other layers) the
        // active one in a single generation: replaces the previous theme's layers; the new ones are appended over
        // the layers loaded so far. A failed load keeps the previous theme. Layers are named "theme.<name>", with
        // ".<n>" when the set has several files.
        // The name is [A-Za-z0-9_-]+ (anything else is refused: it names files and layers). A density with a file adds
        // its layer "density.<name>" over the theme's, so its tokens win; without one the theme reads the base sheet's.
        // The token overrides in force stay across a theme change.
        Expected<void> setTheme(InternedString name, std::span<const std::filesystem::path> paths,
                                const UiThemeDensity& density = {});
        // The same, with these token overrides (replacing the earlier ones) read into the theme layers; none clears
        // them.
        Expected<void> setTheme(InternedString name, std::span<const std::filesystem::path> paths,
                                const UiThemeDensity& density, std::span<const UiTokenOverride> overrides);
        [[nodiscard]] const std::vector<UiTokenOverride>& tokenOverrides() const { return m_overrides; }
        [[nodiscard]] InternedString theme() const { return m_theme; }
        // The density the last setTheme named; DEFAULT_UI_DENSITY before any.
        [[nodiscard]] InternedString density() const { return m_density; }

        // File watch over every layer loaded from a file. pollWatch (cheap, throttled; call once per frame)
        // reloads a changed layer in place, keeping the old content when the new file fails to load, and returns
        // true when anything reloaded.
        void watch(bool enabled);
        bool pollWatch();

        // `styleClass` may list up to three space-separated classes (a fourth is ignored), most specific first: a
        // composite widget's part takes "<family>.label dropdown.label", so a family rule overrides and the part's own
        // rule fills the rest. List order beats layer order (each name is read through every layer, top first, before
        // the next name), and a name's base field outranks a later name's hover or disabled variant of the same field:
        // a family that sets a field overrides the part's own state variants of it.
        [[nodiscard]] UiStyle resolve(InternedString id, UiWidgetKind kind, UiInteractionState state,
                                      InternedString styleClass = {}) const;
        // resolve plus where each field came from; the query path, never the paint path.
        UiStyle explain(InternedString id, UiWidgetKind kind, UiInteractionState state, InternedString styleClass,
                        UiStyleOrigins& origins) const;
        // Every layer in order, the base sheet first.
        [[nodiscard]] std::vector<UiStyleLayerInfo> layers() const;
        // The defining layer and alias chain of the token an explain() origin names; none for a literal.
        [[nodiscard]] std::optional<UiStyleTokenOrigin> tokenOrigin(const UiStyleOrigin& origin) const;
        // Every token the base sheet can read (the active themes' vars, the topmost first, then its own) whose value,
        // aliases followed, is a number: what the view publishes to the layout token table.
        [[nodiscard]] std::unordered_map<InternedString, float> numericTokens() const;
        // The same for the four-number arrays, packed as a style's color field is (the canvas color table's content);
        // a four-corner radius is one too, and harmless.
        [[nodiscard]] std::unordered_map<InternedString, u32> colorTokens() const;
        // The transition set a resolved style names (UiStyle::transition, not 0); valid until the next generation.
        [[nodiscard]] UiTransitionSet transitionSet(u16 transition) const;
        // Asset name for a UiImageComponent slot from the "images" map; the
        // topmost layer that maps the slot wins.
        [[nodiscard]] std::optional<InternedString> imageFor(u64 imageSlot) const;
        // The fonts tried in order for codepoints `font` lacks, from the "fontFallbacks" map ({"font": ["a", "b"]});
        // the topmost layer that maps the font wins. Valid until the next generation.
        [[nodiscard]] std::span<const InternedString> fontFallbacksFor(InternedString font) const;
        // The face rich text and a style's text.weight (UiStyle::fontWeight) take for `font` at `weight`
        // from the "fontWeights" map ({"font": {"700":
        // "font-bold"}}): the declared weight nearest to `weight` (the heavier on a tie); invalid when `font` declares
        // none. The topmost layer that maps the font wins. A face has its own fallback chain.
        [[nodiscard]] InternedString fontFaceFor(InternedString font, u16 weight) const;
        // Bumps on every successful load and on every unload, and when the text scale changes.
        [[nodiscard]] u64 generation() const { return m_generation; }
        // Multiplies every resolved textSize (the accessibility text scale, apart from the UI scale that scales
        // every length); a non-finite or non-positive value is refused, others clamp to [MIN_TEXT_SCALE,
        // MAX_TEXT_SCALE]. A change is a new generation: the views re-measure and repaint once. True when it moved.
        static constexpr float MIN_TEXT_SCALE = 0.5f;
        static constexpr float MAX_TEXT_SCALE = 4.0f;
        bool setTextScale(float scale);
        [[nodiscard]] float textScale() const { return m_textScale; }
        // Distinct keys resolved since the last generation (diagnostics).
        [[nodiscard]] sizet cachedStyleCount() const;

    private:
        struct Content;
        struct Layer;
        Expected<void> readLayer(Layer& layer, const std::filesystem::path& path,
                                 std::span<const UiTokenOverride> overrides = {});
        Expected<void> parseLayer(Layer& layer, std::string_view text, std::span<const UiTokenOverride> overrides);
        Expected<void> loadNamedLayer(InternedString name, const std::filesystem::path& path,
                                      std::optional<std::string_view> text);
        Expected<void> replaceLayers(std::vector<Layer*> order, std::span<Layer* const> fresh, bool tokensMayChange);
        template <bool Trace>
        UiStyle resolveImpl(InternedString id, UiWidgetKind kind, UiInteractionState state, InternedString styleClass,
                            UiStyleOrigins* origins) const;
        template <class T, class Pick>
        std::unordered_map<InternedString, T> tokensWhere(const Pick& pick) const;
        void bump();
        void armWatch();
        std::vector<Layer> m_layers; // [0] is the base sheet, always present
        u64 m_generation = 0;
        float m_textScale = 1.0f;
        InternedString m_theme{};
        InternedString m_density{DEFAULT_UI_DENSITY};
        std::vector<UiTokenOverride> m_overrides;
        struct Cache;
        struct Watcher;
        std::unique_ptr<Cache> m_cache;
        std::unique_ptr<Watcher> m_watcher;
    };
} // namespace spite
