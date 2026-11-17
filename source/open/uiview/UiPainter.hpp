#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "UiFont.hpp"
#include "UiTextCase.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiLayout.hpp"
#include "ui/UiRichText.hpp"
#include "ui/UiTextMeasurer.hpp"
#include "uiview/UiFrame.hpp"
#include "uiruntime/UiPaintRow.hpp"
#include "uiruntime/UiPaintLowering.hpp"
#include "uiview/UiStyleSheet.hpp"

namespace spite
{
    class World;
    class UiContext;

    // Texture sources by id: asset textures and font atlases resolve through
    // the render shim, code-registered bindless textures from the start.
    struct UiTextureTable
    {
        struct Entry
        {
            InternedString asset{}; // set for asset textures
            InternedString file{}; // set for an image read from a file (registerImageFile); the path as registered
            u32 width = 0; // the pixels the resolver made of a file image
            u32 height = 0;
            bool released = false; // a file image no slot shows any more
            FontId font = NO_FONT; // set for font atlas pages
            u32 page = 0;
            std::optional<u32> bindlessIndex;
            u64 backing = 0; // the resolver's handle to the texture it made, read by the render shim
            std::optional<UiSpriteRect>
                sprite; // set when the name resolved to an atlas sprite (bindlessIndex: its page)
            bool failed = false; // will never resolve: the element shows its fill without waiting
        };

        std::vector<Entry> entries;
        std::unordered_map<InternedString, UiTextureId> byAsset;
        std::unordered_map<u32, UiTextureId> byBindless;
        std::unordered_map<InternedString, UiTextureId> byFile;
        std::unordered_map<u64, UiTextureId> imageSlots; // registered, ahead of the stylesheet
        std::vector<UiTextureId> pending;
        std::vector<UiTextureId> released; // file images dropped since the resolver last took them

        UiTextureId forAsset(InternedString asset);
        UiTextureId forBindless(u32 bindlessIndex);
        UiTextureId forFile(InternedString file);
        // Drops the file image `id` when no image slot shows it any more; other textures are never released.
        void releaseIfUnused(UiTextureId id);
        UiTextureId forFont(FontId font, u32 page);
        UiTextureId forImageSlot(u64 imageSlot, const UiStyleSheet& styles);
    };

    // An element whose animation component and resolved style disagree after a paint: the view reconciles it
    // (adds, retargets or removes the UiStyleAnim component) once the repaint is done.
    struct UiPendingTransition
    {
        Entity element;
        UiStyle style;
    };

    // Parsed rich text by markup: a label's markup is parsed on the first request of a text, then
    // looked up (by the markup, not the element). Bounded; a full cache is cleared, so a reference is valid until the
    // next get(). A malformed text is kept too, as its literal fallback, and logs once.
    class UiRichTextCache
    {
    public:
        static constexpr sizet MAX_ENTRIES = 512;

        const UiRichText& get(std::string_view markup);
        [[nodiscard]] sizet size() const { return m_entries.size(); }
        void clear()
        {
            m_entries.clear();
            m_oversizeKey.clear();
            m_oversize = {};
        }

        u64 parses = 0; // markups parsed (misses)
        u64 malformed = 0;

    private:
        std::unordered_map<std::string, UiRichText, UiTransparentStringHash, std::equal_to<>> m_entries;
        std::string m_oversizeKey; // the one over-limit markup kept, as its literal
        UiRichText m_oversize;
    };

    // The area the focused text input reports to the input method, written by paintElement for the
    // element being edited: UI pixels of the view's target; `owner` is the element it belongs to. `scroll` is how far
    // the owner's text, wider than its rect, is shifted left (UI pixels before layout scale): it stays while the caret
    // is inside the rect, and a click maps through it.
    struct UiPaintedTextArea
    {
        Entity owner = Entity::undefined();
        UiTextInputArea area;
        float scroll = 0.0f;
    };

    struct UiPaintInputs
    {
        const World& world;
        UiLayoutView layout;
        const UiStyleSheet& styles;
        UiFonts fonts;
        Entity hovered;
        Entity pressed;
        Entity focused;
        float scale;
        Vec2 targetSize;
        // the paint ranges of the roots bound to OS windows and their windows' rects; an element in one is
        // clipped to that rect instead of the target
        std::span<const UiWindowSurface> windows;
        std::vector<UiPendingTransition>* transitions = nullptr; // null: not collected
        UiRichTextCache* richCache = nullptr; // null: rich labels paint as plain text
        UiPaintedTextArea* textArea = nullptr; // null: not collected
        const UiContext* ui = nullptr; // null: no canvases
        UiCaseLocale caseLocale = UiCaseLocale::Default; // the rules text transforms follow
        Entity stylesheetRoot = Entity::undefined();
        const UiStyleSheet* subtreeStylesheet = nullptr;

        [[nodiscard]] const UiStyleSheet& stylesFor(Entity element) const;
    };

    inline const UiStyleSheet& subtreeStylesFor(const UiHierarchyManager& hierarchy, Entity element,
                                                const UiStyleSheet& styles, const Entity root,
                                                const UiStyleSheet* sheet)
    {
        if (sheet)
        {
            for (; element.isValid(); element = hierarchy.parentOf(element))
            {
                if (element == root)
                {
                    return *sheet;
                }
            }
        }
        return styles;
    }

    inline const UiStyleSheet& UiPaintInputs::stylesFor(const Entity element) const
    {
        return subtreeStylesheet && ui
            ? subtreeStylesFor(ui->hierarchy(), element, styles, stylesheetRoot, subtreeStylesheet)
            : styles;
    }

    // The animation component of a resolved style: displayed and target values both the style's.
    [[nodiscard]] UiStyleAnimComponent animOf(const UiStyle& style);
    // True when the component's targets are the style's transitionable values.
    [[nodiscard]] bool animTargetsStyle(const UiStyleAnimComponent& anim, const UiStyle& style);
    // The style with its transitionable values replaced by the component's displayed ones.
    void applyAnim(UiStyle& style, const UiStyleAnimComponent& anim);

    // The components paintElement reads, as pointers into the world's chunks; a filled row has an identity.
    // The last clip owner's rect and clamped resting corner radius: consecutive elements in paint order mostly
    // share one. Reset it when styles may have changed.
    struct UiClipOwnerCache
    {
        Entity owner = Entity::undefined();
        UiRect rect;
        Vec4 radii; // top-left, top-right, bottom-right, bottom-left
    };

    // The composed UiVisualComponent overrides of an element's ancestors and itself as one transform of target-space
    // points, p' = scale * p + offset, plus the alpha and color factors.
    struct UiVisualEffect
    {
        float opacity = 1.0f;
        Vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
        Vec2 scale{1.0f, 1.0f};
        Vec2 offset{0.0f, 0.0f};
    };

    // Visual overrides as a sweep over paint positions: the owners' subtree spans sorted by start, so
    // seek(position) for increasing positions keeps a stack of the spans containing it, each composed with the
    // ones outside it. Without owners it costs nothing per element.
    class UiVisualSweep
    {
    public:
        // Rebuilds the spans from the hierarchy's owners that are laid out and not the identity.
        void build(const UiHierarchyManager& hierarchy, const World& world);
        [[nodiscard]] bool empty() const { return m_spans.empty(); }
        // Moves to the element at paint position `position`; a smaller position than the last restarts.
        void seek(u32 position);
        // The effect on the element seek() reached; null: none.
        [[nodiscard]] const UiVisualEffect* current() const
        {
            return m_stack.empty() ? nullptr : &m_stack.back().effect;
        }
        // The effect on the ancestor at `position` (an ancestor of the sought element); null: none.
        [[nodiscard]] const UiVisualEffect* at(u32 position) const;

    private:
        struct Span
        {
            u32 start = 0;
            u32 end = 0;
            UiVisualEffect effect; // the owner's own, until composed on push
        };
        std::vector<Span> m_spans;
        std::vector<Span> m_stack; // nested spans containing the sought position, effects composed
        sizet m_next = 0;
        u32 m_last = 0;
    };

    // Stage 1 for one element of the resolved layout: appends its records, all
    // carrying the element's clip; hidden or wholly clipped elements emit none.
    // A text record takes its string from spareTexts when one is left (reused capacity).
    // `visuals` (null: none) must have been seek()ed to the element's paint position; `row` (null: look the
    // components up) is the element's UiPaintRows entry.
    void paintElement(const UiPaintInputs& inputs, UiTextureTable& textures, UiClipOwnerCache& clipCache, Entity entity,
                      const UiVisualSweep* visuals, const UiPaintRow* row, std::vector<UiPaintRecord>& records,
                      std::vector<std::string>& spareTexts);

    // The text measurer behind content sizing: the element's text in its
    // resting style (normal, or disabled when not interactable) — the run
    // width and the line box lowering centers, in UI pixels before layout
    // scale. Hover and press never resize. No font: zero width, the text size
    // as line height. The text is measured converted by the style's text transform under `caseLocale`. A `textSize`
    // above 0 replaces the style's (text scale not applied to it), as a canvas text primitive's size does.
    Vec2 measureUiText(const World& world, const UiStyleSheet& styles, UiFonts fonts, Entity element,
                       std::string_view text, UiCaseLocale caseLocale = UiCaseLocale::Default, float textSize = 0.0f);

    // Whether a label's text lays out as a block: its style breaks lines, ellipsizes, limits its lines or
    // scales the line height. Otherwise it is v1's single line.
    [[nodiscard]] bool usesTextBlock(const UiStyle& style);

    // Measured label blocks by text, font, size, width and line options (not by element): the same text at the same
    // width is laid out once, until the fonts or the stylesheet change. Bounded; a full cache is cleared.
    class UiTextMeasureCache
    {
    public:
        static constexpr sizet MAX_ENTRIES = 4096;

        struct Key
        {
            u64 text = 0; // hash of the text, with its length
            u64 length = 0;
            FontId font = NO_FONT;
            u32 size = 0; // bits of the floats
            u32 width = 0;
            u32 lineHeight = 0;
            u32 letterSpacing = 0;
            u32 maxLines = 0;
            u8 wrap = 0;
            u8 overflow = 0;
            u8 transform = 0;
            bool rich = false;
            bool operator==(const Key&) const = default;
        };

        struct KeyHash
        {
            sizet operator()(const Key& key) const
            {
                u64 h = key.text ^ (key.length * 0x9E3779B97F4A7C15ull);
                for (const u64 part :
                     {u64{key.font} << 32 | key.size, u64{key.width} << 32 | key.lineHeight, u64{key.letterSpacing},
                      u64{key.maxLines} << 25 | u64{key.transform} << 17 | u64{key.rich} << 16 | u64{key.wrap} << 8 |
                          key.overflow})
                {
                    h = (h ^ part) * 0x100000001B3ull;
                    h ^= h >> 29;
                }
                return static_cast<sizet>(h);
            }
        };

        [[nodiscard]] const UiTextMeasure* find(const Key& key) const
        {
            const auto it = m_entries.find(key);
            return it == m_entries.end() ? nullptr : &it->second;
        }
        void insert(const Key& key, const UiTextMeasure& value)
        {
            if (m_entries.size() >= MAX_ENTRIES)
            {
                m_entries.clear();
            }
            m_entries.emplace(key, value);
        }
        void clear() { m_entries.clear(); }
        [[nodiscard]] sizet size() const { return m_entries.size(); }

        u64 layouts = 0; // block layouts run (misses)
        u64 hits = 0;

    private:
        std::unordered_map<Key, UiTextMeasure, KeyHash> m_entries;
    };

    // measureUiText for a label, laid out in lines within `width` (< 0: unbounded; see UiTextMeasurer).
    UiTextMeasure measureUiTextBlock(const World& world, const UiStyleSheet& styles, UiFonts fonts, Entity element,
                                     std::string_view text, float width, UiTextMeasureCache& cache,
                                     UiCaseLocale caseLocale = UiCaseLocale::Default);

    // The elements whose text lowered short of a glyph cell or a page texture, or whose image lowered short of its
    // texture (keyed like a page texture: by texture id), by what they wait on: a resolved
    // dependency wakes its own waiters only. A repaint that still waits on a dependency does not add it twice. An
    // element whose text stopped needing a dependency stays listed until it resolves (one spare repaint).
    class UiWaitSet
    {
    public:
        static u64 glyphKey(const UiGlyph* glyph) { return reinterpret_cast<std::uintptr_t>(glyph); }
        static u64 pageKey(const UiTextureId texture) { return (u64{1} << 63) | texture; }

        void add(const u64 element, const u64 key)
        {
            std::vector<u64>& keys = m_keysOf[element];
            if (std::ranges::find(keys, key) == keys.end())
            {
                keys.push_back(key);
                m_waiters[key].push_back(element);
            }
        }

        // Calls wake(element) for each waiter of `key` and forgets the dependency.
        template <class Wake>
        void resolve(const u64 key, Wake&& wake)
        {
            const auto it = m_waiters.find(key);
            if (it == m_waiters.end())
            {
                return;
            }
            const std::vector<u64> elements = std::move(it->second);
            m_waiters.erase(it);
            for (const u64 element : elements)
            {
                const auto keys = m_keysOf.find(element);
                if (keys != m_keysOf.end())
                {
                    std::erase(keys->second, key);
                    if (keys->second.empty())
                    {
                        m_keysOf.erase(keys);
                    }
                }
                wake(element);
            }
        }

        [[nodiscard]] sizet elementCount() const { return m_keysOf.size(); }

    private:
        std::unordered_map<u64, std::vector<u64>> m_waiters; // dependency -> elements
        std::unordered_map<u64, std::vector<u64>> m_keysOf; // element -> dependencies
    };

    // What measuring a rich label needs besides the world: the parsed markup, the sprites' textures
    // (measuring requests them, and reads the aspect of a resolved one) and the layout waiters.
    struct UiRichMeasureInputs
    {
        UiRichTextCache& markup;
        UiTextureTable& textures;
        UiWaitSet& layoutWaiting;
    };

    // measureUiTextBlock for a rich label: `markup` laid out as its spans, lines of the tallest item of each. A
    // sprite not resolved yet counts as a square and is not cached: the element is added to layoutWaiting under the
    // sprite's texture, and layout runs again when it resolves.
    UiTextMeasure measureRichUiTextBlock(const World& world, const UiStyleSheet& styles, UiFonts fonts, Entity element,
                                         std::string_view markup, float width, UiTextMeasureCache& cache,
                                         const UiRichMeasureInputs& rich);

    void prepareUiPaint(std::span<const UiPaintRecord> records, const UiTextureTable& textures, UiFonts fonts,
                        std::vector<UiPlacedGlyph>& placed, UiPreparedPaint& prepared);
} // namespace spite
