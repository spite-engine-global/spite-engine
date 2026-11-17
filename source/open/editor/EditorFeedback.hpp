#pragma once

#include <array>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/Platform.hpp"
#include "editor/EditorExport.hpp"

namespace spite
{
    class IFormatWriter;
    class Localization;
    struct MessageArg;

    enum class ToastLevel : u8
    {
        Info,
        Warn,
        Error
    };
    inline constexpr std::array<std::string_view, 3> TOAST_LEVEL_NAMES{"info", "warn", "error"};

    struct ToastRecord
    {
        u64 id = 0;
        ToastLevel level = ToastLevel::Info;
        std::string text;
        double postedAt = 0.0; // seconds on the feedback's clock
        double expiresAt = 0.0;
        bool dismissed = false; // by a click, Esc or the dismissToast command
    };

    // What the editor's panels report to the person: toasts and running jobs. The shell owns it and hands
    // it to a panel through EditorPanelContext::feedback; the status bar shows it and the statusBar query returns it,
    // so an LLM and a scenario see what the person sees. The editor model registers its commands:
    // postToast, dismissToast, clearToasts and the toasts query. Main thread only. Its clock is the sum of the frame
    // times advance() is given, not the wall clock: a toast lives as long as frames say, and a test controls it.
    class SPITE_EDITOR_API EditorFeedback
    {
    public:
        static constexpr u32 LOG_CAPACITY = 50; // toasts the statusBar query remembers
        static constexpr u32 MAX_VISIBLE = 4;
        static constexpr double INFO_SECONDS = 4.0;
        static constexpr double ERROR_SECONDS = 8.0;

        explicit EditorFeedback(const Localization* localization) : m_localization(localization) {}

        // Shows a toast of already-localized text for `seconds` (<= 0: 4 s, an error 8 s); returns its id.
        u64 postToast(ToastLevel level, std::string text, double seconds = 0.0);
        // The same for a localization key (a table "editor-<panel>.<name>") resolved now with `args`; the key itself
        // when the table lacks it.
        u64 postToastKey(ToastLevel level, std::string_view key, std::span<const MessageArg> args = {},
                         double seconds = 0.0);

        bool dismissToast(u64 id);
        // Dismisses every toast on screen; how many.
        u32 dismissVisible();

        // Forgets every toast, dismissed or not (ids are not reused); how many.
        u32 clearToasts();

        // The log as the statusBar and toasts queries write it: `visible` (the count on screen) and `log` (id, level,
        // text, ageSeconds, visible, dismissed; oldest first).
        void writeLog(IFormatWriter& out) const;

        // Ids of the toasts on screen, oldest first: the newest MAX_VISIBLE that are neither dismissed nor expired.
        void visibleToasts(std::vector<u64>& out) const;
        [[nodiscard]] bool anyVisible() const;
        [[nodiscard]] const ToastRecord* toast(u64 id) const;
        // oldest first, at most LOG_CAPACITY
        [[nodiscard]] const std::deque<ToastRecord>& toasts() const { return m_toasts; }

        // The jobs a source (a panel: "modules", "project", "profiler") has running; 0 clears it.
        void reportJobs(std::string_view source, u32 count);
        [[nodiscard]] u32 jobCount() const;
        [[nodiscard]] const std::vector<std::pair<std::string, u32>>& jobSources() const { return m_jobs; }

        // Once per frame, by the status bar.
        void advance(const double seconds) { m_now += seconds; }
        [[nodiscard]] double now() const { return m_now; }

    private:
        const Localization* m_localization;
        std::deque<ToastRecord> m_toasts;
        std::vector<std::pair<std::string, u32>> m_jobs;
        double m_now = 0.0;
        u64 m_nextId = 1;
    };
} // namespace spite
