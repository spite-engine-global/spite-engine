#pragma once

#include "application/ApplicationExport.hpp"
#include "base/Platform.hpp"

namespace spite
{
    // A gamepad that exists in SDL without hardware (an SDL virtual joystick): the platform enumerates, opens and
    // polls it like a plugged-in pad, so tests and hardware-free runs exercise the same detection and hot-plug path.
    // Button and axis numbers are SDL's gamepad ones (InputSystem's gamepad indices). Needs SDL's gamepad subsystem
    // running (the input system of an Application starts it, headless too); destroying it unplugs the pad.
    class SPITE_APPLICATION_API VirtualGamepad
    {
    public:
        VirtualGamepad();
        ~VirtualGamepad();
        VirtualGamepad(const VirtualGamepad&) = delete;
        VirtualGamepad& operator=(const VirtualGamepad&) = delete;

        // False when SDL refused to attach it
        [[nodiscard]] bool attached() const { return m_id != 0; }
        void setButton(u32 button, bool pressed);
        void setAxis(u32 axis, i16 value);

        // How many times the platform has enumerated SDL's gamepad list (it rescans on a hot-plug event, not per poll)
        [[nodiscard]] static u64 enumerations();

        // Whether SDL's DirectInput joystick backend is on (the platform leaves it off unless the
        // SDL_JOYSTICK_DIRECTINPUT environment variable asks for it)
        [[nodiscard]] static bool directInputEnabled();

    private:
        u32 m_id = 0;
        void* m_joystick = nullptr; // SDL_Joystick*
    };
} // namespace spite
