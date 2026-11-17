#pragma once

#include "base/Platform.hpp"

namespace spite
{

    /**
     * @brief Common constants used throughout the application layer.
     * Note: Most configuration values should come from ApplicationConfig,
     *       not from these hardcoded values.
     */
    namespace ApplicationConstants
    {

        constexpr u32 MAX_SUPPORTED_GAMEPADS = 4;
        constexpr u32 GAMEPAD_BUTTON_COUNT = 26;
        constexpr u32 GAMEPAD_AXIS_COUNT = 6;
        constexpr u32 GAMEPAD_AXIS_LEFT_TRIGGER = 4;
        constexpr u32 GAMEPAD_AXIS_RIGHT_TRIGGER = 5;

        constexpr const char* DEFAULT_ASSETS_PATH = "assets/";
        constexpr const char* DEFAULT_SHADERS_PATH = "shaders/";
        constexpr const char* DEFAULT_CONFIG_PATH = "config/";
    } // namespace ApplicationConstants

    /**
     * @brief Snapshot of the frame timing state for the current frame.
     */
    struct FrameTiming
    {
        u64 frameIndex = 0;
        float unscaledDeltaTime = 0.0f;
        float deltaTime = 0.0f;
        float elapsedTime = 0.0f;
        float averageDeltaTime = 0.0f;
        float minimumDeltaTime = 0.0f;
        float maximumDeltaTime = 0.0f;
    };

} // namespace spite
