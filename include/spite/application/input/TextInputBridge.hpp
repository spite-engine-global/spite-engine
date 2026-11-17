#pragma once

#include <optional>

#include "application/ApplicationExport.hpp"
#include "application/input/InputTypes.hpp"

namespace spite
{
    class InputSystem;

    // The platform calls text input needs, behind an interface so the policy of when to make them is testable
    // without a window (SdlInputDevice implements it with SDL).
    class ITextInputPlatform
    {
    public:
        virtual ~ITextInputPlatform() = default;
        virtual void setTextInput(bool enabled) = 0;
        virtual void setTextInputArea(const TextInputArea& area) = 0;
        virtual void clearComposition() = 0;
    };

    // Applies the text input requests of the input core to the platform once per frame, calling it only when
    // something changed: text input on or off, a cancelled composition, and the area of a focused text input when
    // it moved. While no text input is focused (no area) nothing about the area is sent, and
    // leaving a field sends nothing: stopping text input hides the input method's window.
    class SPITE_APPLICATION_API TextInputBridge
    {
    public:
        // `enabled`: whether the platform's text input is on to begin with
        explicit TextInputBridge(const bool enabled = true) : m_enabled(enabled) {}

        void apply(InputSystem& input, ITextInputPlatform& platform);

    private:
        bool m_enabled;
        std::optional<TextInputArea> m_sentArea;
    };
} // namespace spite
