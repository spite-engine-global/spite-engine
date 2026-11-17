#include "EditorFeedback.hpp"

#include <algorithm>

#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"

namespace spite
{
    u64 EditorFeedback::postToast(const ToastLevel level, std::string text, double seconds)
    {
        if (seconds <= 0.0)
        {
            seconds = level == ToastLevel::Error ? ERROR_SECONDS : INFO_SECONDS;
        }
        if (m_toasts.size() == LOG_CAPACITY)
        {
            m_toasts.pop_front();
        }
        const u64 id = m_nextId++;
        m_toasts.push_back({id, level, std::move(text), m_now, m_now + seconds, false});
        return id;
    }

    u64 EditorFeedback::postToastKey(const ToastLevel level, const std::string_view key,
                                     const std::span<const MessageArg> args, const double seconds)
    {
        std::string text(key);
        if (m_localization)
        {
            std::string scratch;
            text = std::string(m_localization->resolve(InternedString(key), args, scratch));
        }
        return postToast(level, std::move(text), seconds);
    }

    bool EditorFeedback::dismissToast(const u64 id)
    {
        const auto it = std::ranges::find(m_toasts, id, &ToastRecord::id);
        if (it == m_toasts.end() || it->dismissed)
        {
            return false;
        }
        it->dismissed = true;
        return true;
    }

    u32 EditorFeedback::dismissVisible()
    {
        std::vector<u64> shown;
        visibleToasts(shown);
        for (const u64 id : shown)
        {
            dismissToast(id);
        }
        return static_cast<u32>(shown.size());
    }

    u32 EditorFeedback::clearToasts()
    {
        const u32 cleared = static_cast<u32>(m_toasts.size());
        m_toasts.clear();
        return cleared;
    }

    void EditorFeedback::writeLog(IFormatWriter& out) const
    {
        std::vector<u64> shown;
        visibleToasts(shown);
        out.writeU32("visible", static_cast<u32>(shown.size()));
        out.beginArray("log", static_cast<u32>(m_toasts.size()));
        for (const ToastRecord& toast : m_toasts)
        {
            out.beginObject("");
            out.writeU64("id", toast.id);
            out.writeString("level", TOAST_LEVEL_NAMES[static_cast<u8>(toast.level)]);
            out.writeString("text", toast.text);
            out.writeF64("ageSeconds", m_now - toast.postedAt);
            out.writeBool("visible", std::ranges::find(shown, toast.id) != shown.end());
            out.writeBool("dismissed", toast.dismissed);
            out.endObject();
        }
        out.endArray();
    }

    void EditorFeedback::visibleToasts(std::vector<u64>& out) const
    {
        out.clear();
        for (auto it = m_toasts.rbegin(); it != m_toasts.rend() && out.size() < MAX_VISIBLE; ++it)
        {
            if (!it->dismissed && m_now < it->expiresAt)
            {
                out.push_back(it->id);
            }
        }
        std::ranges::reverse(out);
    }

    bool EditorFeedback::anyVisible() const
    {
        return std::ranges::any_of(m_toasts, [this](const ToastRecord& toast)
                                   { return !toast.dismissed && m_now < toast.expiresAt; });
    }

    const ToastRecord* EditorFeedback::toast(const u64 id) const
    {
        const auto it = std::ranges::find(m_toasts, id, &ToastRecord::id);
        return it == m_toasts.end() ? nullptr : &*it;
    }

    void EditorFeedback::reportJobs(const std::string_view source, const u32 count)
    {
        const auto it = std::ranges::find(m_jobs, source, &std::pair<std::string, u32>::first);
        if (it != m_jobs.end())
        {
            it->second = count;
        }
        else if (count > 0)
        {
            m_jobs.emplace_back(std::string(source), count);
        }
    }

    u32 EditorFeedback::jobCount() const
    {
        u32 total = 0;
        for (const auto& [source, count] : m_jobs)
        {
            total += count;
        }
        return total;
    }
} // namespace spite
