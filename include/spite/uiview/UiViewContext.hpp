#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/Expected.hpp"
#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "ecs/EcsTypes.hpp"
#include "uiview/UiFrame.hpp"
#include "uiview/UiText.hpp"
#include "uiview/UiTextureId.hpp"
#include "uiview/UiViewExport.hpp"

namespace spite
{
    class World;
    class UiContext;
    class UiSeriesTable;
    class UiStyleSheet;
    struct UiPaintRecord;
    struct UiFrameGeometry;
    struct UiSpriteRect;
    enum class UiInteractionState : u8;
    class CommandBuffer;

    // What a texture id stands for: a baked asset (asset set), a page of a font's glyph atlas (font and page set),
    // an image file (file set), or a code-registered bindless texture (neither).
    struct UiTextureSource
    {
        InternedString asset{};
        InternedString file{}; // the path as registerImageFile was given it
        FontId font = NO_FONT;
        u32 page = 0;
    };

    // A glyph atlas page for the render side: square RGBA8 MSDF cells, rows top to bottom. `version` grows with
    // every cell added; `backing` is what the resolver gave resolveTexture (0 until the page has a texture).
    struct UiAtlasPage
    {
        UiTextureId texture = NO_UI_TEXTURE;
        u32 size = 0;
        u64 version = 0;
        u64 backing = 0;
        std::span<const std::byte> rgba8;
    };

    struct UiAtlasRect
    {
        u32 x = 0;
        u32 y = 0;
        u32 width = 0;
        u32 height = 0;
    };

    struct UiFontPageInfo
    {
        u32 atlasPage = 0; // the index atlasPage() takes
        u32 usedRows = 0;
        u64 version = 0;
        bool resolved = false;
    };

    // What the control query shows per font: glyphs by state (pending: metrics known, cell not yet generated)
    // and its pages.
    struct UiFontInfo
    {
        std::string name;
        std::vector<std::string> fallbacks;
        u32 ready = 0;
        u32 blank = 0;
        u32 pending = 0;
        u32 failed = 0;
        u32 absent = 0;
        u32 copied = 0; // of the ready ones, the cells copied from the process-wide cache of cells other views made
        std::vector<UiFontPageInfo> pages;
    };

    enum class UiImageState : u8
    {
        Pending, // the texture or atlas has not resolved: the element shows its fill
        Ready,
        Failed, // never resolved: the load failed or the atlas has no such sprite
    };

    // What an image slot registered from a file shows: its state and, once Ready, the pixels of the texture the
    // resolver made of it (the file's size, scaled down to the resolver's cap), for a caller that keeps the aspect.
    struct UiImageInfo
    {
        UiImageState state = UiImageState::Pending;
        u32 width = 0;
        u32 height = 0;
    };

    // What an element's image record shows (uiTree): the name as the style or slot gave it, and whether it resolved
    // to an atlas sprite or a plain texture.
    struct UiShownImage
    {
        InternedString name{};
        UiImageState state = UiImageState::Pending;
        bool sprite = false; // meaningful once Ready
    };

    // One glyph of laid-out text: the face (a font id) and its glyph index, the byte offset in the text of the first
    // codepoint it renders, and its pen position in px (x from the start of the line, y up from the baseline).
    struct UiTextGlyph
    {
        FontId font = NO_FONT;
        u32 glyph = 0;
        u32 cluster = 0;
        float x = 0.0f;
        float y = 0.0f;
    };

    // One extra record for an element in a repaint batch, addressed by its index in that batch.
    struct UiCustomPaintRecord
    {
        u32 elementIndex = 0;
        UiPaintRecord record;
    };

    using UiCustomPaint = std::function<void(std::span<const Entity>, std::vector<UiCustomPaintRecord>&)>;

    // The view module root for one world: owns the stylesheet,
    // fonts, the texture table and the painted frame. Registers itself as the
    // "UiView" service with the "uiPaint" table, and the "UiViewPaint"
    // PRE_RENDER system that calls update(). Destroy it before the UiContext
    // and the world.
    //
    // A style change animates only through the tweens of UiStyleAnim (UiTweens: the world's frame time, reduced
    // motion snaps), so a quiescent UI costs nothing and a transitioning element repaints alone.
    //
    // Repaint is change-gated and partial: only the hierarchy's paint damage
    // (re-laid out subtrees, paint-marked elements) and the elements whose
    // hovered/pressed/focused state changed repaint; a full resolve, the
    // stylesheet, target size, fonts, image or texture changes repaint
    // everything. Widget setters mark the layout dirty; raw component writes
    // must call UiHierarchyManager::markLayoutDirty, as rect writes do.
    class SPITE_UIVIEW_API UiViewContext
    {
    public:
        static constexpr const char* SPITE_SERVICE_NAME = "UiView";

        UiViewContext(World& world, UiContext& ui);
        ~UiViewContext();
        UiViewContext(const UiViewContext&) = delete;
        UiViewContext& operator=(const UiViewContext&) = delete;

        [[nodiscard]] UiStyleSheet& stylesheet();
        // A draft canvas can use its own stylesheet for layout measurement and paint.
        // The caller owns the sheet and clears the override before destroying it.
        void setSubtreeStylesheet(Entity root, UiStyleSheet* sheet);
        [[nodiscard]] const UiStyleSheet& stylesheetFor(Entity element) const;
        // The display state in one step: the layout scale (UI scale times the window's display scale) and
        // the stylesheet's text scale. A change of either re-lays out the UI once, now, and the next update repaints
        // it; the same values again cost nothing. A non-finite or non-positive scale is ignored. Returns whether
        // anything changed.
        bool applyDisplay(float layoutScale, float textScale);
        // A stylesheet change (a theme or density) in force now: the layout tokens it publishes are written into the
        // bound layout fields and the UI is laid out once, so the next update repaints it without a second resolve.
        // Nothing when the stylesheet is as the last update or call left it.
        void settleStyle();
        // The render target in UI pixels; unclipped elements clip to it, and
        // nothing paints until it is set.
        void setTargetSize(Vec2 size);

        // Loads the face and bakes the ASCII cells of its MSDF atlas; registering a name again returns the existing
        // font. The first font is the default for styles that name none or an unknown one. Other glyphs get their
        // cell at first use, at most glyphBudgetMs per update (the oldest requests first, at least one), and the
        // elements waiting on one repaint alone when it lands. A codepoint a style's font lacks comes
        // from the faces its stylesheet's "fontFallbacks" lists for it, in order.
        Expected<FontId> registerFont(InternedString name, const std::filesystem::path& ttf);
        [[nodiscard]] FontId findFont(InternedString name) const;
        [[nodiscard]] float textAdvance(FontId font, std::string_view text, float sizePx) const;
        // The glyphs a single line of the text lays out as in the font and its fallbacks, in visual order: what a
        // label draws, shaped runs included.
        [[nodiscard]] std::vector<UiTextGlyph> textGlyphs(FontId font, std::string_view text, float sizePx) const;
        // Runs shaped since the fonts loaded: a run of complex-script text is shaped once per face and run text,
        // then answered from the face's cache whatever the size, width or repaint.
        [[nodiscard]] u64 textShapeCount() const;
        // A page of the font's atlas as it is now (page 0: the baked ASCII page).
        [[nodiscard]] UiFontAtlasView fontAtlas(FontId font, u32 page = 0) const;
        void setGlyphBudgetMs(double milliseconds);
        [[nodiscard]] double glyphBudgetMs() const;
        // Cells generated since the view was created.
        [[nodiscard]] u64 glyphsGenerated() const;
        // Multi-line label layouts the text measurer ran for layout; a repeat of the same text, font,
        // size, width and line options is answered from a cache, until the fonts or the stylesheet change.
        [[nodiscard]] u64 textLayoutCount() const;
        // Rich text markups parsed so far: a markup is parsed once, on the first measure or paint of
        // a rich label showing it, and never for a label without UiLabelComponent::rich.
        [[nodiscard]] u64 richTextParseCount() const;
        // Markups the rich text cache holds by key; a markup over the length limit is never one of them.
        [[nodiscard]] sizet richTextCachedCount() const;
        // Elements waiting on a glyph cell, a page texture or an image's texture (the name predates image waiters).
        [[nodiscard]] u32 glyphWaitingCount() const;
        [[nodiscard]] std::vector<UiFontInfo> fontInfo() const;

        // Atlas pages of every font, dense and stable (a page never moves or goes away); the render shim keeps each
        // page texture equal to its pixels.
        [[nodiscard]] u32 atlasPageCount() const;
        [[nodiscard]] UiAtlasPage atlasPage(u32 index) const;
        // Fills `rects` with the cells added to the page after `held` (an atlasPage().version); false when the log
        // no longer reaches back that far: copy the whole page.
        [[nodiscard]] bool atlasDamageSince(u32 index, u64 held, std::vector<UiAtlasRect>& rects) const;

        // Image slots for UiImageComponent; a registration wins over the
        // stylesheet's "images" map. The bindless form shows code-owned
        // textures such as render targets.
        void registerImage(u64 imageSlot, InternedString assetName);
        void registerImage(u64 imageSlot, u32 bindlessIndex);
        // Shows an image file: `path` is absolute and UTF-8, built by the caller from a root it trusts (the Capture
        // panel: the captures root's listed directory plus an entry name). The resolver refuses a path that is not a
        // plain file, reads it off the frame thread (PNG only), refuses a file over its size cap and scales a large
        // image down. The slot shows its element's fill until the texture is
        // ready. A slot replaced or cleared (clearImage) drops its file image, and the resolver frees the texture once
        // no slot shows it; a path another slot shows is shared.
        void registerImageFile(u64 imageSlot, std::string_view path);
        // The slot shows nothing of its own again (the stylesheet's "images" entry, if any, applies).
        void clearImage(u64 imageSlot);
        // The slot's file image; nullopt for a slot that is not registered or is not a file image.
        [[nodiscard]] std::optional<UiImageInfo> imageInfo(u64 imageSlot) const;

        // Render-shim side: asset textures and font atlases
        // wait in pending until resolved to a bindless index; unresolved
        // images and text do not draw. An image name "atlas/sprite" (a style's background image, an image slot's
        // asset) names a sprite when the atlas exists; the resolver decides, after the name pended.
        [[nodiscard]] std::span<const UiTextureId> pendingTextures() const;
        [[nodiscard]] UiTextureSource textureSource(UiTextureId texture) const;
        // `backing` (non-zero) is the resolver's handle to the texture it made for an atlas page, read back by the
        // render shim. A resolved atlas page repaints the elements waiting on it; any other texture, everything.
        void resolveTexture(UiTextureId texture, u32 bindlessIndex, u64 backing = 0);
        // Resolves an image name to a sprite of the atlas page at bindlessIndex. The elements waiting on the name
        // repaint alone (as for a glyph cell), and so do they for resolveTexture of an asset texture.
        void resolveSprite(UiTextureId texture, u32 bindlessIndex, const UiSpriteRect& sprite);
        // Resolves a file image to its texture at bindlessIndex, `width` x `height` pixels; its waiters repaint alone.
        void resolveImageFile(UiTextureId texture, u32 bindlessIndex, u32 width, u32 height);
        // The file images dropped since the last call: the resolver frees what it made for them. Their ids never
        // appear in pendingTextures() again.
        [[nodiscard]] std::vector<UiTextureId> takeReleasedTextures();
        // The name will never resolve: its waiters repaint once and stop waiting. The texture leaves pendingTextures():
        // a failure is final, the name is never resolved again (a new name is a new texture).
        void failTexture(UiTextureId texture);
        // Requeue renderer-owned textures after a device restart. Raw bindless registrations cannot be rebuilt.
        [[nodiscard]] bool canResetGpuTextures() const;
        void resetGpuTextures();
        // The last image record of the element's last paint, if it has one.
        [[nodiscard]] std::optional<UiShownImage> shownImage(Entity element) const;
        // The icon name the element's resolved style (UiStyle::icon) gives it in `state`; invalid: none. The label's
        // own icon (UiLabelComponent::icon) is the element's and wins over it.
        [[nodiscard]] InternedString styleIconOf(Entity element, UiInteractionState state) const;

        // Adds records after each element's normal records. Called once per dirty paint range; output indices
        // refer to the supplied elements. The callback is owned by the view; captured references must outlive
        // registration. Do not mutate the view or hierarchy in the callback. Call invalidateCustomPaint after
        // changing captured data; registering or clearing the callback invalidates the whole view.
        void setCustomPaint(UiCustomPaint callback);
        void invalidateCustomPaint(Entity element);

        // Repaints when a paint input changed; true when it did. Style transitions: after a repaint an
        // element whose resolved style declares a transition and differs from the target its UiStyleAnim
        // component holds gets tweens to the new values (a missing component is added at the style's values: the
        // first paint never animates; one whose style no longer declares a transition loses it). `deferred`
        // (null: the world directly) takes the component adds and removes; the system passes its stage's buffer.
        bool update(CommandBuffer* deferred = nullptr);
        [[nodiscard]] std::span<const UiPaintRecord> paintRecords() const;
        // Elements painted by the last repaint, full or partial.
        [[nodiscard]] u32 paintedElementCount() const;
        [[nodiscard]] const UiFrameGeometry& frame() const;
        // The viewed UI's series buffers, which the render shim keeps a GPU copy of.
        [[nodiscard]] const UiSeriesTable& series() const;

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };

    // A UI and what it draws onto: render view k's output (its target, or the
    // swapchain when k is the swapchain view) or the swapchain itself.
    inline constexpr u32 UI_SWAPCHAIN_TARGET = ~0u;
    struct UiFrameSource
    {
        const UiViewContext* view = nullptr;
        u32 target = UI_SWAPCHAIN_TARGET;
        bool operator==(const UiFrameSource&) const = default;
    };

    // The UIs the UI passes draw, in order per target (later ones on top),
    // registered by the host in the services of the world whose render graph
    // runs the passes. A view may belong to another world (the editor world's UI
    // over the scene's); that world updates first, so its frame is
    // current when the passes record.
    struct UiFrameSources
    {
        std::vector<UiFrameSource> views;
    };
} // namespace spite
