#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "application/ApplicationEvents.hpp"
#include "application/ApplicationTypes.hpp"
#include "application/input/InputActionMap.hpp"
#include "application/input/InputStateMap.hpp"
#include "base/InternedString.hpp"
#include "base/Platform.hpp"

namespace spite
{
    struct InputConfig;

    /**
     * @brief Platform-free input core: owns device state, action resolution,
     *        and input configuration.
     *
     * The platform adapter feeds observations through the ingestion surface
     * (onKey, onMouse*, onTextInput, onFocusChanged, onGamepad*) and a
     * device-poll hook invoked at the top of update() (gamepad hotplug and
     * raw device reads). Cursor and mouse-warp requests are pulled by the
     * adapter from the query accessors; the core never calls the OS.
     */
    class SPITE_APPLICATION_API InputSystem
    {
        InputStateMap m_stateMap;
        InputActionMap m_actionMap;

        MouseState m_mouseState;
        // Mouse button state lives here, not in the key state map.
        ButtonState m_mouseButtons[5]{};

        // Configuration
        InternedString m_mappingsFile{};
        float m_mouseSensitivity = 1.0f;
        bool m_invertYAxis = false;
        bool m_enableController = true;
        bool m_enableTouch = false;
        float m_controllerDeadzone = 0.1f;
        float m_controllerSensitivity = 1.0f;

        // Cursor requests, applied by the adapter
        bool m_cursorVisible = true;
        bool m_cursorCaptured = false;
        bool m_gameInputFocus = true;
        bool m_windowFocused = true;
        bool m_textInputRequested = true;
        bool m_hasPendingWarp = false;
        float m_warpX = 0.0f;
        float m_warpY = 0.0f;

        struct GamepadSlot
        {
            i32 instanceId = -1;
            bool connected = false;
            bool buttonStates[ApplicationConstants::GAMEPAD_BUTTON_COUNT] = {};
            bool prevButtonStates[ApplicationConstants::GAMEPAD_BUTTON_COUNT] = {};
            float buttonHoldTimes[ApplicationConstants::GAMEPAD_BUTTON_COUNT] = {};
            i16 axisValues[ApplicationConstants::GAMEPAD_AXIS_COUNT] = {};
            i16 prevAxisValues[ApplicationConstants::GAMEPAD_AXIS_COUNT] = {};
            // Raw values written by the adapter between frames; update()
            // latches prev <- current <- pending and accumulates hold times.
            bool pendingButtons[ApplicationConstants::GAMEPAD_BUTTON_COUNT] = {};
            i16 pendingAxes[ApplicationConstants::GAMEPAD_AXIS_COUNT] = {};
        };

        GamepadSlot m_gamepads[ApplicationConstants::MAX_SUPPORTED_GAMEPADS];

        std::string m_textInput;
        TextComposition m_composition;
        std::optional<TextInputArea> m_textInputArea;
        bool m_clearCompositionRequested = false;
        ApplicationEventBus* m_eventBus = nullptr;
        std::function<void(InputSystem&)> m_devicePoll;

        [[nodiscard]] GamepadSlot* findGamepadSlot(i32 instanceId);
        void releaseGamepadControls(const GamepadSlot& slot);

    public:
        explicit InputSystem(const InputConfig& config);
        ~InputSystem();
        InputSystem(const InputSystem&) = delete;
        InputSystem& operator=(const InputSystem&) = delete;

        // -- adapter wiring --
        // Invoked at the top of update() so the adapter can feed hotplug and
        // raw device state before hold times advance and actions resolve.
        void setDevicePoll(std::function<void(InputSystem&)> poll);
        // Event emission only (gamepad hotplug). Input observations arrive
        // through ingestion, never through the bus.
        void setEventBus(ApplicationEventBus& eventBus);
        [[nodiscard]] bool isControllerEnabled() const { return m_enableController; }

        // -- ingestion --
        void onKey(u32 scancode, bool isPressed);
        void onMouseButton(MouseButton button, bool isPressed);
        void onMouseMove(const MouseMoveEvent& event);
        void onMouseWheel(float deltaX, float deltaY);
        // The window the next pointer observations come from (MouseState::window).
        void onPointerWindow(u64 window) { m_mouseState.window = m_mouseState.hoverWindow = window; }
        void onPointerHoverWindow(u64 window) { m_mouseState.hoverWindow = window; }
        void onTextInput(const char* text);
        // The input method's composition changed (SDL_EVENT_TEXT_EDITING): empty text ends it. Committed text
        // (onTextInput) and losing focus end it too.
        void onTextEditing(const char* text, i32 start, i32 length);
        void onFocusChanged(bool hasFocus);
        void onGamepadConnected(i32 instanceId, const char* name);
        void onGamepadDisconnected(i32 instanceId, const char* name);
        // One control of a connected pad changed (what the injection layer reports; the adapter's snapshot is
        // ingestGamepadState); latest value before update() wins. Ignored for a pad that is not connected.
        void onGamepadButton(i32 instanceId, u32 button, bool pressed);
        void onGamepadAxis(i32 instanceId, u32 axis, i16 value);
        // Raw per-pad snapshot; latest value before update() wins.
        void ingestGamepadState(i32 instanceId, const bool* buttons, const i16* axes);
        // Silent clear of all slots (no disconnect events) - adapter closes
        // its device handles separately when the controller is disabled.
        void disconnectAllGamepads();

        // Live config updates: scalars apply immediately; a changed
        // mappingsFile reloads the mapping file (fresh bindings).
        void applyConfig(const InputConfig& config);

        // -- frame lifecycle --
        void update(float deltaTime);
        void reset(); // Call at the end of the frame

        // -- queries --
        [[nodiscard]] InputState getKeyState(KeyCode key) const;
        [[nodiscard]] InputState getMouseButtonState(MouseButton button) const;
        [[nodiscard]] MouseState getMouseState() const;
        [[nodiscard]] bool isActionPressed(InternedString actionName) const;
        [[nodiscard]] bool wasActionPressedThisFrame(InternedString actionName) const;
        [[nodiscard]] bool isActionReleased(InternedString actionName) const;
        [[nodiscard]] bool wasActionReleasedThisFrame(InternedString actionName) const;
        [[nodiscard]] bool isActionHeld(InternedString actionName) const;
        [[nodiscard]] bool isActionDown(InternedString actionName) const;
        [[nodiscard]] float getActionHoldTime(InternedString actionName) const;
        [[nodiscard]] float getActionValue(InternedString actionName) const;
        // An action some binding or context registered; getActionValue and the other action queries need one.
        [[nodiscard]] bool hasAction(InternedString actionName) const { return m_actionMap.hasAction(actionName); }
        // Some action is active this frame: false is the cheap answer for an idle one.
        [[nodiscard]] bool hasActiveAction() const { return m_actionMap.anyActionActive(); }
        // The controls an action is bound to, in any context.
        void actionControls(InternedString actionName, std::vector<InputControl>& out) const
        {
            m_actionMap.actionControls(actionName, out);
        }
        [[nodiscard]] int getConnectedGamepadCount() const;
        [[nodiscard]] bool isGamepadConnected(int gamepadIndex) const;
        [[nodiscard]] bool hasGamepad(i32 instanceId) const; // a connected pad with this instance id
        [[nodiscard]] InputState getGamepadButtonState(int gamepadIndex, int button) const;
        // The effective value (stick deadzone and sensitivity applied) and what the device reports before them, in
        // [-1, 1] ([0, 1] for a trigger); 0 for a pad that is not connected or a disabled controller.
        [[nodiscard]] float getGamepadAxisValue(int gamepadIndex, int axis) const;
        [[nodiscard]] float getGamepadRawAxisValue(int gamepadIndex, int axis) const;
        [[nodiscard]] std::string getTextInput() const;

        // Ownership published by the host at frame boundaries, combined with native focus observations.
        // Defaults to focused for standalone and headless/injected input until changed.
        // This does not indicate cursor capture or whether simulation is paused.
        [[nodiscard]] bool hasGameInputFocus() const { return m_gameInputFocus && m_windowFocused; }
        // Host publication; true when ownership changed.
        bool setGameInputFocus(bool focused)
        {
            if (m_gameInputFocus == focused)
                return false;
            m_gameInputFocus = focused;
            return true;
        }

        // Cursor requests: state changes are applied by the adapter on its
        // next poll.
        void setCursorVisible(bool visible);
        [[nodiscard]] bool isCursorVisible() const { return m_cursorVisible; }
        void setCursorCaptured(bool captured);
        [[nodiscard]] bool isCursorCaptured() const { return m_cursorCaptured; }
        // Text input (and IME) request, e.g. while a UI text field holds focus.
        void setTextInputRequested(bool requested) { m_textInputRequested = requested; }
        [[nodiscard]] bool isTextInputRequested() const { return m_textInputRequested; }
        // The composition in progress; it persists across frames until it changes, unlike getTextInput.
        [[nodiscard]] const TextComposition& composition() const { return m_composition; }
        // Drops the composition here and asks the adapter to cancel it in the platform (once).
        void cancelComposition();
        bool consumeClearCompositionRequest();
        // Where the focused text input's insertion point is, in window units (none: no text input has focus). The
        // adapter forwards it to the platform when it changes (TextInputBridge).
        void setTextInputArea(const std::optional<TextInputArea>& area) { m_textInputArea = area; }
        [[nodiscard]] const std::optional<TextInputArea>& textInputArea() const { return m_textInputArea; }
        // Positions the cursor (state + a warp request the adapter consumes).
        void setMousePosition(float x, float y);
        bool consumeMouseWarpRequest(float& outX, float& outY);

        // -- mappings and contexts --
        void mapActionToKey(InternedString actionName, KeyCode key, InputActionType type);
        void mapActionToMouse(InternedString actionName, MouseButton button, InputActionType type);
        void unmapAction(InternedString actionName);
        [[nodiscard]] InputContextId createContext(const InputContextDesc& desc);
        bool destroyContext(InputContextId contextId);
        bool setContextEnabled(InputContextId contextId, bool enabled);
        bool setContextPriority(InputContextId contextId, i32 priority);
        // Contexts with a priority below the floor resolve nothing, as if disabled: the
        // editor mutes gameplay while the game view lacks focus. Raw device state
        // (getKeyState, getMouseState) stays readable.
        void setContextPriorityFloor(i32 floor);
        bool addBinding(InputContextId contextId, const InputBindingDesc& binding);
        bool removeBindings(InputContextId contextId, InternedString actionName);
        // Replaces the context's bindings with a mapping file's (the
        // mappingsFile schema); false when the context or file is unusable.
        bool loadMappings(InputContextId contextId, std::string_view filePath);
        // Withholds a control from every context until reset() — the UI's
        // claim on input it took this frame. Raw key/button queries still
        // report it.
        void claimControl(InputControl control);
        [[nodiscard]] InputMappings mappings() const { return m_actionMap.mappings(); }
        // One context's bindings, its file (the default context's is the mappingsFile config; empty when none was
        // loaded), and the whole-context replace and save the control commands use (InputActionMap).
        [[nodiscard]] InputMappings mappings(InputContextId contextId) const { return m_actionMap.mappings(contextId); }
        [[nodiscard]] std::vector<InputContext> contexts() const { return m_actionMap.contexts(); }
        // Moves with every change of any context's bindings (a load, a replace, a bind or unbind).
        [[nodiscard]] u64 bindingRevision() const { return m_actionMap.bindingRevision(); }
        [[nodiscard]] std::string contextFile(InputContextId contextId) const
        {
            return contextId == DEFAULT_INPUT_CONTEXT_ID ? std::string(m_mappingsFile.toView())
                                                         : m_actionMap.contextFile(contextId);
        }
        bool setContextMappings(InputContextId contextId, const InputMappings& mappings)
        {
            return m_actionMap.setContextMappings(contextId, mappings);
        }
        [[nodiscard]] bool saveContextMappings(InputContextId contextId, std::string_view filePath) const
        {
            return m_actionMap.saveContextMappings(contextId, filePath);
        }
    };
} // namespace spite
