#pragma once

#include "uiruntime/UiRuntimeExport.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/MathTypes.hpp"
#include "base/Platform.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include "ui/UiCanvas.hpp"
#include "ui/UiLayout.hpp"
#include "uiview/UiText.hpp"
#include "uiview/UiTextureId.hpp"

namespace spite
{
    enum class UiPaintKind : u8
    {
        Fill,
        Image,
        Text,
        Shadow, // a blurred rounded rect under its element: rect is the shadow's own (offset applied), blur its spread
        // One row for a canvas element's whole primitive list: rect is the element's, clip the
        // canvas's own clip, color the modulation its primitives multiply by (white: none), gradientColor the text
        // color of the element's style; `primitives` and `generation` say what the list held when it lowered.
        Canvas,
        // The style's outline: a border-only Box record whose rect is the element's rect grown by
        // offset + width, borderColor and borderWidth the outline's; a ring outside the element's own records.
        Outline,
        // An inset shadow entry: rect is the element's own, cornerRadius its radii, color the shadow's;
        // blur, shadowOffset and shadowSpread describe the hole that stays clear (the rect shrunk by the spread and
        // moved by the offset). It paints over the element's fill and images.
        InsetShadow,
    };

    // One run of a rich text record: the record's text bytes [begin, end) in one style, or an inline sprite
    // (the placeholder U+FFFC stands for it in the text). Sizes are UI pixels after layout scale, like textSize.
    struct UiPaintSpan
    {
        u32 begin = 0;
        u32 end = 0;
        u32 color = 0; // rgba8, red in the low byte
        float size = 0.0f; // text size of the run
        FontId font = NO_FONT; // the face of the run's weight
        UiTextureId sprite = NO_UI_TEXTURE; // set: an inline sprite, a box as high as the run's line
        float aspect = 1.0f; // sprite width over height; 1 until the sprite resolves
        SPITE_OBSERVED_SCHEMA(UiPaintSpan, begin, end, color, size, font, sprite, aspect)
    };

    // One primitive in paint order and one row of the
    // "uiPaint" table. Lengths are UI pixels after layout scale.
    struct UiPaintRecord
    {
        Entity element = Entity::undefined();
        UiPaintKind kind = UiPaintKind::Fill;
        UiRect rect;
        UiRect clip; // effective clip, always inside the target
        // rounded clip corners (top-left, top-right, bottom-right, bottom-left): the clipping ancestor's corner
        // radius on the clip corners that are its own corners
        Vec4 clipRadii;
        u32 color = 0; // rgba8, red in the low byte: fill color, image tint, text color
        // top-left, top-right, bottom-right, bottom-left; all equal for a single radius
        Vec4 cornerRadius;
        // Fill decorations: the gradient runs from color to gradientColor (equal: flat) along gradientAngle
        // degrees (CSS); the border is inside rect. Shadow records use blur.
        u32 gradientColor = 0;
        float gradientAngle = 180.0f;
        u32 borderColor = 0;
        float borderWidth = 0.0f;
        float blur = 0.0f;
        // Shadow and InsetShadow records: the entry's offset and spread after layout scale. A Shadow's rect already
        // includes both (moved by the offset, grown by the spread); an InsetShadow's does not.
        Vec2 shadowOffset{};
        float shadowSpread = 0.0f;
        UiTextureId texture = NO_UI_TEXTURE;
        // Image records: the layout scale times the element's visual scale; a 9-slice sprite's corner insets are this
        // many UI pixels per sprite pixel.
        float imageScale = 1.0f;
        std::string text; // Text records: label or input value, laid out in the rect
        FontId font = NO_FONT;
        float textSize = 0.0f;
        UiTextAlign textAlign = UiTextAlign::Left;
        // A label's lines: set when the text breaks lines (explicit newlines, wrap, max lines, ellipsis)
        // or sits other than as one centered line; the run is laid out within rect.size.x. Off: v1's single line.
        bool textBlock = false;
        UiTextWrap textWrap = UiTextWrap::None;
        UiTextOverflow textOverflow = UiTextOverflow::Clip;
        UiTextVAlign textVAlign = UiTextVAlign::Center;
        u32 textMaxLines = 0;
        float textLineHeight = 1.0f;
        // px after each glyph's advance, scaled; plain and block runs both lay out with it
        float textLetterSpacing = 0.0f;
        Vec2 textInset{}; // the fit padding (scaled) the block sits inside
        // Canvas records: the list's length and content generation, and what lowering needs (not table columns): the
        // local-to-target scale and the list, valid only during the repaint that made the record.
        u32 primitives = 0;
        u64 generation = 0;
        Vec2 canvasScale{1.0f, 1.0f};
        const UiCanvasContent* canvas = nullptr;
        // A rich label: `text` is the plain text, laid out as a block, and one span per run in it. Empty
        // for every other record.
        std::vector<UiPaintSpan> spans;
        SPITE_OBSERVED_SCHEMA(UiPaintRecord, element, kind, rect, clip, clipRadii, color, cornerRadius, gradientColor,
                              gradientAngle, borderColor, borderWidth, blur, shadowOffset, shadowSpread, texture,
                              imageScale, text, font, textSize, textAlign, textBlock, textWrap, textOverflow,
                              textVAlign, textMaxLines, textLineHeight, textLetterSpacing, textInset, spans, primitives,
                              generation)
    };

    enum class UiShape : u32
    {
        Solid,
        Textured,
        SdfRounded,
        MsdfGlyph,
        Box, // rounded rect with a gradient and an inside border in one instance (see UiInstance)
        Shadow,
        Sliced, // a 9-slice sprite stretched over the rect (see UiInstance)
        Segment, // a line segment with round caps between two opposite corners of the rect (see UiInstance)
        Triangle, // a triangle inside the rect (see UiInstance)
        // one pixel column of a series primitive over the rect: the vertex stage reads the series and
        // draws the column as a Segment or a Solid bar; textureIndex is the series slot, uvMin (column, columns),
        // uvMax.x 1 for bars, param the half line width
        Series,
    };

    // A sprite of an atlas page in page pixels, origin top-left (the SpriteAtlas of the asset module, without its
    // types: the view module does not depend on assets). Insets are the 9-slice borders; all zero is a plain sprite.
    // The Sliced instance packs these into 12-bit fields: pages and sprites stay within SPRITE_ATLAS_LIMIT_MAX_SIZE.
    struct UiSpriteRect
    {
        u32 pageWidth = 0;
        u32 pageHeight = 0;
        u32 x = 0;
        u32 y = 0;
        u32 width = 0;
        u32 height = 0;
        u32 left = 0;
        u32 top = 0;
        u32 right = 0;
        u32 bottom = 0;

        [[nodiscard]] bool sliced() const { return (left | top | right | bottom) != 0; }
    };

    // A texture index naming render view k's target (UiViewContext::registerImage):
    // the target has one bindless slot per frame slot, so the UiView pass resolves
    // it for the frame it draws. Views below MAX_UI_VIEW_TEXTURES only.
    inline constexpr u32 UI_VIEW_TEXTURE_FLAG = 0x80000000u;
    inline constexpr u32 MAX_UI_VIEW_TEXTURES = 8;
    // Set on a Textured instance's textureIndex when it samples an atlas sprite: the shader reads mip 0 only, so no
    // filter tap crosses into a neighbouring sprite (Sliced always does). Mirrors UI_ATLAS_TEXTURE_FLAG in
    // ui_view.vert.slang.
    inline constexpr u32 UI_ATLAS_TEXTURE_FLAG = 0x40000000u;
    constexpr u32 uiViewTexture(const u32 view) { return UI_VIEW_TEXTURE_FLAG | view; }

    // One primitive as the GPU draws it: the vertex shader expands instance i into a quad, the
    // fragment shader clips it per pixel to its clip rect with rounded corners. A zero size is an empty slot
    // that draws nothing. Mirrored by UiInstance in shaders/source/shared/ui_view.slang.
    struct UiInstance
    {
        Vec2 position; // UI pixels, target space: top-left origin, y-down
        Vec2 size;
        Vec2 uvMin; // image or glyph atlas coordinates at the top-left and bottom-right corners
        Vec2 uvMax;
        Vec2 clipMin; // the clip rect, target space
        Vec2 clipMax;
        Vec4 clipRadii; // rounded clip corners: top-left, top-right, bottom-right, bottom-left
        u32 color = 0; // rgba8, red in the low byte
        u32 textureIndex = 0; // bindless index or uiViewTexture(k); Textured and MsdfGlyph only
        UiShape shape = UiShape::Solid;
        // Box and Shadow reuse the fields their shapes do not read (no new fields): Box: color is the
        // gradient start, textureIndex its end color, uvMin = (gradient angle in radians, border width), uvMax =
        // the border color's rgba8 word as two 16-bit halves (uiPackWord: exact integers, never NaN); Shadow: color is
        // the shadow color, uvMin.x the blur, the instance rect already grown by max(blur, 1) on each side. Sliced
        //: color is the tint, textureIndex the atlas page; uvMin = (x | y << 12, width - 1 | height - 1
        // << 12), uvMax = (left | top << 12, right | bottom << 12) are the sprite's page pixel rect and insets as
        // exact integers (under 2^24), param the UI pixels per sprite pixel of the insets. Segment: the
        // rect is the endpoints' bounding box (a zero extent is 0.001), the endpoints are its opposite corners -
        // top-left to bottom-right for a positive param, bottom-left to top-right for a negative one - and
        // |param| is the half width in pixels; the quad grows by |param| + 1. Triangle: the rect is the corners'
        // bounding box and each corner is two 16-bit fractions of it (x low, y high) of a word: corner 0 is
        // uiPackWord-encoded in uvMin, corner 1 in uvMax, corner 2 is textureIndex.
        // SdfRounded, Box and Shadow with four differing corner radii (uiSetCornerRadii): shape also
        // carries UI_SHAPE_CORNERS and the bottom-left radius in bits 16..23, and param is the exact integer
        // topLeft | topRight << 8 | bottomRight << 16; each radius is whole pixels, 0..UI_CORNER_RADIUS_MAX.
        // An inset shadow is a Shadow with UI_SHAPE_INSET: the rect and radii are the element's own, color
        // the shadow color, uvMin = (blur, spread) and uvMax = the offset, all in pixels as floats.
        float param = 0.0f; // SdfRounded, Box, Shadow: corner radius, or four packed radii (UI_SHAPE_CORNERS);
                            // MsdfGlyph: screen pixel range

        [[nodiscard]] bool isEmpty() const { return size.x <= 0.0f || size.y <= 0.0f; }
    };
    static_assert(sizeof(UiInstance) == 80);

    // Mirrors UI_SHAPE_CORNERS in ui_view.slang: the shape word's flag for packed per-corner radii.
    inline constexpr u32 UI_SHAPE_CORNERS = 0x100u;
    // Mirrors UI_SHAPE_INSET in ui_view.slang: the shape word's flag that makes a Shadow an inset shadow.
    inline constexpr u32 UI_SHAPE_INSET = 0x200u;
    // The largest radius of a packed (differing) set; larger values clamp, in pixels after layout scale. A radius
    // above half the rect's short side is clamped by the shader anyway; a single radius is never packed or clamped.
    inline constexpr float UI_CORNER_RADIUS_MAX = 255.0f;

    // Sets the corner radii (top-left, top-right, bottom-right, bottom-left) of a SdfRounded, Box or Shadow
    // instance whose shape is already set: equal radii stay the one float in param (the shader's unchanged path),
    // differing ones are packed as UiInstance describes.
    inline void uiSetCornerRadii(UiInstance& instance, const Vec4 radii)
    {
        if (radii.x == radii.y && radii.x == radii.z && radii.x == radii.w)
        {
            instance.param = radii.x;
            return;
        }
        const auto whole = [](const float radius)
        { return static_cast<u32>(std::lround(std::clamp(radius, 0.0f, UI_CORNER_RADIUS_MAX))); };
        instance.param = static_cast<float>(whole(radii.x) | (whole(radii.y) << 8) | (whole(radii.z) << 16));
        instance.shape =
            static_cast<UiShape>(static_cast<u32>(instance.shape) | UI_SHAPE_CORNERS | (whole(radii.w) << 16));
    }

    // A 32-bit word carried in two float fields as exact integers (low half, high half; never NaN or denormal);
    // the shader recombines them with uint().
    inline Vec2 uiPackWord(const u32 word)
    {
        return Vec2{static_cast<float>(word & 0xFFFFu), static_cast<float>(word >> 16)};
    }
    inline u32 uiUnpackWord(const Vec2 halves)
    {
        return static_cast<u32>(halves.x) | (static_cast<u32>(halves.y) << 16);
    }

    // The instance run [first, end) one repaint changed; a repaint that changed none logs {generation, 0, 0}.
    struct UiInstanceDamage
    {
        u64 generation = 0;
        u32 first = 0;
        u32 end = 0;
    };

    // the paint positions [start, end) of a root bound to an OS window and that window's rect (layout pixels
    // of the UI): what clips the root's elements.
    struct UiWindowSurface
    {
        u32 start = 0;
        u32 end = 0;
        UiRect rect;
        u64 window = 0;
    };

    // The instances [first, end) a window's bound roots painted, drawn into that window's target with `origin`
    // (the window's rect position) at its top-left; the main target draws every instance outside such runs.
    struct UiWindowRun
    {
        u64 window = 0;
        u32 first = 0;
        u32 end = 0;
        Vec2 origin;
    };

    struct UiFrameGeometry
    {
        // Paint order, drawn as one instanced draw. Each element owns a block of instances; the block's unused
        // tail is empty, so an element whose primitive count changes within its block moves no other instance.
        std::vector<UiInstance> instances;
        Vec2 targetSize;
        std::vector<UiWindowRun> windowRuns; // in instance order, rewritten each update
        u64 generation = 0; // new on every repaint, unique across views; the render shim uploads on change
        // Repaints since baseGeneration, oldest first and bounded: baseGeneration is the geometry the first entry
        // started from (the last full repaint or the oldest generation trimmed away).
        std::vector<UiInstanceDamage> damage;
        u64 baseGeneration = 0;

        // Fills `runs` with the sorted, disjoint instance runs a buffer holding `held` must rewrite to match this
        // frame (none when it already does); false when it must rewrite everything (the generation is unknown).
        SPITE_UI_RUNTIME_API bool staleRuns(const u64 held, std::vector<std::pair<u32, u32>>& runs) const;
    };
} // namespace spite
