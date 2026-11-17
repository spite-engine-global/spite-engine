#include "SdlInputDevice.hpp"

#include <algorithm>
#include <cmath>

#include <SDL3/SDL.h>

#include "base/Logging.hpp"
#include "profiling/Zone.hpp"

namespace spite
{
    namespace
    {
        bool SDLCALL onSdlEvent(void* userdata, SDL_Event* event)
        {
            if (event->type == SDL_EVENT_GAMEPAD_ADDED || event->type == SDL_EVENT_GAMEPAD_REMOVED)
            {
                static_cast<SdlInputDevice*>(userdata)->gamepadsDirty.store(true, std::memory_order_relaxed);
            }
            return true;
        }
    } // namespace

    void SdlInputDevice::attach(InputSystem& input, SDL_Window* sdlWindow)
    {
        window = sdlWindow;
        if (window)
        {
            SDL_StartTextInput(window);
        }
    }

    SdlInputDevice::~SdlInputDevice()
    {
        for (u32 slot = 0; slot < ApplicationConstants::MAX_SUPPORTED_GAMEPADS; ++slot)
        {
            closeHandle(slot);
        }
        if (window)
        {
            SDL_StopTextInput(window);
        }
        if (gamepadStarted)
        {
            SDL_RemoveEventWatch(onSdlEvent, this);
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        }
    }

    // SDL_GetGamepads finds nothing until SDL_INIT_GAMEPAD runs; started here, not in PlatformServices, so a
    // controller-less config (or a tool) never starts the joystick backends. DirectInput is left off: its device
    // enumeration is ~165 ms of the main thread on a machine with many HID devices (the whole start is ~3 ms
    // without it), and XInput, GameInput, WGI, raw input and HIDAPI cover the pads it would add; the
    // SDL_JOYSTICK_DIRECTINPUT environment variable turns it back on
    bool SdlInputDevice::startGamepads()
    {
        if (!gamepadStarted && !gamepadFailed)
        {
            SPITE_ZONE("Input.GamepadStart");
            SDL_SetHint(SDL_HINT_JOYSTICK_DIRECTINPUT, "0");
            gamepadStarted = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
            gamepadFailed = !gamepadStarted;
            if (gamepadStarted)
            {
                // pads present now are found by the first scan (dirty from construction); later ones by their event
                SDL_AddEventWatch(onSdlEvent, this);
            }
            else
            {
                SWARNING_LOG("gamepads unavailable: SDL_INIT_GAMEPAD failed: {}", SDL_GetError());
            }
        }
        return gamepadStarted;
    }

    void SdlInputDevice::setTextInput(const bool enabled)
    {
        if (enabled)
        {
            SDL_StartTextInput(window);
        }
        else
        {
            SDL_StopTextInput(window);
        }
    }

    void SdlInputDevice::setTextInputArea(const TextInputArea& area)
    {
        const SDL_Rect rect{static_cast<int>(std::lround(area.x)), static_cast<int>(std::lround(area.y)),
                            std::max(1, static_cast<int>(std::lround(area.width))),
                            std::max(1, static_cast<int>(std::lround(area.height)))};
        SDL_SetTextInputArea(window, &rect, static_cast<int>(std::lround(area.cursor)));
    }

    void SdlInputDevice::clearComposition() { SDL_ClearComposition(window); }

    void SdlInputDevice::closeHandle(const u32 slot)
    {
        if (slot >= ApplicationConstants::MAX_SUPPORTED_GAMEPADS || !handles[slot])
        {
            return;
        }
        SDL_CloseGamepad(handles[slot]);
        handles[slot] = nullptr;
    }

    void SdlInputDevice::applyCursorState(InputSystem& input)
    {
        const bool visible = input.isCursorVisible();
        const bool captured = input.isCursorCaptured();
        if (visible != appliedVisible)
        {
            appliedVisible = visible;
            if (visible)
            {
                SDL_ShowCursor();
            }
            else
            {
                SDL_HideCursor();
            }
        }
        if (captured != appliedCaptured)
        {
            appliedCaptured = captured;
            if (window)
            {
                SDL_SetWindowRelativeMouseMode(window, captured);
            }
        }

        if (window)
        {
            textInput.apply(input, *this);
        }

        float warpX = 0.0f;
        float warpY = 0.0f;
        if (window && input.consumeMouseWarpRequest(warpX, warpY))
        {
            SDL_WarpMouseInWindow(window, warpX, warpY);
        }
    }

    void SdlInputDevice::synchronizeGamepads(InputSystem& input)
    {
        if (!gamepadsDirty.exchange(false, std::memory_order_relaxed))
        {
            return;
        }
        g_gamepadEnumerations.fetch_add(1, std::memory_order_relaxed);
        int count = 0;
        SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
        const int maxGamepads = static_cast<int>(ApplicationConstants::MAX_SUPPORTED_GAMEPADS);
        bool stillConnected[ApplicationConstants::MAX_SUPPORTED_GAMEPADS] = {};

        if (gamepads)
        {
            const int gamepadsToProcess = std::min(count, maxGamepads);
            for (int i = 0; i < gamepadsToProcess; ++i)
            {
                const SDL_JoystickID instanceId = gamepads[i];

                int slot = -1;
                for (int j = 0; j < maxGamepads; ++j)
                {
                    if (handles[j] && SDL_GetGamepadID(handles[j]) == instanceId)
                    {
                        slot = j;
                        break;
                    }
                }
                if (slot < 0)
                {
                    for (int j = 0; j < maxGamepads; ++j)
                    {
                        if (!handles[j])
                        {
                            slot = j;
                            break;
                        }
                    }
                }
                if (slot < 0)
                {
                    continue;
                }

                if (!handles[slot])
                {
                    handles[slot] = SDL_OpenGamepad(instanceId);
                    if (!handles[slot])
                    {
                        continue;
                    }
                    input.onGamepadConnected(instanceId, SDL_GetGamepadName(handles[slot]));
                }
                stillConnected[slot] = true;
            }
            SDL_free(gamepads);
        }

        for (int slot = 0; slot < maxGamepads; ++slot)
        {
            if (handles[slot] && !stillConnected[slot])
            {
                input.onGamepadDisconnected(SDL_GetGamepadID(handles[slot]), SDL_GetGamepadName(handles[slot]));
                closeHandle(static_cast<u32>(slot));
            }
        }
    }

    void SdlInputDevice::poll(InputSystem& input)
    {
        applyCursorState(input);

        if (!input.isControllerEnabled())
        {
            for (u32 slot = 0; slot < ApplicationConstants::MAX_SUPPORTED_GAMEPADS; ++slot)
            {
                if (handles[slot])
                {
                    input.onGamepadDisconnected(SDL_GetGamepadID(handles[slot]), SDL_GetGamepadName(handles[slot]));
                    closeHandle(slot);
                }
            }
            input.disconnectAllGamepads();
            gamepadsDirty.store(true, std::memory_order_relaxed); // pads still plugged in reopen when re-enabled
            return;
        }

        if (!startGamepads())
        {
            return;
        }
        synchronizeGamepads(input);

        bool buttons[ApplicationConstants::GAMEPAD_BUTTON_COUNT];
        i16 axes[ApplicationConstants::GAMEPAD_AXIS_COUNT];
        for (u32 slot = 0; slot < ApplicationConstants::MAX_SUPPORTED_GAMEPADS; ++slot)
        {
            if (!handles[slot])
            {
                continue;
            }
            const SDL_JoystickID instanceId = SDL_GetGamepadID(handles[slot]);
            for (u32 button = 0; button < ApplicationConstants::GAMEPAD_BUTTON_COUNT; ++button)
            {
                buttons[button] = SDL_GetGamepadButton(handles[slot], static_cast<SDL_GamepadButton>(button));
            }
            for (u32 axis = 0; axis < ApplicationConstants::GAMEPAD_AXIS_COUNT; ++axis)
            {
                axes[axis] = SDL_GetGamepadAxis(handles[slot], static_cast<SDL_GamepadAxis>(axis));
            }
            input.ingestGamepadState(instanceId, buttons, axes);
        }
    }
} // namespace spite
