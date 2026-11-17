#pragma once

#include <deque>
#include <memory>

#include "application/ApplicationExport.hpp"
#include "application/ApplicationTypes.hpp"
#include "base/Platform.hpp"

namespace spite
{
    class SPITE_APPLICATION_API IMonotonicClock
    {
    public:
        virtual ~IMonotonicClock() = default;
        [[nodiscard]] virtual double nowSeconds() const = 0;
    };

    class SPITE_APPLICATION_API SdlMonotonicClock final : public IMonotonicClock
    {
    public:
        [[nodiscard]] double nowSeconds() const override;
    };

    class SPITE_APPLICATION_API ManualMonotonicClock final : public IMonotonicClock
    {
    public:
        [[nodiscard]] double nowSeconds() const override;
        void setSeconds(double seconds);
        void advance(double seconds);

    private:
        double m_seconds = 0.0;
    };

    class SPITE_APPLICATION_API FrameClock
    {
    public:
        explicit FrameClock(std::unique_ptr<IMonotonicClock> clock, float maximumDeltaTime = 1.0f);

        void initialize();
        [[nodiscard]] const FrameTiming& tick();
        [[nodiscard]] const FrameTiming& getTiming() const;
        void setPaused(bool paused);
        [[nodiscard]] bool isPaused() const;
        void setTimeScale(float scale);
        [[nodiscard]] float getTimeScale() const;
        void setMaximumDeltaTime(float maximumDeltaTime);
        void resetStatistics();
        [[nodiscard]] float getAverageDeltaTime(u32 frameCount = 10) const;
        [[nodiscard]] float getMinimumDeltaTime(u32 frameCount = 10) const;
        [[nodiscard]] float getMaximumDeltaTime(u32 frameCount = 10) const;
        [[nodiscard]] float getTimeSinceStatisticsReset() const;
        [[nodiscard]] double nowSeconds() const;

    private:
        static constexpr sizet HISTORY_SIZE = 120;

        std::unique_ptr<IMonotonicClock> m_clock;
        FrameTiming m_timing{};
        std::deque<float> m_unscaledDeltaHistory;
        double m_lastSampleSeconds = 0.0;
        float m_timeAtStatisticsReset = 0.0f;
        float m_maximumDeltaTime = 1.0f;
        float m_timeScale = 1.0f;
        bool m_initialized = false;
        bool m_paused = false;
    };
} // namespace spite
