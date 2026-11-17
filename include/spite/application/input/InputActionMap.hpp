#pragma once

#include <functional>
#include <limits>
#include <optional>
#include <type_traits>
#include <string>
#include <unordered_map>
#include <vector>

#include "application/ApplicationExport.hpp"
#include "application/input/InputStateMap.hpp"
#include "application/input/InputTypes.hpp"
#include "base/InternedString.hpp"
#include "base/Platform.hpp"

namespace spite
{
    struct InputContext
    {
        InputContextId id = INVALID_INPUT_CONTEXT_ID;
        InputContextDesc desc{};
    };

    struct InputBindingAction
    {
        InternedString action;
        InputActionType type;
        InputContextId contextId = DEFAULT_INPUT_CONTEXT_ID;

        // for axis - the value of an axis
        // for holding - desired time in seconds
        float scale{};
        // KeyModifier bits; nonzero: resolves only while exactly these are held
        u16 modifiers = 0;

        InputBindingAction(InternedString action, InputActionType type, float scale,
                           InputContextId contextId = DEFAULT_INPUT_CONTEXT_ID, u16 modifiers = 0);

        bool operator==(const InputBindingAction& other) const;
        bool operator!=(const InputBindingAction& other) const;

        struct hash
        {
            sizet operator()(const InputBindingAction& inputBindingAction) const;
        };
    };

    struct InputControlHash
    {
        sizet operator()(const InputControl& control) const
        {
            sizet result = static_cast<sizet>(control.device);
            const auto combine = [&result](const auto value)
            {
                using Value = std::remove_const_t<decltype(value)>;
                result ^= std::hash<Value>{}(value) + 0x9e3779b9 + (result << 6) + (result >> 2);
            };
            combine(control.code);
            combine(control.deviceId);
            return result;
        }
    };

    // Registered action; holds the CURRENT resolved value for this action.
    struct InputAction
    {
        InternedString name;
        // for binary (press/release) 1.0 - activated, 0.0 - inactive
        // for axis - the value of an axis
        // for holding - time in seconds
        float value{};

        explicit InputAction(InternedString name);
    };

    // The input mapping file schema as plain values: the file text is
    // "device" + "key" strings (JSON glue lives inside the application DLL).
    struct InputBindingMapping
    {
        std::string device; // "KEYBOARD", "MOUSE", "GAMEPAD_BUTTON", "GAMEPAD_AXIS"; case-insensitive on load
        std::string key; // key name for keyboard ("W", "LSHIFT", ...); decimal code for the other devices
        float scale = 1.0f;
        std::string modifiers; // a chord's held modifiers, "CTRL+SHIFT" (SHIFT, CTRL, ALT, SUPER); empty for none
    };

    struct InputActionMapping
    {
        std::string action;
        InputActionType type = InputActionType::None;
        std::vector<InputBindingMapping> bindings;
    };

    struct InputMappings
    {
        std::vector<InputActionMapping> actions;
    };

    // The user-layer counterpart of a mapping file under a project layer directory ("config/project/editor/x.json" ->
    // "config/user/editor/x.json", a path with that component pair too); a file outside it maps to
    // "config/user/<file name>"; a user layer file stays. Editor hotkeys are preferences: a rebinding is saved there
    // and read before the project's.
    [[nodiscard]] SPITE_APPLICATION_API std::string userLayerMappingsFile(std::string_view projectFile);

    // Why the mapping file at a content-relative path cannot be loaded (missing, unreadable, not the mapping
    // schema, an action without name, type or bindings); empty when loadMappings would read it. The reason is also
    // logged. A setConfig of the mapping file refuses a path this rejects.
    SPITE_APPLICATION_API std::string inputMappingsFileProblem(std::string_view filePath);
    // A user-layer file's project-layer file (config/user/ -> config/project/, separators as '/'); another path is
    // returned with its separators as '/'.
    [[nodiscard]] SPITE_APPLICATION_API std::string projectLayerMappingsFile(std::string_view userFile);
    // The mapping file at a content-relative path; nullopt when inputMappingsFileProblem names a problem.
    [[nodiscard]] SPITE_APPLICATION_API std::optional<InputMappings> readInputMappings(std::string_view filePath);

    // Action resolution core: bindings (per control, per context) -> action
    // values. Controls report their state through setControlState (keys via
    // triggerKeyInteraction); every control update re-resolves all actions.
    class SPITE_APPLICATION_API InputActionMap
    {
        std::unordered_map<InputControl, std::vector<InputBindingAction>, InputControlHash> m_bindingActionMap;
        std::unordered_map<InternedString, InputAction> m_inputActionsMap;
        std::unordered_map<InputControl, InputState, InputControlHash> m_controlStates;
        std::unordered_map<InputContextId, InputContext> m_contexts;
        InputContextId m_nextContextId = DEFAULT_INPUT_CONTEXT_ID + 1;
        std::unordered_map<InputContextId, std::string> m_contextFiles;

        // actions activated this frame
        std::vector<InputAction> m_activatedActions;
        // actions activated in the previous frame (for tracking releases)
        std::vector<InputAction> m_previousFrameActivatedActions;

        // Resolution indices (see setControlState). m_statesByKey groups
        // reported state controls by (device, code) so wildcard-deviceId
        // bindings find their states; m_actionControls lists the distinct
        // controls bound to each action so resolution never scans unrelated
        // bindings. Rebuilt lazily after any binding mutation.
        std::unordered_map<u32, std::vector<InputControl>> m_statesByKey;
        std::unordered_map<InternedString, std::vector<InputControl>, InternedString::hash> m_actionControls;
        bool m_actionControlsDirty = false;
        u64 m_bindingRevision = 0;
        // Controls claimed for this frame (cleared by reset): no context
        // resolves them.
        std::vector<InputControl> m_claimedControls;
        // contexts below it resolve nothing, as if disabled (setContextPriorityFloor)
        i32 m_contextPriorityFloor = (std::numeric_limits<i32>::min)();

        [[nodiscard]] static u32 stateKey(u8 device, u16 code) { return (static_cast<u32>(device) << 16) | code; }
        void rebuildActionControls();
        void resolveActions();
        [[nodiscard]] bool isClaimed(InputControl control) const;
        [[nodiscard]] bool isLive(const InputContextDesc& context) const
        {
            return context.enabled && context.priority >= m_contextPriorityFloor;
        }
        // KeyModifier bits of the modifier keys down now, claimed or not.
        [[nodiscard]] u16 heldModifiers() const;
        void addMappings(InputContextId contextId, const InputMappings& mappings);

        void registerDefaultContext();

    public:
        InputActionMap();
        // Loads mappings from a mapping file (JSON on disk; glue is DLL-internal).
        explicit InputActionMap(std::string_view inputActionsJsonPath);

        InputContextId createContext(InputContextDesc desc);
        bool destroyContext(InputContextId contextId);
        bool setContextEnabled(InputContextId contextId, bool enabled);
        bool setContextPriority(InputContextId contextId, i32 priority);
        void setContextPriorityFloor(i32 floor);

        void triggerKeyInteraction(u32 key, bool isJustPressed, bool isDown, float holdingTime);

        // Advances the stored hold time of an already-down control WITHOUT
        // touching its edge flags (isChanged/wasPressed), then re-resolves.
        // The per-frame hold-time re-feed must use this, not setControlState:
        // replacing the state would erase a press edge that arrived earlier
        // in the same frame.
        void updateControlHoldTime(InputControl control, float holdTime);

        void setControlState(InputControl control, InputState state);
        // Withholds the control from every context until reset(), e.g. a key
        // the UI took this frame; its stored state is untouched.
        void claimControl(InputControl control);

        [[nodiscard]] const std::vector<InputAction>& activatedActions();
        [[nodiscard]] const std::vector<InputAction>& previousFrameActivatedActions() const;

        void addBinding(InternedString actionName, u32 scancode, InputActionType type, float scale = 1.0f);
        void addBinding(InternedString actionName, InputControl control, InputActionType type, float scale = 1.0f);
        void addBinding(InputContextId contextId, InternedString actionName, InputControl control, InputActionType type,
                        float scale = 1.0f, u16 modifiers = 0);
        void removeBinding(InternedString actionName, u32 scancode);
        void removeAllBindings(InternedString actionName);
        bool removeBindings(InputContextId contextId, InternedString actionName);

        [[nodiscard]] bool hasAction(InternedString name) const;
        // Some action is active now (resolved, not claimed); false is the cheap answer for an idle frame.
        [[nodiscard]] bool anyActionActive() const;
        // The controls the action is bound to in any context (cleared first); one per control, a wildcard device id
        // stays -1.
        void actionControls(InternedString name, std::vector<InputControl>& out) const;
        [[nodiscard]] bool isActionActive(InternedString name) const;
        [[nodiscard]] bool wasActionActive(InternedString name) const;
        [[nodiscard]] float actionValue(InternedString name) const;

        // Schema snapshot of every binding, grouped by action.
        [[nodiscard]] InputMappings mappings() const;
        // Counts every change of any context's bindings: a client that shows them re-reads when it moves.
        [[nodiscard]] u64 bindingRevision() const { return m_bindingRevision; }
        // The same for one context's bindings; empty for an unknown context.
        [[nodiscard]] InputMappings mappings(InputContextId contextId) const;
        // The contexts, the default one first, then by creation.
        [[nodiscard]] std::vector<InputContext> contexts() const;
        // The file loadMappings last read into the context, as it was given; empty when none.
        [[nodiscard]] std::string contextFile(InputContextId contextId) const;
        // Replaces the context's bindings with the given mappings (an action keeps its live value); false when the
        // context is unknown.
        bool setContextMappings(InputContextId contextId, const InputMappings& mappings);
        // Writes the context's bindings to a mapping file; false when it cannot be written (reason: the log).
        [[nodiscard]] bool saveContextMappings(InputContextId contextId, std::string_view filePath) const;
        // Replaces all bindings and actions with the given mappings (contexts are kept).
        void applyMappings(const InputMappings& mappings);
        // Replaces the context's bindings with a mapping file's; false when
        // the context is unknown or the file does not read.
        bool loadMappings(InputContextId contextId, std::string_view filePath);

        // call this every frame before input processing
        void reset();
        void clear();

        // Saves the current mappings to a mapping file. Returns write success.
        [[nodiscard]] bool saveToFile(std::string_view filePath) const;
    };
} // namespace spite
