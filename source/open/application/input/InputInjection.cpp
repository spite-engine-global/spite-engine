#include "application/input/InputInjection.hpp"

#include "application/input/InputSystem.hpp"

namespace spite
{
    namespace
    {
        u16 heldModifiers(const InputSystem& input)
        {
            const auto held = [&input](const KeyCode left, const KeyCode right)
            { return input.getKeyState(left).isPressed || input.getKeyState(right).isPressed; };
            return static_cast<u16>((held(KeyCode::LSHIFT, KeyCode::RSHIFT) ? KeyModifier::SHIFT : 0) |
                                    (held(KeyCode::LCTRL, KeyCode::RCTRL) ? KeyModifier::CTRL : 0) |
                                    (held(KeyCode::LALT, KeyCode::RALT) ? KeyModifier::ALT : 0) |
                                    (held(KeyCode::LGUI, KeyCode::RGUI) ? KeyModifier::SUPER : 0));
        }
    } // namespace

    void ingestInput(const InputObservation& observation, InputSystem* input, ApplicationEventBus& bus,
                     const WindowId window)
    {
        struct Visitor
        {
            InputSystem* input;
            ApplicationEventBus& bus;
            WindowId window;

            void operator()(const KeyObservation& key) const
            {
                if (input)
                {
                    input->onKey(key.scancode, key.pressed);
                }
                // the platform reports the modifiers after the transition, as the core now holds them
                const u16 modifiers = key.modifiers ? *key.modifiers : input ? heldModifiers(*input) : u16{0};
                bus.enqueue(KeyPressEvent{.scancode = key.scancode,
                                          .isPressed = key.pressed,
                                          .isRepeat = key.repeat,
                                          .modifiers = modifiers,
                                          .windowId = window});
            }
            void operator()(const MouseButtonObservation& button) const
            {
                if (input)
                {
                    input->onPointerWindow(window);
                    input->onMouseButton(button.button, button.pressed);
                }
            }
            void operator()(const MouseMoveObservation& move) const
            {
                if (input)
                {
                    const MouseState previous = input->getMouseState();
                    input->onPointerWindow(window);
                    input->onMouseMove({.x = move.x,
                                        .y = move.y,
                                        .xrel = move.xrel.value_or(move.x - previous.x),
                                        .yrel = move.yrel.value_or(move.y - previous.y)});
                }
            }
            void operator()(const WheelObservation& wheel) const
            {
                if (input)
                {
                    input->onPointerWindow(window);
                    input->onMouseWheel(wheel.x, wheel.y);
                }
            }
            void operator()(const GamepadObservation& pad) const
            {
                if (!input)
                {
                    return;
                }
                using Kind = GamepadObservation::Kind;
                if (pad.kind == Kind::Disconnect)
                {
                    input->onGamepadDisconnected(INJECTED_GAMEPAD_ID, "injected gamepad");
                    return;
                }
                input->onGamepadConnected(INJECTED_GAMEPAD_ID, "injected gamepad"); // a no-op while connected
                if (pad.kind == Kind::Button)
                {
                    input->onGamepadButton(INJECTED_GAMEPAD_ID, pad.index, pad.pressed);
                }
                else if (pad.kind == Kind::Axis)
                {
                    input->onGamepadAxis(INJECTED_GAMEPAD_ID, pad.index, pad.value);
                }
            }
            void operator()(const TextObservation& text) const
            {
                if (!input)
                {
                    return;
                }
                if (text.composition)
                {
                    input->onTextEditing(text.text.c_str(), text.start, text.length);
                }
                else
                {
                    input->onTextInput(text.text.c_str());
                }
            }
        };
        std::visit(Visitor{input, bus, window}, observation);
    }

    void InputInjector::deliver(InputSystem& input, ApplicationEventBus& bus, const WindowId window)
    {
        if (m_batches.empty())
        {
            return;
        }
        Batch batch = std::move(m_batches.front());
        m_batches.pop_front();
        for (const InputObservation& observation : batch.observations)
        {
            ingestInput(observation, &input, bus, window);
        }
        if (batch.delivered)
        {
            batch.delivered();
        }
    }
} // namespace spite
