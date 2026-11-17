#include "base/WritableLayout.hpp"
#include "base/io/FileSystem.hpp"
#include "EditorActions.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "application/input/InputSystem.hpp"
#include "base/ContentRoot.hpp"
#include "base/Logging.hpp"
#include "base/ServiceLocator.hpp"
#include "control/ControlServer.hpp"
#include "EditorContext.hpp"
#include "EditorDialogRequests.hpp"
#include "engine/UiDisplayCommands.hpp"
#include "EditorFeedback.hpp"
#include "EditorGizmoSettings.hpp"
#include "editor/EditorModel.hpp"
#include "EditorSceneView.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        constexpr u32 ACTION_COUNT = static_cast<u32>(EditorActions::Action::Count);
        static_assert(std::size(EditorActions::ACTION_NAMES) == ACTION_COUNT);

        std::string capitalized(std::string_view word)
        {
            std::string text(word);
            for (sizet i = 0; i < text.size(); ++i)
            {
                text[i] = static_cast<char>(i == 0 ? std::toupper(static_cast<unsigned char>(text[i]))
                                                   : std::tolower(static_cast<unsigned char>(text[i])));
            }
            return text;
        }

        // a key name as a reader writes it: LShift, PageUp; the rest is capitalised
        std::string keyText(const std::string& key)
        {
            constexpr std::array<std::pair<std::string_view, std::string_view>, 8> NAMED{{{"LSHIFT", "LShift"},
                                                                                          {"RSHIFT", "RShift"},
                                                                                          {"LCTRL", "LCtrl"},
                                                                                          {"RCTRL", "RCtrl"},
                                                                                          {"LALT", "LAlt"},
                                                                                          {"RALT", "RAlt"},
                                                                                          {"PAGEUP", "PageUp"},
                                                                                          {"PAGEDOWN", "PageDown"}}};
            for (const auto& [name, text] : NAMED)
            {
                if (key == name)
                {
                    return std::string(text);
                }
            }
            if (key == "EQUALS")
                return "=";
            if (key == "MINUS")
                return "-";
            if (key == "KP_PLUS")
                return "Num+";
            if (key == "KP_MINUS")
                return "Num-";
            if (key == "KP_0")
                return "Num0";
            if (key == "KP_EQUALS")
                return "Num=";
            return key.size() == 1 ? key : capitalized(key);
        }
    } // namespace

    // "SHIFT+CTRL" and "Z" as "Ctrl+Shift+Z" (Ctrl, Alt, Shift, Super); a key of one character stays upper case
    std::string EditorActions::chordText(const InputBindingMapping& binding)
    {
        std::string text;
        for (const std::string_view modifier : {"CTRL", "ALT", "SHIFT", "SUPER"})
        {
            for (std::string_view rest = binding.modifiers; !rest.empty();)
            {
                const sizet plus = std::min(rest.find('+'), rest.size());
                if (rest.substr(0, plus) == modifier)
                {
                    text += capitalized(modifier) + "+";
                    break;
                }
                rest.remove_prefix(std::min(plus + 1, rest.size()));
            }
        }
        return text + keyText(binding.key);
    }

    struct EditorActions::State
    {
        Engine* engine;
        const EditorModel* editorModel;
        const EditorFeedback* feedback;
        InputSystem* input = nullptr;
        InputContextId context = INVALID_INPUT_CONTEXT_ID;
        std::array<InternedString, ACTION_COUNT> hotkeys;
        std::vector<Action> queued;
        u64 zoomToast = 0;
        std::function<void(Action)> fileHandler;
        std::function<void()> paletteHandler;

        ControlServer& control() const { return engine->control(); }

        // undo, redo and paste act on what the game view shows
        std::string activeWorld() const { return engine->playWorld() ? "play" : "scene"; }

        Json send(const Json& request) const
        {
            const std::string line = request.dump();
            Json reply = Json::parse(control().handle(line), nullptr, false);
            if (!reply.is_object() || !reply.value("ok", false))
            {
                SWARNING_LOG("editor: {} -> {}", line, reply.dump());
                return Json();
            }
            return std::move(reply["result"]);
        }

        void select(const std::string& world, const Json& ids) const
        {
            send({{"command", "select"}, {"args", {{"world", world}, {"ids", ids}}}});
        }

        // command (id) on each of the selection's topmost entities, one batch
        // for several; returns the ids the replies name
        Json eachSelected(const std::string_view command) const
        {
            const std::string world(editorModel->selectionWorld());
            const std::vector<Entity> roots = EditorModelAccess::topmostSelection(*editorModel);
            Json ids = Json::array();
            if (roots.empty())
            {
                return ids;
            }
            if (roots.size() == 1)
            {
                const Json result =
                    send({{"command", command}, {"args", {{"world", world}, {"id", roots.front().id()}}}});
                if (result.contains("id"))
                {
                    ids.push_back(result["id"]);
                }
                return ids;
            }
            Json commands = Json::array();
            for (const Entity entity : roots)
            {
                commands.push_back({{"command", command}, {"args", {{"id", entity.id()}}}});
            }
            const Json result = send({{"command", "batch"}, {"args", {{"world", world}, {"commands", commands}}}});
            if (result.contains("results"))
            {
                for (const Json& each : result["results"])
                {
                    ids.push_back(each["id"]);
                }
            }
            return ids;
        }

        bool enabled(const Action action) const
        {
            if (action != Action::Undo && action != Action::Redo)
            {
                return true;
            }
            const ControlServer::UndoDepth depth = control().undoDepth(activeWorld());
            return (action == Action::Undo ? depth.undoable : depth.redoable) != 0;
        }

        void run(const Action action)
        {
            switch (action)
            {
            case Action::Undo:
            case Action::Redo:
                if (!enabled(action))
                {
                    break; // an empty stack is not a request: the refusal would be a warning in the Log
                }
                send({{"command", action == Action::Undo ? "undo" : "redo"}, {"args", {{"world", activeWorld()}}}});
                break;
            case Action::Copy:
                send({{"command", "copy"}});
                break;
            case Action::Paste:
                if (EditorModelAccess::hasClipboard(*editorModel)) // none is no request: no refusal in the Log
                {
                    send({{"command", "paste"}});
                }
                break;
            case Action::Duplicate:
                if (const Json ids = eachSelected("duplicateEntity"); !ids.empty())
                {
                    select(std::string(editorModel->selectionWorld()), ids);
                }
                break;
            case Action::Delete:
                eachSelected("deleteEntity");
                break;
            case Action::GizmoTranslate:
            case Action::GizmoRotate:
            case Action::GizmoScale:
                send({{"command", "gizmoMode"},
                      {"args",
                       {{"mode",
                         GIZMO_MODE_NAMES[static_cast<u32>(action) - static_cast<u32>(Action::GizmoTranslate)]}}}});
                break;
            case Action::FrameSelected:
                send({{"command", "editorCamera"},
                      {"args", {{"frame", EditorSceneView::frameTarget(*engine, *editorModel)}}}});
                break;
            case Action::DismissToast:
                if (feedback->anyVisible()) // a view command still bumps the generations the panels poll
                {
                    send({{"command", "dismissToast"}});
                }
                break;
            case Action::FileNew:
            case Action::FileOpen:
            case Action::FileSave:
            case Action::FileSaveAs:
            case Action::FileQuit:
                if (fileHandler)
                {
                    fileHandler(action);
                }
                break;
            case Action::CommandPalette:
                if (paletteHandler)
                {
                    paletteHandler();
                }
                break;
            case Action::ZoomIn:
            case Action::ZoomOut:
            case Action::ZoomReset:
                {
                    const float old = engine->uiDisplay().uiScale;
                    const float next = action == Action::ZoomReset
                        ? 1.0f
                        : std::clamp(std::round((old + (action == Action::ZoomIn ? 0.1f : -0.1f)) * 10.0f) / 10.0f,
                                     UiDisplaySettings::MIN_UI_SCALE, UiDisplaySettings::MAX_UI_SCALE);
                    send({{"command", "editorSettings"}, {"args", {{"uiScale", next}}}});
                    if (zoomToast)
                    {
                        send({{"command", "dismissToast"}, {"args", {{"id", zoomToast}}}});
                    }
                    const std::array<MessageArg, 1> args{
                        {{"percent", static_cast<std::int64_t>(std::lround(next * 100.0f))}}};
                    std::string scratch;
                    const std::string message(
                        engine->localization().resolve(InternedString("editor.zoom.feedback"), args, scratch));
                    const Json result = send(
                        {{"command", "postToast"}, {"args", {{"level", "info"}, {"text", message}, {"seconds", 2}}}});
                    zoomToast = result.value("id", u64{0});
                    break;
                }
            case Action::Count:
                break;
            }
        }
    };

    EditorActions::EditorActions(Engine& engine, const EditorModel& editorModel, const EditorFeedback& feedback,
                                 const std::filesystem::path& hotkeysFile) :
        m_state(std::make_unique<State>(State{.engine = &engine, .editorModel = &editorModel, .feedback = &feedback}))
    {
        State& state = *m_state;
        for (u32 i = 0; i < ACTION_COUNT; ++i)
        {
            state.hotkeys[i] = InternedString(ACTION_NAMES[i]);
        }
        if (engine.services().contains<InputSystem>())
        {
            state.input = &engine.services().get<InputSystem>();
            state.context =
                state.input->createContext({.name = InternedString("editor"), .priority = Engine::TOOL_INPUT_PRIORITY});
            // a rebinding is saved to the user layer (saveBindings) and read before the project's file
            const std::filesystem::path userFile = pathFromUtf8(userLayerMappingsFile(utf8Path(hotkeysFile)));
            const bool user = std::filesystem::exists(resolveUserConfigPath(userFile));
            if (!(user && state.input->loadMappings(state.context, utf8Path(userFile))) &&
                !state.input->loadMappings(state.context, utf8Path(hotkeysFile)))
            {
                SWARNING_LOG("editor: no hotkeys: {} does not load", hotkeysFile.string());
            }
        }
    }

    EditorActions::~EditorActions()
    {
        if (m_state->input)
        {
            m_state->input->destroyContext(m_state->context);
        }
    }

    void EditorActions::request(const Action action) { m_state->queued.push_back(action); }

    void EditorActions::setFileHandler(std::function<void(Action)> handler)
    {
        m_state->fileHandler = std::move(handler);
    }

    void EditorActions::setPaletteHandler(std::function<void()> handler)
    {
        m_state->paletteHandler = std::move(handler);
    }

    std::vector<std::string> EditorActions::shortcuts(const Action action) const
    {
        std::vector<std::string> chords;
        if (!m_state->input || action >= Action::Count)
        {
            return chords;
        }
        const std::string_view name = ACTION_NAMES[static_cast<u32>(action)];
        for (const InputActionMapping& mapping : m_state->input->mappings().actions)
        {
            if (mapping.action != name)
            {
                continue;
            }
            for (const InputBindingMapping& binding : mapping.bindings)
            {
                if (std::ranges::equal(binding.device, std::string_view("KEYBOARD"), [](const char a, const char b)
                                       { return std::toupper(static_cast<unsigned char>(a)) == b; }))
                {
                    chords.push_back(chordText(binding));
                }
            }
        }
        // the plainest chord first: the mappings do not keep the file's order
        std::ranges::sort(chords, [](const std::string& a, const std::string& b)
                          { return a.size() != b.size() ? a.size() < b.size() : a < b; });
        return chords;
    }

    bool EditorActions::enabled(const Action action) const { return m_state->enabled(action); }

    u64 EditorActions::bindingRevision() const { return m_state->input ? m_state->input->bindingRevision() : 0; }

    void EditorActions::update(const bool flying, const bool gameFocused)
    {
        State& state = *m_state;
        // hotkeys are the game's keys while it plays (Delete, Ctrl+Z); the Edit menu still works
        const bool editing = state.engine->playWorld() == nullptr;
        if (state.input)
        {
            state.input->setContextEnabled(state.context, !gameFocused);
        }
        if (state.input && !gameFocused)
        {
            for (u32 i = 0; i < ACTION_COUNT; ++i)
            {
                const bool gizmoKey =
                    i >= static_cast<u32>(Action::GizmoTranslate) && i <= static_cast<u32>(Action::GizmoScale);
                const bool zoom = i >= static_cast<u32>(Action::ZoomIn);
                if ((!editing && !zoom) ||
                    (zoom && EditorModelAccess::dialogs(const_cast<EditorModel&>(*state.editorModel)).open()))
                {
                    continue;
                }
                // W and E also fly the scene view's camera during a right drag
                if (!(flying && gizmoKey) && state.input->wasActionPressedThisFrame(state.hotkeys[i]))
                {
                    state.queued.push_back(static_cast<Action>(i));
                }
            }
        }
        std::vector<Action> actions;
        actions.swap(state.queued);
        for (const Action action : actions)
        {
            state.run(action);
        }
    }
} // namespace spite
