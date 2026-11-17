#include "EditorSceneFlows.hpp"
#include "EditorDialogRequests.hpp"
#include "EditorFeedback.hpp"
#include "editor/EditorModel.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "engine/PlayWorld.hpp"
#include "localization/Localization.hpp"
#include "renderruntime/CameraQueries.hpp"
#include "renderruntime/RenderRuntime.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using Op = EditorSceneFlows::Op;

        constexpr std::string_view TABLE = "editor-file.";
        constexpr std::string_view SCENE_ROOT = "scene";
        constexpr std::string_view IMPORT_ROOT = "import";
        constexpr std::string_view SCENE_EXTENSIONS = "json,spsd";
        constexpr std::string_view SAVE_EXTENSION = "json";
        constexpr std::string_view MODEL_EXTENSIONS = "glb,gltf";

        struct Pending
        {
            Op op;
            std::string path; // OpenRecent
            std::function<void(bool)>
                done; // Save: told whether the scene was written; also when the request is dropped
            Entity entity = Entity::undefined();
            std::optional<u32> layer;
        };

        std::string_view directoryOf(const std::string_view path)
        {
            const sizet slash = path.rfind('/');
            return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
        }

        std::string_view fileOf(const std::string_view path)
        {
            const sizet slash = path.rfind('/');
            return slash == std::string_view::npos ? path : path.substr(slash + 1);
        }
    } // namespace

    struct EditorSceneFlows::State
    {
        using Next = std::function<void(bool go)>;

        State(Engine& engine, EditorModel& model, EditorDialogRequests& dialogs, EditorFeedback& feedback) :
            engine(&engine), control(&engine.control()), model(&model), dialogs(&dialogs), feedback(&feedback),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        EditorModel* model; // the recent scenes
        EditorDialogRequests* dialogs;
        EditorFeedback* feedback;
        const Localization* localization;

        std::vector<Pending> queued;
        bool busy = false; // a flow is between its first dialog and its command's reply
        bool discard = false; // the person chose not to save: the flow's replace command forces

        [[nodiscard]] std::string tr(const std::string_view key, const std::vector<MessageArg>& args = {}) const
        {
            const std::string full = std::string(TABLE) + std::string(key);
            if (!localization)
            {
                return full;
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(full), args, scratch));
        }

        void toast(const ToastLevel level, const std::string_view key, const std::vector<MessageArg>& args = {}) const
        {
            feedback->postToastKey(level, std::string(TABLE) + std::string(key), args);
        }

        // The reply of a request; ok false with the message in `error` on a refusal.
        struct Reply
        {
            bool ok = false;
            std::string error;
            Json result = Json::object();
            Json details = Json::object();
        };

        [[nodiscard]] Reply call(const char* kind, const std::string_view name, const Json& args = Json::object()) const
        {
            Json request{{kind, name}};
            if (!args.empty())
            {
                request["args"] = args;
            }
            const Json reply = Json::parse(
                control->handle(request.dump(-1, ' ', false, Json::error_handler_t::replace)), nullptr, false);
            Reply out;
            if (!reply.is_object())
            {
                out.error = "no reply";
                return out;
            }
            out.ok = reply.value("ok", false);
            if (!out.ok)
            {
                out.error = reply.contains("error") && reply["error"].is_string() ? reply["error"].get<std::string>()
                                                                                  : std::string("refused");
                out.details = reply.value("details", Json::object());
            }
            else if (reply.contains("result") && reply["result"].is_object())
            {
                out.result = reply["result"];
            }
            return out;
        }

        [[nodiscard]] Reply sceneInfo() const { return call("query", "sceneInfo", {{"counts", false}}); }

        void finish()
        {
            busy = false;
            discard = false;
        }

        // The scene was replaced: nothing selected, and the game view has a camera (a scene without its own).
        void afterSwap() const
        {
            (void)call("command", "select");
            if (!engine->playWorld())
            {
                ensureCamera(engine->world(), engine->render().view(engine->gameView()));
            }
        }

        // A New or Open: the command (force when the person chose not to save), then what a swap leaves to fix.
        bool replace(const std::string_view command, Json args, const std::string_view errorKey)
        {
            if (discard)
            {
                args["force"] = true;
            }
            const Reply reply = call("command", command, args);
            if (!reply.ok)
            {
                toast(ToastLevel::Error, errorKey, {{"error", std::string_view(reply.error)}});
                return false;
            }
            afterSwap();
            return true;
        }

        // next(true): the flow goes on, with the scene saved or its changes given up; next(false): it stops.
        std::string sceneLabel(const Json& row) const
        {
            const auto path = row.value("path", std::string{});
            const auto id = row.value("id", 0u);
            return path.empty() ? tr("untitled", {{"number", static_cast<std::int64_t>(id) + 1}}) : path;
        }

        void unsaved(const std::optional<u32> layer, Next next)
        {
            const Reply state = call("query", "sceneLayers", {{"counts", false}});
            if (!state.ok)
            {
                next(false);
                return;
            }
            auto layers = std::make_shared<std::vector<u32>>();
            std::string names;
            const auto append = [&](const std::string_view name)
            {
                if (!names.empty())
                    names += ", ";
                names += name;
            };
            for (const Json& row : state.result.at("layers"))
            {
                if (const auto id = row.at("id").get<u32>(); (!layer || id == *layer) && row.value("dirty", false))
                {
                    layers->push_back(id);
                    append(sceneLabel(row));
                }
            }
            const auto& unloaded = state.result.at("unsavedUnloads");
            const bool hidden = !layer && !unloaded.empty();
            if (hidden)
                for (const Json& path : unloaded)
                    append(path.get<std::string>());
            if (names.empty())
            {
                next(true);
                return;
            }
            EditorConfirm ask;
            ask.title = tr("unsaved.title");
            ask.body = tr(hidden ? "unsaved.unloaded" : "unsaved.body", {{"name", std::string_view(names)}});
            ask.confirmLabel = tr("unsaved.save");
            ask.alternateLabel = tr("unsaved.discard");
            ask.destructive = true;
            const u64 shownAt = control->stateGeneration();
            dialogs->choose(std::move(ask),
                            [this, layer, layers, hidden, shownAt, next = std::move(next)](const EditorAnswer answer)
                            {
                                if (answer == EditorAnswer::Confirm)
                                {
                                    if (hidden)
                                    {
                                        toast(ToastLevel::Warn, "error.unloaded");
                                        next(false);
                                        return;
                                    }
                                    saveList(layers, 0,
                                             [this, layer, next](const bool saved)
                                             {
                                                 if (saved)
                                                     unsaved(layer, next);
                                                 else
                                                     next(false);
                                             });
                                }
                                else if (answer == EditorAnswer::Alternate)
                                {
                                    if (control->stateGeneration() != shownAt)
                                    {
                                        unsaved(layer, next);
                                        return;
                                    }
                                    discard = true;
                                    next(true);
                                }
                                else
                                {
                                    next(false);
                                }
                            });
        }

        void write(const std::string_view command, Json args, Next next)
        {
            const Reply written = call("command", command, args);
            if (!written.ok && written.details.contains("externalReferenceField"))
            {
                const std::string field = written.details["externalReferenceField"].get<std::string>();
                EditorConfirm ask;
                ask.title = tr("references.title");
                ask.body = tr("references.body", {{"field", std::string_view(field)}});
                ask.confirmLabel = tr("references.save");
                ask.destructive = true;
                const u64 shownAt = control->stateGeneration();
                dialogs->choose(std::move(ask),
                                [this, command, shownAt, args = std::move(args),
                                 next = std::move(next)](EditorAnswer answer) mutable
                                {
                                    if (answer == EditorAnswer::Confirm)
                                    {
                                        if (control->stateGeneration() == shownAt)
                                            args["allowExternalReferences"] = true;
                                        write(command, std::move(args), std::move(next));
                                    }
                                    else
                                        next(false);
                                });
                return;
            }
            if (!written.ok)
            {
                toast(ToastLevel::Error, "error.save", {{"error", std::string_view(written.error)}});
                next(false);
                return;
            }
            const auto name = written.result.value("path", std::string{});
            toast(ToastLevel::Info, "saved", {{"name", std::string_view(name)}});
            next(true);
        }

        void save(const std::optional<u32> layer, const bool as, Next next)
        {
            const Reply state = call("query", "sceneLayers", {{"counts", false}});
            if (!state.ok)
            {
                toast(ToastLevel::Error, "error.save", {{"error", std::string_view(state.error)}});
                next(false);
                return;
            }
            const auto id = layer.value_or(state.result.at("active").get<u32>());
            const auto& rows = state.result.at("layers");
            const auto row = std::ranges::find_if(rows, [id](const Json& item) { return item["id"] == id; });
            if (row == rows.end())
            {
                toast(ToastLevel::Warn, "error.closed");
                next(false);
                return;
            }
            const std::string path = row->value("path", std::string{});
            if (!as && !path.empty())
            {
                write("saveScene", {{"layer", id}}, std::move(next));
                return;
            }
            EditorPick pick;
            pick.mode = EditorPickMode::SaveFile;
            pick.title = tr("pick.save");
            pick.roots = {std::string(SCENE_ROOT)};
            pick.root = SCENE_ROOT;
            pick.directory = std::string(directoryOf(path));
            pick.name = path.empty() ? tr("pick.untitled") : std::string(fileOf(path));
            pick.extensions = SAVE_EXTENSION;
            dialogs->pickFile(std::move(pick),
                              [this, id, next = std::move(next)](const std::optional<EditorPicked> picked)
                              {
                                  if (!picked)
                                  {
                                      next(false);
                                      return;
                                  }
                                  write("saveSceneAs", {{"path", picked->path}, {"overwrite", true}, {"layer", id}},
                                        next);
                              });
        }

        void saveList(const std::shared_ptr<std::vector<u32>>& layers, const sizet index, Next next)
        {
            if (index == layers->size())
            {
                next(true);
                return;
            }
            save((*layers)[index], false,
                 [this, layers, index, next = std::move(next)](bool saved)
                 {
                     if (saved)
                         saveList(layers, index + 1, next);
                     else
                         next(false);
                 });
        }

        void saveAll(Next next)
        {
            const Reply state = call("query", "sceneLayers", {{"counts", false}});
            if (!state.ok)
            {
                next(false);
                return;
            }
            auto layers = std::make_shared<std::vector<u32>>();
            for (const Json& row : state.result.at("layers"))
                if (row.value("dirty", false))
                    layers->push_back(row.at("id").get<u32>());
            saveList(layers, 0, std::move(next));
        }

        void saveEntity(const Entity entity)
        {
            EditorPick pick;
            pick.mode = EditorPickMode::SaveFile;
            pick.title = tr("pick.save");
            pick.roots = {std::string(SCENE_ROOT)};
            pick.root = SCENE_ROOT;
            pick.name = tr("pick.untitled");
            pick.extensions = SAVE_EXTENSION;
            dialogs->pickFile(
                std::move(pick),
                [this, entity](const std::optional<EditorPicked> picked)
                {
                    if (picked)
                    {
                        const Reply written = call("command", "saveScene",
                                                   {{"id", entity.id()}, {"path", picked->path}, {"overwrite", true}});
                        if (!written.ok)
                        {
                            toast(ToastLevel::Error, "error.save", {{"error", std::string_view(written.error)}});
                        }
                        else
                        {
                            const std::string name(fileOf(picked->path));
                            toast(ToastLevel::Info, "saved", {{"name", std::string_view(name)}});
                        }
                    }
                    finish();
                });
        }

        void open(const std::string& path)
        {
            const Reply opened = call("command", "loadSceneLayer", {{"path", path}, {"activate", true}});
            if (opened.ok)
                model->followScene(path);
            else
            {
                toast(ToastLevel::Error, "error.open", {{"error", std::string_view(opened.error)}});
                std::filesystem::path file;
                if (!control->resolveFile(SCENE_ROOT, path, file).empty() || !isRegularFile(file).value_or(false))
                    model->forgetRecentScene(path);
            }
        }

        void pickScene(const bool replacing)
        {
            const Reply info = sceneInfo();
            const std::string path = info.ok ? info.result.value("activePath", std::string{}) : std::string{};
            EditorPick pick;
            pick.mode = EditorPickMode::OpenFile;
            pick.title = tr(replacing ? "pick.replace" : "pick.open");
            pick.roots = {std::string(SCENE_ROOT)};
            pick.root = SCENE_ROOT;
            pick.directory = std::string(directoryOf(path));
            pick.extensions = SCENE_EXTENSIONS;
            dialogs->pickFile(std::move(pick),
                              [this, replacing](const std::optional<EditorPicked> picked)
                              {
                                  if (picked)
                                  {
                                      if (replacing)
                                      {
                                          unsaved(std::nullopt,
                                                  [this, path = picked->path](const bool go)
                                                  {
                                                      if (go)
                                                          replace("openScene", {{"path", path}}, "error.open");
                                                      finish();
                                                  });
                                          return;
                                      }
                                      open(picked->path);
                                  }
                                  finish();
                              });
        }

        void pickWorkspace(const bool saving)
        {
            EditorPick pick;
            pick.mode = saving ? EditorPickMode::SaveFile : EditorPickMode::OpenFile;
            pick.title = tr(saving ? "menu.saveWorkspace" : "menu.openWorkspace");
            pick.roots = {std::string(SCENE_ROOT)};
            pick.root = SCENE_ROOT;
            pick.extensions = "spworkspace";
            if (saving)
                pick.name = "workspace.spworkspace";
            dialogs->pickFile(std::move(pick),
                              [this, saving](const std::optional<EditorPicked> picked)
                              {
                                  if (picked)
                                  {
                                      Json args{{"path", picked->path}};
                                      if (saving)
                                          args["overwrite"] = true;
                                      const Reply result =
                                          call("command", saving ? "saveSceneWorkspace" : "openSceneWorkspace", args);
                                      if (!result.ok)
                                          toast(ToastLevel::Error,
                                                saving ? "error.saveWorkspace" : "error.openWorkspace",
                                                {{"error", std::string_view(result.error)}});
                                      else if (saving)
                                          toast(ToastLevel::Info, "saved", {{"name", std::string_view(picked->path)}});
                                  }
                                  finish();
                              });
        }

        void pickModel()
        {
            EditorPick pick;
            pick.mode = EditorPickMode::OpenFile;
            pick.title = tr("pick.import");
            pick.roots = {std::string(IMPORT_ROOT)};
            pick.root = IMPORT_ROOT;
            pick.extensions = MODEL_EXTENSIONS;
            dialogs->pickFile(std::move(pick),
                              [this](const std::optional<EditorPicked> picked)
                              {
                                  if (picked)
                                  {
                                      // the Project panel reports the bake as a job and tells when it settles
                                      const Reply started = call("command", "importModel", {{"path", picked->path}});
                                      if (!started.ok)
                                      {
                                          toast(ToastLevel::Error, "error.import",
                                                {{"error", std::string_view(started.error)}});
                                      }
                                  }
                                  finish();
                              });
        }

        void start(const Pending& pending)
        {
            const bool changesWorkspace = pending.op == Op::New || pending.op == Op::Open ||
                pending.op == Op::OpenRecent || pending.op == Op::Replace || pending.op == Op::Reset ||
                pending.op == Op::OpenWorkspace || pending.op == Op::Close;
            if (changesWorkspace && engine->playWorld())
            {
                toast(ToastLevel::Warn, "error.playing");
                return;
            }
            busy = true;
            discard = false;
            switch (pending.op)
            {
            case Op::New:
                {
                    const Reply created = call("command", "newSceneLayer");
                    if (!created.ok)
                        toast(ToastLevel::Error, "error.new", {{"error", std::string_view(created.error)}});
                    finish();
                    break;
                }
            case Op::Reset:
                unsaved(std::nullopt,
                        [this](const bool go)
                        {
                            if (go)
                            {
                                replace("newScene", Json::object(), "error.new");
                            }
                            finish();
                        });
                break;
            case Op::Open:
                pickScene(false);
                break;
            case Op::Replace:
                pickScene(true);
                break;
            case Op::OpenRecent:
                open(pending.path);
                finish();
                break;
            case Op::Close:
                unsaved(pending.layer,
                        [this, layer = pending.layer](const bool go)
                        {
                            if (go && layer)
                            {
                                const Reply closed =
                                    call("command", "closeSceneLayer", {{"layer", *layer}, {"force", discard}});
                                if (!closed.ok)
                                    toast(ToastLevel::Error, "error.close",
                                          {{"error", std::string_view(closed.error)}});
                            }
                            finish();
                        });
                break;
            case Op::Save:
                save(pending.layer, false,
                     [this, done = pending.done](const bool saved)
                     {
                         finish();
                         if (done)
                         {
                             done(saved);
                         }
                     });
                break;
            case Op::SaveAs:
                save(pending.layer, true, [this](bool) { finish(); });
                break;
            case Op::SaveAll:
                saveAll([this](bool) { finish(); });
                break;
            case Op::SaveWorkspace:
                pickWorkspace(true);
                break;
            case Op::OpenWorkspace:
                pickWorkspace(false);
                break;
            case Op::SaveEntity:
                saveEntity(pending.entity);
                break;
            case Op::Import:
                pickModel();
                break;
            case Op::Quit:
                unsaved(std::nullopt,
                        [this](const bool go)
                        {
                            if (go)
                            {
                                if (const Reply ended = call("command", "shutdown"); !ended.ok)
                                {
                                    toast(ToastLevel::Error, "error.quit", {{"error", std::string_view(ended.error)}});
                                }
                            }
                            finish();
                        });
                break;
            }
        }
    };

    EditorSceneFlows::EditorSceneFlows(Engine& engine, EditorModel& model, EditorDialogRequests& dialogs,
                                       EditorFeedback& feedback) :
        m_state(std::make_unique<State>(engine, model, dialogs, feedback))
    {
    }

    EditorSceneFlows::~EditorSceneFlows() = default;

    void EditorSceneFlows::request(const Op op, std::string path, std::function<void(bool saved)> done)
    {
        m_state->queued.push_back({op, std::move(path), std::move(done)});
    }

    void EditorSceneFlows::saveEntity(const Entity entity)
    {
        m_state->queued.push_back({.op = Op::SaveEntity, .entity = entity});
    }

    void EditorSceneFlows::requestLayer(const Op op, const u32 layer)
    {
        m_state->queued.push_back({.op = op, .layer = layer});
    }

    bool EditorSceneFlows::busy() const { return m_state->busy; }

    void EditorSceneFlows::update()
    {
        State& state = *m_state;
        if (state.queued.empty())
        {
            return;
        }
        std::vector<Pending> queued;
        queued.swap(state.queued);
        const sizet started = state.busy ? 0 : 1;
        if (!state.busy)
        {
            state.start(queued.front());
        }
        for (const Pending& dropped : std::span(queued).subspan(started))
        {
            if (dropped.done)
            {
                dropped.done(false);
            }
        }
    }
} // namespace spite
