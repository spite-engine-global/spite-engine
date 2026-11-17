#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/Expected.hpp"
#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "uiview/UiFrame.hpp"
#include "uiview/UiText.hpp"

namespace msdfgen
{
    class FreetypeHandle;
    class FontHandle;
} // namespace msdfgen

namespace spite
{
    enum class UiGlyphState : u8
    {
        Absent, // the face has no glyph for the codepoint
        Blank, // advance only (space)
        Pending, // metrics known, the MSDF cell is queued for generation
        Ready, // the cell is in its page
        Failed, // the pages are full: advance only, never drawn
    };

    // Metrics in em units: plane bounds relative to the pen on the baseline, y-up; atlas coordinates normalized,
    // y-down, within `page`.
    struct UiGlyph
    {
        float advance = 0.0f;
        u32 glyphIndex = 0;
        UiGlyphState state = UiGlyphState::Absent;
        u16 page = 0;
        Vec2 planeMin;
        Vec2 planeMax;
        Vec2 uvMin;
        Vec2 uvMax;
    };

    class UiFont;
    using UiFonts = std::span<const std::unique_ptr<UiFont>>;

    struct UiPlacedGlyph
    {
        const UiGlyph* glyph;
        const UiFont* font; // the face the glyph came from: its page textures
        float penX; // px from the start of the run
        float offsetY = 0.0f; // px up from the baseline (shaped marks)
    };
    static_assert(sizeof(UiPlacedGlyph) == 24, "every laid-out glyph is written and read per paint: keep it small");

    // One glyph of a shaped run in logical order, metrics in em: `cluster` is the byte offset in the run of the first
    // codepoint it renders; glyph 0 is the face's .notdef (a codepoint it lacks).
    struct UiShapedGlyph
    {
        u32 glyph = 0;
        u32 cluster = 0;
        float advance = 0.0f;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
    };

    struct UiShapedRun
    {
        std::vector<UiShapedGlyph> glyphs;
        bool rtl = false; // drawn right to left: a line shows the run's glyphs in reverse
    };

    struct UiTransparentStringHash
    {
        using is_transparent = void;
        sizet operator()(const std::string_view text) const { return std::hash<std::string_view>{}(text); }
    };

    // Codepoints whose glyphs depend on their neighbours beyond kerning: combining marks, joiners, variation
    // selectors and the scripts from Hebrew to Myanmar, Hangul jamo, Khmer, Mongolian, the Brahmic blocks after
    // them, the Arabic presentation forms and the right-to-left and Brahmic blocks of the supplementary planes.
    // Text without them keeps the per-codepoint path.
    [[nodiscard]] inline bool uiNeedsShaping(const u32 cp)
    {
        return cp >= 0x0300 &&
            (cp <= 0x036F || (cp >= 0x0590 && cp <= 0x109F) || (cp >= 0x1100 && cp <= 0x11FF) ||
             (cp >= 0x1700 && cp <= 0x18AF) || (cp >= 0x1900 && cp <= 0x1CFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
             (cp >= 0x200C && cp <= 0x200F) || (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xA800 && cp <= 0xABFF) ||
             (cp >= 0xD7B0 && cp <= 0xD7FF) || (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
             (cp >= 0xFE20 && cp <= 0xFE2F) || (cp >= 0xFE70 && cp <= 0xFEFE) || (cp >= 0x10800 && cp <= 0x11FFF) ||
             (cp >= 0x1E800 && cp <= 0x1EFFF) || (cp >= 0xE0100 && cp <= 0xE01EF));
    }

    // One new cell in a page: the rectangle a consumer must copy to hold the page at `version`.
    struct UiPageDamage
    {
        u64 version = 0;
        u16 x = 0;
        u16 y = 0;
        u16 width = 0;
        u16 height = 0;
    };

    // A square RGBA8 MSDF page, rows top to bottom, packed in shelves. `version` counts the cells added; damage
    // lists the cells of the versions after baseVersion (bounded: a consumer older than baseVersion copies the
    // whole page).
    struct UiFontPage
    {
        std::vector<std::byte> rgba8;
        u32 penX = 0;
        u32 penY = 0;
        u32 rowHeight = 0;
        u64 version = 0;
        u64 baseVersion = 0;
        std::vector<UiPageDamage> damage;
    };

    struct UiFontCounts
    {
        u32 ready = 0;
        u32 blank = 0;
        u32 pending = 0;
        u32 failed = 0;
        u32 absent = 0;
        u32 copied = 0; // Ready cells taken from the process-wide cell cache, not generated
    };

    // One face: FreeType metrics through msdfgen and kerning, HarfBuzz shaping, and MSDF cells generated on first use
    // into pages. The printable ASCII charset is baked into page 0 at load; any other
    // codepoint gets its metrics at the first layout (an outline load) and its cell later, under the view's per-frame
    // budget. A face lacking a codepoint defers to its fallbacks. Runs of complex scripts are shaped by HarfBuzz once
    // per run text and face; their glyphs are keyed by glyph index. Single-threaded: the world's update
    // thread.
    class UiFont
    {
    public:
        static constexpr sizet MAX_SHAPED_RUNS = 4096;
        static constexpr sizet MAX_SHAPED_BYTES = 4u << 20; // run texts + glyph arrays kept per face
        static constexpr sizet MAX_CACHED_RUN_BYTES = 16u << 10; // a longer run text is shaped but not kept
        static constexpr float EM_PX = 32.0f; // atlas pixels per em
        static constexpr float PX_RANGE = 4.0f; // distance range in atlas pixels
        static constexpr u32 PAGE_SIZE = 512;
        static constexpr u32 MAX_PAGES = 16; // a full set of pages drops further glyphs; nothing is evicted
        static constexpr sizet MAX_PAGE_DAMAGE = 128;
        static constexpr u32 FIRST_CHAR = 32;
        static constexpr u32 LAST_CHAR = 126;

        static Expected<std::unique_ptr<UiFont>> load(msdfgen::FreetypeHandle* library, InternedString name,
                                                      const std::filesystem::path& path);
        ~UiFont();
        UiFont(const UiFont&) = delete;
        UiFont& operator=(const UiFont&) = delete;

        // The glyph for a codepoint, never null; a Pending one is queued for generation.
        const UiGlyph* glyph(u32 codepoint) const;
        // The glyph for a glyph index of this face (a shaped glyph), never null; kept apart from the codepoint
        // glyphs, so a glyph reached both ways has two entries (and cells).
        const UiGlyph* glyphByIndex(u32 glyphIndex) const;
        [[nodiscard]] bool hasCodepoint(u32 codepoint) const;
        // The run shaped by HarfBuzz with this face, direction and script from its text, no language, default
        // features; em units, so one entry serves every size. Shapes on the first request of a run text only
        // (bounded by count and bytes: a full cache is cleared; a run text above MAX_CACHED_RUN_BYTES is not kept); the
        // reference is valid until the next shape() call.
        const UiShapedRun& shape(std::string_view run) const;
        [[nodiscard]] u64 shapeCount() const { return m_shapeCount; }
        // Kerning between two glyphs of this face, in em.
        float kerning(const UiGlyph& left, const UiGlyph& right) const;
        void reportMissing(u32 codepoint) const;

        [[nodiscard]] bool hasDemand() const { return m_demandHead < m_demand.size(); }
        // Generates the oldest queued cell and returns its glyph (Ready, or Failed when the pages are full); null
        // when there is none.
        const UiGlyph* generateOne();

        [[nodiscard]] std::span<const UiFontPage> pages() const { return m_pages; }
        [[nodiscard]] const UiFontCounts& counts() const { return m_counts; }

        InternedString name;
        float ascender = 0.0f;
        float descender = 0.0f;
        std::vector<FontId> fallbacks; // tried in order for codepoints this face lacks
        std::vector<UiTextureId> pageTextures; // set by the view per page

    private:
        UiFont() = default;

        struct Shaper; // HarfBuzz state and the face bytes, created at load
        std::unique_ptr<Shaper> makeShaper(const std::filesystem::path& path) const;

        void loadMetrics(u32 codepoint, UiGlyph& glyph) const;
        void loadIndexMetrics(u32 glyphIndex, UiGlyph& glyph) const;
        void rasterize(UiGlyph& glyph);
        bool allocate(u32 width, u32 height, u16& page, u32& x, u32& y);
        void count(UiGlyphState state, int delta) const;

        msdfgen::FontHandle* m_handle = nullptr;
        std::filesystem::path m_path;
        std::string m_cellKey; // the face file's identity in the process-wide cell cache
        bool m_baking = false; // load() is rasterizing the ASCII page: those cells are the page's, not cached
        std::array<UiGlyph, LAST_CHAR - FIRST_CHAR + 1> m_ascii;
        mutable std::unordered_map<u32, UiGlyph> m_other;
        mutable std::unordered_map<u32, UiGlyph> m_byIndex;
        std::unique_ptr<Shaper> m_shaper;
        mutable UiShapedRun m_uncached;
        mutable sizet m_shapedBytes = 0;
        mutable std::unordered_map<std::string, UiShapedRun, UiTransparentStringHash, std::equal_to<>> m_shaped;
        mutable u64 m_shapeCount = 0;
        mutable std::vector<UiGlyph*> m_demand;
        mutable sizet m_demandHead = 0;
        mutable UiFontCounts m_counts;
        std::vector<UiFontPage> m_pages;
        bool m_fullReported = false;
        mutable std::unordered_set<u32> m_reportedMissing;
    };

    // Decodes one UTF-8 codepoint and advances the cursor; malformed input yields U+FFFD.
    u32 nextCodepoint(std::string_view text, sizet& cursor);

    // The glyph for a codepoint from the first face of `base`'s chain that has it (`owner`: that face); an Absent
    // glyph when no face does.
    const UiGlyph* resolveUiGlyph(UiFonts fonts, const UiFont& base, u32 codepoint, const UiFont*& owner);

    // Pen positions for a UTF-8 run with kerning, each codepoint from the first face of `primary`'s chain that has
    // it; returns the run width in px. Codepoints no face has log once and are skipped.
    // Text with a codepoint that needs shaping lays out as layoutShapedUiLine. `clusters` (null: not wanted) gets
    // each placed glyph's cluster: the byte offset in the text of the first codepoint it renders.
    // `letterSpacing` (px) is added after each glyph's advance (kerning stays); a glyph's advance never goes below 0,
    // nor the returned width.
    float layoutUiText(UiFonts fonts, FontId primary, std::string_view text, float sizePx,
                       std::vector<UiPlacedGlyph>* placed, std::vector<u32>* clusters = nullptr,
                       float letterSpacing = 0.0f);

    // layoutUiText for text with shaped runs (UiTextBlock.cpp): the items of a text block on one line, newlines
    // skipped, in visual order; returns the pen after the last glyph, trailing spaces included.
    float layoutShapedUiLine(UiFonts fonts, FontId primary, std::string_view text, float sizePx,
                             std::vector<UiPlacedGlyph>* placed, std::vector<u32>* clusters, float letterSpacing);
} // namespace spite
