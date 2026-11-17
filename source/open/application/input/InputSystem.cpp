#include "application/input/InputSystem.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "base/ApplicationConfig.hpp"
#include "base/Assert.hpp"

namespace spite
{
    namespace
    {
        constexpr u32 MOUSE_BUTTON_COUNT = 5;

        InputState buttonStateToInputState(const ButtonState& state)
        {
            return InputState{.isPressed = state.isPressed,
                              .wasPressed = state.wasPressed,
                              .holdTime = state.holdingTime,
                              .value = state.isPressed ? 1.0f : 0.0f,
                              .isChanged = state.isPressed != state.wasPressed};
        }
    } // namespace

    InputSystem::InputSystem(const InputConfig& config) : m_actionMap(config.mappingsFile.toView())
    {
        m_mouseSensitivity = config.mouseSensitivity;
        m_invertYAxis = config.invertYAxis;
        m_enableController = config.enableController;
        m_enableTouch = config.enableTouch;
        m_controllerDeadzone = std::clamp(config.controllerDeadzone, 0.0f, 0.99f);
        m_controllerSensitivity = std::max(config.controllerSensitivity, 0.0f);
        m_mappingsFile = config.mappingsFile;
    }

    InputSystem::~InputSystem() = default;

    void InputSystem::applyConfig(const InputConfig& config)
    {
        m_mouseSensitivity = config.mouseSensitivity;
        m_invertYAxis = config.invertYAxis;
        m_enableController = config.enableController;
        m_enableTouch = config.enableTouch;
        m_controllerDeadzone = std::clamp(config.controllerDeadzone, 0.0f, 0.99f);
        m_controllerSensitivity = std::max(config.controllerSensitivity, 0.0f);

        if (m_mappingsFile != config.mappingsFile)
        {
            SASSERTM(config.mappingsFile.toView()[0] != '\0', "Config should specify mappingsFile.");
            m_mappingsFile = config.mappingsFile;
            if (!m_actionMap.loadMappings(DEFAULT_INPUT_CONTEXT_ID,
                                          config.mappingsFile.toView())) // other contexts stay
            {
                m_actionMap.applyMappings({});
            }
        }
    }

    void InputSystem::setDevicePoll(std::function<void(InputSystem&)> poll) { m_devicePoll = std::move(poll); }

    void InputSystem::setEventBus(ApplicationEventBus& eventBus) { m_eventBus = &eventBus; }

    // -- ingestion --

    void InputSystem::onKey(const u32 scancode, const bool isPressed)
    {
        const bool wasPressed = m_stateMap.isDown(scancode);
        if (isPressed)
        {
            m_stateMap.setButtonPress(scancode);
            m_actionMap.triggerKeyInteraction(scancode, !wasPressed, true, m_stateMap.holdingTime(scancode));
        }
        else
        {
            m_stateMap.setButtonRelease(scancode);
            // Passing wasPressed as the hold-time argument latches isChanged
            // for the release edge (0.0f when the key was not down).
            m_actionMap.triggerKeyInteraction(scancode, false, false, wasPressed ? 1.0f : 0.0f);
        }
    }

    void InputSystem::onMouseButton(const MouseButton button, const bool isPressed)
    {
        auto& state = m_mouseButtons[static_cast<u32>(button)];
        state.wasPressed = state.isPressed;
        state.isPressed = isPressed;
        if (!isPressed)
        {
            state.holdingTime = 0.0f;
        }
        m_actionMap.setControlState(
            InputControl{.device = InputControlDevice::MouseButton, .code = static_cast<u16>(button)},
            buttonStateToInputState(state));
    }

    void InputSystem::onMouseMove(const MouseMoveEvent& event)
    {
        m_mouseState.x = event.x;
        m_mouseState.y = event.y;

        float xrel = event.xrel * m_mouseSensitivity;
        float yrel = event.yrel * m_mouseSensitivity;
        if (m_invertYAxis)
        {
            yrel = -yrel;
        }
        m_mouseState.deltaX += xrel;
        m_mouseState.deltaY += yrel;
    }

    void InputSystem::onMouseWheel(const float deltaX, const float deltaY)
    {
        m_mouseState.scrollDeltaX += deltaX;
        m_mouseState.scrollDeltaY += deltaY;
    }

    void InputSystem::onTextInput(const char* text)
    {
        if (text)
        {
            m_textInput += text;
            m_composition = {}; // committed text replaces what was being composed
        }
    }

    void InputSystem::onTextEditing(const char* text, const i32 start, const i32 length)
    {
        m_composition = {text ? text : "", start, length};
    }

    void InputSystem::cancelComposition()
    {
        if (!m_composition.text.empty())
        {
            m_composition = {};
            m_clearCompositionRequested = true;
        }
    }

    bool InputSystem::consumeClearCompositionRequest() { return std::exchange(m_clearCompositionRequested, false); }

    void InputSystem::onFocusChanged(const bool hasFocus)
    {
        m_windowFocused = hasFocus;
        if (!hasFocus)
        {
            m_composition = {};
            m_stateMap.clear();
            m_actionMap.clear();
            m_mouseState = {};
            for (auto& button : m_mouseButtons)
                button = {};
        }
    }

    void InputSystem::onGamepadConnected(const i32 instanceId, const char* name)
    {
        if (!m_enableController)
        {
            return;
        }

        if (const auto existing = findGamepadSlot(instanceId))
        {
            existing->connected = true;
            return;
        }

        for (u32 slot = 0; slot < ApplicationConstants::MAX_SUPPORTED_GAMEPADS; ++slot)
        {
            if (m_gamepads[slot].connected)
            {
                continue;
            }
            auto& gamepad = m_gamepads[slot];
            gamepad = GamepadSlot{};
            gamepad.instanceId = instanceId;
            gamepad.connected = true;
            if (m_eventBus)
            {
                m_eventBus->enqueue(GamepadConnectedEvent{
                    .gamepadIndex = slot, .instanceId = instanceId, .name = InternedString(name ? name : "")});
            }
            return;
        }
    }

    void InputSystem::onGamepadDisconnected(const i32 instanceId, const char* name)
    {
        if (auto* slot = findGamepadSlot(instanceId))
        {
            if (m_eventBus)
            {
                m_eventBus->enqueue(GamepadDisconnectedEvent{.gamepadIndex = static_cast<u32>(slot - m_gamepads),
                                                             .instanceId = instanceId,
                                                             .name = InternedString(name ? name : "")});
            }
            releaseGamepadControls(*slot);
            *slot = {};
        }
    }

    // A pad that vanishes while a control is down would leave its last state in the action map, which only
    // connected pads refresh: a held direction would stay down for good.
    void InputSystem::releaseGamepadControls(const GamepadSlot& slot)
    {
        for (u32 button = 0; button < ApplicationConstants::GAMEPAD_BUTTON_COUNT; ++button)
        {
            if (slot.buttonStates[button])
            {
                m_actionMap.setControlState({.device = InputControlDevice::GamepadButton,
                                             .code = static_cast<u16>(button),
                                             .deviceId = slot.instanceId},
                                            InputState{});
            }
        }
        for (u32 axis = 0; axis < ApplicationConstants::GAMEPAD_AXIS_COUNT; ++axis)
        {
            if (slot.axisValues[axis] != 0)
            {
                m_actionMap.setControlState({.device = InputControlDevice::GamepadAxis,
                                             .code = static_cast<u16>(axis),
                                             .deviceId = slot.instanceId},
                                            InputState{});
            }
        }
    }

    void InputSystem::ingestGamepadState(const i32 instanceId, const bool* buttons, const i16* axes)
    {
        if (auto* slot = findGamepadSlot(instanceId))
        {
            std::copy(buttons, buttons + ApplicationConstants::GAMEPAD_BUTTON_COUNT, slot->pendingButtons);
            std::copy(axes, axes + ApplicationConstants::GAMEPAD_AXIS_COUNT, slot->pendingAxes);
        }
    }

    void InputSystem::onGamepadButton(const i32 instanceId, const u32 button, const bool pressed)
    {
        if (auto* slot = findGamepadSlot(instanceId); slot && button < ApplicationConstants::GAMEPAD_BUTTON_COUNT)
        {
            slot->pendingButtons[button] = pressed;
        }
    }

    void InputSystem::onGamepadAxis(const i32 instanceId, const u32 axis, const i16 value)
    {
        if (auto* slot = findGamepadSlot(instanceId); slot && axis < ApplicationConstants::GAMEPAD_AXIS_COUNT)
        {
            slot->pendingAxes[axis] = value;
        }
    }

    void InputSystem::disconnectAllGamepads()
    {
        for (auto& slot : m_gamepads)
        {
            releaseGamepadControls(slot);
            slot = {};
        }
    }

    InputSystem::GamepadSlot* InputSystem::findGamepadSlot(const i32 instanceId)
    {
        for (auto& slot : m_gamepads)
        {
            if (slot.connected && slot.instanceId == instanceId)
            {
                return &slot;
            }
        }
        return nullptr;
    }

    // -- frame lifecycle --

    void InputSystem::update(const float deltaTime)
    {
        if (m_devicePoll)
        {
            m_devicePoll(*this);
        }

        m_stateMap.update(deltaTime);
        for (const auto& [scancode, state] : m_stateMap.getStateMap())
        {
            if (state.isPressed)
            {
                m_actionMap.updateControlHoldTime(
                    InputControl{.device = InputControlDevice::Keyboard, .code = static_cast<u16>(scancode)},
                    state.holdingTime);
            }
        }

        for (u32 button = 0; button < MOUSE_BUTTON_COUNT; ++button)
        {
            auto& state = m_mouseButtons[button];
            if (state.isPressed)
            {
                state.holdingTime += deltaTime;
            }
            m_actionMap.setControlState(
                InputControl{.device = InputControlDevice::MouseButton, .code = static_cast<u16>(button)},
                buttonStateToInputState(state));
        }

        for (auto& gamepad : m_gamepads)
        {
            if (!gamepad.connected)
            {
                continue;
            }

            for (u32 button = 0; button < ApplicationConstants::GAMEPAD_BUTTON_COUNT; ++button)
            {
                gamepad.prevButtonStates[button] = gamepad.buttonStates[button];
                gamepad.buttonStates[button] = gamepad.pendingButtons[button];
                gamepad.buttonHoldTimes[button] =
                    gamepad.buttonStates[button] ? gamepad.buttonHoldTimes[button] + deltaTime : 0.0f;

                m_actionMap.setControlState(
                    InputControl{.device = InputControlDevice::GamepadButton,
                                 .code = static_cast<u16>(button),
                                 .deviceId = gamepad.instanceId},
                    InputState{.isPressed = gamepad.buttonStates[button],
                               .wasPressed = gamepad.prevButtonStates[button],
                               .holdTime = gamepad.buttonHoldTimes[button],
                               .value = gamepad.buttonStates[button] ? 1.0f : 0.0f,
                               .isChanged = gamepad.buttonStates[button] != gamepad.prevButtonStates[button]});
            }

            for (u32 axis = 0; axis < ApplicationConstants::GAMEPAD_AXIS_COUNT; ++axis)
            {
                gamepad.prevAxisValues[axis] = gamepad.axisValues[axis];
                gamepad.axisValues[axis] = gamepad.pendingAxes[axis];
            }

            for (u32 axis = 0; axis < ApplicationConstants::GAMEPAD_AXIS_COUNT; ++axis)
            {
                const float value =
                    getGamepadAxisValue(static_cast<int>(&gamepad - m_gamepads), static_cast<int>(axis));
                m_actionMap.setControlState(
                    InputControl{.device = InputControlDevice::GamepadAxis,
                                 .code = static_cast<u16>(axis),
                                 .deviceId = gamepad.instanceId},
                    InputState{.isPressed = value != 0.0f,
                               .wasPressed = gamepad.prevAxisValues[axis] != 0,
                               .holdTime = 0.0f,
                               .value = value,
                               .isChanged = gamepad.axisValues[axis] != gamepad.prevAxisValues[axis]});
            }
        }
    }

    void InputSystem::reset()
    {
        m_stateMap.reset();
        m_actionMap.reset();

        m_mouseState.deltaX = 0.0f;
        m_mouseState.deltaY = 0.0f;
        m_mouseState.scrollDeltaX = 0.0f;
        m_mouseState.scrollDeltaY = 0.0f;

        for (auto& state : m_mouseButtons)
        {
            state.wasPressed = state.isPressed;
        }

        m_textInput.clear();
    }

    // -- queries --

    InputState InputSystem::getKeyState(const KeyCode key) const
    {
        const u32 scancode = static_cast<u32>(key);
        InputState state;
        state.isPressed = m_stateMap.isDown(scancode);
        const auto& stateMap = m_stateMap.getStateMap();
        const auto it = stateMap.find(scancode);
        state.wasPressed = it != stateMap.end() && it->second.wasPressed;
        state.holdTime = m_stateMap.holdingTime(scancode);
        state.value = state.isPressed ? 1.0f : 0.0f;
        state.isChanged = m_stateMap.isJustPressed(scancode) || m_stateMap.isJustReleased(scancode);
        return state;
    }

    InputState InputSystem::getMouseButtonState(const MouseButton button) const
    {
        return buttonStateToInputState(m_mouseButtons[static_cast<u32>(button)]);
    }

    MouseState InputSystem::getMouseState() const
    {
        MouseState state = m_mouseState;
        for (u32 button = 0; button < MOUSE_BUTTON_COUNT; ++button)
            state.buttons[button] = getMouseButtonState(static_cast<MouseButton>(button));
        return state;
    }

    bool InputSystem::isActionPressed(const InternedString actionName) const
    {
        return wasActionPressedThisFrame(actionName);
    }

    bool InputSystem::wasActionPressedThisFrame(const InternedString actionName) const
    {
        return isActionDown(actionName) && !m_actionMap.wasActionActive(actionName);
    }

    bool InputSystem::isActionReleased(const InternedString actionName) const
    {
        return wasActionReleasedThisFrame(actionName);
    }

    bool InputSystem::wasActionReleasedThisFrame(const InternedString actionName) const
    {
        return m_actionMap.wasActionActive(actionName) && !isActionDown(actionName);
    }

    bool InputSystem::isActionHeld(const InternedString actionName) const
    {
        return isActionDown(actionName) && !wasActionPressedThisFrame(actionName);
    }

    bool InputSystem::isActionDown(const InternedString actionName) const
    {
        return m_actionMap.isActionActive(actionName);
    }

    float InputSystem::getActionHoldTime(const InternedString actionName) const
    {
        return isActionDown(actionName) ? std::max(0.0f, m_actionMap.actionValue(actionName)) : 0.0f;
    }

    float InputSystem::getActionValue(const InternedString actionName) const
    {
        return m_actionMap.actionValue(actionName);
    }

    int InputSystem::getConnectedGamepadCount() const
    {
        if (!m_enableController)
        {
            return 0;
        }
        int count = 0;
        for (const auto& gamepad : m_gamepads)
        {
            if (gamepad.connected)
            {
                ++count;
            }
        }
        return count;
    }

    bool InputSystem::isGamepadConnected(const int gamepadIndex) const
    {
        if (!m_enableController || gamepadIndex < 0 ||
            gamepadIndex >= static_cast<int>(ApplicationConstants::MAX_SUPPORTED_GAMEPADS))
        {
            return false;
        }
        return m_gamepads[gamepadIndex].connected;
    }

    bool InputSystem::hasGamepad(const i32 instanceId) const
    {
        return m_enableController &&
            std::ranges::any_of(m_gamepads, [instanceId](const GamepadSlot& slot)
                                { return slot.connected && slot.instanceId == instanceId; });
    }

    InputState InputSystem::getGamepadButtonState(const int gamepadIndex, const int button) const
    {
        if (!m_enableController || gamepadIndex < 0 ||
            gamepadIndex >= static_cast<int>(ApplicationConstants::MAX_SUPPORTED_GAMEPADS) || button < 0 ||
            button >= static_cast<int>(ApplicationConstants::GAMEPAD_BUTTON_COUNT) || !isGamepadConnected(gamepadIndex))
        {
            return InputState{};
        }

        const auto& state = m_gamepads[gamepadIndex];
        return InputState{.isPressed = state.buttonStates[button],
                          .wasPressed = state.prevButtonStates[button],
                          .holdTime = state.buttonHoldTimes[button],
                          .value = state.buttonStates[button] ? 1.0f : 0.0f,
                          .isChanged = state.buttonStates[button] != state.prevButtonStates[button]};
    }

    float InputSystem::getGamepadRawAxisValue(const int gamepadIndex, const int axis) const
    {
        if (!m_enableController || gamepadIndex < 0 ||
            gamepadIndex >= static_cast<int>(ApplicationConstants::MAX_SUPPORTED_GAMEPADS) || axis < 0 ||
            axis >= static_cast<int>(ApplicationConstants::GAMEPAD_AXIS_COUNT) || !isGamepadConnected(gamepadIndex))
        {
            return 0.0f;
        }

        const float normalizedValue = static_cast<float>(m_gamepads[gamepadIndex].axisValues[axis]) / 32767.0f;
        const bool trigger = axis == static_cast<int>(ApplicationConstants::GAMEPAD_AXIS_LEFT_TRIGGER) ||
            axis == static_cast<int>(ApplicationConstants::GAMEPAD_AXIS_RIGHT_TRIGGER);
        return std::clamp(normalizedValue, trigger ? 0.0f : -1.0f, 1.0f);
    }

    float InputSystem::getGamepadAxisValue(const int gamepadIndex, const int axis) const
    {
        const float raw = getGamepadRawAxisValue(gamepadIndex, axis);
        if (axis == static_cast<int>(ApplicationConstants::GAMEPAD_AXIS_LEFT_TRIGGER) ||
            axis == static_cast<int>(ApplicationConstants::GAMEPAD_AXIS_RIGHT_TRIGGER))
        {
            return raw;
        }
        if (std::abs(raw) < m_controllerDeadzone)
        {
            return 0.0f;
        }
        const float sign = raw > 0 ? 1.0f : -1.0f;
        return sign * (std::abs(raw) - m_controllerDeadzone) / (1.0f - m_controllerDeadzone) * m_controllerSensitivity;
    }

    std::string InputSystem::getTextInput() const { return m_textInput; }

    // -- cursor --

    void InputSystem::setCursorVisible(const bool visible) { m_cursorVisible = visible; }

    void InputSystem::setCursorCaptured(const bool captured) { m_cursorCaptured = captured; }

    void InputSystem::setMousePosition(const float x, const float y)
    {
        m_mouseState.x = x;
        m_mouseState.y = y;
        m_hasPendingWarp = true;
        m_warpX = x;
        m_warpY = y;
    }

    bool InputSystem::consumeMouseWarpRequest(float& outX, float& outY)
    {
        if (!m_hasPendingWarp)
        {
            return false;
        }
        m_hasPendingWarp = false;
        outX = m_warpX;
        outY = m_warpY;
        return true;
    }

    // -- mappings and contexts --

    void InputSystem::mapActionToKey(const InternedString actionName, const KeyCode key, const InputActionType type)
    {
        m_actionMap.addBinding(actionName, static_cast<u32>(key), type, 1.0f);
    }

    void InputSystem::mapActionToMouse(const InternedString actionName, const MouseButton button,
                                       const InputActionType type)
    {
        m_actionMap.addBinding(
            actionName, InputControl{.device = InputControlDevice::MouseButton, .code = static_cast<u16>(button)}, type,
            1.0f);
    }

    void InputSystem::unmapAction(const InternedString actionName) { m_actionMap.removeAllBindings(actionName); }

    InputContextId InputSystem::createContext(const InputContextDesc& desc) { return m_actionMap.createContext(desc); }

    bool InputSystem::destroyContext(const InputContextId contextId) { return m_actionMap.destroyContext(contextId); }

    bool InputSystem::setContextEnabled(const InputContextId contextId, const bool enabled)
    {
        return m_actionMap.setContextEnabled(contextId, enabled);
    }

    void InputSystem::setContextPriorityFloor(const i32 floor) { m_actionMap.setContextPriorityFloor(floor); }

    bool InputSystem::setContextPriority(const InputContextId contextId, const i32 priority)
    {
        return m_actionMap.setContextPriority(contextId, priority);
    }

    bool InputSystem::addBinding(const InputContextId contextId, const InputBindingDesc& binding)
    {
        if (contextId == INVALID_INPUT_CONTEXT_ID || !binding.action.isValid())
        {
            return false;
        }
        m_actionMap.addBinding(contextId, binding.action, binding.control, binding.trigger, binding.scale,
                               binding.modifiers);
        return true;
    }

    bool InputSystem::loadMappings(const InputContextId contextId, const std::string_view filePath)
    {
        return m_actionMap.loadMappings(contextId, filePath);
    }

    bool InputSystem::removeBindings(const InputContextId contextId, const InternedString actionName)
    {
        return m_actionMap.removeBindings(contextId, actionName);
    }

    void InputSystem::claimControl(const InputControl control) { m_actionMap.claimControl(control); }
} // namespace spite
