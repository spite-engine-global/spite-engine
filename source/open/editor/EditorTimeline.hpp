#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/Platform.hpp"
#include "ecs/EcsTypes.hpp"
#include "EditorPanelUi.hpp"
#include "engine/CaptureStore.hpp"

namespace spite
{
    class ControlArgs;
    class ControlServer;
    class Engine;
    class IFormatWriter;
    class UiScope;
    class Widget;
    struct ProfilerEvent;
    class ProfilerData;

    // The Profiler's zone timeline (): a lane per thread of the selected capture with its
    // zones nested by depth, a time ruler with the frame boundaries, zoom around the pointer, pan by drag or keys, a
    // tooltip per zone, click to select with the zone's statistics across the capture, and find-by-name.
    //
    // Everything is a control command or readable state, as for the rest of the panel: the window and the lane
    // scroll are the ProfilerView component on the panel's root element, written only by the profilerView command
    // (wheel, drag and keys collect the window they want and send one absolute profilerView per frame, applied one
    // frame later), the zone selection is ProfilerSelection's
    // (profilerSelect), a search is profilerFind, and profilerState's `timeline` object holds the lanes, the load, the
    // redraw counters, the hover with the tooltip text and the selected zone's statistics.
    //
    // Cost: the store is read only by the panel's worker (ProfilerData). A view asks for each visible lane's spans
    // (zones at least one pixel wide as they are, narrower ones merged per depth and pixel column by the store), a
    // little beyond the window so a pan needs none; a new fetch waits for the window to rest for a few frames.
    // Drawing walks, per lane and depth, only the spans inside the window (binary search) and merges what is under a
    // pixel into runs, so the primitives follow the pixels, not the zone count. A hidden timeline, or one whose
    // inputs did not change, does nothing per frame beyond comparing a few numbers.
    class ProfilerTimeline
    {
    public:
        // The panel's skeleton keeps the canvas at least this tall, and the find bar and zone statistics (fixed) take
        // chromeHeight more: the bar, and three statistics rows with their gaps
        static constexpr float MIN_HEIGHT = 220.0f;
        [[nodiscard]] static float chromeHeight(const panelui::Density& density)
        {
            return panelui::HEIGHT_BUTTON.of(density) + 3.0f * density.row + 2.0f * density.xs;
        }

        struct Host
        {
            Engine* engine = nullptr;
            ControlServer* control = nullptr;
            UiScope* scope = nullptr;
            panelui::DensityFollower* density = nullptr; // the panel's: the heights and gaps the timeline builds with
            CaptureStore* store = nullptr;
            ProfilerData* data = nullptr;
            Entity root = Entity::undefined(); // holds ProfilerSelection and ProfilerView
            std::string_view owner; // the control entries' owner
            std::function<void(std::string)> send; // queues a control line, as a button press does
            std::function<void(std::string)> sendView; // like send, for the profilerView of a gesture (else send)
            std::function<void()> leave; // Escape in the find box: where the focus goes (empty: it drops)
            // The frames (first, last) the window shows once they are known, or none (the whole capture, or not
            // known): the frame graph marks them; approximate says the last one is an estimate. Called when they
            // change.
            std::function<void(std::optional<std::pair<u64, u64>>, bool approximate)> showFrames;
        };

        // The id this timeline's worker task events carry (the panes' are ProfilerViewId's)
        static constexpr u32 TASK_VIEW = 100;

        ProfilerTimeline(const Host& host, const Widget& parent);
        ~ProfilerTimeline();
        ProfilerTimeline(const ProfilerTimeline&) = delete;
        ProfilerTimeline& operator=(const ProfilerTimeline&) = delete;

        // The selected capture changed (0: none): everything of the old one goes.
        void reset(CaptureId capture);
        // The capture's lanes arrived (Lanes event).
        void onLanes(CaptureId capture, std::vector<CaptureThread> lanes, std::int64_t durationNs);
        // A Marks, Spans, ViewDone, Detail, Stats, Find or the timeline's own Task event; false for any other kind.
        bool onEvent(ProfilerEvent& event);
        // ProfilerSelection changed.
        void onSelection();
        void setLocale(std::string_view locale);
        // The token of the view request in flight (or the last).
        [[nodiscard]] u64 viewToken() const;
        // The token of the match count request in flight (or the last), and a test's answer to one.
        [[nodiscard]] u64 matchToken() const;
        void injectMatches(CaptureId capture, u64 token, const std::string& name, u32 count);
        // Every frame (the panel calls it shown or not; a hidden timeline only notes it).
        void update(bool shown);

        // The profilerView and profilerFind commands.
        std::string view(const ControlArgs& args, IFormatWriter& out);
        std::string find(const ControlArgs& args, IFormatWriter& out);
        void writeState(IFormatWriter& out) const;

        // The timeline canvas, for tests and the panel's layout.
        [[nodiscard]] Widget canvas() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace spite
