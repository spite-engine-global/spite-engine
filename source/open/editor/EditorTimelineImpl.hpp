#pragma once

// The timeline's state, shared by EditorTimeline.cpp (data, commands, input) and EditorTimelineDraw.cpp (what the
// canvases show). Not a public header.

#include <array>
#include <atomic>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "EditorPanelUi.hpp"
#include "EditorStatusText.hpp"
#include "EditorTimeline.hpp"
#include "ProfilerData.hpp"
#include "ProfilerSelection.hpp"
#include "ProfilerView.hpp"
#include "control/ControlServer.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace timeline
    {
        // Layout, in UI pixels (the canvas's local space)
        inline constexpr float GUTTER = 104.0f; // lane names
        inline constexpr float RULER = 26.0f;
        inline constexpr float LANE_HEADER = 16.0f;
        inline constexpr float ROW = 14.0f;
        inline constexpr float LANE_GAP = 4.0f;
        inline constexpr u32 MAX_ROWS = 12; // depths drawn (and read) per lane
        inline constexpr float LABEL_MIN_WIDTH = 56.0f;
        inline constexpr u32 MAX_LABELS = 300;
        inline constexpr u32 MAX_LANES = 256; // lanes the timeline lays out; the store allows 4096
        inline constexpr u32 MAX_NAMES = 8192; // distinct zone names kept; later ones draw as "..."
        inline constexpr u32 MAX_NAME_BYTES = 128;
        inline constexpr u32 QUIET_FRAMES = 4; // frames the window rests before a new fetch
        inline constexpr u32 HOVER_FRAMES = 3; // frames the pointer rests on a zone before its detail is asked for
        inline constexpr u32 MIN_COLUMNS = 64;
        inline constexpr u32 TIP_NAME_CHARS = 96;
        inline constexpr u32 HISTOGRAM_BINS = 24;
        inline constexpr u32 FIND_MAX_NAME = 256;
        inline constexpr u32 MAX_MATCHES = 5000; // zones the match count reads at most
        inline constexpr float RULER_LABEL_GAP = 6.0f; // the least space between two ruler labels
        // Zoom per wheel notch or key press, 5/4; at most this many notches per event
        inline constexpr u32 ZOOM_NUM = 5, ZOOM_DEN = 4, MAX_NOTCHES = 8;
    } // namespace timeline

    struct TlSpan
    {
        std::int64_t start = 0, end = 0;
        u64 count = 0, firstSeq = 0, longestSeq = 0, totalNs = 0, longestNs = 0;
        u32 depth = 0;
        u32 name = 0; // index into Impl::names
    };

    struct TlLane
    {
        CaptureThread info;
        std::vector<TlSpan> spans; // by (depth, start)
        std::vector<u32> depthBegin; // spans of depth d are [depthBegin[d], depthBegin[d + 1])
        bool loaded = false;
        bool cut = false;
        std::int64_t from = 0, to = 0; // the window the spans were read for
        u32 columns = 0;
        u32 rows = 0; // depth rows drawn
        float top = 0.0f, height = 0.0f; // in the lane column, scroll not applied
    };

    // A name's palette slot; OVERFLOW_NAME is the "..." the names past MAX_NAMES share.
    inline constexpr u8 TL_OVERFLOW_NAME = 10;

    struct TlName
    {
        std::string text;
        u8 slot = 0;
    };

    // What the canvases paint with: the theme's canvas.* colors, read again on a theme change.
    struct TlColors
    {
        u32 header = 0, body = 0, grid = 0, text = 0, dimText = 0, narrow = 0, merged = 0;
        u32 frameA = 0, frameB = 0, outline = 0, selected = 0, crosshair = 0, tipFill = 0, onSpan = 0, bar = 0;
        std::array<u32, TL_OVERFLOW_NAME + 1> names{}; // the palette, then the overflow name's

        [[nodiscard]] static TlColors read(const UiScope& scope);
    };

    struct TlMatchKey
    {
        std::int64_t start = 0;
        u32 lane = 0;
        u64 index = 0;
        auto operator<=>(const TlMatchKey&) const = default;
    };

    struct TlMark
    {
        std::int64_t start = 0, end = 0;
        u64 index = 0;
    };

    // A text the canvas drew, in the canvas's pixels: what the ruler and the crosshair say, for the query and the tests
    struct TlLabel
    {
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
        std::string text;
    };

    // The frames of the capture the timeline's window shows, as the frame graph numbers them
    struct TlFrames
    {
        bool has = false;
        u64 first = 0, last = 0;
        bool approximate = false; // last was counted from the mean frame length (the marks were cut)
        bool operator==(const TlFrames&) const = default;
    };

    struct TlHover
    {
        bool has = false; // the pointer is over the plot
        bool onSpan = false;
        u32 lane = 0;
        u32 depth = 0;
        TlSpan span;
        std::int64_t ns = 0; // the time under the pointer
        Vec2 point{};
        u32 stable = 0;
        bool wantDetail = false;
    };

    struct TlDetail
    {
        bool valid = false;
        CaptureZoneDetail detail;
    };

    struct ProfilerTimeline::Impl
    {
        using Json = panelui::Json;

        Impl(const Host& host, const Widget& parent);

        // -- host and widgets -----------------------------------------------------------------------------------
        Host host;
        Canvas canvas;
        Canvas overlay;
        Canvas histogram;
        Panel bar;
        TextInput findBox;
        Button prevButton, nextButton, fitButton, zoneButton;
        panelui::StatusTips statusTips;
        panelui::StatusText info;
        std::array<panelui::StatusText, 3> statLines;
        Tooltip tip;
        std::string locale = "en";

        // -- the capture ---------------------------------------------------------------------------------------
        CaptureId capture = 0;
        std::int64_t durationNs = 0;
        std::vector<TlLane> lanes;
        std::vector<TlName> names;
        struct NameHash
        {
            using is_transparent = void;
            sizet operator()(std::string_view text) const { return std::hash<std::string_view>{}(text); }
        };
        std::unordered_map<std::string, u32, NameHash, std::equal_to<>> nameIds;
        std::vector<TlMark> marks; // frames [start, end) in the fetched window
        std::int64_t marksFrom = 0, marksTo = 0; // the window the marks were read for
        bool marksTruncated = false;
        float contentHeight = 0.0f;
        float maxScroll = 0.0f;

        // -- fetching --------------------------------------------------------------------------------------------
        u64 nextToken = 0;
        std::array<u64, static_cast<sizet>(ProfilerSlot::Count)> tokens{};
        bool viewInFlight = false;
        std::int64_t fetchFrom = 0, fetchTo = 0; // the window of the request in flight (or the last)
        u32 fetchColumns = 0;
        std::string viewError;
        u32 quiet = 0; // frames since the window last changed
        bool fetchDue = false;

        // -- zoom, hover, selection, search --------------------------------------------------------------------
        TlHover hover;
        TlDetail hoverDetail; // the detail of the zone under the pointer (valid for hover.span.firstSeq)
        TlDetail selDetail; // the selected zone's
        bool selDetailAsked = false; // a detail is wanted (or here) for askedLane / askedIndex
        u32 askedLane = 0;
        u64 askedIndex = 0;
        bool selStatsValid = false;
        CaptureZoneStats selStats;
        std::string selStatsName;
        std::string statsError;
        std::string tooltip; // the text the tooltip shows (rich markup)
        struct Pending
        {
            bool zoom = false;
            u32 lane = 0;
            u64 seq = 0;
        } pending;
        struct Find
        {
            std::string name;
            std::string direction;
            std::string state = "idle"; // idle, searching, found, notFound, error
            std::string error;
            bool found = false;
            CaptureZone zone;
            u64 searches = 0;
            bool zoomOnFound = true;
            bool cursorValid =
                false; // the zone found is still the selection (or about to be): next/previous go from it
        } findState;
        // The zones whose name contains the last search text, by (start, lane, index) as a search walks them
        using MatchKey = TlMatchKey;
        struct Matches
        {
            std::string name;
            bool ready = false;
            bool truncated = false; // more than MAX_MATCHES: the count is a lower bound and no position is given
            std::string error;
            std::vector<MatchKey> keys;
        } matches;
        std::shared_ptr<std::atomic<u64>> matchGeneration = std::make_shared<std::atomic<u64>>(0);
        u64 matchValue = 0;
        struct Drag
        {
            bool active = false;
            profview::Window window;
            float scroll = 0.0f;
            Vec2 total{};
        } drag;
        bool dragged = false;
        bool hoverStale = false; // the window moved under a resting pointer: the zone under it is read again
        // What the gestures of this frame want: sent as one profilerView by update(), dropped the update after (the
        // command has run by then); further gestures build on it meanwhile.
        struct Wanted
        {
            bool has = false;
            bool windowChanged = false;
            bool sent = false;
            profview::Window window;
            std::optional<float> scroll;
        } wanted;

        // -- drawing -----------------------------------------------------------------------------------------------
        TlColors colors;
        bool shown = false;
        bool dirtyMain = true;
        bool dirtyOverlay = true;
        bool dirtyStats = true;
        bool dirtyInfo = true;
        bool dirtyHistogram = true;
        bool dirtyFrames = true;
        Vec2 canvasSize{}, histogramSize{};
        u32 histogramBars = 0; // bars the zone histogram drew
        std::vector<TlLabel> rulerLabels; // the tick labels of the last ruler, left to right
        std::vector<std::string> emptyLines; // the lines of the last empty-canvas note (no capture, error, loading)
        TlLabel hoverLabel; // the crosshair's time, empty text when none
        TlFrames shownFrames; // what the graph was last told
        std::string lastInfo;
        u64 redraws = 0, overlayRedraws = 0, fetches = 0, viewDones = 0, spanBatches = 0, droppedEvents = 0;
        u64 viewChanges = 0, details = 0, searchesDone = 0;
        u32 lastPrimitives = 0, maxPrimitives = 0, lastDropped = 0, lastSpansDrawn = 0, lastSpansVisited = 0;
        u32 lastLanesDrawn = 0, lastLabels = 0;
        u64 seenLayout = 0; // the layout generation the label metrics were last read at
        float probeWidth = -1.0f; // what a probe text measured then: a change redraws the labels
        panelui::ThemeObserver theme; // last: reads the members above when it fires

        // -- state -------------------------------------------------------------------------------------------------
        [[nodiscard]] ProfilerViewComponent& viewComponent() const
        {
            return host.scope->world().getComponent<ProfilerViewComponent>(host.root);
        }
        [[nodiscard]] const ProfilerViewComponent& viewConst() const // reads must not mark the chunk modified
        {
            return host.scope->world().getConstComponent<ProfilerViewComponent>(host.root);
        }
        [[nodiscard]] const ProfilerSelectionComponent& selection() const
        {
            return host.scope->world().getConstComponent<ProfilerSelectionComponent>(host.root);
        }
        [[nodiscard]] profview::Window window() const;
        [[nodiscard]] profview::Window gestureWindow() const { return wanted.has ? wanted.window : window(); }
        [[nodiscard]] float gestureScroll() const;
        [[nodiscard]] double plotWidth() const
        {
            return std::max(1.0, static_cast<double>(canvasSize.x - timeline::GUTTER));
        }
        [[nodiscard]] u64 newToken(ProfilerSlot slot) { return tokens[static_cast<sizet>(slot)] = ++nextToken; }
        [[nodiscard]] u64 token(ProfilerSlot slot) const { return tokens[static_cast<sizet>(slot)]; }

        // view (EditorTimeline.cpp)
        bool applyView(const profview::Window& next, std::optional<float> scroll = std::nullopt);
        void want(const profview::Window& next, std::optional<float> scroll = std::nullopt); // a gesture
        void sendWanted();
        void setScroll(float scroll);
        void zoomAt(double x, std::int32_t notches);
        void zoomToZone(const CaptureZone& zone);
        void layoutLanes();
        void requestZoneZoom(u32 lane, u64 seq);
        std::string viewCommand(const ControlArgs& args, IFormatWriter& out);
        std::string findCommand(const ControlArgs& args, IFormatWriter& out);
        void startFind(const std::string& name, const std::string& direction);
        void writeView(IFormatWriter& out) const;
        void requestMatches(const std::string& name);
        [[nodiscard]] std::string frameBounds(u64 first, u64 last, std::int64_t& start, std::int64_t& end) const;
        // The frames the window shows, when the marks cover it (else none)
        [[nodiscard]] TlFrames visibleFrames() const;
        void pushFrames();
        [[nodiscard]] std::string matchCount() const;

        // data (EditorTimeline.cpp)
        void clearCapture();
        void fetchIfDue();
        [[nodiscard]] bool laneNeeds(const TlLane& lane, const profview::Window& w) const;
        [[nodiscard]] bool laneVisible(const TlLane& lane, float margin) const;
        void applySpans(ProfilerEvent& event);
        u32 internName(std::string_view name);
        void requestSelectionDetail();
        void clearSelectionData();

        // input (EditorTimeline.cpp)
        void onPointer(const UiCanvasPointer& event);
        bool onKey(u32 scancode, u8 modifiers);
        void updateHover(Vec2 local);
        void clearHover();
        void click();
        [[nodiscard]] const TlLane* laneAt(float y, float& laneY) const;

        // drawing (EditorTimelineDraw.cpp)
        void restyle(); // the theme changed: read its colors, repaint
        void drawMain();
        void drawOverlay();
        void drawHistogram();
        void showStats();
        void showInfo();
        void buildTooltip();
        // The width a canvas label of `text` paints with in `element`'s font and letter spacing; the glyph upper bound
        // while there is no measurer
        [[nodiscard]] float textWidth(Entity element, std::string_view text) const;
        // `text` cut at a character boundary to the longest prefix of at most `room` px
        [[nodiscard]] std::string fitText(Entity element, std::string_view text, float room) const;
        // `text` broken at spaces into lines of at most `room` px, at most `maxLines` of them (the last ends in "..."
        // when words are left; a word wider than `room` is cut at a character)
        [[nodiscard]] std::vector<std::string> wrapText(Entity element, std::string_view text, float room,
                                                        u32 maxLines) const;
        // Redraws the labels when a layout change moved the width of text (a letter spacing, a font)
        void followTextMetrics();
        // a message of the profiler's table (ProfilerViews.hpp profilerText)
        [[nodiscard]] std::string say(std::string_view key, std::initializer_list<MessageArg> args = {}) const;
        [[nodiscard]] std::string fmtNs(std::int64_t ns) const;
        [[nodiscard]] std::string whole(u64 value) const;
        [[nodiscard]] std::string ms3(double ns) const;

        void writeStateImpl(IFormatWriter& out) const;
    };
} // namespace spite
