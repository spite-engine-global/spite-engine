#include "base/WritableLayout.hpp"
#include "base/io/FileSystem.hpp"
#include "ecs/World.hpp"
#include "engine/InputCommands.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "application/input/InputInjection.hpp"
#include "application/input/InputSystem.hpp"
#include "base/ContentRoot.hpp"
#include "base/ServiceLocator.hpp"
#include "control/ControlServer.hpp"
#include "serialization/FormatWalk.hpp"

namespace spite
{
    namespace
    {
        constexpr std::string_view OWNER = "engine";
        constexpr u32 MAX_SCANCODE = 511;
        constexpr u64 MAX_FRAMES = 1000;
        constexpr std::string_view NO_INPUT = "no input system: this engine runs without an application";

        const ArgDesc STATE_ARG = requiredArg("state", ArgKind::String, "press or release");
        const ArgDesc INPUT_WORLD_ARG = optionalArg(
            "world", ArgKind::String,
            "the game world this input plays: its command log records the request and a paused one steps; without it "
            "the input is plain device input (editor GUI), neither logged nor stepped",
            "none");
        const ArgDesc FRAMES_ARG = optionalArg("frames", ArgKind::Integer,
                                               "simulation frames the named paused world steps in the frame the input "
                                               "is delivered; 0 delivers without stepping",
                                               "1 for a named paused world, 0 otherwise");

        // press: true, release: false
        std::optional<bool> parseState(const ControlArgs& args)
        {
            const std::optional<std::string> state = args.string("state");
            if (state == "press")
            {
                return true;
            }
            return state == "release" ? std::optional<bool>(false) : std::nullopt;
        }

        std::optional<MouseButton> parseButton(const std::string_view name)
        {
            for (const auto& [text, button] : {std::pair{"left", MouseButton::Left},
                                               {"right", MouseButton::Right},
                                               {"middle", MouseButton::Middle},
                                               {"x1", MouseButton::X1},
                                               {"x2", MouseButton::X2}})
            {
                if (name == text)
                {
                    return button;
                }
            }
            return std::nullopt;
        }

        // SDL's gamepad button and axis names, in the order of its enums (the indices InputSystem uses)
        constexpr std::string_view GAMEPAD_BUTTONS[] = {
            "south",      "east",         "west",          "north",  "back",     "guide",    "start",    "leftstick",
            "rightstick", "leftshoulder", "rightshoulder", "dpadup", "dpaddown", "dpadleft", "dpadright"};
        constexpr std::string_view GAMEPAD_AXES[] = {"leftx",  "lefty",       "rightx",
                                                     "righty", "lefttrigger", "righttrigger"};

        static_assert(std::size(GAMEPAD_BUTTONS) <= ApplicationConstants::GAMEPAD_BUTTON_COUNT &&
                      std::size(GAMEPAD_AXES) == ApplicationConstants::GAMEPAD_AXIS_COUNT);

        // the name of a gamepad button or axis: SDL's, else its index (the buttons past the named ones)
        template <sizet N>
        std::string indexName(const std::string_view (&names)[N], const u32 index)
        {
            return index < N ? std::string(names[index]) : std::to_string(index);
        }

        // a gamepad button or axis by name or decimal index below `count`; nullopt when unknown
        template <sizet N>
        std::optional<u32> parseIndex(const std::string_view text, const std::string_view (&names)[N], const u32 count)
        {
            u32 index = 0;
            if (const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), index);
                error == std::errc{} && end == text.data() + text.size())
            {
                return index < count ? std::optional<u32>(index) : std::nullopt;
            }
            const auto* found = std::ranges::find(names, text);
            return found == std::end(names) ? std::nullopt
                                            : std::optional<u32>(static_cast<u32>(found - std::begin(names)));
        }

        // a key name of the mapping schema, or a decimal scancode
        std::optional<u32> parseKey(const std::string_view text)
        {
            u32 code = 0;
            if (const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), code);
                error == std::errc{} && end == text.data() + text.size())
            {
                return code <= MAX_SCANCODE ? std::optional<u32>(code) : std::nullopt;
            }
            return keyCodeFromName(text);
        }

        // why the injected gamepad cannot be plugged in: every gamepad slot holds another pad
        std::string injectedPadRefused(const InputSystem& input)
        {
            if (input.hasGamepad(INJECTED_GAMEPAD_ID) ||
                input.getConnectedGamepadCount() < static_cast<int>(ApplicationConstants::MAX_SUPPORTED_GAMEPADS))
            {
                return {};
            }
            return "all " + std::to_string(ApplicationConstants::MAX_SUPPORTED_GAMEPADS) +
                " gamepad slots are in use: the injected gamepad cannot connect";
        }

        // a mouse or gamepad button binding's number; nullopt when the key is not a decimal number
        std::optional<u32> bindingNumber(const std::string_view key)
        {
            u32 number = 0;
            const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), number);
            return error == std::errc{} && end == key.data() + key.size() ? std::optional<u32>(number) : std::nullopt;
        }

        // reads an optional number member into value; returns an error text when it is not a number
        std::string number(const ControlArgs& args, const std::string_view key, std::optional<float>& value)
        {
            const std::optional<std::string> text = args.json(key);
            if (!text)
            {
                return {};
            }
            float parsed = 0.0f;
            const auto [end, error] = std::from_chars(text->data(), text->data() + text->size(), parsed);
            if (error != std::errc{} || end != text->data() + text->size())
            {
                return std::string(key) + " must be a number";
            }
            value = parsed;
            return {};
        }

        // the optional x and y numbers of a command
        std::string numberPair(const ControlArgs& args, std::optional<float>& x, std::optional<float>& y)
        {
            if (std::string error = number(args, "x", x); !error.empty())
            {
                return error;
            }
            return number(args, "y", y);
        }

        std::string upperAscii(const std::string_view text)
        {
            std::string upper(text);
            std::ranges::transform(upper, upper.begin(), [](const char c)
                                   { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
            return upper;
        }

        std::string_view actionTypeName(const InputActionType type)
        {
            switch (type)
            {
            case InputActionType::Press:
                return "press";
            case InputActionType::Release:
                return "release";
            case InputActionType::Hold:
                return "hold";
            case InputActionType::Axis:
                return "axis";
            default:
                return "none";
            }
        }

        std::optional<InputActionType> parseActionType(const std::string_view text)
        {
            for (const InputActionType type :
                 {InputActionType::Press, InputActionType::Release, InputActionType::Hold, InputActionType::Axis})
            {
                if (upperAscii(text) == upperAscii(actionTypeName(type)))
                {
                    return type;
                }
            }
            return std::nullopt;
        }

        constexpr std::string_view ACTION_TYPES[] = {"press", "release", "hold", "axis"};
        constexpr std::string_view BINDING_DEVICES[] = {"keyboard", "mouse", "gamepad_button", "gamepad_axis"};
        constexpr std::string_view MODIFIER_WORDS[] = {"SHIFT", "CTRL", "ALT", "SUPER"};

        // editor contexts hold preferences (the user layer); every other one belongs to the project
        std::string_view contextKind(const std::string_view name)
        {
            const std::string upper = upperAscii(name);
            return upper == "EDITOR" ? "editor" : upper == "UI" ? "ui" : "game";
        }

        // from "config/" on: a client is told a content-relative path, never the machine's layout
        std::string shownPath(const std::string& file)
        {
            std::string text = file;
            std::ranges::replace(text, '\\', '/');
            const sizet config = text.rfind("config/");
            const sizet slash = text.rfind('/');
            return config != std::string::npos ? text.substr(config)
                                               : text.substr(slash == std::string::npos ? 0 : slash + 1);
        }

        // where saveBindings writes a context: the editor's preferences the user layer, a game's mappings its file
        std::string saveTarget(const InputSystem& input, const InputContext& context)
        {
            const std::string file = input.contextFile(context.id);
            return contextKind(context.desc.name.toView()) == "editor" ? userLayerMappingsFile(file) : file;
        }

        // The project-layer actions a context read from a user copy lacks: the copy shadows the project file, so an
        // action added there later never reaches it. Empty for a context that is no user copy or whose project file
        // does not read.
        InputMappings projectActionsMissing(const InputSystem& input, const InputContext& context)
        {
            std::string copy = input.contextFile(context.id);
            std::ranges::replace(copy, '\\', '/');
            const std::string project = projectLayerMappingsFile(copy);
            std::optional<InputMappings> file;
            if (project != copy)
            {
                file = readInputMappings(project);
            }
            InputMappings missing;
            if (file)
            {
                const InputMappings held = input.mappings(context.id);
                std::ranges::copy_if(
                    file->actions, std::back_inserter(missing.actions), [&](const InputActionMapping& action)
                    { return !std::ranges::contains(held.actions, action.action, &InputActionMapping::action); });
            }
            return missing;
        }

        const InputContext* findContext(const std::vector<InputContext>& contexts, const std::string_view name)
        {
            const std::string wanted = upperAscii(name);
            const auto it = std::ranges::find_if(contexts, [&](const InputContext& context)
                                                 { return upperAscii(context.desc.name.toView()) == wanted; });
            return it == contexts.end() ? nullptr : &*it;
        }

        std::string contextNames(const std::vector<InputContext>& contexts)
        {
            std::string names;
            for (const InputContext& context : contexts)
            {
                names += (names.empty() ? "" : ", ") + std::string(context.desc.name.toView());
            }
            return names;
        }

        // "ctrl+shift" as "CTRL+SHIFT" in the schema's order; problem set for an unknown word
        std::string normalizeModifiers(const std::string_view text, std::string& problem)
        {
            u32 present = 0;
            for (sizet start = 0; start < text.size();)
            {
                const sizet end = std::min(text.find('+', start), text.size());
                const std::string word = upperAscii(text.substr(start, end - start));
                const auto* it = std::ranges::find(MODIFIER_WORDS, std::string_view(word));
                if (it == std::end(MODIFIER_WORDS))
                {
                    problem = "unknown modifier '" + word + "'; modifiers: SHIFT, CTRL, ALT, SUPER joined with +";
                    return {};
                }
                present |= 1u << (it - std::begin(MODIFIER_WORDS));
                start = end + 1;
            }
            std::string normal;
            for (u32 i = 0; i < std::size(MODIFIER_WORDS); ++i)
            {
                if ((present >> i) & 1u)
                {
                    normal += (normal.empty() ? "" : "+") + std::string(MODIFIER_WORDS[i]);
                }
            }
            return normal;
        }

        // device and key as the schema holds them (KEYBOARD and an upper-case key name, or a MOUSE or GAMEPAD number);
        // the refusal otherwise
        std::string normalizeBinding(const std::string_view deviceText, const std::string_view keyText,
                                     InputBindingMapping& binding)
        {
            const std::string device = upperAscii(deviceText.empty() ? "keyboard" : deviceText);
            if (device == "KEYBOARD")
            {
                const std::optional<u32> code = keyCodeFromName(keyText);
                if (!code)
                {
                    std::string names;
                    for (const std::string& name : keyNames())
                    {
                        names += (names.empty() ? "" : ", ") + name;
                    }
                    return "unknown key '" + std::string(keyText) + "'; valid names (any case): " + names;
                }
                binding.device = "KEYBOARD";
                binding.key = keyNameOf(*code); // an alias is stored and compared as its canonical name
                return {};
            }
            if (device != "MOUSE" && device != "GAMEPAD_BUTTON" && device != "GAMEPAD_AXIS")
            {
                return "device must be keyboard, mouse, gamepad_button or gamepad_axis";
            }
            const std::optional<u32> number = bindingNumber(keyText);
            const u32 limit = device == "MOUSE" ? 4u : 255u;
            if (!number || *number > limit)
            {
                return "a " + std::string(deviceText) + " binding's key is a number 0.." + std::to_string(limit);
            }
            binding.device = device;
            binding.key = std::to_string(*number);
            return {};
        }

        bool sameControl(const InputBindingMapping& a, const InputBindingMapping& b)
        {
            return upperAscii(a.device) == upperAscii(b.device) && upperAscii(a.key) == upperAscii(b.key) &&
                a.modifiers == b.modifiers;
        }

        nlohmann::json mappingsJson(const InputMappings& mappings)
        {
            nlohmann::json actions = nlohmann::json::array();
            for (const InputActionMapping& action : mappings.actions)
            {
                nlohmann::json bindings = nlohmann::json::array();
                for (const InputBindingMapping& binding : action.bindings)
                {
                    bindings.push_back({{"device", binding.device},
                                        {"key", binding.key},
                                        {"scale", binding.scale},
                                        {"modifiers", binding.modifiers}});
                }
                actions.push_back({{"action", action.action},
                                   {"type", std::string(actionTypeName(action.type))},
                                   {"bindings", std::move(bindings)}});
            }
            return actions;
        }

        // The actions array of restoreBindings as mappings, every field type-checked and every control normalized as
        // setBinding would; the problem (empty on success) names the offending field. Nothing here throws: the
        // control server does not catch a handler's exceptions.
        std::string mappingsFromJson(const nlohmann::json& actions, InputMappings& out)
        {
            if (!actions.is_array())
            {
                return "actions must be an array of {action, type, bindings: [{device, key, scale, modifiers}]}";
            }
            for (sizet i = 0; i < actions.size(); ++i)
            {
                const nlohmann::json& entry = actions[i];
                const std::string at = "actions[" + std::to_string(i) + "]";
                if (!entry.is_object())
                {
                    return at + " must be an object {action, type, bindings}";
                }
                for (const char* key : {"action", "type"})
                {
                    if (!entry.contains(key) || !entry[key].is_string())
                    {
                        return at + "." + key + " must be a string";
                    }
                }
                if (!entry.contains("bindings") || !entry["bindings"].is_array())
                {
                    return at + ".bindings must be an array";
                }
                const std::optional<InputActionType> type = parseActionType(entry["type"].get<std::string>());
                if (!type)
                {
                    return at + ".type must be press, release, hold or axis";
                }
                InputActionMapping action{.action = entry["action"].get<std::string>(), .type = *type};
                for (sizet b = 0; b < entry["bindings"].size(); ++b)
                {
                    const nlohmann::json& binding = entry["bindings"][b];
                    const std::string bindingAt = at + ".bindings[" + std::to_string(b) + "]";
                    if (!binding.is_object())
                    {
                        return bindingAt + " must be an object {device, key, scale, modifiers}";
                    }
                    const auto text = [&](const char* key, const std::string& fallback, std::string& value)
                    {
                        const auto found = binding.find(key);
                        if (found == binding.end())
                        {
                            value = fallback;
                            return std::string{};
                        }
                        if (!found->is_string())
                        {
                            return bindingAt + "." + key + " must be a string";
                        }
                        value = found->get<std::string>();
                        return std::string{};
                    };
                    std::string device;
                    std::string key;
                    std::string modifiers;
                    for (const std::string& error :
                         {text("device", "KEYBOARD", device), text("key", "", key), text("modifiers", "", modifiers)})
                    {
                        if (!error.empty())
                        {
                            return error;
                        }
                    }
                    InputBindingMapping mapped;
                    if (std::string error = normalizeBinding(device, key, mapped); !error.empty())
                    {
                        return bindingAt + ": " + error;
                    }
                    std::string problem;
                    mapped.modifiers = normalizeModifiers(modifiers, problem);
                    if (!problem.empty())
                    {
                        return bindingAt + ": " + problem;
                    }
                    if (const auto scale = binding.find("scale"); scale != binding.end())
                    {
                        if (!scale->is_number())
                        {
                            return bindingAt + ".scale must be a number";
                        }
                        mapped.scale = scale->get<float>();
                    }
                    action.bindings.push_back(std::move(mapped));
                }
                out.actions.push_back(std::move(action));
            }
            return {};
        }
    } // namespace

    void registerInputCommands(ControlServer& control, ServiceLocator& services)
    {
        // queues one batch for the next frame's poll; with a world, the request is logged there and replays
        // the handlers own the token, so a batch still queued when the control server goes finds it expired
        const auto alive = std::make_shared<bool>(true);
        const auto queue = [&control, &services, alive](const ControlArgs& args, IFormatWriter& out,
                                                        std::vector<InputObservation> observations) -> std::string
        {
            if (!services.contains<InputInjector>() || !services.contains<InputSystem>())
            {
                return std::string(NO_INPUT);
            }
            // without a world the input is plain device input: no log row and no stepping, because the default
            // world is the edit scene, whose editor actions log their own commands
            const bool named = args.string("world").has_value();
            const World* world = named ? control.world(args) : nullptr;
            if (named && !world)
            {
                return control.unknownWorld(args);
            }
            const bool paused = world && world->simulationState().paused;
            u32 frames = paused ? 1 : 0;
            if (const std::optional<u64> asked = args.integer("frames"))
            {
                if (*asked > MAX_FRAMES)
                {
                    return "frames must be 0.." + std::to_string(MAX_FRAMES);
                }
                frames = static_cast<u32>(*asked);
            }
            if (frames > 0 && !named)
            {
                return "frames steps a world: name it with world";
            }
            if (frames > 0 && !paused)
            {
                return "frames steps a paused world; this one runs, so it simulates every frame";
            }
            const auto count = static_cast<u32>(observations.size());
            // with a reply sink the request answers once its frames ran (armed by the delivering frame)
            const DeferredReply reply = frames > 0
                ? control.deferUntilStepped(args, false, {{"observations", count}, {"frames", frames}})
                : DeferredReply{};
            services.get<InputInjector>().queue(
                {.observations = std::move(observations),
                 .delivered = [&control, token = std::weak_ptr<bool>(alive),
                               name = world ? control.worldName(args) : std::string{}, frames, reply]
                 {
                     if (token.expired())
                     {
                         return;
                     }
                     // a step or resume between the request and its frame may have changed the world
                     if (const World* target = frames > 0 ? control.world(name) : nullptr;
                         target && target->simulationState().paused)
                     {
                         target->stepSimulation(frames);
                     }
                     if (reply.isValid())
                     {
                         control.armStepReply(reply);
                     }
                 }});
            if (named)
            {
                control.recordReplay(std::format(R"({{"frames":{}}})", frames));
            }
            if (reply.isValid())
            {
                return {};
            }
            out.writeU32("observations", count);
            out.writeU32("frames", frames);
            out.writeU32(
                "pending",
                static_cast<u32>(services.get<InputInjector>().pending())); // batches queued, this one included
            return {};
        };

        control.registerCommand(
            "injectKey", OWNER, "injects a key press or release, delivered in the next frame like a device's",
            {requiredArg("key", ArgKind::String,
                         "a key name in any case (W, F5, SPACE, LSHIFT, ...) or a decimal scancode"),
             STATE_ARG, INPUT_WORLD_ARG, FRAMES_ARG},
            [queue](const ControlArgs& args, IFormatWriter& out)
            {
                const std::string keyText = args.string("key").value_or("");
                const std::optional<u32> key = parseKey(keyText);
                const std::optional<bool> pressed = parseState(args);
                if (!key)
                {
                    std::string names;
                    for (const std::string& name : keyNames())
                    {
                        names += (names.empty() ? "" : ", ") + name;
                    }
                    return "unknown key '" + keyText + "'; valid names (any case): " + names +
                        ", or a decimal scancode 0.." + std::to_string(MAX_SCANCODE);
                }
                if (!pressed)
                {
                    return std::string("state must be press or release");
                }
                return queue(args, out, {KeyObservation{.scancode = *key, .pressed = *pressed}});
            });

        control.suggestArgValues("injectKey", "key", &keyNames);

        control.registerCommand(
            "injectMouseButton", OWNER, "injects a mouse button press or release, after moving to x, y when given",
            {optionalArg("button", ArgKind::String, "left, right, middle, x1 or x2", "left"), STATE_ARG,
             optionalArg("x", ArgKind::Number, "pointer x in window units, with y"),
             optionalArg("y", ArgKind::Number, "pointer y in window units, with x"), INPUT_WORLD_ARG, FRAMES_ARG},
            [queue](const ControlArgs& args, IFormatWriter& out)
            {
                const std::optional<MouseButton> button = parseButton(args.string("button").value_or("left"));
                const std::optional<bool> pressed = parseState(args);
                if (!button)
                {
                    return std::string("button must be left, right, middle, x1 or x2");
                }
                if (!pressed)
                {
                    return std::string("state must be press or release");
                }
                std::optional<float> x;
                std::optional<float> y;
                if (std::string error = numberPair(args, x, y); !error.empty())
                {
                    return error;
                }
                if (x.has_value() != y.has_value())
                {
                    return std::string("x and y go together");
                }
                std::vector<InputObservation> observations;
                if (x)
                {
                    observations.emplace_back(MouseMoveObservation{.x = *x, .y = *y});
                }
                observations.emplace_back(MouseButtonObservation{*button, *pressed});
                return queue(args, out, std::move(observations));
            });

        control.registerCommand(
            "injectMouseMove", OWNER, "moves the pointer to x, y; the relative motion is from where it was",
            {requiredArg("x", ArgKind::Number, "pointer x in window units"),
             requiredArg("y", ArgKind::Number, "pointer y in window units"), INPUT_WORLD_ARG, FRAMES_ARG},
            [queue](const ControlArgs& args, IFormatWriter& out)
            {
                std::optional<float> x;
                std::optional<float> y;
                if (std::string error = numberPair(args, x, y); !error.empty())
                {
                    return error;
                }
                return queue(args, out, {MouseMoveObservation{.x = x.value_or(0.0f), .y = y.value_or(0.0f)}});
            });

        control.registerCommand("injectScroll", OWNER, "injects a mouse wheel delta (up is positive y)",
                                {optionalArg("x", ArgKind::Number, "horizontal wheel delta", "0"),
                                 optionalArg("y", ArgKind::Number, "vertical wheel delta", "0"), INPUT_WORLD_ARG,
                                 FRAMES_ARG},
                                [queue](const ControlArgs& args, IFormatWriter& out)
                                {
                                    std::optional<float> x;
                                    std::optional<float> y;
                                    if (std::string error = numberPair(args, x, y); !error.empty())
                                    {
                                        return error;
                                    }
                                    return queue(args, out, {WheelObservation{x.value_or(0.0f), y.value_or(0.0f)}});
                                });

        control.registerCommand(
            "injectText", OWNER,
            "injects typed text, as the text field of a focused UI receives it; with composition, the text an input "
            "method is composing (underlined at the caret until committed: a later plain injectText commits it, "
            "empty composition text cancels it)",
            {requiredArg("text", ArgKind::String, "the text"),
             optionalArg("composition", ArgKind::Boolean, "the text is an input method's composition, not committed",
                         "false"),
             optionalArg("cursor", ArgKind::Integer, "composition: the first codepoint of the selected clause"),
             optionalArg("selected", ArgKind::Integer, "composition: codepoints in the selected clause", "0"),
             INPUT_WORLD_ARG, FRAMES_ARG},
            [queue](const ControlArgs& args, IFormatWriter& out)
            {
                TextObservation text{.text = args.string("text").value_or("")};
                text.composition = args.boolean("composition").value_or(false);
                if (text.composition && args.integer("cursor"))
                {
                    text.start = static_cast<i32>(*args.integer("cursor"));
                    text.length = static_cast<i32>(args.integer("selected").value_or(0));
                }
                return queue(args, out, {std::move(text)});
            });

        control.registerCommand(
            "injectAction", OWNER,
            "presses or releases a keyboard, mouse or gamepad control an input action is bound to; contexts decide "
            "whether it fires",
            {requiredArg("action", ArgKind::String, "the action name"), STATE_ARG,
             optionalArg("device", ArgKind::String,
                         "the device whose binding to press: keyboard, mouse or gamepad (the injected gamepad, "
                         "connected by the press)",
                         "the first keyboard binding, else mouse, else gamepad"),
             INPUT_WORLD_ARG, FRAMES_ARG},
            [queue, &services](const ControlArgs& args, IFormatWriter& out)
            {
                // a failed request writes no result members
                const auto report = [](std::string error, const std::string& control, IFormatWriter& result)
                {
                    if (error.empty())
                    {
                        result.writeString("control", control);
                    }
                    return error;
                };
                const std::optional<bool> pressed = parseState(args);
                if (!pressed)
                {
                    return std::string("state must be press or release");
                }
                if (!services.contains<InputSystem>())
                {
                    return std::string(NO_INPUT);
                }
                const std::string name = args.string("action").value_or("");
                const InputMappings mappings = services.get<InputSystem>().mappings();
                const auto action = std::ranges::find(mappings.actions, name, &InputActionMapping::action);
                if (action == mappings.actions.end())
                {
                    return "unknown action '" + name + "'";
                }
                // an axis action takes its first key or button: it reads that control's scale
                const std::string device = args.string("device").value_or("");
                if (!device.empty() && device != "keyboard" && device != "mouse" && device != "gamepad")
                {
                    return std::string("device must be keyboard, mouse or gamepad");
                }
                // the first binding of the device without a chord (a chord needs its modifiers held: the client's to
                // press); the schema's device names are case-insensitive
                const auto bindingOf = [&](const std::string_view wanted) -> const InputBindingMapping*
                {
                    for (const InputBindingMapping& binding : action->bindings)
                    {
                        std::string upper = binding.device;
                        std::ranges::transform(upper, upper.begin(),
                                               [](const char c) { return static_cast<char>(std::toupper(c)); });
                        if (binding.modifiers.empty() && upper == wanted &&
                            (wanted != "KEYBOARD" || keyCodeFromName(binding.key).has_value()))
                        {
                            return &binding;
                        }
                    }
                    return nullptr;
                };
                if (device.empty() || device == "keyboard")
                {
                    if (const InputBindingMapping* binding = bindingOf("KEYBOARD"))
                    {
                        return report(
                            queue(args, out,
                                  {KeyObservation{.scancode = *keyCodeFromName(binding->key), .pressed = *pressed}}),
                            "key " + binding->key, out);
                    }
                }
                if (device.empty() || device == "mouse")
                {
                    if (const InputBindingMapping* binding = bindingOf("MOUSE"))
                    {
                        const std::optional<u32> button = bindingNumber(binding->key);
                        if (!button)
                        {
                            return "mouse binding key '" + binding->key + "' is not a button number";
                        }
                        return report(
                            queue(args, out, {MouseButtonObservation{static_cast<MouseButton>(*button), *pressed}}),
                            "mouse button " + binding->key, out);
                    }
                }
                if (device.empty() || device == "gamepad")
                {
                    if (const InputBindingMapping* binding = bindingOf("GAMEPAD_BUTTON"))
                    {
                        const std::optional<u32> button = bindingNumber(binding->key);
                        if (!button)
                        {
                            return "gamepad binding key '" + binding->key + "' is not a button number";
                        }
                        if (std::string refused = injectedPadRefused(services.get<InputSystem>()); !refused.empty())
                        {
                            return refused;
                        }
                        return report(queue(args, out,
                                            {GamepadObservation{.kind = GamepadObservation::Kind::Button,
                                                                .index = *button,
                                                                .pressed = *pressed}}),
                                      "gamepad button " + binding->key, out);
                    }
                }
                return "'" + name + "' has no " +
                    (device.empty() ? std::string("keyboard, mouse or gamepad button") : device) +
                    " binding without a chord: inject its controls";
            });

        control.registerCommand(
            "injectGamepad", OWNER,
            "changes the injected gamepad, a pad that exists in the input core without hardware (one gamepad slot "
            "while connected; a button or axis connects it): a button press or release, an axis value, or its "
            "connection",
            {optionalArg("button", ArgKind::String,
                         "a button: south, east, west, north, back, guide, start, leftstick, rightstick, "
                         "leftshoulder, rightshoulder, dpadup, dpaddown, dpadleft, dpadright, or its SDL index"),
             optionalArg("state", ArgKind::String, "press or release, with button"),
             optionalArg("axis", ArgKind::String,
                         "an axis: leftx, lefty (down is positive), rightx, righty, lefttrigger, righttrigger, or its "
                         "SDL index"),
             optionalArg("value", ArgKind::Number, "with axis: -1..1 of the full deflection (triggers 0..1)"),
             optionalArg("connected", ArgKind::Boolean,
                         "false unplugs it (what it held is released), true plugs it in"),
             INPUT_WORLD_ARG, FRAMES_ARG},
            [queue, &services](const ControlArgs& args, IFormatWriter& out)
            {
                if (services.contains<InputSystem>() && !services.get<InputSystem>().isControllerEnabled())
                {
                    return std::string("controllers are disabled (the enableController input config)");
                }
                using Kind = GamepadObservation::Kind;
                const std::optional<std::string> button = args.string("button");
                const std::optional<std::string> axis = args.string("axis");
                const std::optional<bool> connected = args.boolean("connected");
                if (int{button.has_value()} + int{axis.has_value()} + int{connected.has_value()} != 1)
                {
                    return std::string("injectGamepad needs exactly one of button, axis or connected");
                }
                // everything but unplugging plugs the pad in
                if (connected.value_or(true) && services.contains<InputSystem>())
                {
                    if (std::string refused = injectedPadRefused(services.get<InputSystem>()); !refused.empty())
                    {
                        return refused;
                    }
                }
                if (connected)
                {
                    return queue(args, out,
                                 {GamepadObservation{.kind = *connected ? Kind::Connect : Kind::Disconnect}});
                }
                if (button)
                {
                    const std::optional<u32> index =
                        parseIndex(*button, GAMEPAD_BUTTONS, ApplicationConstants::GAMEPAD_BUTTON_COUNT);
                    const std::optional<bool> pressed = parseState(args);
                    if (!index)
                    {
                        return std::string("unknown button");
                    }
                    if (!pressed)
                    {
                        return std::string("state must be press or release");
                    }
                    return queue(args, out,
                                 {GamepadObservation{.kind = Kind::Button, .index = *index, .pressed = *pressed}});
                }
                const std::optional<u32> index =
                    parseIndex(*axis, GAMEPAD_AXES, ApplicationConstants::GAMEPAD_AXIS_COUNT);
                std::optional<float> value;
                if (std::string error = number(args, "value", value); !error.empty())
                {
                    return error;
                }
                if (!index)
                {
                    return std::string("unknown axis");
                }
                if (!value || !(*value >= -1.0f && *value <= 1.0f))
                {
                    return std::string("value must be a number from -1 to 1");
                }
                return queue(args, out,
                             {GamepadObservation{.kind = Kind::Axis,
                                                 .index = *index,
                                                 .value = static_cast<i16>(std::lround(*value * 32767.0f))}});
            });

        control.registerQuery(
            "gamepads", OWNER,
            "the gamepads the input core holds: connected pads (hardware or injected) with the buttons down and the "
            "axis values, as the input contexts read them (axes: deadzone and sensitivity applied; rawAxes: what the "
            "device reports before them)",
            {},
            [&services](const ControlArgs&, IFormatWriter& out)
            {
                if (!services.contains<InputSystem>())
                {
                    return std::string(NO_INPUT);
                }
                const InputSystem& input = services.get<InputSystem>();
                out.writeBool("controllerEnabled", input.isControllerEnabled());
                out.writeU32("count", static_cast<u32>(input.getConnectedGamepadCount()));
                out.beginArray("pads", static_cast<u32>(input.getConnectedGamepadCount()));
                for (int pad = 0; pad < static_cast<int>(ApplicationConstants::MAX_SUPPORTED_GAMEPADS); ++pad)
                {
                    if (!input.isGamepadConnected(pad))
                    {
                        continue;
                    }
                    out.beginObject("");
                    out.writeU32("index", static_cast<u32>(pad));
                    u32 down = 0;
                    for (u32 button = 0; button < ApplicationConstants::GAMEPAD_BUTTON_COUNT; ++button)
                    {
                        down += input.getGamepadButtonState(pad, static_cast<int>(button)).isPressed ? 1u : 0u;
                    }
                    out.beginArray("buttons", down);
                    for (u32 button = 0; button < ApplicationConstants::GAMEPAD_BUTTON_COUNT; ++button)
                    {
                        if (input.getGamepadButtonState(pad, static_cast<int>(button)).isPressed)
                        {
                            out.writeString("", indexName(GAMEPAD_BUTTONS, button));
                        }
                    }
                    out.endArray();
                    out.beginObject("axes");
                    for (u32 axis = 0; axis < ApplicationConstants::GAMEPAD_AXIS_COUNT; ++axis)
                    {
                        out.writeF64(GAMEPAD_AXES[axis], input.getGamepadAxisValue(pad, static_cast<int>(axis)));
                    }
                    out.endObject();
                    out.beginObject("rawAxes");
                    for (u32 axis = 0; axis < ApplicationConstants::GAMEPAD_AXIS_COUNT; ++axis)
                    {
                        out.writeF64(GAMEPAD_AXES[axis], input.getGamepadRawAxisValue(pad, static_cast<int>(axis)));
                    }
                    out.endObject();
                    out.endObject();
                }
                out.endArray();
                return std::string{};
            });

        // ---- bindings ----
        // The context's bindings replaced by mappings; the previous ones are the undo step.
        const auto replaceBindings = [&control, &services](const InputContext& context, const InputMappings& mappings,
                                                           IFormatWriter& out) -> std::string
        {
            InputSystem& input = services.get<InputSystem>();
            const nlohmann::json previous = mappingsJson(input.mappings(context.id));
            const std::string name(context.desc.name.toView());
            if (!input.setContextMappings(context.id, mappings))
            {
                return "context '" + name + "' is gone";
            }
            const nlohmann::json inverse = {{"context", name}, {"actions", previous}};
            control.recordInverse("restoreBindings", inverse.dump());
            out.writeString("context", name);
            return {};
        };
        // the context a request names, or why not; storage keeps the list the pointer points into
        const auto contextOf = [&services](const ControlArgs& args, const InputContext*& out,
                                           std::vector<InputContext>& storage) -> std::string
        {
            if (!services.contains<InputSystem>())
            {
                return std::string(NO_INPUT);
            }
            storage = services.get<InputSystem>().contexts();
            const std::string name = args.string("context").value_or("");
            out = findContext(storage, name);
            if (!out)
            {
                return "unknown input context '" + name + "'; contexts: " + contextNames(storage);
            }
            return {};
        };
        const auto writeBinding = [](IFormatWriter& out, const InputBindingMapping& binding)
        {
            out.writeString("device", binding.device);
            out.writeString("key", binding.key);
            out.writeF32("scale", binding.scale);
            out.writeString("modifiers", binding.modifiers);
        };
        const ArgDesc CONTEXT_ARG =
            requiredArg("context", ArgKind::String, "an input context's name, as inputActions lists it (any case)");
        const ArgDesc ACTION_ARG = requiredArg("action", ArgKind::String, "the action name");
        const ArgDesc DEVICE_ARG =
            optionalArg("device", ArgKind::String, "the control's device", "keyboard", BINDING_DEVICES);
        const ArgDesc MODIFIERS_ARG =
            optionalArg("modifiers", ArgKind::String,
                        "a chord's held modifiers: SHIFT, CTRL, ALT, SUPER joined with + (any case)", "none");

        control.registerQuery(
            "inputActions", OWNER,
            "the input actions per context (game contexts by their names, ui, editor): type, the live value, whether "
            "it is down and the bindings (device, key, scale, modifiers); the file each context was read from and "
            "where saveBindings writes it (and whether that file exists); projectMissing: the project file's actions a "
            "user copy lacks (mergeProjectBindings adds them); the devices the input core sees",
            {optionalArg("context", ArgKind::String, "only this context, by name (any case)", "every context"),
             optionalArg("action", ArgKind::String, "only this action", "every action")},
            [&services, contextOf, writeBinding](const ControlArgs& args, IFormatWriter& out)
            {
                if (!services.contains<InputSystem>())
                {
                    return std::string(NO_INPUT);
                }
                const InputSystem& input = services.get<InputSystem>();
                std::vector<InputContext> contexts = input.contexts();
                if (args.string("context"))
                {
                    const InputContext* only = nullptr;
                    std::vector<InputContext> storage;
                    if (std::string error = contextOf(args, only, storage); !error.empty())
                    {
                        return error;
                    }
                    contexts = {*only};
                }
                const std::string actionFilter = args.string("action").value_or("");
                out.beginObject("devices");
                out.writeBool("keyboard", true);
                out.writeBool("mouse", true);
                out.writeBool("controllerEnabled", input.isControllerEnabled());
                out.writeU32("gamepads", static_cast<u32>(input.getConnectedGamepadCount()));
                out.endObject();
                out.beginArray("contexts", static_cast<u32>(contexts.size()));
                for (const InputContext& context : contexts)
                {
                    const std::string name(context.desc.name.toView());
                    InputMappings mappings = input.mappings(context.id);
                    if (!actionFilter.empty())
                    {
                        std::erase_if(mappings.actions,
                                      [&](const InputActionMapping& action) { return action.action != actionFilter; });
                    }
                    out.beginObject("");
                    out.writeU32("id", context.id);
                    out.writeString("name", name);
                    out.writeString("kind", contextKind(name));
                    out.writeI32("priority", context.desc.priority);
                    out.writeBool("enabled", context.desc.enabled);
                    out.writeString("file", shownPath(input.contextFile(context.id)));
                    out.beginObject("saveTo");
                    out.writeString("layer", contextKind(name) == "editor" ? "user" : "project");
                    out.writeString("file", shownPath(saveTarget(input, context)));
                    out.writeBool(
                        "exists",
                        std::filesystem::exists(resolveUserConfigPath(pathFromUtf8(saveTarget(input, context)))));
                    out.endObject();
                    const InputMappings missing = projectActionsMissing(input, context);
                    out.beginArray("projectMissing", static_cast<u32>(missing.actions.size()));
                    for (const InputActionMapping& action : missing.actions)
                    {
                        out.writeString("", action.action);
                    }
                    out.endArray();
                    out.beginArray("actions", static_cast<u32>(mappings.actions.size()));
                    for (const InputActionMapping& action : mappings.actions)
                    {
                        const InternedString key(action.action);
                        out.beginObject("");
                        out.writeString("action", action.action);
                        out.writeString("type", actionTypeName(action.type));
                        out.writeF32("value", input.getActionValue(key));
                        out.writeBool("down", input.isActionDown(key));
                        out.beginArray("bindings", static_cast<u32>(action.bindings.size()));
                        for (const InputBindingMapping& binding : action.bindings)
                        {
                            out.beginObject("");
                            writeBinding(out, binding);
                            out.endObject();
                        }
                        out.endArray();
                        out.endObject();
                    }
                    out.endArray();
                    out.endObject();
                }
                out.endArray();
                return std::string{};
            });

        control.registerUndoableViewCommand(
            "setBinding", OWNER,
            "binds a control to an action in a context (a new action needs its type) -> action, bindings, and "
            "conflicts: the other actions of the context on the same control and chord, reported and kept. replace "
            "names the key of a binding to swap out. Not saved: saveBindings writes the file",
            {CONTEXT_ARG, ACTION_ARG, DEVICE_ARG,
             requiredArg("key", ArgKind::String, "a key name in any case, or a button or axis number"), MODIFIERS_ARG,
             optionalArg("scale", ArgKind::Number, "an axis binding's value", "1"),
             optionalArg("type", ArgKind::String, "press, release, hold or axis: required for a new action",
                         "the action's", ACTION_TYPES),
             optionalArg("replace", ArgKind::String, "the key of the action's binding (on the same device) to replace",
                         "none")},
            [&services, contextOf, replaceBindings, writeBinding](const ControlArgs& args, IFormatWriter& out)
            {
                const InputContext* context = nullptr;
                std::vector<InputContext> storage;
                if (std::string error = contextOf(args, context, storage); !error.empty())
                {
                    return error;
                }
                const std::string actionName = args.string("action").value_or("");
                if (actionName.empty())
                {
                    return std::string("setBinding needs action");
                }
                InputBindingMapping binding;
                if (std::string error =
                        normalizeBinding(args.string("device").value_or(""), args.string("key").value_or(""), binding);
                    !error.empty())
                {
                    return error;
                }
                std::string problem;
                binding.modifiers = normalizeModifiers(args.string("modifiers").value_or(""), problem);
                if (!problem.empty())
                {
                    return problem;
                }
                std::optional<float> scale;
                if (std::string error = number(args, "scale", scale); !error.empty())
                {
                    return error;
                }
                binding.scale = scale.value_or(1.0f);
                const std::optional<std::string> typeText = args.string("type");
                const std::optional<InputActionType> type = typeText ? parseActionType(*typeText) : std::nullopt;
                if (typeText && !type)
                {
                    return std::string("type must be press, release, hold or axis");
                }
                InputMappings mappings = services.get<InputSystem>().mappings(context->id);
                auto action = std::ranges::find(mappings.actions, actionName, &InputActionMapping::action);
                if (action == mappings.actions.end())
                {
                    if (!type)
                    {
                        return "'" + actionName + "' is new in context '" + std::string(context->desc.name.toView()) +
                            "': give its type (press, release, hold or axis)";
                    }
                    action = mappings.actions.insert(mappings.actions.end(), InputActionMapping{actionName, *type, {}});
                }
                else if (type)
                {
                    action->type = *type;
                }
                if (const std::optional<std::string> replace = args.string("replace"))
                {
                    InputBindingMapping old;
                    if (std::string error = normalizeBinding(binding.device, *replace, old); !error.empty())
                    {
                        return error;
                    }
                    const auto before = std::ranges::find_if(
                        action->bindings, [&](const InputBindingMapping& existing)
                        { return upperAscii(existing.device) == old.device && upperAscii(existing.key) == old.key; });
                    if (before == action->bindings.end())
                    {
                        return "'" + actionName + "' has no " + binding.device + " binding '" + *replace +
                            "' to replace";
                    }
                    action->bindings.erase(before);
                }
                if (std::ranges::none_of(action->bindings, [&](const InputBindingMapping& existing)
                                         { return sameControl(existing, binding); }))
                {
                    action->bindings.push_back(binding);
                }
                std::vector<std::string> conflicts;
                for (const InputActionMapping& other : mappings.actions)
                {
                    if (other.action != actionName &&
                        std::ranges::any_of(other.bindings, [&](const InputBindingMapping& existing)
                                            { return sameControl(existing, binding); }))
                    {
                        conflicts.push_back(other.action);
                    }
                }
                if (std::string error = replaceBindings(*context, mappings, out); !error.empty())
                {
                    return error;
                }
                out.writeString("action", actionName);
                out.beginArray("bindings", static_cast<u32>(action->bindings.size()));
                for (const InputBindingMapping& existing : action->bindings)
                {
                    out.beginObject("");
                    writeBinding(out, existing);
                    out.endObject();
                }
                out.endArray();
                out.beginArray("conflicts", static_cast<u32>(conflicts.size()));
                for (const std::string& other : conflicts)
                {
                    out.writeString("", other);
                }
                out.endArray();
                return std::string{};
            });

        control.registerUndoableViewCommand(
            "removeBinding", OWNER,
            "removes the binding of a control from an action in a context; without key, every binding of the action "
            "-> removed, remaining. Not saved: saveBindings writes the file",
            {CONTEXT_ARG, ACTION_ARG, DEVICE_ARG,
             optionalArg("key", ArgKind::String, "the binding's key; absent removes all the action's bindings", "all"),
             MODIFIERS_ARG},
            [&services, contextOf, replaceBindings](const ControlArgs& args, IFormatWriter& out)
            {
                const InputContext* context = nullptr;
                std::vector<InputContext> storage;
                if (std::string error = contextOf(args, context, storage); !error.empty())
                {
                    return error;
                }
                const std::string actionName = args.string("action").value_or("");
                InputMappings mappings = services.get<InputSystem>().mappings(context->id);
                const auto action = std::ranges::find(mappings.actions, actionName, &InputActionMapping::action);
                if (action == mappings.actions.end())
                {
                    std::string names;
                    for (const InputActionMapping& known : mappings.actions)
                    {
                        names += (names.empty() ? "" : ", ") + known.action;
                    }
                    return "context '" + std::string(context->desc.name.toView()) + "' has no action '" + actionName +
                        "'; actions: " + names;
                }
                u32 removed = 0;
                if (const std::optional<std::string> key = args.string("key"))
                {
                    InputBindingMapping wanted;
                    if (std::string error = normalizeBinding(args.string("device").value_or(""), *key, wanted);
                        !error.empty())
                    {
                        return error;
                    }
                    std::string problem;
                    wanted.modifiers = normalizeModifiers(args.string("modifiers").value_or(""), problem);
                    if (!problem.empty())
                    {
                        return problem;
                    }
                    removed = static_cast<u32>(std::erase_if(action->bindings,
                                                             [&](const InputBindingMapping& existing)
                                                             {
                                                                 return upperAscii(existing.device) == wanted.device &&
                                                                     upperAscii(existing.key) == wanted.key &&
                                                                     existing.modifiers == wanted.modifiers;
                                                             }));
                    if (removed == 0)
                    {
                        return "'" + actionName + "' has no " + wanted.device + " binding '" + *key + "'" +
                            (wanted.modifiers.empty() ? std::string{} : " with " + wanted.modifiers);
                    }
                }
                else
                {
                    removed = static_cast<u32>(action->bindings.size());
                    action->bindings.clear();
                }
                if (std::string error = replaceBindings(*context, mappings, out); !error.empty())
                {
                    return error;
                }
                out.writeString("action", actionName);
                out.writeU32("removed", removed);
                out.writeU32("remaining", static_cast<u32>(action->bindings.size()));
                return std::string{};
            });

        control.registerUndoableViewCommand(
            "mergeProjectBindings", OWNER,
            "adds the project file's actions (with their bindings) that a context's user copy lacks, as inputActions "
            "lists them in projectMissing (an action the copy has fully unbound is offered again); the copy's own "
            "actions stay as they are -> added (the action names), conflicts: {action, with, device, key} for an "
            "added action on a control an earlier action of the context has, reported and kept as setBinding does. "
            "Not saved: saveBindings writes the file. Refused when nothing is missing",
            {CONTEXT_ARG},
            [&services, contextOf, replaceBindings](const ControlArgs& args, IFormatWriter& out)
            {
                const InputContext* context = nullptr;
                std::vector<InputContext> storage;
                if (std::string error = contextOf(args, context, storage); !error.empty())
                {
                    return error;
                }
                const InputSystem& input = services.get<InputSystem>();
                const InputMappings missing = projectActionsMissing(input, *context);
                if (missing.actions.empty())
                {
                    return "context '" + std::string(context->desc.name.toView()) +
                        "' has no project action its user copy lacks";
                }
                InputMappings mappings = input.mappings(context->id);
                const std::size_t firstAdded = mappings.actions.size();
                mappings.actions.insert(mappings.actions.end(), missing.actions.begin(), missing.actions.end());
                // an added action on a control an earlier action has: reported and kept, as setBinding does
                struct Clash
                {
                    std::string action;
                    std::string with;
                    std::string device;
                    std::string key;
                };
                std::vector<Clash> clashes;
                for (std::size_t added = firstAdded; added < mappings.actions.size(); ++added)
                {
                    for (const InputBindingMapping& binding : mappings.actions[added].bindings)
                    {
                        for (std::size_t other = 0; other < added; ++other)
                        {
                            if (std::ranges::any_of(mappings.actions[other].bindings,
                                                    [&](const InputBindingMapping& held)
                                                    { return sameControl(held, binding); }))
                            {
                                clashes.push_back({mappings.actions[added].action, mappings.actions[other].action,
                                                   binding.device, binding.key});
                            }
                        }
                    }
                }
                if (std::string error = replaceBindings(*context, mappings, out); !error.empty())
                {
                    return error;
                }
                out.beginArray("added", static_cast<u32>(missing.actions.size()));
                for (const InputActionMapping& action : missing.actions)
                {
                    out.writeString("", action.action);
                }
                out.endArray();
                out.beginArray("conflicts", static_cast<u32>(clashes.size()));
                for (const Clash& clash : clashes)
                {
                    out.beginObject("");
                    out.writeString("action", clash.action);
                    out.writeString("with", clash.with);
                    out.writeString("device", clash.device);
                    out.writeString("key", clash.key);
                    out.endObject();
                }
                out.endArray();
                return std::string{};
            });

        control.registerUndoableViewCommand(
            "restoreBindings", OWNER,
            "replaces a context's bindings with the actions given, as inputActions lists them (type by name): what "
            "setBinding and removeBinding record to undo themselves",
            {CONTEXT_ARG,
             requiredArg("actions", ArgKind::Json,
                         "an array of {action, type, bindings: [{device, key, scale, modifiers}]}")},
            [contextOf, replaceBindings](const ControlArgs& args, IFormatWriter& out)
            {
                const InputContext* context = nullptr;
                std::vector<InputContext> storage;
                if (std::string error = contextOf(args, context, storage); !error.empty())
                {
                    return error;
                }
                const nlohmann::json actions =
                    nlohmann::json::parse(args.json("actions").value_or("null"), nullptr, false);
                InputMappings mappings;
                if (std::string error = mappingsFromJson(actions, mappings); !error.empty())
                {
                    return error;
                }
                return replaceBindings(*context, mappings, out);
            });

        control.registerCommand(
            "saveBindings", OWNER,
            "writes a context's bindings to its mapping file: an editor context to the user layer (config/user, read "
            "before the project's file at the next start), any other to its own file in the project layer -> context, "
            "file, layer, actions. Refused with the reason when the context has no file or it cannot be written",
            {CONTEXT_ARG},
            [&services, contextOf](const ControlArgs& args, IFormatWriter& out)
            {
                const InputContext* context = nullptr;
                std::vector<InputContext> storage;
                if (std::string error = contextOf(args, context, storage); !error.empty())
                {
                    return error;
                }
                const InputSystem& input = services.get<InputSystem>();
                const std::string target = saveTarget(input, *context);
                const std::string name(context->desc.name.toView());
                // the default context's file is the mappingsFile config, set whether or not it ever loaded: saving
                // would create a file the engine never read; an editor context's user file is created by its first save
                if (target.empty() ||
                    (contextKind(name) != "editor" && !std::filesystem::exists(resolveContentPath(target))))
                {
                    return "context '" + name + "' has no mapping file" +
                        (target.empty() ? std::string(": nothing was loaded into it")
                                        : ": " + shownPath(target) + " was never loaded (it does not exist)");
                }
                if (!input.saveContextMappings(context->id, utf8Path(resolveUserConfigPath(pathFromUtf8(target)))))
                {
                    return "cannot write " + shownPath(target) +
                        ": the layer is read-only or the file cannot be created";
                }
                out.writeString("context", name);
                out.writeString("file", shownPath(target));
                out.writeString("layer", contextKind(name) == "editor" ? "user" : "project");
                out.writeU32("actions", static_cast<u32>(input.mappings(context->id).actions.size()));
                return std::string{};
            });

        control.registerCommand(
            "resetBindings", OWNER,
            "resets a context to its project defaults: reads the project layer's file again (unsaved edits are "
            "dropped) and, for an editor context, removes the user layer's copy that shadows it; refused while that "
            "copy exists unless force=true -> context, file, removed (the user copy), actions. Not undoable: the copy "
            "is gone",
            {CONTEXT_ARG, optionalArg("force", ArgKind::Boolean, "remove your saved copy", "false")},
            [&services, contextOf](const ControlArgs& args, IFormatWriter& out)
            {
                const InputContext* context = nullptr;
                std::vector<InputContext> storage;
                if (std::string error = contextOf(args, context, storage); !error.empty())
                {
                    return error;
                }
                InputSystem& input = services.get<InputSystem>();
                const std::string name(context->desc.name.toView());
                const std::string project = projectLayerMappingsFile(input.contextFile(context->id));
                // the project file is validated before anything is removed or replaced: a file that does not read
                // leaves the user copy and the bindings as they are
                const std::string problem = project.empty() ? std::string{} : inputMappingsFileProblem(project);
                if (project.empty() || !problem.empty())
                {
                    return "context '" + name + "' has no project file to read" +
                        (project.empty() ? std::string{} : ": " + problem);
                }
                // the copy goes first: when it cannot be removed it would shadow the project file at the next start,
                // so the reset fails and the context's bindings stay as they are
                bool removed = false;
                if (contextKind(name) == "editor")
                {
                    const std::filesystem::path copy =
                        resolveUserConfigPath(pathFromUtf8(userLayerMappingsFile(project)));
                    std::error_code failure;
                    if (!args.boolean("force").value_or(false) && std::filesystem::exists(copy, failure))
                    {
                        return "your saved copy " + shownPath(copy.string()) +
                            " would be removed: pass force=true to discard it";
                    }
                    removed = std::filesystem::remove(copy, failure);
                    std::error_code absent;
                    if (failure || std::filesystem::exists(copy, absent))
                    {
                        return "cannot remove your saved copy " + shownPath(copy.string()) + ": " +
                            (failure ? failure.message() : std::string("it is still there")) +
                            ", it would still replace the project's file at the next start";
                    }
                }
                if (!input.loadMappings(context->id, resolveContentPath(project).string()))
                {
                    return "context '" + name + "' has no project file to read: " + shownPath(project) +
                        " does not load";
                }
                out.writeString("context", name);
                out.writeString("file", shownPath(project));
                out.writeBool("removed", removed);
                out.writeU32("actions", static_cast<u32>(input.mappings(context->id).actions.size()));
                return std::string{};
            });
    }
} // namespace spite
