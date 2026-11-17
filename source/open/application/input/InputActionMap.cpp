#include "base/WritableLayout.hpp"
#include "application/input/InputActionMap.hpp"
#include "base/ContentRoot.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <limits>
#include <optional>
#include <tuple>

#include "application/ApplicationEvents.hpp"
#include "application/input/InputInjection.hpp"
#include "base/Assert.hpp"
#include "base/io/FileSystem.hpp"
#include "serialization/Serializer.hpp"

namespace spite
{
    // The mapping file as schema values; the action type is its file text
    // ("PRESS"/"Press"), the public InputActionMapping carries the enum.
    namespace
    {
        struct InputBindingFile
        {
            std::string device;
            std::string key;
            float scale = 1.0f;
            std::string modifiers;

            SPITE_SCHEMA(InputBindingFile, device, key, scale, modifiers)
        };

        // Missing keys keep their value, so a list holding this placeholder after a read was absent from the file.
        constexpr std::string_view ABSENT = "\x01"
                                            "absent";

        struct InputActionFile
        {
            std::string action;
            std::string type;
            std::vector<InputBindingFile> bindings{InputBindingFile{.key = std::string(ABSENT)}};

            SPITE_SCHEMA(InputActionFile, action, type, bindings)
        };

        struct InputMappingsFile
        {
            std::vector<InputActionFile> actions{InputActionFile{.action = std::string(ABSENT)}};

            SPITE_SCHEMA(InputMappingsFile, actions)
        };

        InputActionType inputActionTypeFromString(std::string_view inputStr)
        {
            if (inputStr == "PRESS" || inputStr == "Press")
                return InputActionType::Press;
            if (inputStr == "RELEASE" || inputStr == "Release")
                return InputActionType::Release;
            if (inputStr == "HOLD" || inputStr == "Hold")
                return InputActionType::Hold;
            if (inputStr == "AXIS" || inputStr == "Axis")
                return InputActionType::Axis;

            SASSERTM(false, "Parsed input action type string {} is invalid ", inputStr);
            return InputActionType::None;
        }

        std::string inputActionTypeToString(InputActionType type)
        {
            switch (type)
            {
            case InputActionType::Press:
                return "PRESS";
            case InputActionType::Release:
                return "RELEASE";
            case InputActionType::Hold:
                return "HOLD";
            case InputActionType::Axis:
                return "AXIS";
            default:
                return "NO_ACTION";
            }
        }

        struct KeyNamePair
        {
            const char* name;
            u32 code;
        };

        static const KeyNamePair KEY_NAME_TABLE[] = {{"EQUALS", static_cast<u32>(KeyCode::EQUALS)},
                                                     {"MINUS", static_cast<u32>(KeyCode::MINUS)},
                                                     {"KP_PLUS", static_cast<u32>(KeyCode::KP_PLUS)},
                                                     {"KP_MINUS", static_cast<u32>(KeyCode::KP_MINUS)},
                                                     {"KP_0", static_cast<u32>(KeyCode::KP_0)},
                                                     {"KP_EQUALS", static_cast<u32>(KeyCode::KP_EQUALS)},
                                                     {"F1", static_cast<u32>(KeyCode::F1)},
                                                     {"F2", static_cast<u32>(KeyCode::F2)},
                                                     {"F3", static_cast<u32>(KeyCode::F3)},
                                                     {"F4", static_cast<u32>(KeyCode::F4)},
                                                     {"F5", static_cast<u32>(KeyCode::F5)},
                                                     {"F6", static_cast<u32>(KeyCode::F6)},
                                                     {"F7", static_cast<u32>(KeyCode::F7)},
                                                     {"F8", static_cast<u32>(KeyCode::F8)},
                                                     {"F9", static_cast<u32>(KeyCode::F9)},
                                                     {"F10", static_cast<u32>(KeyCode::F10)},
                                                     {"F11", static_cast<u32>(KeyCode::F11)},
                                                     {"F12", static_cast<u32>(KeyCode::F12)},
                                                     {"UP", static_cast<u32>(KeyCode::UP)},
                                                     {"DOWN", static_cast<u32>(KeyCode::DOWN)},
                                                     {"LEFT", static_cast<u32>(KeyCode::LEFT)},
                                                     {"RIGHT", static_cast<u32>(KeyCode::RIGHT)},
                                                     {"RETURN", static_cast<u32>(KeyCode::RETURN)},
                                                     {"ENTER", static_cast<u32>(KeyCode::RETURN)},
                                                     {"ESCAPE", static_cast<u32>(KeyCode::ESCAPE)},
                                                     {"ESC", static_cast<u32>(KeyCode::ESCAPE)},
                                                     {"BACKSPACE", static_cast<u32>(KeyCode::BACKSPACE)},
                                                     {"TAB", static_cast<u32>(KeyCode::TAB)},
                                                     {"SPACE", static_cast<u32>(KeyCode::SPACE)},
                                                     {"DELETE", static_cast<u32>(KeyCode::DEL)},
                                                     {"LSHIFT", static_cast<u32>(KeyCode::LSHIFT)},
                                                     {"RSHIFT", static_cast<u32>(KeyCode::RSHIFT)},
                                                     {"LCTRL", static_cast<u32>(KeyCode::LCTRL)},
                                                     {"RCTRL", static_cast<u32>(KeyCode::RCTRL)},
                                                     {"LALT", static_cast<u32>(KeyCode::LALT)},
                                                     {"RALT", static_cast<u32>(KeyCode::RALT)},
                                                     {"HOME", static_cast<u32>(KeyCode::HOME)},
                                                     {"END", static_cast<u32>(KeyCode::END)},
                                                     {"PAGEUP", static_cast<u32>(KeyCode::PAGEUP)},
                                                     {"PAGEDOWN", static_cast<u32>(KeyCode::PAGEDOWN)},
                                                     {"INSERT", static_cast<u32>(KeyCode::INSERT)}};

        // Engine key-code names for the mapping schema. Values mirror the
        // platform scancode layout (see KeyCode), but the table is plain data.
        std::optional<u32> findKeyCode(std::string_view str)
        {
            if (str.empty() || str[0] == '\0')
                return static_cast<u32>(KeyCode::UNKNOWN);

            if (str.length() == 1)
            {
                const char c = str[0];
                if (c >= 'a' && c <= 'z')
                {
                    return static_cast<u32>(static_cast<u32>(KeyCode::A) + (c - 'a'));
                }
                if (c >= 'A' && c <= 'Z')
                {
                    return static_cast<u32>(static_cast<u32>(KeyCode::A) + (c - 'A'));
                }
                if (c > '0' && c <= '9')
                {
                    return static_cast<u32>(static_cast<u32>(KeyCode::NUM_1) + (c - '1'));
                }
                if (c == '0')
                {
                    return static_cast<u32>(KeyCode::NUM_0);
                }
            }


            // the names are case-insensitive
            std::string upper(str);
            std::ranges::transform(upper, upper.begin(), [](const char c)
                                   { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
            for (const auto& pair : KEY_NAME_TABLE)
            {
                if (upper == pair.name)
                {
                    return pair.code;
                }
            }

            // a key outside the name table is written KEY_<scancode> (keyCodeToString): it reads back as that code
            constexpr std::string_view CODE_PREFIX = "KEY_";
            constexpr u32 MAX_SCANCODE = 511;
            if (upper.starts_with(CODE_PREFIX) && upper.size() > CODE_PREFIX.size())
            {
                u32 code = 0;
                const char* const first = upper.data() + CODE_PREFIX.size();
                const char* const last = upper.data() + upper.size();
                const auto [end, error] = std::from_chars(first, last, code);
                if (error == std::errc{} && end == last && code <= MAX_SCANCODE)
                {
                    return code;
                }
            }

            return std::nullopt;
        }

        u32 keyCodeFromString(const std::string_view str)
        {
            const std::optional<u32> code = findKeyCode(str);
            SASSERTM(code.has_value(), "Parsed input key string {} is invalid", str);
            return code.value_or(static_cast<u32>(KeyCode::UNKNOWN));
        }

        std::string keyCodeToString(u32 keyCode)
        {
            struct CodeNamePair
            {
                u32 code;
                const char* name;
            };
            static const CodeNamePair keyCodeMap[] = {{static_cast<u32>(KeyCode::A), "A"},
                                                      {static_cast<u32>(KeyCode::B), "B"},
                                                      {static_cast<u32>(KeyCode::C), "C"},
                                                      {static_cast<u32>(KeyCode::D), "D"},
                                                      {static_cast<u32>(KeyCode::E), "E"},
                                                      {static_cast<u32>(KeyCode::F), "F"},
                                                      {static_cast<u32>(KeyCode::G), "G"},
                                                      {static_cast<u32>(KeyCode::H), "H"},
                                                      {static_cast<u32>(KeyCode::I), "I"},
                                                      {static_cast<u32>(KeyCode::J), "J"},
                                                      {static_cast<u32>(KeyCode::K), "K"},
                                                      {static_cast<u32>(KeyCode::L), "L"},
                                                      {static_cast<u32>(KeyCode::M), "M"},
                                                      {static_cast<u32>(KeyCode::N), "N"},
                                                      {static_cast<u32>(KeyCode::O), "O"},
                                                      {static_cast<u32>(KeyCode::P), "P"},
                                                      {static_cast<u32>(KeyCode::Q), "Q"},
                                                      {static_cast<u32>(KeyCode::R), "R"},
                                                      {static_cast<u32>(KeyCode::S), "S"},
                                                      {static_cast<u32>(KeyCode::T), "T"},
                                                      {static_cast<u32>(KeyCode::U), "U"},
                                                      {static_cast<u32>(KeyCode::V), "V"},
                                                      {static_cast<u32>(KeyCode::W), "W"},
                                                      {static_cast<u32>(KeyCode::X), "X"},
                                                      {static_cast<u32>(KeyCode::Y), "Y"},
                                                      {static_cast<u32>(KeyCode::Z), "Z"},
                                                      {static_cast<u32>(KeyCode::NUM_1), "1"},
                                                      {static_cast<u32>(KeyCode::NUM_2), "2"},
                                                      {static_cast<u32>(KeyCode::NUM_3), "3"},
                                                      {static_cast<u32>(KeyCode::NUM_4), "4"},
                                                      {static_cast<u32>(KeyCode::NUM_5), "5"},
                                                      {static_cast<u32>(KeyCode::NUM_6), "6"},
                                                      {static_cast<u32>(KeyCode::NUM_7), "7"},
                                                      {static_cast<u32>(KeyCode::NUM_8), "8"},
                                                      {static_cast<u32>(KeyCode::NUM_9), "9"},
                                                      {static_cast<u32>(KeyCode::NUM_0), "0"},
                                                      {static_cast<u32>(KeyCode::EQUALS), "EQUALS"},
                                                      {static_cast<u32>(KeyCode::MINUS), "MINUS"},
                                                      {static_cast<u32>(KeyCode::KP_PLUS), "KP_PLUS"},
                                                      {static_cast<u32>(KeyCode::KP_MINUS), "KP_MINUS"},
                                                      {static_cast<u32>(KeyCode::KP_0), "KP_0"},
                                                      {static_cast<u32>(KeyCode::KP_EQUALS), "KP_EQUALS"},
                                                      {static_cast<u32>(KeyCode::F1), "F1"},
                                                      {static_cast<u32>(KeyCode::F2), "F2"},
                                                      {static_cast<u32>(KeyCode::F3), "F3"},
                                                      {static_cast<u32>(KeyCode::F4), "F4"},
                                                      {static_cast<u32>(KeyCode::F5), "F5"},
                                                      {static_cast<u32>(KeyCode::F6), "F6"},
                                                      {static_cast<u32>(KeyCode::F7), "F7"},
                                                      {static_cast<u32>(KeyCode::F8), "F8"},
                                                      {static_cast<u32>(KeyCode::F9), "F9"},
                                                      {static_cast<u32>(KeyCode::F10), "F10"},
                                                      {static_cast<u32>(KeyCode::F11), "F11"},
                                                      {static_cast<u32>(KeyCode::F12), "F12"},
                                                      {static_cast<u32>(KeyCode::UP), "UP"},
                                                      {static_cast<u32>(KeyCode::DOWN), "DOWN"},
                                                      {static_cast<u32>(KeyCode::LEFT), "LEFT"},
                                                      {static_cast<u32>(KeyCode::RIGHT), "RIGHT"},
                                                      {static_cast<u32>(KeyCode::RETURN), "RETURN"},
                                                      {static_cast<u32>(KeyCode::ESCAPE), "ESCAPE"},
                                                      {static_cast<u32>(KeyCode::BACKSPACE), "BACKSPACE"},
                                                      {static_cast<u32>(KeyCode::TAB), "TAB"},
                                                      {static_cast<u32>(KeyCode::SPACE), "SPACE"},
                                                      {static_cast<u32>(KeyCode::DEL), "DELETE"},
                                                      {static_cast<u32>(KeyCode::LSHIFT), "LSHIFT"},
                                                      {static_cast<u32>(KeyCode::RSHIFT), "RSHIFT"},
                                                      {static_cast<u32>(KeyCode::LCTRL), "LCTRL"},
                                                      {static_cast<u32>(KeyCode::RCTRL), "RCTRL"},
                                                      {static_cast<u32>(KeyCode::LALT), "LALT"},
                                                      {static_cast<u32>(KeyCode::RALT), "RALT"},
                                                      {static_cast<u32>(KeyCode::HOME), "HOME"},
                                                      {static_cast<u32>(KeyCode::END), "END"},
                                                      {static_cast<u32>(KeyCode::PAGEUP), "PAGEUP"},
                                                      {static_cast<u32>(KeyCode::PAGEDOWN), "PAGEDOWN"},
                                                      {static_cast<u32>(KeyCode::INSERT), "INSERT"}};

            for (const auto& pair : keyCodeMap)
            {
                if (pair.code == keyCode)
                {
                    return pair.name;
                }
            }

            char buf[32];
            snprintf(buf, sizeof(buf), "KEY_%u", keyCode);
            return buf;
        }

        InputMappings mappingsFromFile(const InputMappingsFile& file)
        {
            InputMappings mappings;
            mappings.actions.reserve(file.actions.size());
            for (const auto& action : file.actions)
            {
                InputActionMapping mapping;
                mapping.action = action.action;
                mapping.type = inputActionTypeFromString(action.type);
                mapping.bindings.reserve(action.bindings.size());
                for (const auto& binding : action.bindings)
                {
                    mapping.bindings.push_back(
                        InputBindingMapping{binding.device, binding.key, binding.scale, binding.modifiers});
                }
                mappings.actions.push_back(std::move(mapping));
            }
            return mappings;
        }

        InputMappingsFile fileFromMappings(const InputMappings& mappings)
        {
            InputMappingsFile file;
            file.actions.clear();
            file.actions.reserve(mappings.actions.size());
            for (const auto& mapping : mappings.actions)
            {
                InputActionFile action;
                action.bindings.clear();
                action.action = mapping.action;
                action.type = inputActionTypeToString(mapping.type);
                action.bindings.reserve(mapping.bindings.size());
                for (const auto& binding : mapping.bindings)
                {
                    action.bindings.push_back(
                        InputBindingFile{binding.device, binding.key, binding.scale, binding.modifiers});
                }
                file.actions.push_back(std::move(action));
            }
            return file;
        }

        // why: the reason a file is refused, as asked (the log has the resolved path too: a control client is told
        // the path it sent, never the machine's directory layout)
        std::optional<InputMappings> readMappingsFile(std::string_view filePath, std::string* why = nullptr)
        {
            const auto refuse = [why](std::string message, std::string asked = {})
            {
                SWARNING_LOG("[InputActionMap] {}", message);
                if (why)
                {
                    *why = asked.empty() ? std::move(message) : std::move(asked);
                }
                return std::optional<InputMappings>{};
            };
            const auto text = readTextFile(resolveUserConfigPath(pathFromUtf8(filePath)));
            if (!text.has_value())
            {
                return refuse(text.error().message, std::format("{}: the file cannot be read", filePath));
            }
            InputMappingsFile file;
            std::string error;
            if (!deserializeJson(InputMappingsFile::spiteSchema(), &file, text.value(), error))
            {
                return refuse(std::format("{}: {}", filePath, error));
            }
            if (file.actions.size() == 1 && file.actions[0].action == ABSENT)
            {
                return refuse(std::format("{}: no \"actions\" list.", filePath));
            }
            const bool complete =
                std::ranges::all_of(file.actions,
                                    [](const InputActionFile& action)
                                    {
                                        return !action.action.empty() && !action.type.empty() &&
                                            !(action.bindings.size() == 1 && action.bindings[0].key == ABSENT);
                                    });
            if (!complete)
            {
                return refuse(std::format("{}: an action lacks its name, type or bindings.", filePath));
            }
            return mappingsFromFile(file);
        }

        bool writeMappingsFile(std::string_view filePath, const InputMappings& mappings)
        {
            std::string text;
            const InputMappingsFile file = fileFromMappings(mappings);
            serializeJson(InputMappingsFile::spiteSchema(), &file, text, 2);
            text += '\n';
            const auto result = writeTextFileAtomically(pathFromUtf8(filePath), text);
            if (!result.has_value())
            {
                SERROR_LOG("[InputActionMap] failed to save to file {}", result.error().message.c_str());
                return false;
            }
            return true;
        }

        std::string toUpperAscii(std::string_view str)
        {
            std::string upper(str);
            for (char& c : upper)
            {
                if (c >= 'a' && c <= 'z')
                {
                    c = static_cast<char>(c - 'a' + 'A');
                }
            }
            return upper;
        }

        constexpr std::pair<std::string_view, u16> MODIFIER_NAMES[]{{"SHIFT", KeyModifier::SHIFT},
                                                                    {"CTRL", KeyModifier::CTRL},
                                                                    {"ALT", KeyModifier::ALT},
                                                                    {"SUPER", KeyModifier::SUPER}};

        // "CTRL+SHIFT" -> KeyModifier bits; nullopt for an unknown name.
        std::optional<u16> modifiersFromString(const std::string_view text)
        {
            u16 bits = 0;
            for (sizet start = 0; start < text.size();)
            {
                const sizet end = std::min(text.find('+', start), text.size());
                const std::string name = toUpperAscii(text.substr(start, end - start));
                const auto it =
                    std::ranges::find(MODIFIER_NAMES, std::string_view(name), &std::pair<std::string_view, u16>::first);
                if (it == std::end(MODIFIER_NAMES))
                {
                    SWARNING_LOG("[InputActionMap] unknown modifier '{}' in '{}'.", name, text);
                    return std::nullopt;
                }
                bits |= it->second;
                start = end + 1;
            }
            return bits;
        }

        std::string modifiersToString(const u16 bits)
        {
            std::string text;
            for (const auto& [name, bit] : MODIFIER_NAMES)
            {
                if ((bits & bit) != 0)
                {
                    text += text.empty() ? "" : "+";
                    text += name;
                }
            }
            return text;
        }

        // Resolves one schema binding to a control; nullopt for entries that
        // must be skipped (unparseable/out-of-range code), per-file tolerance.
        std::optional<InputControl> controlFromBinding(const InputBindingMapping& binding)
        {
            const std::string device = toUpperAscii(binding.device);
            const bool isMouseBinding = device == "MOUSE";
            const bool isGamepadButtonBinding = device == "GAMEPAD_BUTTON";
            const bool isGamepadAxisBinding = device == "GAMEPAD_AXIS";
            if (!isMouseBinding && !isGamepadButtonBinding && !isGamepadAxisBinding)
            {
                return InputControl{.device = InputControlDevice::Keyboard,
                                    .code = static_cast<u16>(keyCodeFromString(binding.key))};
            }

            unsigned int parsedCode = 0;
            const auto [end, error] =
                std::from_chars(binding.key.data(), binding.key.data() + binding.key.size(), parsedCode);
            const unsigned int maxControlCode = isMouseBinding ? 4u : 255u;
            if (error != std::errc{} || end != binding.key.data() + binding.key.size() || parsedCode > maxControlCode)
            {
                SWARNING_LOG("[InputActionMap] invalid device binding '{}'.", binding.key);
                return std::nullopt;
            }
            return InputControl{.device = isMouseBinding     ? InputControlDevice::MouseButton
                                    : isGamepadButtonBinding ? InputControlDevice::GamepadButton
                                                             : InputControlDevice::GamepadAxis,
                                .code = static_cast<u16>(parsedCode)};
        }
    } // namespace

    std::vector<std::string> keyNames()
    {
        std::vector<std::string> names;
        for (char letter = 'A'; letter <= 'Z'; ++letter)
        {
            names.emplace_back(1, letter);
        }
        for (char digit = '0'; digit <= '9'; ++digit)
        {
            names.emplace_back(1, digit);
        }
        for (const KeyNamePair& pair : KEY_NAME_TABLE)
        {
            names.emplace_back(pair.name);
        }
        return names;
    }

    std::string keyNameOf(const u32 code) { return keyCodeToString(code); }

    std::optional<u32> keyCodeFromName(const std::string_view name)
    {
        const std::optional<u32> code = findKeyCode(name);
        return code && *code != static_cast<u32>(KeyCode::UNKNOWN) ? code : std::nullopt;
    }

    bool InputStateMap::isDown(const u32 scancode) const
    {
        const auto it = m_stateMap.find(scancode);
        return it != m_stateMap.end() && it->second.isPressed;
    }

    bool InputStateMap::isJustPressed(const u32 scancode) const
    {
        const auto it = m_stateMap.find(scancode);
        return it != m_stateMap.end() && it->second.isPressed && !it->second.wasPressed;
    }

    bool InputStateMap::isJustReleased(const u32 scancode) const
    {
        const auto it = m_stateMap.find(scancode);
        return it != m_stateMap.end() && !it->second.isPressed && it->second.wasPressed;
    }

    float InputStateMap::holdingTime(const u32 scancode) const
    {
        const auto it = m_stateMap.find(scancode);
        return it != m_stateMap.end() ? it->second.holdingTime : 0.0f;
    }

    void InputStateMap::setButtonState(const u32 scancode, const bool pressed)
    {
        registerKey(scancode);
        auto& button = m_stateMap[scancode];
        button.isPressed = pressed;
        if (!pressed)
        {
            button.holdingTime = 0.0f;
        }
    }

    void InputStateMap::setButtonPress(const u32 scancode)
    {
        registerKey(scancode);
        auto& state = m_stateMap[scancode];
        state.wasPressed = state.isPressed;
        state.isPressed = true;
    }

    void InputStateMap::setButtonRelease(const u32 scancode)
    {
        registerKey(scancode);
        auto& state = m_stateMap[scancode];
        state.wasPressed = state.isPressed;
        state.isPressed = false;
        state.holdingTime = 0.0f;
    }

    void InputStateMap::update(const float deltaTime)
    {
        for (auto& [_, state] : m_stateMap)
        {
            if (state.isPressed)
            {
                state.holdingTime += deltaTime;
            }
        }
    }

    void InputStateMap::reset()
    {
        for (auto& [_, state] : m_stateMap)
        {
            state.wasPressed = state.isPressed;
        }
    }

    void InputStateMap::clear() { m_stateMap.clear(); }

    void InputStateMap::registerKey(const u32 scancode) { m_stateMap.try_emplace(scancode, ButtonState{}); }

    // --- InputActionMap ---

    InputBindingAction::InputBindingAction(InternedString action, const InputActionType type, const float scale,
                                           const InputContextId contextId, const u16 modifiers) :
        action(action), type(type), contextId(contextId), scale(scale), modifiers(modifiers)
    {
    }

    bool InputBindingAction::operator==(const InputBindingAction& other) const
    {
        return action == other.action && type == other.type;
    }

    bool InputBindingAction::operator!=(const InputBindingAction& other) const { return !(*this == other); }

    sizet InputBindingAction::hash::operator()(const InputBindingAction& inputBindingAction) const
    {
        return InternedString::hash()(inputBindingAction.action);
    }

    InputAction::InputAction(InternedString name) : name(name) {}

    bool operator==(const InputAction& lhs, const InputAction& rhs) { return lhs.name == rhs.name; }

    bool operator!=(const InputAction& lhs, const InputAction& rhs) { return !(lhs == rhs); }

    void InputActionMap::registerDefaultContext()
    {
        m_contexts.emplace(
            DEFAULT_INPUT_CONTEXT_ID,
            InputContext{.id = DEFAULT_INPUT_CONTEXT_ID, .desc = InputContextDesc{.name = InternedString("default")}});
    }

    InputActionMap::InputActionMap() { registerDefaultContext(); }

    InputActionMap::InputActionMap(const std::string_view inputActionsJsonPath)
    {
        registerDefaultContext();
        if (auto mappings = readMappingsFile(inputActionsJsonPath))
        {
            applyMappings(mappings.value());
        }
    }

    void InputActionMap::applyMappings(const InputMappings& mappings)
    {
        // the default context's bindings only: other contexts keep theirs
        for (auto& [_, bindings] : m_bindingActionMap)
        {
            std::erase_if(bindings, [](const InputBindingAction& binding)
                          { return binding.contextId == DEFAULT_INPUT_CONTEXT_ID; });
        }
        std::erase_if(m_bindingActionMap, [](const auto& entry) { return entry.second.empty(); });
        m_controlStates.clear();
        m_statesByKey.clear();
        m_activatedActions.clear();
        m_previousFrameActivatedActions.clear();
        m_actionControlsDirty = true;
        ++m_bindingRevision;

        addMappings(DEFAULT_INPUT_CONTEXT_ID, mappings);
    }

    void InputActionMap::addMappings(const InputContextId contextId, const InputMappings& mappings)
    {
        for (const auto& inputAction : mappings.actions)
        {
            const InternedString actionName(inputAction.action);
            m_inputActionsMap.try_emplace(actionName, InputAction(actionName));

            for (const auto& binding : inputAction.bindings)
            {
                const auto control = controlFromBinding(binding);
                const auto modifiers = modifiersFromString(binding.modifiers);
                if (!control || !modifiers)
                {
                    continue;
                }
                m_bindingActionMap[*control].push_back(
                    InputBindingAction(actionName, inputAction.type, binding.scale, contextId, *modifiers));
            }
        }
        m_actionControlsDirty = true;
        ++m_bindingRevision;
    }

    std::string inputMappingsFileProblem(const std::string_view filePath)
    {
        std::string why;
        return readMappingsFile(filePath, &why) ? std::string{} : why;
    }

    bool InputActionMap::loadMappings(const InputContextId contextId, const std::string_view filePath)
    {
        if (!m_contexts.contains(contextId))
        {
            return false;
        }
        const std::optional<InputMappings> mappings = readMappingsFile(filePath);
        if (!mappings)
        {
            return false;
        }
        for (auto& [_, bindings] : m_bindingActionMap)
        {
            std::erase_if(bindings,
                          [contextId](const InputBindingAction& binding) { return binding.contextId == contextId; });
        }
        addMappings(contextId, *mappings);
        m_contextFiles[contextId] = std::string(filePath);
        return true;
    }

    std::string InputActionMap::contextFile(const InputContextId contextId) const
    {
        const auto it = m_contextFiles.find(contextId);
        return it == m_contextFiles.end() ? std::string{} : it->second;
    }

    std::vector<InputContext> InputActionMap::contexts() const
    {
        std::vector<InputContext> list;
        list.reserve(m_contexts.size());
        for (const auto& [_, context] : m_contexts)
        {
            list.push_back(context);
        }
        std::ranges::sort(list, {}, &InputContext::id);
        return list;
    }

    bool InputActionMap::setContextMappings(const InputContextId contextId, const InputMappings& mappings)
    {
        if (!m_contexts.contains(contextId))
        {
            return false;
        }
        for (auto& [_, bindings] : m_bindingActionMap)
        {
            std::erase_if(bindings,
                          [contextId](const InputBindingAction& binding) { return binding.contextId == contextId; });
        }
        std::erase_if(m_bindingActionMap, [](const auto& entry) { return entry.second.empty(); });
        addMappings(contextId, mappings);
        return true;
    }

    bool InputActionMap::saveContextMappings(const InputContextId contextId, const std::string_view filePath) const
    {
        if (!m_contexts.contains(contextId))
        {
            return false;
        }
        std::error_code ignored;
        std::filesystem::create_directories(pathFromUtf8(filePath).parent_path(), ignored);
        return writeMappingsFile(filePath, mappings(contextId));
    }

    std::optional<InputMappings> readInputMappings(const std::string_view filePath)
    {
        return readMappingsFile(filePath);
    }

    std::string projectLayerMappingsFile(const std::string_view userFile)
    {
        std::string text(userFile);
        std::ranges::replace(text, '\\', '/');
        constexpr std::string_view USER = "config/user/";
        if (const sizet at = text.find(USER); at != std::string::npos)
        {
            text.replace(at, USER.size(), "config/project/");
        }
        return text;
    }

    std::string userLayerMappingsFile(const std::string_view projectFile)
    {
        std::string text(projectFile);
        std::ranges::replace(text, '\\', '/');
        constexpr std::string_view PROJECT = "config/project/";
        if (text.contains("config/user/"))
        {
            return text;
        }
        if (const sizet at = text.find(PROJECT); at != std::string::npos)
        {
            return text.replace(at, PROJECT.size(), "config/user/");
        }
        return "config/user/" + text.substr(text.rfind('/') == std::string::npos ? 0 : text.rfind('/') + 1);
    }

    InputMappings InputActionMap::mappings() const { return mappings(INVALID_INPUT_CONTEXT_ID); }

    // contextId INVALID: every context's bindings
    InputMappings InputActionMap::mappings(const InputContextId contextId) const
    {
        // Group the binding-action map by action; the schema has one type per
        // action, taken from the action's first binding.
        std::unordered_map<InternedString, std::vector<std::pair<InputControl, InputBindingAction>>> groupedByAction;
        for (const auto& [control, bindings] : m_bindingActionMap)
        {
            for (const auto& bindingAction : bindings)
            {
                if (contextId == INVALID_INPUT_CONTEXT_ID || bindingAction.contextId == contextId)
                {
                    groupedByAction[bindingAction.action].emplace_back(control, bindingAction);
                }
            }
        }

        InputMappings result;
        result.actions.reserve(groupedByAction.size());
        for (const auto& [actionName, bindings] : groupedByAction)
        {
            InputActionMapping mapping;
            mapping.action = std::string(actionName.toView());
            if (!bindings.empty())
            {
                mapping.type = bindings[0].second.type;
                mapping.bindings.reserve(bindings.size());
                for (const auto& [control, bindingAction] : bindings)
                {
                    mapping.bindings.push_back(InputBindingMapping{
                        control.device == InputControlDevice::MouseButton         ? "MOUSE"
                            : control.device == InputControlDevice::GamepadButton ? "GAMEPAD_BUTTON"
                            : control.device == InputControlDevice::GamepadAxis   ? "GAMEPAD_AXIS"
                                                                                  : "KEYBOARD",
                        control.device == InputControlDevice::Keyboard ? keyCodeToString(control.code)
                                                                       : std::to_string(control.code),
                        bindingAction.scale, modifiersToString(bindingAction.modifiers)});
                }
                // the control map is unordered: a stable order keeps a save of unchanged bindings byte-identical
                std::ranges::sort(mapping.bindings,
                                  [](const InputBindingMapping& a, const InputBindingMapping& b)
                                  {
                                      return std::tie(a.device, a.key, a.modifiers, a.scale) <
                                          std::tie(b.device, b.key, b.modifiers, b.scale);
                                  });
            }
            result.actions.push_back(std::move(mapping));
        }
        std::ranges::sort(result.actions, {}, &InputActionMapping::action);
        return result;
    }

    InputContextId InputActionMap::createContext(InputContextDesc desc)
    {
        if (!desc.name.isValid())
        {
            return INVALID_INPUT_CONTEXT_ID;
        }

        const InputContextId contextId = m_nextContextId++;
        m_contexts.emplace(contextId, InputContext{.id = contextId, .desc = std::move(desc)});
        return contextId;
    }

    bool InputActionMap::destroyContext(const InputContextId contextId)
    {
        if (contextId == DEFAULT_INPUT_CONTEXT_ID || m_contexts.erase(contextId) == 0)
        {
            return false;
        }

        for (auto& [_, bindings] : m_bindingActionMap)
        {
            std::erase_if(bindings,
                          [contextId](const InputBindingAction& binding) { return binding.contextId == contextId; });
        }
        m_actionControlsDirty = true;
        ++m_bindingRevision;
        return true;
    }

    bool InputActionMap::setContextEnabled(const InputContextId contextId, const bool enabled)
    {
        const auto contextIt = m_contexts.find(contextId);
        if (contextIt == m_contexts.end())
        {
            return false;
        }
        contextIt->second.desc.enabled = enabled;
        return true;
    }

    void InputActionMap::setContextPriorityFloor(const i32 floor)
    {
        if (floor != m_contextPriorityFloor)
        {
            m_contextPriorityFloor = floor;
            resolveActions();
        }
    }

    bool InputActionMap::setContextPriority(const InputContextId contextId, const i32 priority)
    {
        const auto contextIt = m_contexts.find(contextId);
        if (contextIt == m_contexts.end())
        {
            return false;
        }
        contextIt->second.desc.priority = priority;
        return true;
    }

    void InputActionMap::triggerKeyInteraction(const u32 scancode, const bool isJustPressed, const bool isDown,
                                               const float holdingTime)
    {
        InputState state{};
        state.isPressed = isDown;
        state.wasPressed = !isJustPressed && !isDown;
        state.holdTime = isDown ? holdingTime : 0.0f;
        state.value = isDown ? 1.0f : 0.0f;
        state.isChanged = isJustPressed || (!isDown && static_cast<bool>(holdingTime));
        setControlState(InputControl{.device = InputControlDevice::Keyboard, .code = static_cast<u16>(scancode)},
                        state);
    }

    void InputActionMap::updateControlHoldTime(const InputControl control, const float holdTime)
    {
        const auto it = m_controlStates.find(control);
        if (it == m_controlStates.end())
        {
            return;
        }
        it->second.holdTime = holdTime;
        setControlState(control, it->second);
    }

    void InputActionMap::setControlState(const InputControl control, const InputState state)
    {
        const auto [stateIt, inserted] = m_controlStates.try_emplace(control, state);
        if (inserted)
        {
            m_statesByKey[stateKey(static_cast<u8>(control.device), control.code)].push_back(control);
        }
        else
        {
            stateIt->second = state;
        }
        resolveActions();
    }

    // a claim without a device id (-1) holds the control on every device
    bool InputActionMap::isClaimed(const InputControl control) const
    {
        return std::ranges::any_of(m_claimedControls,
                                   [control](const InputControl& claimed)
                                   {
                                       return claimed.device == control.device && claimed.code == control.code &&
                                           (claimed.deviceId == -1 || claimed.deviceId == control.deviceId);
                                   });
    }

    u16 InputActionMap::heldModifiers() const
    {
        const auto down = [this](const KeyCode key)
        {
            const auto it =
                m_controlStates.find({.device = InputControlDevice::Keyboard, .code = static_cast<u16>(key)});
            return it != m_controlStates.end() && it->second.isPressed;
        };
        u16 held = 0;
        held |= down(KeyCode::LSHIFT) || down(KeyCode::RSHIFT) ? KeyModifier::SHIFT : 0;
        held |= down(KeyCode::LCTRL) || down(KeyCode::RCTRL) ? KeyModifier::CTRL : 0;
        held |= down(KeyCode::LALT) || down(KeyCode::RALT) ? KeyModifier::ALT : 0;
        held |= down(KeyCode::LGUI) || down(KeyCode::RGUI) ? KeyModifier::SUPER : 0;
        return held;
    }

    void InputActionMap::claimControl(const InputControl control)
    {
        if (isClaimed(control))
        {
            return;
        }
        m_claimedControls.push_back(control);
        resolveActions();
    }

    void InputActionMap::resolveActions()
    {
        if (m_actionControlsDirty)
        {
            rebuildActionControls();
        }

        // Consumption arbitration for every state matched by a control that
        // has an enabled consuming-context binding. Consuming contexts are
        // rare, so this stays proportional to the consuming bindings.
        std::unordered_map<InputControl, i32, InputControlHash> consumingPriorities;
        for (const auto& [bindingControl, bindings] : m_bindingActionMap)
        {
            bool hasConsumer = false;
            i32 priority = std::numeric_limits<i32>::min();
            for (const InputBindingAction& binding : bindings)
            {
                const auto contextIt = m_contexts.find(binding.contextId);
                if (contextIt == m_contexts.end() || !isLive(contextIt->second.desc) ||
                    !contextIt->second.desc.consumesControls)
                {
                    continue;
                }
                hasConsumer = true;
                priority = std::max(priority, contextIt->second.desc.priority);
            }
            if (!hasConsumer)
            {
                continue;
            }

            for (const InputControl& stateControl :
                 m_statesByKey[stateKey(static_cast<u8>(bindingControl.device), bindingControl.code)])
            {
                if (bindingControl.deviceId != -1 && stateControl.deviceId != bindingControl.deviceId)
                {
                    continue;
                }
                const InputState& matchedState = m_controlStates[stateControl];
                if (!matchedState.isPressed && !matchedState.isChanged && matchedState.value == 0.0f)
                {
                    continue;
                }
                auto [priorityIt, insertedPriority] = consumingPriorities.try_emplace(stateControl, priority);
                priorityIt->second = std::max(priorityIt->second, priority);
            }
        }

        static constexpr i32 noPriority = std::numeric_limits<i32>::min();
        const u16 held = heldModifiers();
        for (auto& [actionName, action] : m_inputActionsMap)
        {
            const auto controlsIt = m_actionControls.find(actionName);
            if (controlsIt == m_actionControls.end())
            {
                action.value = 0.0f;
                const auto activatedIt = std::find(m_activatedActions.begin(), m_activatedActions.end(), action);
                if (activatedIt != m_activatedActions.end())
                {
                    m_activatedActions.erase(activatedIt);
                }
                continue;
            }

            // Pass 1: the highest context priority that produces a non-zero
            // contribution for this action. Pass 2 keeps only that context's
            // contributions and takes the strongest one. Equivalent to the
            // previous cross-product scan: lower-priority contributions were
            // always discarded there, and equal-priority ones competed on
            // magnitude regardless of iteration order.
            i32 activePriority = noPriority;
            for (const InputControl& boundControl : controlsIt->second)
            {
                const auto bindingsIt = m_bindingActionMap.find(boundControl);
                if (bindingsIt == m_bindingActionMap.end())
                {
                    continue;
                }
                for (const InputControl& stateControl :
                     m_statesByKey[stateKey(static_cast<u8>(boundControl.device), boundControl.code)])
                {
                    if ((boundControl.deviceId != -1 && stateControl.deviceId != boundControl.deviceId) ||
                        isClaimed(stateControl))
                    {
                        continue;
                    }
                    const InputState& bindingState = m_controlStates[stateControl];
                    for (const InputBindingAction& binding : bindingsIt->second)
                    {
                        if (binding.action != actionName || (binding.modifiers != 0 && binding.modifiers != held))
                        {
                            continue;
                        }
                        const auto contextIt = m_contexts.find(binding.contextId);
                        if (contextIt == m_contexts.end() || !isLive(contextIt->second.desc))
                        {
                            continue;
                        }
                        const auto consumedIt = consumingPriorities.find(stateControl);
                        if (consumedIt != consumingPriorities.end() &&
                            consumedIt->second > contextIt->second.desc.priority)
                        {
                            continue;
                        }
                        float contribution = 0.0f;
                        switch (binding.type)
                        {
                        case InputActionType::Press:
                            contribution = bindingState.isChanged && bindingState.isPressed ? 1.0f : 0.0f;
                            break;
                        case InputActionType::Release:
                            contribution = bindingState.isChanged && !bindingState.isPressed && bindingState.wasPressed
                                ? 1.0f
                                : 0.0f;
                            break;
                        case InputActionType::Hold:
                            contribution = bindingState.isPressed && bindingState.holdTime >= binding.scale
                                ? bindingState.holdTime
                                : 0.0f;
                            break;
                        case InputActionType::Axis:
                            contribution = bindingState.value * binding.scale;
                            break;
                        default:
                            break;
                        }
                        if (contribution != 0.0f)
                        {
                            activePriority = std::max(activePriority, contextIt->second.desc.priority);
                        }
                    }
                }
            }

            float value = 0.0f;
            bool active = false;
            if (activePriority != noPriority)
            {
                for (const InputControl& boundControl : controlsIt->second)
                {
                    const auto bindingsIt = m_bindingActionMap.find(boundControl);
                    if (bindingsIt == m_bindingActionMap.end())
                    {
                        continue;
                    }
                    for (const InputControl& stateControl :
                         m_statesByKey[stateKey(static_cast<u8>(boundControl.device), boundControl.code)])
                    {
                        if ((boundControl.deviceId != -1 && stateControl.deviceId != boundControl.deviceId) ||
                            isClaimed(stateControl))
                        {
                            continue;
                        }
                        const InputState& bindingState = m_controlStates[stateControl];
                        for (const InputBindingAction& binding : bindingsIt->second)
                        {
                            if (binding.action != actionName || (binding.modifiers != 0 && binding.modifiers != held))
                            {
                                continue;
                            }
                            const auto contextIt = m_contexts.find(binding.contextId);
                            if (contextIt == m_contexts.end() || !isLive(contextIt->second.desc) ||
                                contextIt->second.desc.priority != activePriority)
                            {
                                continue;
                            }
                            const auto consumedIt = consumingPriorities.find(stateControl);
                            if (consumedIt != consumingPriorities.end() &&
                                consumedIt->second > contextIt->second.desc.priority)
                            {
                                continue;
                            }
                            float contribution = 0.0f;
                            switch (binding.type)
                            {
                            case InputActionType::Press:
                                contribution = bindingState.isChanged && bindingState.isPressed ? 1.0f : 0.0f;
                                break;
                            case InputActionType::Release:
                                contribution =
                                    bindingState.isChanged && !bindingState.isPressed && bindingState.wasPressed ? 1.0f
                                                                                                                 : 0.0f;
                                break;
                            case InputActionType::Hold:
                                contribution = bindingState.isPressed && bindingState.holdTime >= binding.scale
                                    ? bindingState.holdTime
                                    : 0.0f;
                                break;
                            case InputActionType::Axis:
                                contribution = bindingState.value * binding.scale;
                                break;
                            default:
                                break;
                            }
                            if (std::abs(contribution) > std::abs(value))
                            {
                                value = contribution;
                            }
                            active |= contribution != 0.0f;
                        }
                    }
                }
            }

            action.value = value;
            const auto activatedIt = std::find(m_activatedActions.begin(), m_activatedActions.end(), action);
            if (active && activatedIt == m_activatedActions.end())
            {
                m_activatedActions.push_back(action);
            }
            else if (!active && activatedIt != m_activatedActions.end())
            {
                m_activatedActions.erase(activatedIt);
            }
        }
    }
    const std::vector<InputAction>& InputActionMap::activatedActions() { return m_activatedActions; }

    bool InputActionMap::hasAction(const InternedString name) const { return m_inputActionsMap.contains(name); }

    bool InputActionMap::anyActionActive() const { return !m_activatedActions.empty(); }

    void InputActionMap::actionControls(const InternedString name, std::vector<InputControl>& out) const
    {
        out.clear();
        for (const auto& [control, bindings] : m_bindingActionMap)
        {
            if (std::ranges::any_of(bindings,
                                    [name](const InputBindingAction& binding) { return binding.action == name; }))
            {
                out.push_back(control);
            }
        }
    }

    const std::vector<InputAction>& InputActionMap::previousFrameActivatedActions() const
    {
        return m_previousFrameActivatedActions;
    }

    bool InputActionMap::isActionActive(InternedString name) const
    {
        return std::find(m_activatedActions.begin(), m_activatedActions.end(), InputAction(name)) !=
            m_activatedActions.end();
    }

    bool InputActionMap::wasActionActive(InternedString name) const
    {
        return std::find(m_previousFrameActivatedActions.begin(), m_previousFrameActivatedActions.end(),
                         InputAction(name)) != m_previousFrameActivatedActions.end();
    }

    float InputActionMap::actionValue(InternedString name) const
    {
        SASSERTM(m_inputActionsMap.find(name) != m_inputActionsMap.end(), "Input action %s is not registered! \n",
                 name.toView());
        return m_inputActionsMap.at(name).value;
    }

    void InputActionMap::reset()
    {
        // Move current frame activated actions to previous frame
        m_previousFrameActivatedActions = m_activatedActions;

        // Clear current frame activated actions
        m_activatedActions.clear();
        m_claimedControls.clear();
        for (auto& [_, state] : m_controlStates)
        {
            state.wasPressed = state.isPressed;
            state.isChanged = false;
        }
    }

    void InputActionMap::clear()
    {
        m_controlStates.clear();
        m_statesByKey.clear();
        m_actionControlsDirty = true;
        ++m_bindingRevision;
        m_activatedActions.clear();
        m_previousFrameActivatedActions.clear();
        for (auto& [_, action] : m_inputActionsMap)
        {
            action.value = 0.0f;
        }
    }

    void InputActionMap::addBinding(InternedString actionName, const u32 scancode, const InputActionType type,
                                    const float scale)
    {
        addBinding(actionName, InputControl{.device = InputControlDevice::Keyboard, .code = static_cast<u16>(scancode)},
                   type, scale);
    }

    void InputActionMap::addBinding(InternedString actionName, const InputControl control, const InputActionType type,
                                    const float scale)
    {
        addBinding(DEFAULT_INPUT_CONTEXT_ID, actionName, control, type, scale);
    }

    void InputActionMap::addBinding(const InputContextId contextId, InternedString actionName,
                                    const InputControl control, const InputActionType type, const float scale,
                                    const u16 modifiers)
    {
        if (!m_contexts.contains(contextId))
        {
            return;
        }
        if (m_inputActionsMap.find(actionName) == m_inputActionsMap.end())
        {
            m_inputActionsMap.emplace(actionName, InputAction(actionName));
        }

        m_bindingActionMap[control].push_back(InputBindingAction(actionName, type, scale, contextId, modifiers));
        m_actionControlsDirty = true;
        ++m_bindingRevision;
    }

    void InputActionMap::removeBinding(InternedString actionName, const u32 scancode)
    {
        const auto it = m_bindingActionMap.find(
            InputControl{.device = InputControlDevice::Keyboard, .code = static_cast<u16>(scancode)});
        if (it != m_bindingActionMap.end())
        {
            auto& bindings = it->second;
            bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [&actionName](const InputBindingAction& b)
                                          { return b.action == actionName; }),
                           bindings.end());
            m_actionControlsDirty = true;
            ++m_bindingRevision;
        }
    }

    void InputActionMap::removeAllBindings(InternedString actionName)
    {
        for (auto& [_, bindings] : m_bindingActionMap)
        {
            bindings.erase(std::remove_if(bindings.begin(), bindings.end(), [&actionName](const InputBindingAction& b)
                                          { return b.action == actionName; }),
                           bindings.end());
        }
        m_inputActionsMap.erase(actionName);
        m_actionControlsDirty = true;
        ++m_bindingRevision;
    }

    bool InputActionMap::removeBindings(const InputContextId contextId, const InternedString actionName)
    {
        if (!m_contexts.contains(contextId))
        {
            return false;
        }

        for (auto& [_, bindings] : m_bindingActionMap)
        {
            std::erase_if(bindings, [contextId, actionName](const InputBindingAction& binding)
                          { return binding.contextId == contextId && binding.action == actionName; });
        }
        m_actionControlsDirty = true;
        ++m_bindingRevision;
        return true;
    }

    void InputActionMap::rebuildActionControls()
    {
        m_actionControls.clear();
        for (auto& [control, bindings] : m_bindingActionMap)
        {
            for (const InputBindingAction& binding : bindings)
            {
                auto& controls = m_actionControls[binding.action];
                if (std::find(controls.begin(), controls.end(), control) == controls.end())
                {
                    controls.push_back(control);
                }
            }
        }
        m_actionControlsDirty = false;
    }

    bool InputActionMap::saveToFile(const std::string_view filePath) const
    {
        return writeMappingsFile(filePath, mappings());
    }
} // namespace spite
