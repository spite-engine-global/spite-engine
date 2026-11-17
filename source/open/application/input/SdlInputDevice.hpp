#pragma once

#include <atomic>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_mouse.h>

#include "application/ApplicationTypes.hpp"
#include "application/input/InputSystem.hpp"
#include "application/input/TextInputBridge.hpp"

struct SDL_Window;

namespace spite
{
    // enumerations of SDL's gamepad list (VirtualGamepad::enumerations): rescans happen on a hot-plug event, not per
    // poll
    inline std::atomic<u64> g_gamepadEnumerations{0};

    // SDL adapter for the InputSystem core: owns device handles and OS
    // cursor/text-input calls, feeds the core through ingestion. No engine
    // policy lives here.
    struct SdlInputDevice final : ITextInputPlatform
    {
        SDL_Window* window = nullptr;
        SDL_Gamepad* handles[ApplicationConstants::MAX_SUPPORTED_GAMEPADS] = {};
        bool appliedVisible = true;
        bool appliedCaptured = false;
        bool gamepadStarted = false; // SDL's gamepad subsystem, started on the first poll with the controller enabled
        bool gamepadFailed = false;
        std::atomic<bool> gamepadsDirty{true}; // set by the SDL event watch on a gamepad added/removed event
        TextInputBridge textInput; // text input starts on in attach

        void attach(InputSystem& input, SDL_Window* sdlWindow);
        void poll(InputSystem& input);
        SdlInputDevice() = default;
        SdlInputDevice(const SdlInputDevice&) = delete;
        SdlInputDevice& operator=(const SdlInputDevice&) = delete;
        ~SdlInputDevice() override;

        void setTextInput(bool enabled) override;
        void setTextInputArea(const TextInputArea& area) override;
        void clearComposition() override;

    private:
        bool startGamepads();
        void closeHandle(u32 slot);
        void synchronizeGamepads(InputSystem& input);
        void applyCursorState(InputSystem& input);
    };
} // namespace spite
