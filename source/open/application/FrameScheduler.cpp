#include "application/FrameScheduler.hpp"

#include "application/FrameClock.hpp"

#include <algorithm>
#include <cmath>

namespace spite
{
    FrameScheduler::FrameScheduler(const FrameSchedulerConfig& config) : m_config(config)
    {
        setFixedDeltaTime(config.fixedDeltaTime);
    }

    FixedStepResult FrameScheduler::advance(const float scaledDeltaTime)
    {
        m_accumulator += std::max(0.0f, scaledDeltaTime);

        FixedStepResult result{};
        while (m_accumulator >= m_config.fixedDeltaTime && result.stepCount < m_config.maximumFixedSteps)
        {
            m_accumulator -= m_config.fixedDeltaTime;
            result.stepCount++;
        }

        if (m_accumulator >= m_config.fixedDeltaTime)
        {
            const float droppedSteps = std::floor(m_accumulator / m_config.fixedDeltaTime);
            result.droppedSimulationTime = droppedSteps * m_config.fixedDeltaTime;
            m_accumulator -= result.droppedSimulationTime;
            m_droppedSimulationTime += result.droppedSimulationTime;
        }

        result.interpolationAlpha = std::clamp(m_accumulator / m_config.fixedDeltaTime, 0.0f, 1.0f);
        return result;
    }

    float FrameScheduler::getFixedDeltaTime() const { return m_config.fixedDeltaTime; }

    void FrameScheduler::setFixedDeltaTime(const float fixedDeltaTime)
    {
        m_config.fixedDeltaTime = std::max(0.001f, fixedDeltaTime);
        m_accumulator = std::min(m_accumulator, m_config.fixedDeltaTime);
    }

    void FrameScheduler::setMaximumFixedSteps(const u32 maximumFixedSteps)
    {
        m_config.maximumFixedSteps = std::max(1u, maximumFixedSteps);
    }

    u32 FrameScheduler::getTargetFps() const { return m_config.targetFps; }

    void FrameScheduler::setTargetFps(const u32 targetFps) { m_config.targetFps = targetFps; }

    bool FrameScheduler::shouldLimitFps() const { return m_config.limitFps && m_config.targetFps > 0; }

    void FrameScheduler::setLimitFps(const bool limitFps) { m_config.limitFps = limitFps; }

    float FrameScheduler::getSleepDuration(const double frameStartSeconds, const double currentSeconds,
                                           const bool vsyncEnabled) const
    {
        if (vsyncEnabled || !shouldLimitFps())
            return 0.0f;
        const float targetDuration = 1.0f / static_cast<float>(m_config.targetFps);
        return std::max(0.0f, targetDuration - static_cast<float>(currentSeconds - frameStartSeconds));
    }

    float FrameScheduler::getDroppedSimulationTime() const { return m_droppedSimulationTime; }
} // namespace spite
