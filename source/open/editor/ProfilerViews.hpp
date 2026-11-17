#pragma once

#include <cmath>
#include <functional>
#include <chrono>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

#include "EditorPanelUi.hpp"
#include "ProfilerData.hpp"
#include "ProfilerSelection.hpp"
#include "control/ControlServer.hpp"
#include "editor/EditorExport.hpp"
#include "engine/CaptureStore.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    // The Profiler's strings are the table "editor-profiler" (assets/loc/<locale>/editor/profiler.json). A fixed text
    // takes its key (profilerKey: setLocalizedText, setAccessibleNameKey) and follows the locale by itself; a sentence
    // built from numbers is resolved here with its arguments when it is rebuilt, and the views rebuild on a locale
    // change (their numbers do too). Numbers go in as text the host formatted (whole, number, ms).
    [[nodiscard]] InternedString profilerKey(std::string_view key);
    [[nodiscard]] std::string profilerText(const UiScope& scope, std::string_view key,
                                           std::initializer_list<MessageArg> args = {});

    // The views under the Profiler panel's timeline (): counter tracks, the GPU lane, the
    // memory pane and the call tree. Each is a class of its own file over this interface; EditorProfilerPanes hosts
    // them behind a tab bar. A view reads the store through the panel's worker (ProfilerData::requestTask) and the
    // selection through the host, and every action it offers is a control command the host dispatches next frame:
    // what a click does and what an LLM sends are the same line.
    struct ProfilerViewHost
    {
        UiScope* scope = nullptr;
        panelui::DensityFollower* density = nullptr; // the panel's: the heights and gaps the views build with
        ControlServer* control = nullptr;
        CaptureStore* store = nullptr;
        ProfilerData* data = nullptr; // null without a store
        const std::string* locale = nullptr;
        std::string_view owner; // the control owner the panel's commands register under (unregistered with it)
        std::function<const ProfilerSelectionComponent&()> selection;
        // Queues a control line for the panel's next update, as a click on it would
        std::function<void(const nlohmann::json& line)> send;
        std::function<void()>
            sync; // brings the panel's store mirror and the worker's answers up to date (profilerState's)

        // The element's size as laid out, in unscaled UI pixels (Widget::size is the authored one: 0 for a fill item)
        [[nodiscard]] Vec2 laidOutSize(const Widget& widget) const;
        [[nodiscard]] std::string text(const std::string_view key,
                                       const std::initializer_list<MessageArg> args = {}) const
        {
            return profilerText(*scope, key, args);
        }
        [[nodiscard]] std::string number(double value) const;
        // up to three fraction digits, the locale's separators
        [[nodiscard]] std::string precise(double value) const;
        [[nodiscard]] std::string whole(u64 value) const;
        // 1536 -> "1.5 KB": binary units, the locale's separators
        [[nodiscard]] std::string bytes(u64 value) const;
        // ns -> "1.25 ms"
        [[nodiscard]] std::string ms(double ns) const;
        // A cell with no value
        [[nodiscard]] std::string none() const { return text("table.none"); }
        // A sortable column header's accessible name: "Self (ms), sorted descending" (order: none, ascending,
        // descending)
        [[nodiscard]] std::string sortName(std::string_view title, std::string_view order) const
        {
            return text(order == "none"            ? "table.sort"
                            : order == "ascending" ? "table.sorted.ascending"
                                                   : "table.sorted.descending",
                        {{"title", std::string(title)}});
        }
    };

    class ProfilerView
    {
    public:
        virtual ~ProfilerView() = default;
        [[nodiscard]] virtual Widget root() const = 0;
        // The view's pane became the shown one (or stopped being): a hidden view asks the worker for nothing
        virtual void activate(bool active) = 0;
        // The selected capture changed (0: none, or no longer ready); info is valid when capture != 0
        virtual void captureChanged(CaptureId capture, const CaptureInfo& info) = 0;
        // The frame range or the zone changed
        virtual void selectionChanged(const ProfilerSelectionComponent& selection) = 0;
        // A Task event of this view (the host routes by event.view); true when taken
        virtual bool take(ProfilerEvent& event) = 0;
        // Once per panel update while active: pending work that is not a reaction to an event
        virtual void update() = 0;
        virtual void writeState(IFormatWriter& out) = 0;
        virtual void registerCommands() = 0;
        // True while the view waits for the worker
        [[nodiscard]] virtual bool busy() const = 0;
        // The height the panes give the view while it is shown
        [[nodiscard]] virtual float preferredHeight() const { return 320.0f; }
    };

    enum class ProfilerViewId : u8
    {
        Counters,
        Gpu,
        Memory,
        Calls,
        Compare,
    };
    inline constexpr u32 PROFILER_VIEW_COUNT = 5;

    std::unique_ptr<ProfilerView> makeCounterView(const ProfilerViewHost& host);
    std::unique_ptr<ProfilerView> makeGpuView(const ProfilerViewHost& host);
    std::unique_ptr<ProfilerView> makeMemoryView(const ProfilerViewHost& host);
    std::unique_ptr<ProfilerView> makeCallsView(const ProfilerViewHost& host);
    std::unique_ptr<ProfilerView> makeCompareView(const ProfilerViewHost& host);

    // A store call on the worker: waits out a capture another reader holds ("busy"), for about a second, and gives up
    // with "superseded" once cancelled() is true. Returns the call's error text (empty on success).
    template <class Call>
    std::string profilerStoreCall(const CaptureCancelFn& cancelled, Call&& call)
    {
        for (u32 attempt = 0;; ++attempt)
        {
            if (cancelled())
            {
                return "superseded";
            }
            std::string error = call();
            if (!error.starts_with("busy") || attempt >= PROFILER_MAX_RETRIES)
            {
                return error;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // A store call on the frame thread (a control command): the store already waits its lock wait for a capture the
    // worker holds, so a second and third try cover a step that began just before; no sleeping here.
    template <class Call>
    std::string profilerFrameStoreCall(Call&& call)
    {
        std::string error;
        for (u32 attempt = 0; attempt < 3; ++attempt)
        {
            error = call();
            if (!error.starts_with("busy"))
            {
                break;
            }
        }
        return error;
    }

    // A zone, counter or tag name is untrusted text: cut at a code point boundary, control characters replaced
    [[nodiscard]] std::string profilerSafeText(std::string_view text, u32 limit = 96);
    // A stable color per name (pass and zone names)
    [[nodiscard]] u32 profilerNameColor(std::string_view name);

    // "2026-10-09 11:47:03" in the machine's time zone
    [[nodiscard]] std::string profilerLocalTime(std::int64_t unixMs);

    // A decimal number with more than three fraction digits (4.3978434, 61.376997700000004) cut to two: a PerfCli
    // warning carries its raw numbers
    [[nodiscard]] SPITE_EDITOR_API std::string profilerRoundedNumbers(std::string_view text);
} // namespace spite
