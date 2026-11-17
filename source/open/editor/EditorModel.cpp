#include "editor/EditorModel.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

#include "EditorAnimation.hpp"
#include "EditorCaptureFiles.hpp"
#include "EditorBuildState.hpp"
#include "EditorContext.hpp"
#include "EditorDialogRequests.hpp"
#include "EditorFeedback.hpp"
#include "EditorSceneFlows.hpp"
#include "EditorGizmoSettings.hpp"
#include "EditorHierarchyFilter.hpp"
#include "EditorSettings.hpp"
#include "ProjectSettingsModel.hpp"
#include "base/Assert.hpp"
#include "base/ContentRoot.hpp"
#include "base/WritableLayout.hpp"
#include "base/Logging.hpp"
#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "engine/PlayWorld.hpp"
#include "scene/SceneCollect.hpp"
#include "scene/SceneStream.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"
#include "uiview/UiStyleSheet.hpp"
#include "uiview/UiViewContext.hpp"
#include "world/EntityTransformManager.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;

        constexpr std::string_view OWNER = "editorModel";
        constexpr std::string_view DEFAULT_WORLD = "scene";
        constexpr std::string_view PLAY_WORLD = "play";
        constexpr std::string_view RECENT_FILE = "editor-recent-scenes.json";
        constexpr sizet MAX_RECENT = 10;
        constexpr sizet MAX_RECENT_BYTES = 64 * 1024;
        constexpr sizet MAX_RECENT_PATH = 512;
        constexpr sizet MAX_TOAST_BYTES = 400;

        // paths are compared the way the engine compares the open scene's: by name, ignoring ASCII case
        bool samePath(const std::string_view a, const std::string_view b)
        {
            return std::ranges::equal(
                a, b, [](const char x, const char y)
                { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
        }
    } // namespace

    struct EditorModel::State
    {
        // The editor's UI world: added before the settings apply their theme to its stylesheet, removed after
        // everything else of the model let go of it.
        struct UiSeam
        {
            explicit UiSeam(Engine& engine) : engine(&engine), ui(engine.addUiWorld(std::string(EDITOR_UI_WORLD)))
            {
                SASSERTM(ui, "A UI world named editor already exists: one editor model per engine.");
                // the themes and densities the settings name; the shell registers the fonts the styles resolve when
                // they paint
                if (const auto styles = ui->uiView().stylesheet().load(resolveContentPath("assets/ui/engine.json"));
                    !styles)
                {
                    SWARNING_LOG("editor model: {}", styles.error().message);
                }
            }
            ~UiSeam() { engine->removeUiWorld(*ui); }
            UiSeam(const UiSeam&) = delete;
            UiSeam& operator=(const UiSeam&) = delete;

            Engine* engine;
            UiWorld* ui;
        };

        State(Engine& engine, EditorModelDesc desc) :
            engine(&engine), control(&engine.control()), seam(engine),
            settings(engine,
                     desc.settingsFile.empty() ? std::filesystem::path{} : resolveUserConfigPath(desc.settingsFile),
                     resolveContentPath(desc.forgeTokensFile)),
            dialogs(engine),
            feedback(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr),
            recentFile(desc.settingsFile.empty() ? std::filesystem::path{}
                                                 : resolveUserConfigPath(desc.settingsFile).parent_path() / RECENT_FILE)
        {
            loadRecent();
        }

        Engine* engine;
        ControlServer* control;
        UiSeam seam;
        EditorSettingsService settings;
        EditorCaptureFiles captureFiles{*engine};
        std::string world{DEFAULT_WORLD};
        std::vector<Entity> selection;
        u64 revision = 0; // bumped by every change of the world or the entities
        std::function<void()> replacing;
        std::function<void()> selected;
        GizmoSettings gizmo;
        GizmoView gizmoView;
        HierarchyFilter hierarchyFilter{*engine, world, selection};
        EditorAnimation animation{*engine, world, revision, selection};
        EditorDialogRequests dialogs;
        EditorFeedback feedback; // before the flows that post to it
        EditorBuildState build{*engine, feedback};
        std::unique_ptr<EditorSceneFlows> flows;
        std::filesystem::path recentFile; // empty: not persisted
        std::vector<std::string> recent; // newest first
        std::string openPath; // the open scene's path as the host last told
        std::string clipboard; // a scene document (copy)

        void prune()
        {
            const World* target = control->world(world);
            const sizet erased = !target
                ? std::exchange(selection, {}).size()
                : std::erase_if(selection, [target](const Entity entity) { return !target->isEntityAlive(entity); });
            revision += erased != 0;
        }

        // the live selected entities no other selected entity contains
        std::vector<Entity> topmost() const
        {
            const World* target = control->world(world);
            if (!target)
            {
                return {};
            }
            std::vector<Entity> alive;
            for (const Entity entity : selection)
            {
                if (target->isEntityAlive(entity))
                {
                    alive.push_back(entity);
                }
            }
            if (!target->services().contains<EntityTransformManager>())
            {
                return alive;
            }
            EntityTransformManager& transforms = target->services().write<EntityTransformManager>().get();
            transforms.compile();
            std::vector<Entity> roots;
            for (const Entity entity : alive)
            {
                bool nested = false;
                for (Entity parent = transforms.parentOf(entity); parent.isValid() && !nested;
                     parent = transforms.parentOf(parent))
                {
                    nested = std::ranges::find(alive, parent) != alive.end();
                }
                if (!nested)
                {
                    roots.push_back(entity);
                }
            }
            return roots;
        }

        // a request run from inside a command: its reply's result, or the refusal's text in `error`
        Json run(const Json& request, std::string& error) const
        {
            Json reply = Json::parse(control->handle(request.dump()), nullptr, false);
            if (!reply.is_object() || !reply.value("ok", false))
            {
                error = reply.is_object() ? reply.value("error", std::string("failed")) : std::string("failed");
                return Json();
            }
            return std::move(reply["result"]);
        }

        // copy: the selection's topmost entities as a scene document, which paste instantiates (so it outlives them)
        std::string copySelection(IFormatWriter& out)
        {
            const std::vector<Entity> roots = topmost();
            if (roots.empty())
            {
                return "nothing selected to copy";
            }
            const World* target = control->world(world);
            if (!target->services().contains<EntityTransformManager>())
            {
                return "world has no transform hierarchy";
            }
            World copy = *target;
            const Expected<SceneDocument> document =
                collectScene(copy, copy.services().write<EntityTransformManager>().get(), roots);
            if (!document)
            {
                return std::string(document.error().message.c_str());
            }
            clipboard.clear();
            emitSceneJson(*document, clipboard);
            out.writeString("world", world);
            out.writeU32("entities", static_cast<u32>(document->entities.size()));
            return std::string{};
        }

        // paste [world]: instantiates the clipboard and selects what it made
        std::string pasteClipboard(const ControlArgs& args, IFormatWriter& out)
        {
            if (clipboard.empty())
            {
                return "nothing copied to paste";
            }
            const std::string name =
                args.string("world").value_or(std::string(engine->playWorld() ? PLAY_WORLD : DEFAULT_WORLD));
            std::string error;
            const Json made = run(
                {{"command", "instantiate"}, {"args", {{"world", name}, {"document", Json::parse(clipboard)}}}}, error);
            if (!error.empty())
            {
                return error;
            }
            const Json roots = made.value("roots", Json::array());
            run({{"command", "select"}, {"args", {{"world", name}, {"ids", roots}}}}, error);
            out.writeString("world", name);
            out.beginArray("roots", static_cast<u32>(roots.size()));
            for (const Json& id : roots)
            {
                out.writeU64("", id.get<u64>());
            }
            out.endArray();
            return error;
        }

        std::string postToast(const ControlArgs& args, IFormatWriter& out)
        {
            const auto level = std::ranges::find(TOAST_LEVEL_NAMES, args.string("level").value_or(""));
            if (level == TOAST_LEVEL_NAMES.end())
            {
                return "postToast level is info, warn or error";
            }
            std::string text = args.string("text").value_or("");
            if (text.empty() || text.size() > MAX_TOAST_BYTES)
            {
                return std::format("postToast text is 1 to {} bytes", MAX_TOAST_BYTES);
            }
            double seconds = 0.0;
            if (const std::optional<std::string> given = args.json("seconds"))
            {
                char* end = nullptr;
                seconds = std::strtod(given->c_str(), &end);
                if (end == given->c_str() || !(seconds >= 0.5 && seconds <= 60.0))
                {
                    return "postToast seconds is a number from 0.5 to 60";
                }
            }
            out.writeU64("id",
                         feedback.postToast(static_cast<ToastLevel>(level - TOAST_LEVEL_NAMES.begin()), std::move(text),
                                            seconds));
            return std::string{};
        }

        void loadRecent()
        {
            if (recentFile.empty() || !isRegularFile(recentFile).value_or(false))
            {
                return;
            }
            const Expected<uintmax_t> size = fileSize(recentFile);
            if (!size || *size > MAX_RECENT_BYTES)
            {
                return;
            }
            const Expected<std::string> text = readTextFile(recentFile);
            if (!text)
            {
                return;
            }
            const Json document = Json::parse(*text, nullptr, false);
            if (!document.is_object() || !document.contains("scenes") || !document["scenes"].is_array())
            {
                return;
            }
            for (const Json& entry : document["scenes"])
            {
                const std::string path = entry.is_string() ? entry.get<std::string>() : std::string{};
                if (!path.empty() && path.size() <= MAX_RECENT_PATH && recent.size() < MAX_RECENT)
                {
                    recent.push_back(path);
                }
            }
        }

        void saveRecent() const
        {
            if (recentFile.empty())
            {
                return;
            }
            const std::string text =
                Json{{"scenes", recent}}.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
            Expected<void> written = ensureDirectory(recentFile.parent_path());
            if (written)
            {
                written = writeTextFileAtomically(recentFile, text);
            }
            if (!written)
            {
                SWARNING_LOG("editor model: recent scenes not saved: {}", written.error().message.c_str());
            }
        }

        void writeSelection(IFormatWriter& out) const
        {
            out.writeString("world", world);
            out.beginArray("selection", static_cast<u32>(selection.size()));
            for (const Entity entity : selection)
            {
                out.writeU64("", entity.id());
            }
            out.endArray();
        }

        // Play moves a scene selection onto the clones, Stop a play selection onto the entities they were cloned from
        void carry(const bool playing)
        {
            PlayWorld* play = engine->playWorld();
            if (!play || world != (playing ? DEFAULT_WORLD : PLAY_WORLD))
            {
                return;
            }
            const World& to = playing ? play->world() : engine->world();
            std::vector<Entity> carried;
            for (const Entity entity : selection)
            {
                const Entity other = playing ? play->cloneOf(entity) : Entity(play->sourceOf(entity.id()));
                if (other.isValid() && to.isEntityAlive(other))
                {
                    carried.push_back(other);
                }
            }
            world = playing ? PLAY_WORLD : DEFAULT_WORLD;
            selection = std::move(carried);
            ++revision;
            if (selected)
            {
                selected();
            }
        }

        // gizmoMode [mode] [space] [snap] [enabled] [translateStep] [rotateStep] [scaleStep]; a refusal changes nothing
        std::string setGizmo(const ControlArgs& args, IFormatWriter& out)
        {
            const auto pick = [&args](const std::string_view key, const auto& names, auto& field)
            {
                if (const std::optional<std::string> name = args.string(key))
                {
                    field = static_cast<std::remove_reference_t<decltype(field)>>(std::ranges::find(names, *name) -
                                                                                  names.begin());
                }
            };
            GizmoSettings next = gizmo;
            pick("mode", GIZMO_MODE_NAMES, next.mode);
            pick("space", GIZMO_SPACE_NAMES, next.space);
            f32* const steps[3]{&next.translateStep, &next.rotateStep, &next.scaleStep};
            const char* const stepKeys[3]{"translateStep", "rotateStep", "scaleStep"};
            for (u32 i = 0; i < 3; ++i)
            {
                if (const std::optional<std::string> text = args.json(stepKeys[i]))
                {
                    const Json step = Json::parse(*text, nullptr, false);
                    if (!step.is_number() || !(step.get<f32>() > 0.0f))
                    {
                        return std::format("gizmoMode: {} is a positive number", stepKeys[i]);
                    }
                    *steps[i] = step.get<f32>();
                }
            }
            next.snap = args.boolean("snap").value_or(next.snap);
            next.enabled = args.boolean("enabled").value_or(next.enabled);
            if (gizmoView.dragging && gizmoView.dragging() &&
                (next.mode != gizmo.mode || next.enabled != gizmo.enabled))
            {
                return std::string("gizmoMode: the mode and the gizmo's visibility cannot change during a drag");
            }
            out.writeString("previous", GIZMO_MODE_NAMES[static_cast<u8>(gizmo.mode)]);
            gizmo = next;
            writeGizmo(out);
            return std::string{};
        }

        void writeGizmo(IFormatWriter& out) const
        {
            out.writeString("mode", GIZMO_MODE_NAMES[static_cast<u8>(gizmo.mode)]);
            out.writeString("space", GIZMO_SPACE_NAMES[static_cast<u8>(gizmo.space)]);
            out.writeBool("snap", gizmo.snap);
            out.writeBool("enabled", gizmo.enabled);
            out.writeU32("componentCategories", gizmo.componentCategories);
            out.writeBool("selectedComponents", gizmo.selectedComponentGizmos);
            out.beginObject("componentGizmos");
            constexpr const char* names[] = {"lights", "colliders", "cameras", "custom"};
            for (u32 i = 0; i < 4; ++i)
                out.writeBool(names[i], (gizmo.componentCategories & (1u << i)) != 0);
            out.writeBool("selected", gizmo.selectedComponentGizmos);
            out.endObject();
            out.writeF32("translateStep", gizmo.translateStep);
            out.writeF32("rotateStep", gizmo.rotateStep);
            out.writeF32("scaleStep", gizmo.scaleStep);
        }

        void registerToasts()
        {
            control->registerViewCommand(
                "postToast", OWNER, "shows a toast above the status bar; the toasts query keeps the last 50",
                {requiredArg("level", ArgKind::String, "info, warn or error", TOAST_LEVEL_NAMES),
                 requiredArg("text", ArgKind::String, "the message"),
                 optionalArg("seconds", ArgKind::Number, "how long it stays, 0.5 to 60", "4, an error 8")},
                [this](const ControlArgs& args, IFormatWriter& out) { return postToast(args, out); });
            control->registerViewCommand(
                "dismissToast", OWNER, "dismisses a toast, or every toast on screen without an id",
                {optionalArg("id", ArgKind::Integer, "the toast's id, as postToast and toasts give it")},
                [this](const ControlArgs& args, IFormatWriter& out)
                {
                    const std::optional<u64> id = args.integer("id");
                    out.writeU32("dismissed", id ? (feedback.dismissToast(*id) ? 1 : 0) : feedback.dismissVisible());
                    return std::string{};
                });
            control->registerViewCommand("clearToasts", OWNER,
                                         "forgets every toast, dismissed or not (ids are not reused)", {},
                                         [this](const ControlArgs&, IFormatWriter& out)
                                         {
                                             out.writeU32("cleared", feedback.clearToasts());
                                             return std::string{};
                                         });
            control->registerQuery("toasts", OWNER,
                                   "the toast log, oldest first, the last 50: visible (how many are on screen) and "
                                   "log {id, level, text, ageSeconds, visible, dismissed}",
                                   {},
                                   [this](const ControlArgs&, IFormatWriter& out)
                                   {
                                       feedback.writeLog(out);
                                       return std::string{};
                                   });
        }

        void registerClipboard()
        {
            control->registerCommand("copy", OWNER, "copies the selection's topmost entities for paste", {},
                                     [this](const ControlArgs&, IFormatWriter& out) { return copySelection(out); });
            control->registerCommand(
                "paste", OWNER, "instantiates what copy put on the clipboard and selects the new roots",
                {optionalArg("world", ArgKind::String, "the world to paste into", "play while playing, else scene")},
                [this](const ControlArgs& args, IFormatWriter& out) { return pasteClipboard(args, out); });
            control->registerQuery("selection", OWNER, "the selection: its world and the selected entity ids", {},
                                   [this](const ControlArgs&, IFormatWriter& out)
                                   {
                                       prune();
                                       writeSelection(out);
                                       return std::string{};
                                   });
        }

        void registerCommands()
        {
            dialogs.registerCommands(*control, OWNER);
            control->registerViewCommand("quit", OWNER, "requests quit, asking about unsaved scenes", {},
                                         [this](const ControlArgs&, IFormatWriter&)
                                         {
                                             flows->request(EditorSceneFlows::Op::Quit);
                                             return std::string{};
                                         });
            projectsettings::registerDiffQuery(*control, engine->localization(), OWNER);
            registerToasts();
            registerClipboard();
            control->registerQuery("recentScenes", OWNER,
                                   "the scenes the File menu's Open Recent lists, newest first (paths under the scene "
                                   "root), and the file they persist in (empty: not persisted)",
                                   {},
                                   [this](const ControlArgs&, IFormatWriter& out)
                                   {
                                       out.beginArray("scenes", static_cast<u32>(recent.size()));
                                       for (const std::string& path : recent)
                                       {
                                           out.writeString("", path);
                                       }
                                       out.endArray();
                                       out.writeString("file",
                                                       recentFile.empty() ? std::string{} : utf8Path(recentFile));
                                       return std::string{};
                                   });
            control->registerViewCommand(
                "componentGizmoVisibility", OWNER,
                "sets component gizmo overlay visibility; replies componentCategories (a bit mask: light, collider, "
                "camera, custom), selectedComponents (whether selected objects show their component gizmos), and "
                "componentGizmos (the same category and selected flags as named booleans)",
                {optionalArg("lights", ArgKind::Boolean, "show light gizmos"),
                 optionalArg("colliders", ArgKind::Boolean, "show collider gizmos"),
                 optionalArg("cameras", ArgKind::Boolean, "show camera gizmos"),
                 optionalArg("custom", ArgKind::Boolean, "show custom gizmos"),
                 optionalArg("selected", ArgKind::Boolean, "show selected component gizmos")},
                [this](const ControlArgs& args, IFormatWriter& out)
                {
                    constexpr const char* names[] = {"lights", "colliders", "cameras", "custom"};
                    for (u32 i = 0; i < 4; ++i)
                        if (const auto value = args.boolean(names[i]))
                            gizmo.componentCategories =
                                *value ? gizmo.componentCategories | (1u << i) : gizmo.componentCategories & ~(1u << i);
                    gizmo.selectedComponentGizmos = args.boolean("selected").value_or(gizmo.selectedComponentGizmos);
                    writeGizmo(out);
                    return std::string{};
                });
            control->registerViewCommand(
                "gizmoMode", OWNER,
                "sets the transform gizmo's mode, space, snapping and visibility; replies componentCategories as "
                "the category bit mask, selectedComponents as a boolean, and componentGizmos as named visibility "
                "booleans alongside the transform settings",
                {optionalArg("mode", ArgKind::String, "translate, rotate or scale", {}, GIZMO_MODE_NAMES),
                 optionalArg("space", ArgKind::String, "world or local axes (scale always uses local ones)", {},
                             GIZMO_SPACE_NAMES),
                 optionalArg("snap", ArgKind::Boolean, "snap drags to the steps"),
                 optionalArg("enabled", ArgKind::Boolean, "false hides the gizmo and its handles (default shown)"),
                 optionalArg("translateStep", ArgKind::Number, "world units"),
                 optionalArg("rotateStep", ArgKind::Number, "degrees"),
                 optionalArg("scaleStep", ArgKind::Number, "of the scale factor")},
                [this](const ControlArgs& args, IFormatWriter& out) { return setGizmo(args, out); });
            control->registerQuery("gizmo", OWNER,
                                   "the transform gizmo's settings, pivot, axes, handle pixels and drag; "
                                   "componentCategories is the light/collider/camera/custom bit mask, "
                                   "selectedComponents is whether selected objects show component gizmos, and "
                                   "componentGizmos holds these category and selected flags as named booleans",
                                   {},
                                   [this](const ControlArgs&, IFormatWriter& out)
                                   {
                                       writeGizmo(out);
                                       if (gizmoView.write)
                                       {
                                           gizmoView.write(out);
                                           return std::string{};
                                       }
                                       out.writeBool("visible", false);
                                       out.beginObject("drag");
                                       out.writeBool("active", false);
                                       out.endObject();
                                       return std::string{};
                                   });
            // select [world=scene] ids=[...] | id: the hierarchy, scene picking and the actions send it
            control->registerCommand(
                "select", OWNER, "sets the selection the inspector shows and edits; neither id nor ids clears it",
                {optionalArg("world", ArgKind::String, "the selection's world", DEFAULT_WORLD),
                 optionalArg("ids", ArgKind::Json, "an array of entity ids"),
                 optionalArg("id", ArgKind::Entity, "one entity, without ids")},
                [this](const ControlArgs& args, IFormatWriter& out)
                {
                    const std::string name = args.string("world").value_or(std::string(DEFAULT_WORLD));
                    const World* target = control->world(name);
                    if (!target)
                    {
                        return control->unknownWorld(name);
                    }
                    std::vector<Entity> picked;
                    if (const std::optional<std::string> ids = args.json("ids"))
                    {
                        const Json list = Json::parse(*ids, nullptr, false);
                        if (!list.is_array() || !std::ranges::all_of(list, &Json::is_number_unsigned))
                        {
                            return std::string("ids must be an array of entity ids");
                        }
                        for (const Json& id : list)
                        {
                            picked.push_back(Entity(id.get<u64>()));
                        }
                    }
                    else if (const std::optional<u64> id = args.integer("id"))
                    {
                        picked.push_back(Entity(*id));
                    }
                    std::vector<Entity> unique;
                    for (const Entity entity : picked)
                    {
                        if (!target->isEntityAlive(entity))
                        {
                            return "no live entity " + std::to_string(entity.id());
                        }
                        if (std::ranges::find(unique, entity) == unique.end())
                        {
                            unique.push_back(entity);
                        }
                    }
                    if (name != world || unique != selection)
                    {
                        if (replacing)
                        {
                            replacing();
                        }
                        ++revision;
                    }
                    world = name;
                    selection = std::move(unique);
                    if (selected)
                    {
                        selected();
                    }
                    writeSelection(out);
                    return std::string{};
                });
        }
    };

    EditorModel::EditorModel(Engine& engine, EditorModelDesc desc) :
        m_state(std::make_unique<State>(engine, std::move(desc)))
    {
        // the listener first: it asserts when another one is set, before select is registered with this state
        engine.setPlayListener([&state = *m_state](const bool playing) { state.carry(playing); });
        m_state->flows = std::make_unique<EditorSceneFlows>(engine, *this, m_state->dialogs, m_state->feedback);
        m_state->registerCommands();
        m_state->build.registerCommands();
        m_state->hierarchyFilter.registerCommands(OWNER);
        m_state->animation.registerCommands(OWNER);
    }

    EditorModel::~EditorModel()
    {
        m_state->engine->setPlayListener({});
        m_state->control->unregisterOwner(OWNER);
    }

    const std::string& EditorModel::selectionWorld() const { return m_state->world; }

    const std::vector<Entity>& EditorModel::selection() const { return m_state->selection; }

    u64 EditorModel::selectionRevision() const { return m_state->revision; }

    void EditorModel::pruneSelection() { m_state->prune(); }

    void EditorModel::setSelectListener(std::function<void()> replacing, std::function<void()> selected)
    {
        SASSERTM(!(replacing || selected) || !(m_state->replacing || m_state->selected),
                 "A select listener is already set: clear it first.");
        m_state->replacing = std::move(replacing);
        m_state->selected = std::move(selected);
    }

    const std::vector<std::string>& EditorModel::recentScenes() const { return m_state->recent; }

    void EditorModel::followScene(const std::string_view path)
    {
        State& state = *m_state;
        if (path == state.openPath)
        {
            return;
        }
        state.openPath = std::string(path);
        if (path.empty())
        {
            return;
        }
        std::erase_if(state.recent, [path](const std::string& other) { return samePath(other, path); });
        state.recent.insert(state.recent.begin(), std::string(path));
        state.recent.resize(std::min(state.recent.size(), MAX_RECENT));
        state.saveRecent();
    }

    void EditorModel::forgetRecentScene(const std::string_view path)
    {
        State& state = *m_state;
        if (std::erase_if(state.recent, [path](const std::string& other) { return samePath(other, path); }) > 0)
        {
            state.saveRecent();
        }
    }

    EditorDialogRequests& EditorModelAccess::dialogs(EditorModel& model) { return model.m_state->dialogs; }

    std::vector<Entity> EditorModelAccess::topmostSelection(const EditorModel& model)
    {
        return model.m_state->topmost();
    }

    bool EditorModelAccess::hasClipboard(const EditorModel& model) { return !model.m_state->clipboard.empty(); }

    EditorFeedback& EditorModelAccess::feedback(EditorModel& model) { return model.m_state->feedback; }

    EditorBuildState& EditorModelAccess::build(const EditorModel& model) { return model.m_state->build; }

    EditorAnimation& EditorModelAccess::animation(const EditorModel& model) { return model.m_state->animation; }

    EditorSceneFlows& EditorModelAccess::flows(const EditorModel& model) { return *model.m_state->flows; }

    EditorCaptureFiles& EditorModelAccess::captureFiles(const EditorModel& model)
    {
        return model.m_state->captureFiles;
    }

    EditorSettingsService& EditorModelAccess::settings(EditorModel& model) { return model.m_state->settings; }

    const GizmoSettings& EditorModelAccess::gizmo(const EditorModel& model) { return model.m_state->gizmo; }
    GizmoSettings& EditorModelAccess::mutableGizmo(EditorModel& model) { return model.m_state->gizmo; }

    const HierarchyFilter& EditorModelAccess::hierarchyFilter(const EditorModel& model)
    {
        return model.m_state->hierarchyFilter;
    }

    void EditorModelAccess::setHierarchyView(const EditorModel& model, HierarchyFilterView view)
    {
        model.m_state->hierarchyFilter.setView(std::move(view));
    }

    void EditorModelAccess::setGizmoView(EditorModel& model, GizmoView view)
    {
        GizmoView& held = model.m_state->gizmoView;
        SASSERTM(!(view.dragging || view.write) || !(held.dragging || held.write),
                 "A gizmo view is already set: clear it first.");
        held = std::move(view);
    }
} // namespace spite
