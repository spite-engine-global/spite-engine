#include "application/input/VirtualGamepad.hpp"

#include <SDL3/SDL.h>

#include "SdlInputDevice.hpp"

namespace spite
{
    VirtualGamepad::VirtualGamepad()
    {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
        desc.button_mask = (1u << (SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1)) - 1;
        desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
        desc.name = "Spite virtual gamepad";
        m_id = SDL_AttachVirtualJoystick(&desc);
        // held open so its state can be set before the platform opens the pad
        m_joystick = m_id != 0 ? SDL_OpenJoystick(m_id) : nullptr;
    }

    VirtualGamepad::~VirtualGamepad()
    {
        if (m_joystick)
        {
            SDL_CloseJoystick(static_cast<SDL_Joystick*>(m_joystick));
        }
        if (m_id != 0)
        {
            SDL_DetachVirtualJoystick(m_id);
        }
    }

    u64 VirtualGamepad::enumerations() { return g_gamepadEnumerations.load(std::memory_order_relaxed); }

    bool VirtualGamepad::directInputEnabled() { return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_DIRECTINPUT, true); }

    void VirtualGamepad::setButton(const u32 button, const bool pressed)
    {
        if (m_joystick)
        {
            SDL_SetJoystickVirtualButton(static_cast<SDL_Joystick*>(m_joystick), static_cast<int>(button), pressed);
            SDL_UpdateJoysticks(); // what the event pump does, so the platform polls the new state
        }
    }

    void VirtualGamepad::setAxis(const u32 axis, const i16 value)
    {
        if (m_joystick)
        {
            SDL_SetJoystickVirtualAxis(static_cast<SDL_Joystick*>(m_joystick), static_cast<int>(axis), value);
            SDL_UpdateJoysticks();
        }
    }
} // namespace spite
