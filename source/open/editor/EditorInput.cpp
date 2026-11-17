#include "EditorInput.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorActions.hpp"
#include "EditorDialogs.hpp"
#include "EditorFeedback.hpp"
#include "EditorPageScroll.hpp"
#include "EditorPanelList.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "application/ApplicationTypes.hpp"
#include "application/input/InputActionMap.hpp"
#include "application/input/InputInjection.hpp"
#include "application/input/InputSystem.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "serialization/FormatWalk.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiForm.hpp"
#include "ui/UiInputSnapshot.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using namespace panelui;

        constexpr std::string_view INPUT_VIEW = "editor.input";
        constexpr std::string_view INPUT_BAR = "input.bar";
        constexpr std::string_view INPUT_CONTEXT = "input.context";
        constexpr std::string_view INPUT_FILTER = "input.filter";
        constexpr std::string_view INPUT_ROWS = "input.rows";
        constexpr std::string_view INPUT_EDIT = "input.edit";
        constexpr std::string_view INPUT_FILE = "input.file";
        constexpr std::string_view INPUT_STATUS = "input.status";
        constexpr std::array<std::string_view, 3> PAGES{"actions", "devices", "settings"};
        constexpr std::string_view EDITOR_KIND = "editor";
        constexpr std::string_view INPUT_SECTION = "application.input";
        constexpr u32 LIVE_FRAMES = 8;
        constexpr u32 COMMAND_REFRESH_FRAMES = 20;
        constexpr double CAPTURE_SECONDS = 10.0; // an armed capture nobody answers ends after this long
        constexpr u32 KEY_CODES = 512;
        constexpr u32 ESCAPE_CODE = static_cast<u32>(KeyCode::ESCAPE);
        constexpr float AXIS_CAPTURE = 0.5f; // an axis this far from rest is the one moved
        constexpr std::array<std::pair<u8, std::string_view>, 4> MODIFIER_BITS{{{UiKeyModifier::SHIFT, "SHIFT"},
                                                                                {UiKeyModifier::CTRL, "CTRL"},
                                                                                {UiKeyModifier::ALT, "ALT"},
                                                                                {UiKeyModifier::SUPER, "SUPER"}}};

        // The input settings the form edits (the application.input section): a field's range and its default.
        struct SettingField
        {
            std::string_view path;
            UiFormKind kind;
            double min;
            double max;
            double step;
            double defaultValue;
        };
        constexpr std::array<SettingField, 4> SETTINGS{{
            {"mouseSensitivity", UiFormKind::Number, 0.0, 20.0, 0.05, 1.0},
            {"controllerSensitivity", UiFormKind::Number, 0.0, 20.0, 0.05, 1.0},
            {"controllerDeadzone", UiFormKind::Number, 0.0, 0.99, 0.01, 0.1},
            {"invertYAxis", UiFormKind::Bool, 0.0, 1.0, 1.0, 0.0},
        }};
        constexpr float TAB_PAD = 16.0f; // a tab is as wide as its text and this much
        constexpr float CONTEXT_WIDTH = 160.0f;
        constexpr float SAVE_SETTINGS_WIDTH = 150.0f;
        // SDL's gamepad button and axis names in the order of its enums, as injectGamepad names them
        constexpr std::array<std::string_view, 15> PAD_BUTTONS{
            "south",      "east",         "west",          "north",  "back",     "guide",    "start",    "leftstick",
            "rightstick", "leftshoulder", "rightshoulder", "dpadup", "dpaddown", "dpadleft", "dpadright"};
        constexpr std::array<std::string_view, ApplicationConstants::GAMEPAD_AXIS_COUNT> PAD_AXES{
            "leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"};
        constexpr std::array<std::string_view, 5> MOUSE_BUTTONS{"left", "right", "middle", "x1", "x2"};

        struct Binding
        {
            std::string device;
            std::string key;
            std::string modifiers;
            float scale = 1.0f;
        };

        struct ActionInfo
        {
            std::string name;
            std::string type;
            std::vector<Binding> bindings;
        };

        struct ContextInfo
        {
            std::string name;
            std::string kind;
            std::string file; // the file it was read from, from config/ on
            std::string layer; // where a save writes
            std::string saveFile;
            bool saved = false; // the save file exists
            std::vector<std::string> projectMissing; // the project file's actions a user copy lacks
            std::vector<ActionInfo> actions;
        };

        // one table row: a binding of an action, or the action with none
        struct Row
        {
            std::string action;
            std::string type;
            Binding binding;
            bool bound = false;
            InternedString name;
            float value = 0.0f;
        };

        struct InputCells
        {
            std::string action;
            std::string type;
            std::string binding;
            std::string value;
            SPITE_OBSERVED_SCHEMA(InputCells, action, type, binding, value)
        };

        using DeviceRow = std::array<std::string, 4>; // device, control, value, state

        bool isModifierKey(const u32 code)
        {
            return code >= static_cast<u32>(KeyCode::LCTRL) && code <= static_cast<u32>(KeyCode::RGUI);
        }

        // what a capture armed: the action it binds (and the binding it replaces, when it rebinds one)
        struct Capture
        {
            bool active = false;
            std::string context;
            std::string action;
            std::string type;
            bool replacing = false;
            Binding old;
            u32 modifierKey = 0; // a modifier pressed alone: it is the binding when released with no other key
            std::array<std::array<bool, 32>, ApplicationConstants::MAX_SUPPORTED_GAMEPADS> buttons{};
            std::array<std::array<bool, 8>, ApplicationConstants::MAX_SUPPORTED_GAMEPADS> axes{};
        };

        // the config stores floats: a value read back is the float's double, so defaults and values compare as floats
        double asFloat(const double value) { return static_cast<double>(static_cast<float>(value)); }

        UiTableColumn column(const std::string_view field, const float width)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString("editor-input.column." + std::string(field))};
        }

        std::string lower(std::string text)
        {
            std::ranges::transform(text, text.begin(), [](const char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        std::string upper(std::string text)
        {
            std::ranges::transform(text, text.begin(), [](const char c) { return static_cast<char>(std::toupper(c)); });
            return text;
        }

        // "ctrl+shift" and "SHIFT+CTRL" are one chord
        std::string canonicalModifiers(const std::string_view text)
        {
            std::vector<std::string> words;
            for (sizet start = 0; start < text.size();)
            {
                const sizet end = std::min(text.find('+', start), text.size());
                words.push_back(upper(std::string(text.substr(start, end - start))));
                start = end + 1;
            }
            std::ranges::sort(words);
            std::string joined;
            for (const std::string& word : words)
            {
                joined += (joined.empty() ? "" : "+") + word;
            }
            return joined;
        }

        bool sameControl(const Binding& a, const Binding& b)
        {
            return upper(a.device) == upper(b.device) && upper(a.key) == upper(b.key) &&
                canonicalModifiers(a.modifiers) == canonicalModifiers(b.modifiers);
        }
    } // namespace

    struct EditorInput::State
    {
        State(Engine& engine, EditorDialogs& dialogs, EditorFeedback& feedback) :
            engine(&engine), control(&engine.control()), scope(editorUi(engine).ui()), dialogs(&dialogs),
            feedback(&feedback),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr),
            input(engine.services().contains<InputSystem>() ? &engine.services().get<InputSystem>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        UiScope scope;
        DensityFollower density{scope};
        EditorDialogs* dialogs;
        EditorFeedback* feedback;
        const Localization* localization;
        InputSystem* input;
        Panel root;
        PageScroll scroll; // the page scrolls where the panel is shorter than its bars and lists
        std::array<Button, PAGES.size()> tabs;
        Label contextLabel;
        Dropdown contextChoice;
        Label source;
        Label hint;
        Button rebind;
        Button add;
        Button remove;
        Button save;
        Button reset;
        Button merge;
        Button saveSettings;
        std::unique_ptr<PropertyForm> form;
        Capture capture;
        Entity selectedKey = Entity::undefined();
        std::unique_ptr<FilteredTable> list;
        std::unique_ptr<Table> devices;
        std::unique_ptr<UiDocumentView> view;

        std::vector<ContextInfo> contexts;
        std::map<std::string, std::map<std::string, std::string>>
            known; // context -> action -> type, kept for the session
        std::vector<Row> rows;
        std::vector<InputCells> cells; // parallel to rows
        std::vector<Entity> keys; // parallel to rows
        std::map<std::string, u32> keyIndex;
        std::vector<DeviceRow> deviceRows;
        std::vector<std::function<void()>> pending;
        std::vector<std::string> contextNames; // what the dropdown holds
        std::string selectedContext;
        std::string shownSignature;
        std::string shownSource;
        std::string shownHint;
        std::string shownRebind;
        u32 page = 0;
        double deadzone = 0.1;
        bool wasShown = false;
        bool commanded = false;
        bool stale = false;
        u32 sinceRefresh = 0;
        u32 sinceLive = 0;
        double armedSeconds = 0.0; // the frames' unscaled seconds since the capture was armed
        u64 seenCommand = 0;
        u64 seenLocale = 0;
        u64 refreshes = 0;
        u64 liveReads = 0;

        [[nodiscard]] std::string message(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            std::string scratch;
            return localization ? std::string(localization->resolve(InternedString(key), args, scratch))
                                : std::string(key);
        }

        [[nodiscard]] const ContextInfo* current() const
        {
            const auto it = std::ranges::find(contexts, selectedContext, &ContextInfo::name);
            return it == contexts.end() ? nullptr : &*it;
        }

        [[nodiscard]] Entity keyOf(const Row& row, const std::string_view context)
        {
            const std::string id = std::format("{}|{}|{}|{}|{}", context, row.action, row.binding.device,
                                               row.binding.key, canonicalModifiers(row.binding.modifiers));
            const auto [it, added] = keyIndex.try_emplace(id, static_cast<u32>(keyIndex.size() + 1));
            return Entity(it->second, 1u);
        }

        // "Ctrl+Shift+Z", "Mouse right", "Pad button south"; a scale other than 1 follows
        [[nodiscard]] std::string bindingText(const Row& row) const
        {
            if (!row.bound)
            {
                return message("editor-input.unbound");
            }
            const Binding& binding = row.binding;
            const std::string device = upper(binding.device);
            const auto named = [&](const auto& names)
            {
                const u32 index = static_cast<u32>(std::strtoul(binding.key.c_str(), nullptr, 10));
                return index < names.size() ? std::string(names[index]) : binding.key;
            };
            std::string text;
            if (device == "KEYBOARD")
            {
                text = EditorActions::chordText(InputBindingMapping{.device = binding.device,
                                                                    .key = binding.key,
                                                                    .scale = binding.scale,
                                                                    .modifiers = binding.modifiers});
            }
            else if (device == "MOUSE")
            {
                text = message("editor-input.device.mouse") + " " + named(MOUSE_BUTTONS);
            }
            else
            {
                const bool axis = device == "GAMEPAD_AXIS";
                text = message(axis ? "editor-input.device.padAxis" : "editor-input.device.padButton") + " " +
                    (axis ? named(PAD_AXES) : named(PAD_BUTTONS));
            }
            if (binding.scale != 1.0f)
            {
                text += std::format(" x{:g}", binding.scale);
            }
            return text;
        }

        [[nodiscard]] static std::string valueText(const float value) { return std::format("{:.2f}", value); }

        [[nodiscard]] float liveValue(const InternedString name) const
        {
            return input && input->hasAction(name) ? input->getActionValue(name) : 0.0f;
        }

        // the other actions of the context that hold the binding's control and chord
        [[nodiscard]] static std::vector<std::string> holdersOf(const ContextInfo& context, const std::string& except,
                                                                const Binding& binding)
        {
            std::vector<std::string> others;
            for (const ActionInfo& action : context.actions)
            {
                if (action.name != except &&
                    std::ranges::any_of(action.bindings, [&](const Binding& b) { return sameControl(b, binding); }))
                {
                    others.push_back(action.name);
                }
            }
            return others;
        }

        void pushRow(Row row, const ContextInfo& context)
        {
            row.name = InternedString(row.action);
            row.value = liveValue(row.name);
            std::string shown = bindingText(row);
            if (row.bound)
            {
                std::string joined;
                for (const std::string& other : holdersOf(context, row.action, row.binding))
                {
                    joined += (joined.empty() ? "" : ", ") + other;
                }
                if (!joined.empty())
                {
                    const MessageArg args[]{{"binding", std::string_view(shown)},
                                            {"actions", std::string_view(joined)}};
                    shown = message("editor-input.shared", args);
                }
            }
            cells.push_back(
                {row.action, message("editor-input.type." + row.type), std::move(shown), valueText(row.value)});
            keys.push_back(keyOf(row, context.name));
            rows.push_back(std::move(row));
        }

        // the selected context's actions, one row per binding (an action with none shows one unbound row)
        void buildRows()
        {
            rows.clear();
            cells.clear();
            keys.clear();
            const ContextInfo* context = current();
            if (!context)
            {
                return;
            }
            std::vector<std::string> present;
            for (const ActionInfo& action : context->actions)
            {
                present.push_back(action.name);
                for (const Binding& binding : action.bindings)
                {
                    pushRow({.action = action.name, .type = action.type, .binding = binding, .bound = true}, *context);
                }
                if (action.bindings.empty())
                {
                    pushRow({.action = action.name, .type = action.type}, *context);
                }
            }
            // an action whose last binding went is not in the context any more: it stays here, to bind again
            for (const auto& [name, type] : known[context->name])
            {
                if (!std::ranges::contains(present, name))
                {
                    pushRow({.action = name, .type = type}, *context);
                }
            }
        }

        void setLabel(Label& label, std::string& shown, const std::string& key, const std::string_view argName = {},
                      const std::string& arg = {}, const bool number = false)
        {
            const std::string signature = key + "|" + arg;
            if (signature == shown)
            {
                return;
            }
            shown = signature;
            label.setLocalizedText(InternedString(key));
            if (!argName.empty())
            {
                if (number)
                {
                    label.setLocalizedArg(argName, arg);
                }
                else
                {
                    label.setLocalizedArgText(argName, arg);
                }
            }
        }

        void showStatus()
        {
            const ContextInfo* context = current();
            const bool userCopy = context && context->file.contains("config/user/");
            const std::string& file = context ? context->file : std::string();
            if (capture.active)
            {
                setLabel(source, shownSource, "editor-input.capture", "action", capture.action);
            }
            else if (!context || file.empty())
            {
                setLabel(source, shownSource, "editor-input.source.none");
            }
            else
            {
                const bool shadowed = !userCopy && context->layer == "user" && context->saved;
                if (userCopy && !context->projectMissing.empty())
                {
                    setLabel(source, shownSource, "editor-input.source.userNewer", "count",
                             std::to_string(context->projectMissing.size()), true);
                }
                else
                {
                    setLabel(source, shownSource,
                             userCopy       ? "editor-input.source.user"
                                 : shadowed ? "editor-input.source.shadowed"
                                            : "editor-input.source.project",
                             "file", shadowed ? context->saveFile : file);
                }
            }
            // where Save writes is said only when it is not the file the line above says the bindings came from
            const bool sameFile = context && !context->saveFile.empty() && context->saveFile == file;
            if (hint.isVisible() == sameFile)
            {
                hint.setVisible(!sameFile);
            }
            if (context && !context->saveFile.empty())
            {
                setLabel(hint, shownHint, "editor-input.saveTo." + context->layer, "file", context->saveFile);
            }
            else
            {
                setLabel(hint, shownHint, "editor-input.saveTo.none");
            }
        }

        void toast(const ToastLevel level, const std::string_view key, const std::span<const MessageArg> args = {})
        {
            feedback->postToastKey(level, std::string("editor-input.") + std::string(key), args);
        }

        // Runs a command now; whether it was accepted (a refusal posts an error toast), and its result.
        bool run(const std::string_view command, const Json& args, Json* result = nullptr)
        {
            commanded = true;
            const Json reply = request(*control, Json{{"command", command}, {"args", args}});
            if (reply.value("ok", false))
            {
                if (result && reply.contains("result"))
                {
                    *result = reply["result"];
                }
                return true;
            }
            const std::string error = reply.contains("error") && reply["error"].is_string()
                ? reply["error"].get<std::string>()
                : std::string("refused");
            const MessageArg args2[]{{"error", std::string_view(error)}};
            toast(ToastLevel::Error, "error", args2);
            return false;
        }

        [[nodiscard]] const Row* selectedRow() const
        {
            const auto it = std::ranges::find(keys, selectedKey);
            return it == keys.end() || !std::ranges::contains(list->shown(), static_cast<u32>(it - keys.begin()))
                ? nullptr
                : &rows[static_cast<u32>(it - keys.begin())];
        }

        void showButtons()
        {
            const Row* row = selectedRow();
            const ContextInfo* context = current();
            const bool idle = !capture.active;
            const std::array<std::pair<Button*, bool>, 6> usable{
                {{&rebind, row != nullptr},
                 {&add, row != nullptr && idle},
                 {&remove, row != nullptr && row->bound && idle},
                 {&save, context && !context->saveFile.empty() && idle},
                 {&reset, context && !context->file.empty() && idle},
                 {&merge, context && !context->projectMissing.empty() && idle}}};
            for (const auto& [button, on] : usable)
            {
                if (button->isInteractable() != on)
                {
                    button->setInteractable(on);
                }
            }
            const std::string key = capture.active ? "editor-input.cancel" : "editor-input.rebind";
            if (key != shownRebind)
            {
                shownRebind = key;
                rebind.setLocalizedText(InternedString(key));
            }
        }

        // the controls of another action of the context on the same control and chord
        [[nodiscard]] std::vector<std::string> conflictsOf(const Capture& done, const Binding& binding) const
        {
            const auto context = std::ranges::find(contexts, done.context, &ContextInfo::name);
            return context == contexts.end() ? std::vector<std::string>{} : holdersOf(*context, done.action, binding);
        }

        // Applies a captured binding: one undo step (a batch) that drops the replaced binding, the other actions'
        // holders of the control when asked, and binds the new one.
        void bind(const Capture& done, const Binding& binding, const bool takeOver,
                  const std::vector<std::string>& others)
        {
            Json commands = Json::array();
            const auto drop = [&](const std::string& action, const Binding& of)
            {
                commands.push_back({{"command", "removeBinding"},
                                    {"args",
                                     {{"context", done.context},
                                      {"action", action},
                                      {"device", lower(of.device)},
                                      {"key", of.key},
                                      {"modifiers", of.modifiers}}}});
            };
            if (takeOver)
            {
                for (const std::string& other : others)
                {
                    drop(other, binding);
                }
            }
            if (done.replacing && !sameControl(done.old, binding))
            {
                drop(done.action, done.old);
            }
            commands.push_back({{"command", "setBinding"},
                                {"args",
                                 {{"context", done.context},
                                  {"action", done.action},
                                  {"device", lower(binding.device)},
                                  {"key", binding.key},
                                  {"modifiers", binding.modifiers},
                                  {"scale", binding.scale},
                                  {"type", done.type}}}});
            if (commands.size() == 1)
            {
                run("setBinding", commands[0]["args"]);
            }
            else
            {
                run("batch", {{"commands", commands}});
            }
        }

        // The captured control is the binding; another action holding it is asked about first.
        void apply(const Capture& done, Binding binding)
        {
            if (done.replacing)
            {
                binding.scale = done.old.scale;
            }
            const std::vector<std::string> others = conflictsOf(done, binding);
            if (others.empty())
            {
                bind(done, binding, false, others);
                return;
            }
            std::string joined;
            for (const std::string& other : others)
            {
                joined += (joined.empty() ? "" : ", ") + other;
            }
            const Row text{.action = done.action, .binding = binding, .bound = true};
            const std::string shownBinding = bindingText(text);
            const MessageArg args[]{{"binding", std::string_view(shownBinding)}, {"actions", std::string_view(joined)}};
            dialogs->choose({.title = message("editor-input.conflict.title"),
                             .body = message("editor-input.conflict.body", args),
                             .confirmLabel = message("editor-input.conflict.replace"),
                             .alternateLabel = message("editor-input.conflict.both")},
                            [this, done, binding, others](const EditorAnswer answer)
                            {
                                if (answer != EditorAnswer::Cancel)
                                {
                                    bind(done, binding, answer == EditorAnswer::Confirm, others);
                                    commanded = true;
                                }
                            });
        }

        // The capture ends with a control (deferred: the key handler runs inside the UI's dispatch).
        void finishCapture(Binding binding)
        {
            const Capture done = std::exchange(capture, Capture{});
            pending.push_back([this, done, binding] { apply(done, binding); });
            showStatus();
            showButtons();
        }

        void cancelCapture()
        {
            if (capture.active)
            {
                capture = Capture{};
                showStatus();
                showButtons();
            }
        }

        void select(const Row& row)
        {
            const Entity key = keys[static_cast<u32>(&row - rows.data())];
            selectedKey = key;
            list->table().setSelection(std::span(&key, 1));
            list->table().scrollTo(key);
            showButtons();
        }

        // Arms the capture for a row (replacing its binding), or for a new binding of its action.
        void arm(const Row& row, const bool replacing)
        {
            select(row);
            capture = Capture{};
            armedSeconds = 0.0;
            capture.active = true;
            capture.context = selectedContext;
            capture.action = row.action;
            capture.type = row.type;
            capture.replacing = replacing && row.bound;
            capture.old = row.binding;
            if (input)
            {
                for (u32 pad = 0; pad < capture.buttons.size(); ++pad)
                {
                    for (u32 button = 0; button < ApplicationConstants::GAMEPAD_BUTTON_COUNT; ++button)
                    {
                        capture.buttons[pad][button] =
                            input->getGamepadButtonState(static_cast<int>(pad), static_cast<int>(button)).isPressed;
                    }
                    for (u32 axis = 0; axis < PAD_AXES.size(); ++axis)
                    {
                        capture.axes[pad][axis] =
                            std::abs(input->getGamepadAxisValue(static_cast<int>(pad), static_cast<int>(axis))) >=
                            AXIS_CAPTURE;
                    }
                }
            }
            scope.router().requestFocus(rebind.entity());
            showStatus();
            showButtons();
        }

        // A key reached the focused button while capturing: it is claimed (true), and it is the binding unless it is
        // Escape (cancel), a modifier (the binding when released alone) or a pad button's navigation key.
        bool captureKey(const u32 scancode, const u8 modifiers, const bool repeat)
        {
            if (!capture.active)
            {
                return false;
            }
            // a pad button reaches the focus as the key its navigation intent stands for (South as Return): it is
            // claimed and left to pollCapture, which binds the button itself
            if (repeat || scope.router().keyIsNavigation())
            {
                return true;
            }
            if (scancode == ESCAPE_CODE && modifiers == 0)
            {
                cancelCapture();
            }
            else if (isModifierKey(scancode))
            {
                capture.modifierKey = scancode;
            }
            else
            {
                std::string chord;
                for (const auto& [bit, word] : MODIFIER_BITS)
                {
                    chord += (modifiers & bit) != 0 ? (chord.empty() ? "" : "+") + std::string(word) : "";
                }
                finishCapture({"KEYBOARD", keyNameOf(scancode), chord, 1.0f});
            }
            return true;
        }

        // A gamepad button pressed or an axis moved since the last frame, and a modifier released alone.
        void pollCapture()
        {
            if (!input)
            {
                return;
            }
            if (capture.modifierKey != 0 && !input->getKeyState(static_cast<KeyCode>(capture.modifierKey)).isPressed)
            {
                finishCapture({"KEYBOARD", keyNameOf(capture.modifierKey), "", 1.0f});
                return;
            }
            for (u32 pad = 0; pad < capture.buttons.size(); ++pad)
            {
                const bool connected = input->isGamepadConnected(static_cast<int>(pad));
                for (u32 button = 0; button < capture.buttons[pad].size(); ++button)
                {
                    const bool down = connected && button < ApplicationConstants::GAMEPAD_BUTTON_COUNT &&
                        input->getGamepadButtonState(static_cast<int>(pad), static_cast<int>(button)).isPressed;
                    if (down && !capture.buttons[pad][button])
                    {
                        finishCapture({"GAMEPAD_BUTTON", std::to_string(button), "", 1.0f});
                        return;
                    }
                    capture.buttons[pad][button] = down;
                }
                for (u32 axis = 0; axis < PAD_AXES.size(); ++axis)
                {
                    const bool moved = connected &&
                        std::abs(input->getGamepadAxisValue(static_cast<int>(pad), static_cast<int>(axis))) >=
                            AXIS_CAPTURE;
                    if (moved && !capture.axes[pad][axis])
                    {
                        finishCapture({"GAMEPAD_AXIS", std::to_string(axis), "", 1.0f});
                        return;
                    }
                    capture.axes[pad][axis] = moved;
                }
            }
        }

        // the input config's values into the form and the deadzone preview
        void readSettings()
        {
            const Json reply =
                request(*control, Json{{"query", "config"}, {"args", {{"section", std::string(INPUT_SECTION)}}}});
            const Json sections = reply.value("result", Json::object()).value("sections", Json::array());
            if (sections.empty())
            {
                return;
            }
            const Json values = sections[0].value("values", Json::object());
            deadzone = values.value("controllerDeadzone", deadzone);
            for (const SettingField& field : SETTINGS)
            {
                const InternedString id{field.path};
                const auto found = values.find(std::string(field.path));
                if (found == values.end() || form->isEditing(id))
                {
                    continue;
                }
                const double value =
                    found->is_boolean() ? (found->get<bool>() ? 1.0 : 0.0) : asFloat(found->get<double>());
                if (asFloat(form->value(id).number[0]) != value)
                {
                    form->setValue(id, UiFormValue::ofNumber(value));
                }
            }
        }

        void refresh()
        {
            SPITE_ZONE("EditorInput.refresh");
            ++refreshes;
            const Json reply = request(*control, Json{{"query", "inputActions"}}).value("result", Json::object());
            contexts.clear();
            for (const Json& entry : reply.value("contexts", Json::array()))
            {
                ContextInfo context{.name = entry.value("name", std::string{}),
                                    .kind = entry.value("kind", std::string{}),
                                    .file = entry.value("file", std::string{})};
                const Json saveTo = entry.value("saveTo", Json::object());
                context.layer = saveTo.value("layer", std::string{});
                context.saveFile = saveTo.value("file", std::string{});
                context.saved = saveTo.value("exists", false);
                for (const Json& name : entry.value("projectMissing", Json::array()))
                {
                    context.projectMissing.push_back(name.get<std::string>());
                }
                for (const Json& action : entry.value("actions", Json::array()))
                {
                    ActionInfo info{.name = action.value("action", std::string{}),
                                    .type = action.value("type", std::string{})};
                    known[context.name][info.name] = info.type;
                    for (const Json& binding : action.value("bindings", Json::array()))
                    {
                        info.bindings.push_back(
                            {binding.value("device", std::string{}), binding.value("key", std::string{}),
                             binding.value("modifiers", std::string{}), binding.value("scale", 1.0f)});
                    }
                    context.actions.push_back(std::move(info));
                }
                if (context.kind == EDITOR_KIND)
                {
                    for (const std::string_view name : EditorActions::ACTION_NAMES)
                    {
                        known[context.name].try_emplace(std::string(name), "press");
                    }
                }
                contexts.push_back(std::move(context));
            }
            if (!current() && !contexts.empty())
            {
                const auto first =
                    std::ranges::find_if(contexts, [](const ContextInfo& c) { return !c.actions.empty(); });
                selectedContext = (first == contexts.end() ? contexts.front() : *first).name;
            }
            std::vector<std::string> names;
            for (const ContextInfo& context : contexts)
            {
                names.push_back(context.name);
            }
            if (names != contextNames)
            {
                contextNames = names;
                std::vector<UiChoiceItem> items;
                for (const std::string& name : names)
                {
                    items.push_back({.id = InternedString(name), .label = name});
                }
                contextChoice.setItems(items);
            }
            const auto at = std::ranges::find(names, selectedContext);
            if (contextChoice.selected() != static_cast<i32>(at - names.begin()) && at != names.end())
            {
                contextChoice.setSelected(static_cast<i32>(at - names.begin()));
            }
            buildRows();
            seenLocale = localization ? localization->revision() : 0;
            std::string signature = selectedContext + "\n";
            for (u32 i = 0; i < cells.size(); ++i)
            {
                const InputCells& c = cells[i];
                signature += std::format("{}|{}|{}|{}|{}\n", c.action, c.type, c.binding, c.value, keys[i].id());
            }
            if (const ContextInfo* context = current())
            {
                signature += context->file + "|" + context->saveFile + (context->saved ? "|saved" : "");
            }
            showStatus();
            showButtons();
            if (signature != shownSignature)
            {
                shownSignature = std::move(signature);
                list->setItems(static_cast<u32>(rows.size()));
            }
        }

        // The live values of the shown rows, from the input system; the table is touched only when a text moved.
        void liveValues()
        {
            ++liveReads;
            bool changed = false;
            for (u32 i = 0; i < rows.size(); ++i)
            {
                const float value = liveValue(rows[i].name);
                if (value != rows[i].value)
                {
                    rows[i].value = value;
                    cells[i].value = valueText(value);
                    changed = true;
                }
            }
            if (changed)
            {
                list->setItems(static_cast<u32>(rows.size()));
            }
        }

        [[nodiscard]] static std::string bar(const float value, const bool signedAxis)
        {
            std::string text(11, '-');
            const float clamped = std::clamp(value, signedAxis ? -1.0f : 0.0f, 1.0f);
            text[static_cast<sizet>(signedAxis ? std::lround(clamped * 5.0f) + 5 : std::lround(clamped * 10.0f))] = '#';
            return text;
        }

        // The devices as the input core reads them now: rows of device, control, value and state.
        std::vector<DeviceRow> readDevices() const
        {
            std::vector<DeviceRow> out;
            if (!input)
            {
                return out;
            }
            const std::string idle = message("editor-input.state.idle");
            const auto list = [](const std::vector<std::string>& names)
            {
                std::string joined;
                for (const std::string& name : names)
                {
                    joined += (joined.empty() ? "" : ", ") + name;
                }
                return joined.empty() ? std::string("-") : joined;
            };
            std::vector<std::string> down;
            for (u32 code = 0; code < KEY_CODES; ++code)
            {
                if (input->getKeyState(static_cast<KeyCode>(code)).isPressed)
                {
                    down.push_back(keyNameOf(code));
                }
            }
            const std::string keyboard = message("editor-input.device.keyboard");
            out.push_back({keyboard, message("editor-input.devices.keys"), list(down), down.empty() ? idle : ""});
            const MouseState mouse = input->getMouseState();
            const std::string device = message("editor-input.device.mouse");
            out.push_back({device, message("editor-input.devices.position"),
                           std::format("{:.0f}, {:.0f}", mouse.x, mouse.y), ""});
            down.clear();
            for (u32 i = 0; i < MOUSE_BUTTONS.size(); ++i)
            {
                if (mouse.buttons[i].isPressed)
                {
                    down.emplace_back(MOUSE_BUTTONS[i]);
                }
            }
            out.push_back({device, message("editor-input.devices.buttons"), list(down), down.empty() ? idle : ""});
            bool any = false;
            for (int pad = 0; pad < static_cast<int>(ApplicationConstants::MAX_SUPPORTED_GAMEPADS); ++pad)
            {
                if (!input->isGamepadConnected(pad))
                {
                    continue;
                }
                any = true;
                const MessageArg args[]{{"index", static_cast<std::int64_t>(pad)}};
                const std::string name = message("editor-input.device.gamepad", args);
                down.clear();
                for (u32 button = 0; button < ApplicationConstants::GAMEPAD_BUTTON_COUNT; ++button)
                {
                    if (input->getGamepadButtonState(pad, static_cast<int>(button)).isPressed)
                    {
                        down.emplace_back(button < PAD_BUTTONS.size() ? std::string(PAD_BUTTONS[button])
                                                                      : std::to_string(button));
                    }
                }
                out.push_back({name, message("editor-input.devices.buttons"), list(down), down.empty() ? idle : ""});
                for (u32 axis = 0; axis < PAD_AXES.size(); ++axis)
                {
                    const float value = input->getGamepadAxisValue(pad, static_cast<int>(axis));
                    const bool trigger = axis >= 4;
                    const float raw = input->getGamepadRawAxisValue(pad, static_cast<int>(axis));
                    const std::string rawText = valueText(raw);
                    const MessageArg rawArgs[]{{"raw", std::string_view(rawText)}};
                    out.push_back({name, std::string(PAD_AXES[axis]), valueText(value) + " " + bar(value, !trigger),
                                   trigger             ? (value != 0.0f ? message("editor-input.state.active") : idle)
                                       : value != 0.0f ? message("editor-input.state.activeRaw", rawArgs)
                                       : raw != 0.0f   ? message("editor-input.state.deadzoneRaw", rawArgs)
                                                       : message("editor-input.state.deadzone")});
                }
            }
            if (!any)
            {
                out.push_back({message("editor-input.device.gamepadNone"), "-",
                               message(input->isControllerEnabled() ? "editor-input.devices.none"
                                                                    : "editor-input.devices.disabled"),
                               ""});
            }
            out.push_back({message("editor-input.device.gamepadNone"), message("editor-input.devices.deadzone"),
                           valueText(static_cast<float>(deadzone)), ""});
            return out;
        }

        void liveDevices()
        {
            ++liveReads;
            std::vector<DeviceRow> next = readDevices();
            if (next == deviceRows)
            {
                return;
            }
            deviceRows = std::move(next);
            devices->setRowCount(static_cast<u32>(deviceRows.size()));
            devices->rebindVisible();
        }

        void showPage()
        {
            for (u32 i = 0; i < tabs.size(); ++i)
            {
                tabs[i].setLocalizedText(InternedString("editor-input.tab." + std::string(PAGES[i])));
                tabs[i].setAccessibleNameKey(InternedString("editor-input.tab." + std::string(PAGES[i])));
                showSelected(tabs[i], i == page);
            }
            // the pages are the code's content of the document's slots: hidden with their page
            const bool actions = page == 0;
            list->setVisible(actions);
            for (Widget widget : {static_cast<Widget>(contextLabel), static_cast<Widget>(contextChoice),
                                  static_cast<Widget>(source), static_cast<Widget>(hint), static_cast<Widget>(rebind),
                                  static_cast<Widget>(add), static_cast<Widget>(remove), static_cast<Widget>(save),
                                  static_cast<Widget>(reset), static_cast<Widget>(merge)})
            {
                widget.setVisible(actions);
            }
            devices->list.setVisible(page == 1);
            form->root().setVisible(page == 2);
            saveSettings.setVisible(page == 2);
        }

        void setPage(const u32 index)
        {
            cancelCapture();
            page = index;
            showPage();
            wasShown = false;
        }

        // a command that names a row reads what changed first: its caller may have just changed it
        void ensureRead()
        {
            const u64 command = control->commandGeneration();
            if (!wasShown || commanded || stale || command != seenCommand)
            {
                wasShown = true;
                commanded = false;
                stale = false;
                seenCommand = command;
                sinceRefresh = 0;
                refresh();
            }
        }

        // the row a command names: the action's, or its binding's by key (device and chord optional)
        const Row* findRow(const ControlArgs& args, std::string& error)
        {
            ensureRead();
            const std::string action = args.string("action").value_or("");
            const std::optional<std::string> key = args.string("key");
            const std::string device = upper(args.string("device").value_or("KEYBOARD"));
            const std::optional<std::string> chord = args.string("modifiers");
            for (const Row& row : rows)
            {
                if (row.action == action &&
                    (!key ||
                     (row.bound && upper(row.binding.key) == upper(*key) && upper(row.binding.device) == device &&
                      (!chord || canonicalModifiers(row.binding.modifiers) == canonicalModifiers(*chord)))))
                {
                    return &row;
                }
            }
            error = "no row of '" + action + "' in context '" + selectedContext + "'" +
                (key ? " with the binding '" + *key + "'" : std::string{});
            return nullptr;
        }

        void registerCommands()
        {
            const std::array<ArgDesc, 4> rowArgs{
                optionalArg("action", ArgKind::String, "the action: one of the selected context's rows"),
                optionalArg("key", ArgKind::String, "the binding's key; absent names the action's first row"),
                optionalArg("device", ArgKind::String, "the binding's device", "KEYBOARD"),
                optionalArg("modifiers", ArgKind::String, "the binding's chord, SHIFT+CTRL as in the file", "any")};
            control->registerViewCommand("inputSelect", "editorInput", "selects a binding's row on the actions page",
                                         {rowArgs[0], rowArgs[1], rowArgs[2], rowArgs[3]},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         {
                                             std::string error;
                                             const Row* row = findRow(args, error);
                                             if (!row)
                                             {
                                                 return error;
                                             }
                                             select(*row);
                                             return std::string{};
                                         });
            control->registerViewCommand("inputTab", "editorInput", "sets the page the input panel shows",
                                         {requiredArg("tab", ArgKind::String, "actions, devices or settings", PAGES)},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         {
                                             const auto it = std::ranges::find(PAGES, args.string("tab").value_or(""));
                                             if (it == PAGES.end())
                                             {
                                                 return std::string("tab must be actions, devices or settings");
                                             }
                                             setPage(static_cast<u32>(it - PAGES.begin()));
                                             return std::string{};
                                         });
            control->registerViewCommand(
                "inputContext", "editorInput", "selects the input context the actions page lists",
                {requiredArg("context", ArgKind::String, "a context's name, as inputActions lists it")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    ensureRead();
                    const std::string name = args.string("context").value_or("");
                    const auto it = std::ranges::find_if(contexts, [&](const ContextInfo& c)
                                                         { return upper(c.name) == upper(name); });
                    if (it == contexts.end())
                    {
                        return "unknown input context '" + name + "'";
                    }
                    selectedContext = it->name;
                    cancelCapture();
                    selectedKey = Entity::undefined();
                    refresh();
                    return std::string{};
                });
            control->registerViewCommand(
                "inputCapture", "editorInput",
                "arms the capture of a binding on the actions page: the next key (with the modifiers held), gamepad "
                "button or moved axis becomes a binding of the action, replacing the one named by key when it is "
                "given; Escape or cancel=true ends it",
                {rowArgs[0], rowArgs[1], rowArgs[2], rowArgs[3],
                 optionalArg("cancel", ArgKind::Boolean, "ends an armed capture", "false")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    if (args.boolean("cancel").value_or(false))
                    {
                        cancelCapture();
                        return std::string{};
                    }
                    std::string error;
                    const Row* row = findRow(args, error);
                    if (!row)
                    {
                        return error;
                    }
                    if (page != 0)
                    {
                        setPage(0);
                    }
                    arm(*row, args.string("key").has_value());
                    return std::string{};
                });
            control->registerQuery(
                "inputView", "editorInput",
                "the input panel: its page, the selected context and its source (projectMissing: the project actions "
                "its user copy lacks, which Add project actions merges), the action rows (action, type, "
                "binding, value; the binding's device, key, modifiers, bound), the device rows (device, control, "
                "value, state) and the read counters",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    out.writeString("page", PAGES[page]);
                    out.writeString("context", selectedContext);
                    out.beginArray("contexts", static_cast<u32>(contextNames.size()));
                    for (const std::string& name : contextNames)
                    {
                        out.writeString("", name);
                    }
                    out.endArray();
                    const ContextInfo* context = current();
                    out.writeString("file", context ? context->file : std::string{});
                    out.writeString("saveFile", context ? context->saveFile : std::string{});
                    out.writeString("layer", context ? context->layer : std::string{});
                    out.beginArray("projectMissing", context ? static_cast<u32>(context->projectMissing.size()) : 0u);
                    for (const std::string& name : context ? context->projectMissing : std::vector<std::string>{})
                    {
                        out.writeString("", name);
                    }
                    out.endArray();
                    out.beginArray("rows", static_cast<u32>(rows.size()));
                    for (u32 i = 0; i < rows.size(); ++i)
                    {
                        out.beginObject("");
                        writeValue(InputCells::spiteSchema(), &cells[i], out, FieldSet::All);
                        out.writeString("device", rows[i].binding.device);
                        out.writeString("key", rows[i].binding.key);
                        out.writeString("modifiers", rows[i].binding.modifiers);
                        out.writeBool("bound", rows[i].bound);
                        out.endObject();
                    }
                    out.endArray();
                    out.beginArray("devices", static_cast<u32>(deviceRows.size()));
                    for (const DeviceRow& row : deviceRows)
                    {
                        out.beginObject("");
                        out.writeString("device", row[0]);
                        out.writeString("control", row[1]);
                        out.writeString("value", row[2]);
                        out.writeString("state", row[3]);
                        out.endObject();
                    }
                    out.endArray();
                    out.beginObject("capture");
                    out.writeBool("active", capture.active);
                    out.writeString("action", capture.action);
                    out.writeBool("replacing", capture.replacing);
                    out.endObject();
                    out.beginObject("settings");
                    for (const SettingField& field : SETTINGS)
                    {
                        const UiFormRowState state = form->state(InternedString(field.path));
                        out.beginObject(field.path);
                        out.writeF64("value", state.value.number[0]);
                        out.writeBool("modified", state.modified);
                        out.endObject();
                    }
                    out.endObject();
                    out.writeU64("refreshes", refreshes);
                    out.writeU64("liveReads", liveReads);
                    list->writeView(out);
                    return std::string{};
                });
        }
    };

    EditorInput::EditorInput(Engine& engine, EditorDialogs& dialogs, EditorFeedback& feedback) :
        m_state(std::make_unique<State>(engine, dialogs, feedback))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.root = scope.createPanel("editor.panel", columnDesc());
        state.scroll =
            PageScroll(scope, InternedString("input.scroll"), InternedString("editor-input.title"), state.root);
        for (u32 i = 0; i < state.tabs.size(); ++i)
        {
            UiRectDesc tab = barDesc(Vec2{});
            tab.contentSize =
                UiContentSizeComponent{.fitWidth = true, .fitHeight = false, .padding = Vec2{TAB_PAD, 0.0f}};
            state.tabs[i] = state.density.track(scope.createButton("input.tab", "", tab), HEIGHT_CONTROL);
            state.tabs[i].onClick(
                [&state, i]
                { state.pending.push_back([&state, i] { state.run("inputTab", {{"tab", std::string(PAGES[i])}}); }); });
        }
        UiRectDesc contextText = barDesc(Vec2{});
        contextText.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = false};
        state.contextLabel =
            state.density.track(scope.createLabel("input.contextLabel", "", contextText), HEIGHT_CONTROL);
        state.contextLabel.setLocalizedText(InternedString("editor-input.context"));
        UiDropdownDesc dropdown;
        dropdown.rect = barDesc(Vec2{CONTEXT_WIDTH, 0.0f});
        state.contextChoice =
            state.density.track(scope.createDropdown("input.contextChoice", dropdown), HEIGHT_CONTROL);
        state.contextChoice.setAccessibleNameKey(InternedString("editor-input.context"));
        state.contextChoice.onChange(
            [&state](const u32 index)
            {
                if (index < state.contextNames.size())
                {
                    state.pending.push_back([&state, name = state.contextNames[index]]
                                            { state.run("inputContext", {{"context", name}}); });
                }
            });
        state.source = state.density.track(scope.createLabel("input.source", "", barDesc(Vec2{})), HEIGHT_ROW);
        state.source.setStyleClass(InternedString("input.note"));
        state.hint = state.density.track(scope.createLabel("input.hint", "", barDesc(Vec2{})), HEIGHT_ROW);
        state.hint.setStyleClass(InternedString("input.note"));
        // a bar's buttons share its width; one with a width of its own keeps it
        const auto button = [&](const std::string_view id, const std::string_view key, const std::string_view tip,
                                const float width = 0.0f, const bool fit = false)
        {
            UiRectDesc desc = fit ? fitWidthDesc() : barDesc(Vec2{width, 0.0f});
            if (width == 0.0f && !fit)
            {
                desc.layoutItem = fillItem();
            }
            Button made = state.density.track(scope.createButton(InternedString(id), "", desc), HEIGHT_CONTROL);
            if (fit)
            {
                bindFitPadding(made, SPACE_MD, SPACE_NONE);
            }
            made.setLocalizedText(InternedString("editor-input." + std::string(key)));
            made.setTooltip(InternedString("editor-input.hint." + std::string(tip)));
            return made;
        };
        state.rebind = button("input.rebind", "rebind", "rebind", 0.0f, true);
        state.add = button("input.add", "add", "add", 0.0f, true);
        state.remove = button("input.remove", "remove", "remove", 0.0f, true);
        state.save = button("input.save", "save", "save", 0.0f, true);
        state.reset = button("input.reset", "reset", "reset", 0.0f, true);
        state.merge = button("input.merge", "merge", "merge", 0.0f, true);
        state.saveSettings = button("input.saveSettings", "saveSettings", "saveSettings", SAVE_SETTINGS_WIDTH);
        state.shownRebind = "editor-input.rebind";
        for (Button* edit : {&state.rebind, &state.add, &state.remove})
        {
            edit->onKeyDown([&state](const u32 scancode, const u8 modifiers, const bool repeat)
                            { return state.captureKey(scancode, modifiers, repeat); });
        }
        state.rebind.onClick(
            [&state]
            {
                state.pending.push_back(
                    [&state]
                    {
                        if (state.capture.active)
                        {
                            state.cancelCapture();
                        }
                        else if (const Row* row = state.selectedRow())
                        {
                            state.arm(*row, true);
                        }
                    });
            });
        state.add.onClick(
            [&state]
            {
                state.pending.push_back(
                    [&state]
                    {
                        if (const Row* row = state.selectedRow())
                        {
                            state.arm(*row, false);
                        }
                    });
            });
        state.remove.onClick(
            [&state]
            {
                state.pending.push_back(
                    [&state]
                    {
                        if (const Row* row = state.selectedRow(); row && row->bound)
                        {
                            state.run("removeBinding",
                                      {{"context", state.selectedContext},
                                       {"action", row->action},
                                       {"device", lower(row->binding.device)},
                                       {"key", row->binding.key},
                                       {"modifiers", row->binding.modifiers}});
                        }
                    });
            });
        state.save.onClick(
            [&state]
            {
                state.pending.push_back(
                    [&state]
                    {
                        Json written;
                        if (state.run("saveBindings", {{"context", state.selectedContext}}, &written))
                        {
                            const std::string file = written.value("file", std::string{});
                            const MessageArg args[]{{"file", std::string_view(file)}};
                            state.toast(ToastLevel::Info, "saved", args);
                        }
                    });
            });
        state.reset.onClick(
            [&state]
            {
                state.pending.push_back(
                    [&state]
                    {
                        const ContextInfo* context = state.current();
                        if (!context)
                        {
                            return;
                        }
                        const bool user = context->file.contains("config/user/") || context->saved;
                        const MessageArg args[]{{"context", std::string_view(context->name)},
                                                {"file", std::string_view(context->saveFile)}};
                        state.dialogs->confirm(
                            {.title = state.message("editor-input.reset.title"),
                             .body =
                                 state.message(user ? "editor-input.reset.body.user" : "editor-input.reset.body", args),
                             .confirmLabel = state.message("editor-input.reset.confirm"),
                             .destructive = true},
                            [&state, name = context->name](const bool confirmed)
                            {
                                Json result;
                                if (confirmed &&
                                    state.run("resetBindings", {{"context", name}, {"force", true}}, &result))
                                {
                                    const std::string file = result.value("file", std::string{});
                                    const MessageArg done[]{{"context", std::string_view(name)},
                                                            {"file", std::string_view(file)}};
                                    state.toast(ToastLevel::Info, "reset.done", done);
                                }
                            });
                    });
            });
        state.merge.onClick(
            [&state]
            {
                state.pending.push_back(
                    [&state]
                    {
                        Json merged;
                        if (state.run("mergeProjectBindings", {{"context", state.selectedContext}}, &merged))
                        {
                            const MessageArg args[]{
                                {"count", static_cast<std::int64_t>(merged.value("added", Json::array()).size())},
                                {"context", std::string_view(state.selectedContext)}};
                            state.toast(ToastLevel::Info, "merged", args);
                            // an added action on a control another action has: kept, and said (the table also marks it)
                            const Json clashes = merged.value("conflicts", Json::array());
                            if (!clashes.empty())
                            {
                                const std::string action = clashes[0].value("action", std::string{});
                                const std::string with = clashes[0].value("with", std::string{});
                                const std::string key = clashes[0].value("key", std::string{});
                                const MessageArg clash[]{{"count", static_cast<std::int64_t>(clashes.size())},
                                                         {"action", std::string_view(action)},
                                                         {"with", std::string_view(with)},
                                                         {"key", std::string_view(key)}};
                                state.toast(ToastLevel::Warn, "merged.conflicts", clash);
                            }
                        }
                    });
            });
        state.saveSettings.onClick(
            [&state]
            {
                state.pending.push_back(
                    [&state]
                    {
                        Json written;
                        if (state.run("saveConfig", {{"section", std::string(INPUT_SECTION)}}, &written))
                        {
                            const std::string file = written.value("file", std::string{});
                            const MessageArg args[]{{"file", std::string_view(file)}};
                            state.toast(ToastLevel::Info, "saved", args);
                        }
                    });
            });
        UiFormDesc formDesc;
        formDesc.rect = topLeft();
        for (const SettingField& field : SETTINGS)
        {
            UiFormField entry;
            entry.id = InternedString(field.path);
            entry.labelKey = InternedString("editor-input.setting." + std::string(field.path));
            entry.docKey = InternedString("editor-input.setting." + std::string(field.path) + ".doc");
            entry.kind = field.kind;
            entry.min = field.min;
            entry.max = field.max;
            entry.step = field.step;
            entry.decimals = 2;
            entry.defaultValue = UiFormValue::ofNumber(asFloat(field.defaultValue));
            formDesc.fields.push_back(std::move(entry));
        }
        state.form = std::make_unique<PropertyForm>(scope, InternedString("input.form"), std::move(formDesc));
        state.form->onEdit(
            [&state](const UiFormEdit& edit)
            {
                const std::string path(edit.field.toView());
                const bool flag = std::ranges::find(SETTINGS, path, &SettingField::path)->kind == UiFormKind::Bool;
                const Json value = flag ? Json(edit.value.number[0] != 0.0) : Json(edit.value.number[0]);
                state.pending.push_back(
                    [&state, path, value, reset = edit.reset]
                    {
                        if (reset)
                        {
                            state.run("resetConfig", {{"section", std::string(INPUT_SECTION)}, {"path", path}});
                        }
                        else
                        {
                            state.run("setConfig",
                                      {{"section", std::string(INPUT_SECTION)}, {"path", path}, {"value", value}});
                        }
                    });
            });

        state.list = std::make_unique<FilteredTable>(
            scope, *state.control,
            FilteredTableDesc{.id = "input",
                              .owner = "editorInput",
                              .nameKey = InternedString("editor-input.table"),
                              .schema = &InputCells::spiteSchema(),
                              .columns = {column("action", 190.0f), column("type", 60.0f), column("binding", 130.0f),
                                          column("value", 56.0f)},
                              .row = [&state](const u32 item) -> const void* { return &state.cells[item]; },
                              .key = [&state](const u32 item) { return state.keys[item]; },
                              .matches =
                                  [&state](const u32 item, const std::string_view needle)
                              {
                                  const InputCells& row = state.cells[item];
                                  return containsIgnoringCase(row.action, needle) ||
                                      containsIgnoringCase(row.binding, needle) ||
                                      containsIgnoringCase(row.type, needle);
                              }});
        state.devices = std::make_unique<Table>(
            scope,
            TableDesc{.id = "input.devices",
                      .columns = {96.0f, 110.0f, 130.0f},
                      .cells = 4,
                      .read = [&state](const u32 row, const std::span<std::string> cells, std::string&)
                      {
                          for (u32 i = 0; i < cells.size(); ++i)
                          {
                              cells[i] = state.deviceRows[row][i];
                          }
                      }});
        state.devices->list.setLayoutItem(fillItem());
        state.list->table().onSelect(
            [&state](const std::span<const Entity> selected)
            {
                state.selectedKey = selected.empty() ? Entity::undefined() : selected[0];
                state.showButtons();
            });
        state.registerCommands();
        scope.commit();

        // the slots whose buttons moved to another bar stay in the document and take no room
        const auto hideSlot = [](const Widget& slot)
        {
            Widget hidden = slot;
            hidden.setVisible(false);
        };
        UiViewContract contract;
        contract.slot(InternedString(INPUT_BAR),
                      [&state](const Widget& slot)
                      {
                          for (Button& tab : state.tabs)
                          {
                              putIn(state.scope, slot, tab);
                              setOwnClass(tab, slotFields(state.scope, slot).childClass);
                          }
                          // the context choice shares the tabs' bar (one bar fewer in a 1024x600 layout)
                          putIn(state.scope, slot, state.contextLabel);
                          putIn(state.scope, slot, state.contextChoice);
                      });
        contract.slot(InternedString(INPUT_CONTEXT), hideSlot);
        contract.slot(InternedString(INPUT_FILTER), [&state](const Widget& slot) { state.list->fillFilter(slot); });
        // the actions table, the devices list and the settings form share the rows slot, one of them shown: the form
        // sits at its top, with its Save button under it
        contract.slot(InternedString(INPUT_ROWS),
                      [&state](const Widget& slot)
                      {
                          state.list->fillRows(slot);
                          fillList(state.scope, slot, *state.devices);
                          putIn(state.scope, slot, state.form->root());
                          putIn(state.scope, slot, state.saveSettings);
                      });
        contract.slot(InternedString(INPUT_EDIT),
                      [&state](const Widget& slot)
                      {
                          // the edit and file buttons share one bar: five 34 px bars and the status left the list no
                          // room in a 1024x600 layout; the file slot stays in the document, empty
                          for (Button* button :
                               {&state.rebind, &state.add, &state.remove, &state.save, &state.reset, &state.merge})
                          {
                              putIn(state.scope, slot, *button);
                              button->setStyleClass(slotFields(state.scope, slot).childClass);
                          }
                      });
        contract.slot(InternedString(INPUT_FILE), hideSlot);
        contract.slot(InternedString(INPUT_STATUS),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.source);
                          putIn(state.scope, slot, state.hint);
                      });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                // a bar of the page that is shown: its height is its content's, none while the page is hidden
                // the buttons' bars wrap onto another line when the panel is narrower than they are
                const auto bar = [&](const std::string_view id,
                                     const UiLayoutDirection direction = UiLayoutDirection::Row,
                                     const bool wrap = false, const bool stretch = false)
                {
                    const Density density = Density::read(viewScope);
                    UiRectDesc desc = topLeft();
                    desc.layout = stack(direction, density.xs, Vec4{density.xs, density.xxs, density.xs, density.xxs},
                                        direction == UiLayoutDirection::Column || stretch ? UiLayoutAlign::Stretch
                                                                                          : UiLayoutAlign::Start);
                    desc.layout->wrap = wrap;
                    desc.layoutItem = fixedItem();
                    desc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                    Panel panel = viewScope.createPanel(InternedString(id), desc);
                    bindGap(panel, SPACE_XS);
                    bindPadding(panel, SPACE_XS, SPACE_XXS, SPACE_XS, SPACE_XXS);
                    panel.setParent(root);
                    return panel;
                };
                bar(INPUT_BAR, UiLayoutDirection::Row, true, true);
                bar(INPUT_CONTEXT);
                bar(INPUT_FILTER);
                fillSlotPanel(viewScope, INPUT_ROWS, root);
                bar(INPUT_EDIT, UiLayoutDirection::Row, true);
                bar(INPUT_FILE);
                bar(INPUT_STATUS, UiLayoutDirection::Column);
            });
        state.view =
            openView(engine, INPUT_VIEW, "assets/ui/editor/input.json", std::move(contract), state.scroll.page());
        state.showPage();
    }

    EditorInput::~EditorInput() { m_state->control->unregisterOwner("editorInput"); }

    Widget EditorInput::content() const { return m_state->root; }

    void EditorInput::update()
    {
        State& state = *m_state;
        if (!state.pending.empty())
        {
            std::vector<std::function<void()>> actions;
            actions.swap(state.pending);
            for (const std::function<void()>& action : actions)
            {
                action();
            }
        }
        state.list->update();
        if (state.capture.active)
        {
            // the capture belongs to the focused Rebind, Add or Remove button: focus elsewhere or silence ends it
            const Entity focus = state.scope.router().focused();
            const bool held =
                focus == state.rebind.entity() || focus == state.add.entity() || focus == state.remove.entity();
            state.armedSeconds += state.engine->lastFrameSeconds();
            if (!held || state.armedSeconds >= CAPTURE_SECONDS)
            {
                state.cancelCapture();
            }
            else
            {
                state.pollCapture();
            }
        }
        if (!shown(state.scope, state.root))
        {
            state.wasShown = false;
            state.cancelCapture();
            return;
        }
        state.scroll.fit(state.density.density());
        const u64 command = state.control->commandGeneration();
        if (command != state.seenCommand)
        {
            state.seenCommand = command;
            state.stale = true;
        }
        ++state.sinceRefresh;
        if (!state.wasShown || state.commanded || (state.stale && state.sinceRefresh >= COMMAND_REFRESH_FRAMES) ||
            (state.localization && state.localization->revision() != state.seenLocale))
        {
            state.wasShown = true;
            state.commanded = false;
            state.stale = false;
            state.sinceRefresh = 0;
            state.sinceLive = 0;
            state.refresh();
            if (state.page != 0)
            {
                state.readSettings();
            }
            if (state.page == 1)
            {
                state.deviceRows.clear();
                state.liveDevices();
            }
            return;
        }
        if (++state.sinceLive >= LIVE_FRAMES)
        {
            state.sinceLive = 0;
            if (state.page == 0)
            {
                state.liveValues();
            }
            else if (state.page == 1)
            {
                state.liveDevices();
            }
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "input",
             .category = "tools",
             .order = 35,
             .dock = {.target = "scene"}, // the Scene group is the wide one; the bottom strip is full
             .zone = "EditorInput",
             .make = [](EditorPanelContext& c)
             { return makeEditorPanel<EditorInput>(c.engine, c.dialogs, c.feedback); }}};
    } // namespace
} // namespace spite
