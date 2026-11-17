#pragma once

#include <memory>

#include "application/ApplicationExport.hpp"
#include "base/ApplicationConfig.hpp"
#include "application/Window.hpp"

namespace spite
{
    class InputSystem;
    struct InputConfig;

    class SPITE_APPLICATION_API PlatformServices
    {
    public:
        // headless: the SDL event subsystem only, no video or audio
        explicit PlatformServices(bool headless = false);
        ~PlatformServices();

        PlatformServices(const PlatformServices&) = delete;
        PlatformServices& operator=(const PlatformServices&) = delete;

        // Builds the input core and wires the SDL adapter to it; the adapter
        // stays alive through the poll hook captured inside the core. The adapter starts SDL's gamepad subsystem
        // when the controller is enabled (also headless), so a pad plugged in later is found.
        std::unique_ptr<InputSystem> createInputSystem(const InputConfig& config, const Window* window = nullptr);

        void sleep(float seconds);
    };
} // namespace spite
