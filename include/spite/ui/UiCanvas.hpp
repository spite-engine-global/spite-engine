#pragma once

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/MathTypes.hpp"
#include "base/Platform.hpp"
#include "ui/UiExport.hpp"

namespace spite
{
    // A canvas is one element whose rects, lines, triangles and text runs a presenter emits into
    // a per-element list in the element's local space: unscaled UI pixels from its top-left corner. The view lowers
    // the list to instances only when the list's content generation changed (or the element repaints for another
    // reason), clipped to the element, in paint order between the element's own fill and its children. Colors are
    // sRGB rgba8 with red in the low byte, as in styles.

    inline constexpr u32 UI_CANVAS_DEFAULT_PRIMITIVES = 100000;
    inline constexpr u32 UI_CANVAS_MAX_PRIMITIVES = 1000000; // the hard bound a desc's maxPrimitives clamps to
    inline constexpr u32 UI_CANVAS_MAX_REGIONS = 4096;
    // The primitives all the canvases of one UI may hold together (UiContext::setCanvasBudget): an instance is 80 bytes
    // on the GPU and lowering costs about 6 ms per million, so the sum is bounded, not only each canvas.
    inline constexpr u32 UI_CANVAS_CONTEXT_BUDGET = 2000000;
    inline constexpr u32 UI_CANVAS_NO_REGION = ~0u;

    [[nodiscard]] constexpr u32 uiCanvasColor(const u8 r, const u8 g, const u8 b, const u8 a = 255)
    {
        return u32{r} | (u32{g} << 8) | (u32{b} << 16) | (u32{a} << 24);
    }

    enum class UiCanvasKind : u8
    {
        Rect, // a: position, b: size, width: corner radius
        Line, // a: from, b: to, width: thickness; round caps
        Triangle, // a, b, c: the corners
        Text, // a: position, b: size (the run sits in this box, vertically centered), width: text size (0: the
              // element's style), text: index into UiCanvasContent::texts
        Series, // a: position, b: size, width: line thickness, text: the series slot, style: lines or bars
    };

    enum class UiCanvasAlign : u8
    {
        Left,
        Center,
        Right,
    };

    // How a series primitive draws its samples: a line through them (min/max per pixel column once there are more
    // samples than columns), or bars from the value 0 (clamped into the range) to each sample (to the column's
    // extremes when decimated).
    enum class UiSeriesStyle : u8
    {
        Line,
        Bars,
    };

    // 40 bytes: a 50k-rect canvas is 2 MB.
    struct UiCanvasPrimitive
    {
        UiCanvasKind kind = UiCanvasKind::Rect;
        UiCanvasAlign align = UiCanvasAlign::Left; // Text
        UiSeriesStyle style = UiSeriesStyle::Line; // Series
        u32 color = 0;
        Vec2 a{};
        Vec2 b{};
        Vec2 c{};
        float width = 0.0f;
        u32 text = 0;
    };
    static_assert(sizeof(UiCanvasPrimitive) == 40);

    // A rectangle that answers pointer events with its id (the last one added under the pointer wins).
    struct UiCanvasRegion
    {
        u32 id = 0;
        Vec2 position{};
        Vec2 size{};
    };

    // Series buffers: large append-only sample arrays (frame times, counters) a canvas
    // owns and draws with series primitives. They live in the canvas's UI (UiContext::series) and go with the canvas;
    // the render shim keeps a GPU copy of each and uploads only the samples appended since the copy it holds, and the
    // vertex stage turns each of a series primitive's pixel columns into a segment or a min/max bar from that copy. An
    // append or a range change never lowers the canvas again. A full series drops its oldest sample per append.
    inline constexpr u32 UI_SERIES_MAX_CAPACITY = 1u << 22; // samples of one series (16 MB)
    inline constexpr u32 UI_SERIES_MAX_COLUMNS = 8192; // instances a series primitive lowers to at most
    // The samples all the series of one UI may hold together (UiContext::setSeriesBudget): 4 bytes each on the CPU
    // and again on the GPU. A create past it returns an invalid id.
    inline constexpr u64 UI_SERIES_CONTEXT_BUDGET = 64ull << 20;

    struct UiSeriesId
    {
        u32 slot = ~0u;
        u32 generation = 0;
        [[nodiscard]] bool isValid() const { return slot != ~0u; }
        bool operator==(const UiSeriesId&) const = default;
    };

    // The samples a series primitive shows, by logical index (0: the oldest held), and the values its rect spans
    // (min at the bottom, max at the top; samples outside draw at the edge). count 0 shows from `first` to the newest;
    // tail shows the newest `count` (all when 0) and follows appends.
    struct UiSeriesRange
    {
        u32 first = 0;
        u32 count = 0;
        bool tail = false;
        float min = 0.0f;
        float max = 1.0f;
        bool operator==(const UiSeriesRange&) const = default;
    };

    struct UiSeries
    {
        std::vector<float> samples; // a ring of the capacity: logical index i is at (head + i) % capacity
        u32 head = 0;
        u32 count = 0;
        u64 appended = 0; // samples appended since the last reset (the shim copies those past what it holds)
        u64 reset = 0; // grows when the samples are cleared: a copy starts over
        u64 version = 0; // grows with every change, the range's included
        UiSeriesRange range;
        u64 owner = 0; // the canvas element's entity id; 0: a free slot
        u32 generation = 0;

        [[nodiscard]] u32 capacity() const { return static_cast<u32>(samples.size()); }
        // The range resolved against what is held: the first logical index shown and how many.
        [[nodiscard]] std::pair<u32, u32> visible() const
        {
            if (range.tail)
            {
                const u32 shown = range.count == 0 ? count : std::min(range.count, count);
                return {count - shown, shown};
            }
            const u32 first = std::min(range.first, count);
            const u32 rest = count - first;
            return {first, range.count == 0 ? rest : std::min(range.count, rest)};
        }
        // The sample at logical index i (< count).
        [[nodiscard]] float at(const u32 i) const
        {
            const u32 index = head + i;
            return samples[index >= capacity() ? index - capacity() : index];
        }
    };

    // The series of one UI, in stable slots (a series primitive names its slot). Canvas creates, appends and
    // releases through it; the render shim reads it. `revision` grows with every change of any series, so a reader
    // with nothing new compares one number; revisions and series generations are unique across tables.
    class SPITE_UI_API UiSeriesTable
    {
    public:
        // A series of capacity samples (clamped to 1..UI_SERIES_MAX_CAPACITY), owned by the element `owner`. An
        // invalid id, counted in refused(), when the samples held would pass the budget.
        UiSeriesId create(u64 owner, u32 capacity);
        bool release(UiSeriesId id);
        void releaseOwner(u64 owner);
        // False, appending nothing, for a stale id or a non-finite sample.
        bool append(UiSeriesId id, std::span<const float> samples);
        bool setRange(UiSeriesId id, const UiSeriesRange& range);
        bool clear(UiSeriesId id);

        [[nodiscard]] const UiSeries* find(UiSeriesId id) const; // null: released (or never created)
        [[nodiscard]] const UiSeries* at(u32 slot) const; // null: a free slot
        [[nodiscard]] u32 slotCount() const { return static_cast<u32>(m_slots.size()); }
        [[nodiscard]] u64 revision() const { return m_revision; }
        [[nodiscard]] u64 samples() const { return m_samples; } // capacity of the live series together
        [[nodiscard]] u64 budget() const { return m_budget; }
        void setBudget(const u64 samples) { m_budget = samples; }
        [[nodiscard]] u64 refused() const { return m_refused; } // creates the budget refused
        // True for the first refusal after a release (the budget freed up and refuses again): the caller logs it.
        [[nodiscard]] bool takeRefusalNotice() { return std::exchange(m_noticePending, false); }

    private:
        UiSeries* live(UiSeriesId id);

        std::vector<UiSeries> m_slots;
        u64 m_revision = 0;
        u64 m_samples = 0;
        u64 m_budget = UI_SERIES_CONTEXT_BUDGET;
        u64 m_refused = 0;
        bool m_noticePending = false;
        bool m_noticeArmed = true;
    };

    // What a presenter emitted, in paint order. Owned by the canvas's scope; the view reads it while it repaints.
    struct UiCanvasContent
    {
        std::vector<UiCanvasPrimitive> primitives;
        std::vector<std::string> texts;
        std::vector<UiCanvasRegion> regions;
        u64 generation = 0; // bumped by every update; 0: never emitted
        u32 maxPrimitives = UI_CANVAS_DEFAULT_PRIMITIVES;
        u32 allowed = ~0u; // what the context budget leaves this canvas for the emission under way (UiCanvas.cpp)
        u32 extra = 0; // instances the series primitives lower to beyond one each: they count against the budgets
        const UiSeriesTable* series = nullptr; // the UI's: series primitives draw the canvas's own series from it

        // What the canvas charges against maxPrimitives and the context budget.
        [[nodiscard]] u64 charged() const { return primitives.size() + extra; }
    };

    // The emitting side, handed to Canvas::update and Canvas::append. Every call returns false and adds nothing once
    // the canvas holds maxPrimitives (regions: UI_CANVAS_MAX_REGIONS); the primitives that did not fit are counted
    // (UiCanvasComponent::dropped) and logged once per update.
    class SPITE_UI_API UiCanvasPainter
    {
    public:
        explicit UiCanvasPainter(UiCanvasContent& content) : m_content(&content) {}

        // Empties the list and the regions.
        void clear();
        // Room for `count` more primitives, so a large emission allocates once.
        void reserve(u32 count);
        bool rect(Vec2 position, Vec2 size, u32 color, float cornerRadius = 0.0f);
        bool line(Vec2 from, Vec2 to, float width, u32 color);
        // width-thick segments joined by round caps; false when not all fit
        bool polyline(const Vec2* points, u32 count, float width, u32 color);
        bool triangle(Vec2 a, Vec2 b, Vec2 c, u32 color);
        // One line of text in the box at `position` of `size`; textSize 0 takes the element's style size, the font
        // is the style's.
        bool text(Vec2 position, Vec2 size, std::string_view text, u32 color, UiCanvasAlign align = UiCanvasAlign::Left,
                  float textSize = 0.0f);
        bool region(u32 id, Vec2 position, Vec2 size);
        // The canvas's series in the box at `position` of `size`, over its range (Canvas::setSeriesRange); width is
        // the line thickness. One instance per pixel column of the box (at most UI_SERIES_MAX_COLUMNS); a series the
        // canvas does not own when the list lowers draws nothing, nor does one released and replaced since (the
        // primitive holds the series' generation). It counts as its column estimate (the box's width in unscaled
        // pixels, at most UI_SERIES_MAX_COLUMNS) against maxPrimitives and the context budget; at layout scale s it
        // lowers to up to s times that many instances (at most UI_SERIES_MAX_COLUMNS).
        bool series(UiSeriesId id, Vec2 position, Vec2 size, u32 color, float width = 1.0f,
                    UiSeriesStyle style = UiSeriesStyle::Line);

        [[nodiscard]] u32 primitives() const { return static_cast<u32>(m_content->primitives.size()); }
        [[nodiscard]] u32 dropped() const { return m_dropped; }
        [[nodiscard]] u32 regionsDropped() const { return m_regionsDropped; }

    private:
        bool add(const UiCanvasPrimitive& primitive, u32 extra = 0);

        UiCanvasContent* m_content;
        u32 m_dropped = 0;
        u32 m_regionsDropped = 0;
    };

    enum class UiCanvasPointerKind : u8
    {
        Enter,
        Move,
        Leave,
        Down,
        Up,
        Click,
        DragStart,
        DragMove,
        DragEnd,
        Wheel,
    };

    // A pointer event on a canvas, in the canvas's local space (unscaled UI pixels from its top-left corner, so a
    // presenter's own picking compares it with what it emitted). `delta` is the drag step in the same units;
    // `region` the id of the last region under the pointer, UI_CANVAS_NO_REGION for none.
    struct UiCanvasPointer
    {
        UiCanvasPointerKind kind = UiCanvasPointerKind::Move;
        Vec2 local{};
        Vec2 delta{};
        Vec2 wheel{};
        u32 region = UI_CANVAS_NO_REGION;
        u8 button = 0;
        u8 modifiers = 0; // UiKeyModifier bits
    };
} // namespace spite
