#include "application/FrameClock.hpp"

#include "base/CrashCapture.hpp"

#include <SDL3/SDL_timer.h>

#include <algorithm>

namespace spite
{
    double SdlMonotonicClock::nowSeconds() const
    {
        return static_cast<double>(SDL_GetPerformanceCounter()) / static_cast<double>(SDL_GetPerformanceFrequency());
    }

    double ManualMonotonicClock::nowSeconds() const { return m_seconds; }

    void ManualMonotonicClock::setSeconds(const double seconds) { m_seconds = std::max(0.0, seconds); }

    void ManualMonotonicClock::advance(const double seconds) { m_seconds = std::max(0.0, m_seconds + seconds); }

    FrameClock::FrameClock(std::unique_ptr<IMonotonicClock> clock, const float maximumDeltaTime) :
        m_clock(std::move(clock))
    {
        setMaximumDeltaTime(maximumDeltaTime);
    }

    void FrameClock::initialize()
    {
        m_lastSampleSeconds = nowSeconds();
        m_timing = {};
        m_unscaledDeltaHistory.clear();
        m_timeAtStatisticsReset = 0.0f;
        m_initialized = true;
    }

    const FrameTiming& FrameClock::tick()
    {
        if (!m_initialized)
        {
            initialize();
            return m_timing;
        }

        const double currentSeconds = nowSeconds();
        const float rawDeltaTime = static_cast<float>(std::max(0.0, currentSeconds - m_lastSampleSeconds));
        m_lastSampleSeconds = currentSeconds;

        m_timing.frameIndex++;
        spite::g_crashContext.frame.store(m_timing.frameIndex, std::memory_order_relaxed);
        m_timing.unscaledDeltaTime = std::min(rawDeltaTime, m_maximumDeltaTime);
        m_timing.deltaTime = m_paused ? 0.0f : m_timing.unscaledDeltaTime * m_timeScale;
        m_timing.elapsedTime += m_timing.deltaTime;

        m_unscaledDeltaHistory.push_back(m_timing.unscaledDeltaTime);
        if (m_unscaledDeltaHistory.size() > HISTORY_SIZE)
        {
            m_unscaledDeltaHistory.pop_front();
        }

        m_timing.averageDeltaTime = getAverageDeltaTime();
        m_timing.minimumDeltaTime = getMinimumDeltaTime();
        m_timing.maximumDeltaTime = getMaximumDeltaTime();
        return m_timing;
    }

    const FrameTiming& FrameClock::getTiming() const { return m_timing; }

    void FrameClock::setPaused(const bool paused)
    {
        m_paused = paused;
        m_lastSampleSeconds = nowSeconds();
    }

    bool FrameClock::isPaused() const { return m_paused; }

    void FrameClock::setTimeScale(const float scale) { m_timeScale = std::max(0.0f, scale); }

    float FrameClock::getTimeScale() const { return m_timeScale; }

    void FrameClock::setMaximumDeltaTime(const float maximumDeltaTime)
    {
        m_maximumDeltaTime = std::max(0.0f, maximumDeltaTime);
    }

    void FrameClock::resetStatistics()
    {
        m_unscaledDeltaHistory.clear();
        m_timeAtStatisticsReset = m_timing.elapsedTime;
        m_timing.averageDeltaTime = 0.0f;
        m_timing.minimumDeltaTime = 0.0f;
        m_timing.maximumDeltaTime = 0.0f;
    }

    float FrameClock::getAverageDeltaTime(const u32 frameCount) const
    {
        const sizet count = std::min<sizet>(frameCount, m_unscaledDeltaHistory.size());
        if (count == 0)
            return 0.0f;
        float sum = 0.0f;
        for (sizet i = 0; i < count; ++i)
            sum += m_unscaledDeltaHistory[m_unscaledDeltaHistory.size() - 1 - i];
        return sum / static_cast<float>(count);
    }

    float FrameClock::getMinimumDeltaTime(const u32 frameCount) const
    {
        const sizet count = std::min<sizet>(frameCount, m_unscaledDeltaHistory.size());
        if (count == 0)
            return 0.0f;
        float result = m_unscaledDeltaHistory.back();
        for (sizet i = 1; i < count; ++i)
            result = std::min(result, m_unscaledDeltaHistory[m_unscaledDeltaHistory.size() - 1 - i]);
        return result;
    }

    float FrameClock::getMaximumDeltaTime(const u32 frameCount) const
    {
        const sizet count = std::min<sizet>(frameCount, m_unscaledDeltaHistory.size());
        if (count == 0)
            return 0.0f;
        float result = m_unscaledDeltaHistory.back();
        for (sizet i = 1; i < count; ++i)
            result = std::max(result, m_unscaledDeltaHistory[m_unscaledDeltaHistory.size() - 1 - i]);
        return result;
    }

    float FrameClock::getTimeSinceStatisticsReset() const { return m_timing.elapsedTime - m_timeAtStatisticsReset; }

    double FrameClock::nowSeconds() const { return m_clock->nowSeconds(); }
} // namespace spite
