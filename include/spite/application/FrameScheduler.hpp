#pragma once

#include "FrameClock.hpp"

namespace spite
{
    struct FrameSchedulerConfig
    {
        float fixedDeltaTime = 1.0f / 60.0f;
        u32 maximumFixedSteps = 4;
        u32 targetFps = 0;
        bool limitFps = false;
    };

    struct FixedStepResult
    {
        u32 stepCount = 0;
        float interpolationAlpha = 0.0f;
        float droppedSimulationTime = 0.0f;
    };

    class SPITE_APPLICATION_API FrameScheduler
    {
    public:
        explicit FrameScheduler(const FrameSchedulerConfig& config = {});

        [[nodiscard]] FixedStepResult advance(float scaledDeltaTime);
        [[nodiscard]] float getFixedDeltaTime() const;
        void setFixedDeltaTime(float fixedDeltaTime);
        void setMaximumFixedSteps(u32 maximumFixedSteps);
        [[nodiscard]] u32 getTargetFps() const;
        void setTargetFps(u32 targetFps);
        [[nodiscard]] bool shouldLimitFps() const;
        void setLimitFps(bool limitFps);
        [[nodiscard]] float getSleepDuration(double frameStartSeconds, double currentSeconds, bool vsyncEnabled) const;
        [[nodiscard]] float getDroppedSimulationTime() const;

    private:
        FrameSchedulerConfig m_config;
        float m_accumulator = 0.0f;
        float m_droppedSimulationTime = 0.0f;
    };
} // namespace spite
