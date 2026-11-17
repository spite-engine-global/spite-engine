#pragma once

#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "application/ApplicationEvents.hpp"
#include "application/ApplicationExport.hpp"
#include "application/input/InputTypes.hpp"

namespace spite
{
    class InputSystem;

    // One device observation, as the platform reports it. The event dispatcher turns every SDL input event into one
    // and ingests it; injected input is the same type through the same ingestInput, so it meets the input contexts and
    // the UI's claims exactly as device input does.
    struct KeyObservation
    {
        u32 scancode = 0;
        bool pressed = false;
        bool repeat = false;
        // KeyModifier bits the platform reports; empty = the modifier keys held in the input core after this event
        std::optional<u16> modifiers;
    };

    struct MouseButtonObservation
    {
        MouseButton button = MouseButton::Left;
        bool pressed = false;
    };

    // window units; a nullopt delta is the move from the input core's last position
    struct MouseMoveObservation
    {
        float x = 0.0f;
        float y = 0.0f;
        std::optional<float> xrel;
        std::optional<float> yrel;
    };

    struct WheelObservation
    {
        float x = 0.0f;
        float y = 0.0f;
    };

    // Committed text, or with `composition` the input method's composition: the text being composed ("" ends it) and
    // the selected clause in codepoints (-1: none), the observation SDL_EVENT_TEXT_EDITING makes.
    struct TextObservation
    {
        std::string text;
        bool composition = false;
        i32 start = -1;
        i32 length = -1;
    };

    // The instance id of the one injected gamepad: a pad that exists in the input core only, without hardware or
    // SDL. SDL's ids are positive, so it never collides with a real pad; it holds one of the core's gamepad slots
    // while connected.
    inline constexpr i32 INJECTED_GAMEPAD_ID = -2;

    // A change of the injected gamepad. A button or axis connects it first when it is not; Disconnect releases
    // whatever it held. Numbers are SDL's gamepad button and axis ones (InputSystem's gamepad indices); an axis is
    // the raw signed 16-bit value the platform reports (a stick 32767, a trigger 0..32767).
    struct GamepadObservation
    {
        enum class Kind : u8
        {
            Connect,
            Disconnect,
            Button,
            Axis
        };
        Kind kind = Kind::Button;
        u32 index = 0;
        bool pressed = false;
        i16 value = 0;
    };

    using InputObservation = std::variant<KeyObservation, MouseButtonObservation, MouseMoveObservation,
                                          WheelObservation, TextObservation, GamepadObservation>;

    // Feeds one observation to the input core (null: none, as for a tool without input), and a key to the event bus
    // too, for the UI's key transitions. Never reads the OS.
    SPITE_APPLICATION_API void ingestInput(const InputObservation& observation, InputSystem* input,
                                           ApplicationEventBus& bus, WindowId window);

    // A scancode from a key name of the mapping schema ("W", "F5", "LSHIFT", "SPACE"); nullopt when unknown.
    // ASCII case-insensitive.
    [[nodiscard]] SPITE_APPLICATION_API std::optional<u32> keyCodeFromName(std::string_view name);
    // The canonical name of a scancode (ENTER is RETURN; a key outside the table is KEY_<code>), as the mapping files
    // and inputActions write it.
    [[nodiscard]] SPITE_APPLICATION_API std::string keyNameOf(u32 code);
    // Every name keyCodeFromName takes: letters, digits, then the named keys (ENTER and ESC alias RETURN and ESCAPE).
    [[nodiscard]] SPITE_APPLICATION_API std::vector<std::string> keyNames();

    // Observations queued by control commands, delivered at the top of the frame's event poll, before the input
    // core updates and the UI routes: one batch per frame, so requests made in one pump still land in separate
    // frames and a press and its release never share one.
    class SPITE_APPLICATION_API InputInjector
    {
    public:
        struct Batch
        {
            std::vector<InputObservation> observations;
            // runs right after the batch is ingested, in its frame (a paused world steps then)
            std::function<void()> delivered;
        };

        void queue(Batch batch) { m_batches.push_back(std::move(batch)); }
        [[nodiscard]] sizet pending() const { return m_batches.size(); }
        // Delivers the oldest batch, if any.
        void deliver(InputSystem& input, ApplicationEventBus& bus, WindowId window);

    private:
        std::deque<Batch> m_batches;
    };
} // namespace spite
