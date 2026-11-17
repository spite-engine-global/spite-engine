#include "control/ControlServer.hpp"
#if defined(_WIN32)
#include "platform/ExecutablePath.hpp"
#include "control/ControlPipe.hpp"
#endif

#include "base/CrashCapture.hpp"
#include "profiling/Zone.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <ranges>
#include <span>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#elif !defined(SPITE_SHIPPING)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "base/Assert.hpp"
#include "base/Logging.hpp"
#include "base/io/FileSystem.hpp"
#include "base/ServiceLocator.hpp"
#include "ecs/Aspect.hpp"
#include "ecs/Chunk.hpp"
#include "ecs/ComponentMetadata.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "ecs/EcsStats.hpp"
#include "ecs/World.hpp"
#include "serialization/FormatWalk.hpp"
#include "serialization/JsonFormat.hpp"

#include "CommandHistory.hpp"
#include "EcsTables.hpp"
#include "FindEntities.hpp"
#include "control/Snapshot.hpp"

namespace spite
{
    namespace
    {
        const nlohmann::json& asJson(const void* json) { return *static_cast<const nlohmann::json*>(json); }

        constexpr std::string_view SOURCE_NAMES[] = {"host", "editor", "stdio", "pipe"};
        constexpr sizet MAX_CLIENT_NAME = 64;

        constexpr std::string_view sourceName(const RequestSource source)
        {
            return SOURCE_NAMES[static_cast<u8>(source)];
        }

        // a reply line: an invalid UTF-8 byte in a string (a bad path, a stale name) becomes U+FFFD, never an exception
        std::string replyText(const nlohmann::json& reply)
        {
            return reply.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        }

        std::string utcNow()
        {
            return std::format("{:%FT%T}Z",
                               std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
        }

        // the commands whose failure the command log keeps: edits, which a client reviews
        bool logsFailure(const std::string_view command, const bool undoable)
        {
            return undoable || command == "batch" || command == "undo" || command == "redo" ||
                command == "revertStep" || command == "historyJump";
        }
        void writeAttribution(const Attribution& by, IFormatWriter& out)
        {
            out.writeString("source", by.source);
            out.writeString("client", by.client);
            out.writeString("time", by.time);
            out.writeU64("frame", by.frame);
        }
        constexpr std::string_view ARG_KIND_NAMES[] = {"string", "integer",   "number", "boolean",
                                                       "entity", "component", "json"};

        // what each kind means in args and in replies; describe reports them as `kinds`
        constexpr std::string_view ARG_KIND_DOCS[] = {
            "text; an integer is taken as its decimal text",
            "an unsigned integer, or a string holding one",
            "any number",
            "true or false",
            "an entity id; 0 is no entity, and an Entity field with no entity (invalid) reads 0 in replies",
            "a registered component's name",
            "any JSON value: objects, arrays, lists of names"};

        constexpr sizet HELP_LINE = 100; // characters of a doc the help query shows

        std::string_view kindName(const ArgKind kind) { return ARG_KIND_NAMES[static_cast<u8>(kind)]; }

        // A declared arg, owned: a module's strings go when it unloads.
        struct Arg
        {
            std::string name;
            ArgKind kind = ArgKind::String;
            bool required = false;
            std::string doc;
            std::string defaultValue;
            std::vector<std::string> choices;
            std::string root; // the named root a file arg lives under, HOST_ROOT or empty
            // the values the arg usually takes, read at describe time; not enforced (suggestArgValues)
            ControlServer::SuggestFn suggest;
        };

        std::string utf8Text(const std::filesystem::path& path)
        {
            const std::u8string text = path.u8string();
            return {text.begin(), text.end()};
        }

        // Why text is not a plain relative file path: the reason, empty when it is one. A ':' names an NTFS
        // alternate data stream or a drive; a DOS device stem opens a device, not a file. A directory may be empty
        // (the root itself) or end in a separator.
        std::string pathProblem(const std::string_view text, const bool directory = false)
        {
            if (text.empty())
            {
                return directory ? std::string{} : "is empty";
            }
            if (text.find('\0') != std::string_view::npos)
            {
                return "has a NUL byte";
            }
            const std::filesystem::path path(std::u8string(text.begin(), text.end())); // UTF-8, not the ANSI page
            if (path.has_root_name() || path.has_root_directory())
            {
                return "is absolute";
            }
            if (!directory && (!path.has_filename() || path.filename() == "."))
            {
                return "names no file";
            }
            for (const std::filesystem::path& part : path)
            {
                const std::u8string wide = part.u8string();
                const std::string name(wide.begin(), wide.end());
                // Win32 drops trailing spaces and dots from a component: ".. ", "..." and ". .." name the parent
                if (name.find_first_not_of(" .") == std::string::npos && std::ranges::count(name, '.') >= 2)
                {
                    return "has '..'";
                }
                if (name.find(':') != std::string::npos)
                {
                    return "has a ':' (a drive or a stream)";
                }
                if (isWindowsDeviceName(name))
                {
                    return "is a device name";
                }
            }
            return {};
        }

        bool unsignedText(const nlohmann::json& value)
        {
            if (!value.is_string())
            {
                return false;
            }
            const std::string& text = value.get_ref<const std::string&>();
            u64 parsed = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
            return !text.empty() && error == std::errc{} && end == text.data() + text.size();
        }

        // as the ControlArgs accessors read them
        bool fits(const ArgKind kind, const nlohmann::json& value)
        {
            switch (kind)
            {
            case ArgKind::String:
                return value.is_string() || value.is_number_integer();
            case ArgKind::Integer:
            case ArgKind::Entity:
                return value.is_number_unsigned() || unsignedText(value);
            case ArgKind::Number:
                return value.is_number();
            case ArgKind::Boolean:
                return value.is_boolean();
            case ArgKind::Component:
                return value.is_string();
            default:
                return true;
            }
        }

        // args against declared: one {arg, problem, expected} per problem;
        // problem is missing, unknown, kind, choice, path or unknownComponent
        nlohmann::json checkArgs(const std::vector<Arg>& declared, const nlohmann::json& args)
        {
            nlohmann::json problems = nlohmann::json::array();
            const auto add =
                [&problems](const std::string_view arg, const std::string_view problem, const std::string_view expected)
            { problems.push_back({{"arg", arg}, {"problem", problem}, {"expected", expected}}); };
            for (const auto& [key, value] : args.items())
            {
                const auto arg = std::ranges::find(declared, key, &Arg::name);
                if (arg == declared.end())
                {
                    std::string names;
                    for (const Arg& known : declared)
                    {
                        names += (names.empty() ? "" : ", ") + known.name;
                    }
                    add(key, "unknown", names.empty() ? "no args" : "one of " + names);
                }
                else if (!fits(arg->kind, value))
                {
                    add(key, "kind", kindName(arg->kind));
                }
                else if (!arg->choices.empty() &&
                         !(value.is_string() &&
                           std::ranges::contains(arg->choices, value.get_ref<const std::string&>())))
                {
                    std::string names;
                    for (const std::string& choice : arg->choices)
                    {
                        names += (names.empty() ? "" : ", ") + choice;
                    }
                    add(key, "choice", "one of " + names);
                }
                else if (!arg->root.empty() && arg->root != HOST_ROOT && value.is_string() &&
                         !pathProblem(value.get_ref<const std::string&>(), arg->root == ANY_ROOT).empty())
                {
                    const bool any = arg->root == ANY_ROOT;
                    add(key, "path",
                        (any ? std::string("a plain relative directory path, '<root>/<directory>', but it ")
                             : "a plain relative file path under the '" + arg->root + "' root, but it ") +
                            pathProblem(value.get_ref<const std::string&>(), any));
                }
                else if (arg->kind == ArgKind::Component &&
                         getComponentIdByName(value.get_ref<const std::string&>().c_str()) == INVALID_COMPONENT_ID)
                {
                    add(key, "unknownComponent", "a registered component name");
                }
            }
            for (const Arg& arg : declared)
            {
                if (arg.required && !args.contains(arg.name))
                {
                    add(arg.name, "missing", kindName(arg.kind));
                }
            }
            return problems;
        }

        std::string argsMessage(const std::string_view name, const nlohmann::json& problems)
        {
            std::string message = "'" + std::string(name) + "' args:";
            for (const nlohmann::json& problem : problems)
            {
                const std::string arg = problem["arg"].get<std::string>();
                const std::string kind = problem["problem"].get<std::string>();
                const std::string expected = problem["expected"].get<std::string>();
                message += (&problem == &problems.front() ? " " : "; ") +
                    (kind == "missing"       ? "missing '" + arg + "' (" + expected + ")"
                         : kind == "unknown" ? "unknown '" + arg + "' (" + expected + ")"
                         : kind == "kind"    ? "'" + arg + "' must be " + expected
                         : kind == "choice"  ? "'" + arg + "' must be " + expected
                         : kind == "path"    ? "'" + arg + "' must be " + expected
                                             : "'" + arg + "' names no registered component");
            }
            return message;
        }

        // Rewrites the entity ids inside a stored set or addComponent value
        void forwardValueRefs(std::string_view command, nlohmann::json& args, const CommandHistory& history);
    } // namespace

    std::optional<std::string> ControlArgs::string(const std::string_view key) const
    {
        const nlohmann::json& args = asJson(m_json);
        const auto it = args.find(key);
        if (it == args.end())
        {
            return std::nullopt;
        }
        if (it->is_string())
        {
            return it->get<std::string>();
        }
        if (it->is_number_integer())
        {
            return it->dump();
        }
        return std::nullopt;
    }

    std::optional<std::string> ControlArgs::json(const std::string_view key) const
    {
        const nlohmann::json& args = asJson(m_json);
        const auto it = args.find(key);
        return it == args.end() ? std::nullopt : std::optional<std::string>(it->dump());
    }

    std::optional<u64> ControlArgs::integer(const std::string_view key) const
    {
        const nlohmann::json& args = asJson(m_json);
        const auto it = args.find(key);
        if (it == args.end())
        {
            return std::nullopt;
        }
        if (it->is_number_unsigned())
        {
            return it->get<u64>();
        }
        if (it->is_string())
        {
            const std::string& text = it->get_ref<const std::string&>();
            u64 value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error == std::errc{} && end == text.data() + text.size())
            {
                return value;
            }
        }
        return std::nullopt;
    }

    std::optional<bool> ControlArgs::boolean(const std::string_view key) const
    {
        const nlohmann::json& args = asJson(m_json);
        const auto it = args.find(key);
        if (it != args.end() && it->is_boolean())
        {
            return it->get<bool>();
        }
        return std::nullopt;
    }

    struct ControlServer::State
    {
        struct WorldEntry
        {
            std::string name;
            World world;
            CommandHistory* history = nullptr;
            u64 serial = 0; // unique per addWorld: a world removed and another added under its name differ
            SourceFn source; // the id in the world this one was cloned from, or 0
            mutable AgentTimeline timeline;
            mutable u32 timelineTransport = ~0u;
            mutable u64 timelineEntityStep = 0; // the apply or its revert, for related readbacks
            mutable u64 timelineStepState = 0; // a later edit can fold into the same history index
            mutable u64 timelineActivity = 0;
        };
        std::vector<WorldEntry> worlds;
        u64 nextWorldSerial = 1;
        u64 agentTimelineRevision = 0;
        std::vector<AgentActivity> activities;
        std::vector<std::pair<u64, u32>> explicitActivityTransports;
        u64 nextActivity = 1, activityRevision = 0, runningActivity = 0;
        std::function<u64()> activityClock;
        AgentTraffic agentTraffic;

        [[nodiscard]] u64 activityNowMs() const
        {
            return activityClock ? activityClock()
                                 : static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                        std::chrono::steady_clock::now().time_since_epoch())
                                                        .count());
        }

        struct EditorLease
        {
            u32 transport;
            std::string owner;
            u64 heartbeatMs;
            u64 expiresAtMs;
        };
        std::optional<EditorLease> editorLease;

        bool liveLeaseTransport(const u32 source)
        {
            if (source == LOCAL)
                return true;
            if (source == STDIO)
            {
#if defined(_WIN32) || !defined(SPITE_SHIPPING)
                return !inputClosed.load(std::memory_order_acquire) ||
                    pendingStdioRequests.load(std::memory_order_acquire) != 0;
#else
                return false;
#endif
            }
#if defined(_WIN32)
            const std::lock_guard lock(pipeMutex);
            return pipeConnection != 0 && source == pipeConnection;
#else
            return false;
#endif
        }

        void refreshEditorLease()
        {
            if (!editorLease)
                return;
            if (activityNowMs() >= editorLease->expiresAtMs || !liveLeaseTransport(editorLease->transport))
                editorLease.reset();
        }

        void writeEditorLease(IFormatWriter& out)
        {
            refreshEditorLease();
            const u64 now = activityNowMs();
            const bool held = editorLease && now < editorLease->expiresAtMs;
            if (!held)
                editorLease.reset();
            out.writeBool("held", held);
            out.writeString("owner", held ? editorLease->owner : "");
            out.writeString("transport",
                            held ? (editorLease->transport == LOCAL       ? "local"
                                        : editorLease->transport == STDIO ? "stdio"
                                                                          : "pipe")
                                 : "");
            out.writeU64("heartbeatMs", held ? editorLease->heartbeatMs : 0);
            out.writeU64("expiresAtMs", held ? editorLease->expiresAtMs : 0);
            out.writeU64("remainingMs", held ? editorLease->expiresAtMs - now : 0);
        }

        const AgentActivity* activity(const u64 id) const
        {
            const auto it = std::ranges::find(activities, id, &AgentActivity::id);
            return it == activities.end() ? nullptr : &*it;
        }

        AgentActivity* activity(const u64 id)
        {
            const auto it = std::ranges::find(activities, id, &AgentActivity::id);
            return it == activities.end() ? nullptr : &*it;
        }

        u64 startActivity(const std::string_view kind, const std::string_view title, const std::string_view why)
        {
            if (!explicitActivityTransports.empty())
            {
                u32 connectedPipe = 0;
#if defined(_WIN32)
                {
                    const std::lock_guard lock(pipeMutex);
                    connectedPipe = pipeConnection;
                }
#endif
                bool stdioFinished = false;
#if defined(_WIN32) || !defined(SPITE_SHIPPING)
                stdioFinished =
                    inputClosed.load(std::memory_order_acquire) && !hasInput.load(std::memory_order_acquire);
#endif
                std::erase_if(explicitActivityTransports,
                              [&](const auto& owner)
                              {
                                  const auto [id, source] = owner;
                                  const AgentActivity* row = activity(id);
                                  if (!row || !row->active())
                                      return true;
                                  if (source == transport ||
                                      !(source == STDIO ? stdioFinished : source != connectedPipe))
                                      return false;
                                  updateActivity(id, row->phase, activityStep(*row) ? "ready" : "cancelled",
                                                 row->action, row->evidence, row->reported);
                                  return true;
                              });
            }
            constexpr sizet capacity = 64;
            if (activities.size() == capacity)
            {
                const auto removable =
                    std::ranges::find_if(activities, [](const AgentActivity& row) { return !row.active(); });
                if (removable == activities.end())
                    return 0;
                activities.erase(removable);
            }
            activities.push_back({.id = nextActivity++,
                                  .startedMs = activityNowMs(),
                                  .kind = std::string(kind),
                                  .title = std::string(title),
                                  .why = std::string(why),
                                  .phase = "inspect",
                                  .state = "running"});
            ++activityRevision;
            return activities.back().id;
        }

        void updateActivity(const u64 id, const std::string_view phase, const std::string_view status,
                            const std::string_view action, const std::string_view evidence, const bool reported = false)
        {
            AgentActivity* row = activity(id);
            if (!row ||
                (row->phase == phase && row->state == status && row->action == action && row->evidence == evidence &&
                 row->reported == reported))
                return;
            row->phase = phase;
            row->state = status;
            row->action = action;
            row->evidence = evidence;
            row->reported = reported;
            ++activityRevision;
        }

        const UndoStep* activityStep(const AgentActivity& row) const
        {
            const auto world = std::ranges::find(worlds, row.worldSerial, &WorldEntry::serial);
            if (world == worlds.end())
                return nullptr;
            const auto position = world->history->find(row.step);
            if (!position || *position >= world->history->done())
                return nullptr;
            const UndoStep& step = world->history->steps()[*position];
            return step.state == row.stepState && !step.view ? &step : nullptr;
        }

        void updateActivityUndo(const WorldEntry& world)
        {
            for (AgentActivity& row : activities)
            {
                if (row.worldSerial != world.serial || (row.state != "ready" && row.state != "undone"))
                    continue;
                if (const auto position = world.history->find(row.step))
                    updateActivity(row.id, row.phase, *position < world.history->done() ? "ready" : "undone",
                                   row.action, row.evidence, row.reported);
            }
        }
        struct Handler
        {
            bool command = false;
            bool undoable = false;
            std::string owner;
            HandlerFn fn;
            bool described = false; // args are checked against args
            std::string doc;
            std::vector<Arg> args;
            bool view = false; // changes no scene or component state
            PreflightFn preflight;
        };
        std::map<std::string, Handler, std::less<>> handlers;
        std::vector<std::pair<std::string, std::string>> refusals; // owner, message of a refused registration
        std::map<std::string, std::string, std::less<>> unavailable; // name -> reason (markUnavailable)
        struct Root
        {
            std::filesystem::path directory;
            std::string doc;
        };
        std::map<std::string, Root, std::less<>> roots; // setRoot

        std::string resolveRooted(const std::string_view root, const std::string_view text, std::filesystem::path& out,
                                  const bool directory = false) const
        {
            const auto it = roots.find(root);
            if (it == roots.end())
            {
                return "no '" + std::string(root) + "' root is set in this engine";
            }
            if (const std::string problem = pathProblem(text, directory); !problem.empty())
            {
                return "path '" + std::string(text) + "' is not a plain relative " +
                    (directory ? "directory" : "file") + " path under the '" + std::string(root) + "' root: it " +
                    problem;
            }
            const std::filesystem::path file = text.empty()
                ? it->second.directory
                : it->second.directory / std::filesystem::path(std::u8string(text.begin(), text.end()));
            std::error_code fileError;
            std::error_code rootError;
            const std::filesystem::path relative =
                std::filesystem::weakly_canonical(file, fileError)
                    .lexically_relative(std::filesystem::weakly_canonical(it->second.directory, rootError));
            if (fileError || rootError || relative.empty() || *relative.begin() == "..")
            {
                return "path '" + std::string(text) + "' leaves the '" + std::string(root) + "' root through a link";
            }
            out = file;
            return {};
        }

        // the "entries" array of describe: every handler keep accepts, with its args
        template <class Keep>
        void writeEntries(IFormatWriter& out, const Keep& keep) const
        {
            out.beginArray("entries", 0);
            for (const auto& [name, handler] : handlers)
            {
                if (!keep(name, handler))
                {
                    continue;
                }
                out.beginObject("");
                out.writeString("name", name);
                out.writeString("kind", handler.command ? "command" : "query");
                out.writeBool("undoable", handler.undoable);
                out.writeBool("view", handler.view);
                out.writeString("owner", handler.owner);
                // an entry registered without args takes any, unchecked
                out.writeBool("described", handler.described);
                if (handler.described)
                {
                    out.writeString("doc", handler.doc);
                    out.beginArray("args", static_cast<u32>(handler.args.size()));
                    for (const Arg& arg : handler.args)
                    {
                        out.beginObject("");
                        out.writeString("name", arg.name);
                        out.writeString("kind", kindName(arg.kind));
                        out.writeBool("required", arg.required);
                        out.writeString("doc", arg.doc);
                        if (!arg.root.empty())
                        {
                            out.writeString("root", arg.root);
                        }
                        if (!arg.defaultValue.empty())
                        {
                            out.writeString("default", arg.defaultValue);
                        }
                        if (!arg.choices.empty())
                        {
                            out.beginArray("choices", static_cast<u32>(arg.choices.size()));
                            for (const std::string& choice : arg.choices)
                            {
                                out.writeString("", choice);
                            }
                            out.endArray();
                        }
                        else if (arg.suggest)
                        {
                            // open: other values may be accepted; the handler refuses the unknown ones by name
                            const std::vector<std::string> values = arg.suggest();
                            out.beginArray("choices", static_cast<u32>(values.size()));
                            for (const std::string& value : values)
                            {
                                out.writeString("", value);
                            }
                            out.endArray();
                            out.writeBool("open", true);
                        }
                        out.endObject();
                    }
                    out.endArray();
                }
                out.endObject();
            }
            out.endArray();
        }

        std::string unknownEntry(const std::string_view name) const
        {
            const auto it = unavailable.find(name);
            return it != unavailable.end() ? "'" + std::string(name) + "' is unavailable: " + it->second
                                           : "unknown query or command '" + std::string(name) + "'";
        }

        // A name taken by another owner or kind is never replaced: the entry may be running, and its owner's
        // unload would delete the replacement.
        void place(const std::string_view name, Handler handler)
        {
            const auto taken = handlers.find(name);
            if (taken != handlers.end() &&
                (taken->second.owner != handler.owner || taken->second.command != handler.command ||
                 taken->second.undoable != handler.undoable))
            {
                std::string message = std::format(
                    "{} `{}` of {} refused: the name is taken by {} `{}` of {}", handler.command ? "command" : "query",
                    name, handler.owner, taken->second.command ? "command" : "query", name, taken->second.owner);
                SERROR_LOG("control: {}", message);
                refusals.emplace_back(handler.owner, std::move(message));
                return;
            }
            handlers.insert_or_assign(std::string(name), std::move(handler));
        }
        nlohmann::json argErrors; // the refused request's, for its reply
        nlohmann::json refusal; // details of a refusal that changed nothing (revertStep), for its reply
        ComponentEditGuardFn componentGuard;
        ComponentAddedFn componentAdded;
        HierarchyReaderFn hierarchy;
        EntityLayerFn entityLayer;

        // the row members of an entity's hierarchy place: parent (a root has none) and the direct child count
        void writeRelation(const World& world, const Entity entity, IFormatWriter& out) const
        {
            if (!hierarchy)
            {
                return;
            }
            const EntityRelation relation = hierarchy(world, entity);
            if (relation.parent.isValid())
            {
                out.writeU64("parent", relation.parent.id());
            }
            out.writeU32("children", relation.children);
            out.writeBool("enabled", relation.enabled);
            out.writeBool("active", relation.active);
            if (entityLayer)
            {
                out.writeU32("layer", entityLayer(world, entity));
            }
        }
        UndoLimits undoLimits;
        u64 commandGeneration = 0;
        BatchStatus batchStatus;
        bool batchReady = false; // the queued head has had one frame to show its active status
        u32 batchReadyTransport = 0;
        u64 stateGeneration = 0;

        // What the running command recorded for undo (recordInverse,
        // recordCreated) and the dead entity references it wrote.
        struct Capture
        {
            bool undoable = false;
            std::vector<CommandRequest> inverse;
            std::vector<u64> created;
            // touchLayer's notes; allocated by the first call, as debug builds allocate for every empty vector
            struct LayerNotes
            {
                std::vector<u32> touched;
                std::vector<u32> saved;
            };
            std::unique_ptr<LayerNotes> layers;
            std::vector<std::string> warnings;
            std::optional<nlohmann::json> forward; // recordForward
            bool replay = false; // recordReplay
            std::string replayWorld; // recordReplayIn
            std::optional<nlohmann::json> replayArgs;
            bool threw = false; // the handler threw: it may have acted before
        };
        Capture* capture = nullptr;
        // the step of the batch running now and its world: its commands are not in the history yet (layerDirty)
        const UndoStep* batchStep = nullptr;
        std::string batchWorld;
        // > 0 while rerun executes a stored request (undo, redo, a batch's rollback); requests a client sends never
        // pass through rerun
        u32 replaying = 0;

        // Deferred replies (deferReply): the running request's id and where a
        // later reply goes; deferredNow is what its handler took.
        struct Deferred
        {
            nlohmann::json requestId;
            ReplyFn send;
            std::string owner; // the deferring handler's
        };
        const nlohmann::json* runningId = nullptr;
        const std::string* runningOwner = nullptr;
        const ReplyFn* runningSend = nullptr;
        u64 deferredNow = 0;
        u64 nextDeferred = 1;
        std::map<u64, Deferred> deferred;
        // results deferReplyAsync callers queue from other threads; shared so a late caller outlives the server
        struct Mailbox
        {
            std::mutex mutex;
            std::vector<std::pair<DeferredReply, ResultFn>> items;
            std::atomic<bool> any{false};
        };
        std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
        // step replies waiting for their world's pending steps to run
        struct PendingStep
        {
            DeferredReply reply;
            std::string world;
            u64 worldSerial = 0;
            bool armed = true; // false: steps are still to be requested
            std::vector<std::pair<std::string, u32>> counts;
            u64 activity = 0;
        };
        std::vector<PendingStep> pendingSteps;
        // replyAfterFrames: replies due once `left` more pumps have run
        struct FrameWait
        {
            DeferredReply reply;
            u32 left = 1;
            ResultFn fill;
        };
        std::vector<FrameWait> frameWaits;
        struct FrameStartRequest
        {
            std::string owner, request;
            std::function<void(const std::string&)> done;
        };
        std::vector<FrameStartRequest> frameStart;
        struct FrameCallback
        {
            std::string owner;
            std::function<void()> fn;
        };
        std::vector<FrameCallback> frameCallbacks, runningFrameCallbacks;
        std::string frameCallbackOwner;
        bool frameCallbacksRunning = false;


        // Transports queue (source, line) pairs; pump() drains them and replies
        // on the source: STDIO, or the pipe connection that sent the line.
        static constexpr u32 STDIO = 0;
#if defined(_WIN32) || !defined(SPITE_SHIPPING)
        std::mutex inboxMutex;
        std::vector<std::pair<u32, std::string>> inbox;
        std::atomic<bool> hasInput{false};
#endif
        // moves whenever attachedClient() may answer differently: a pipe connects or drops, stdio opens or closes, a
        // client announces its name
        std::atomic<u64> attachedEpoch{0};

        // attribution, frame thread only: the connection pump() runs a line of (LOCAL outside), the
        // running request's source and transport, and the names connections announced with hello
        static constexpr u32 LOCAL = ~0u;
        RequestSource localSource = RequestSource::Host;
        u32 transport = LOCAL;
        u32 runningTransport = LOCAL;
        std::string stdioClient;
        u32 pipeClientConnection = 0;
        std::string pipeClient;
        u64 frames = 0; // pumps run
        u64 attributed = 0; // requests handle() attributed
        u64 clientCommands = 0; // of those, the ones that reported a pipe or stdio source
        bool runningCommand = false; // the running request is a command: running is its attribution
        Attribution running;
        std::chrono::steady_clock::time_point runningStart;

        [[nodiscard]] Attribution attributionOf(const u32 from) const
        {
            if (from == LOCAL)
            {
                return {.source = std::string(sourceName(localSource))};
            }
            if (from == STDIO)
            {
                return {.source = "stdio", .client = stdioClient};
            }
            return {.source = "pipe", .client = from == pipeClientConnection ? pipeClient : std::string()};
        }

        [[nodiscard]] std::string_view agentClient() const
        {
            return transport == STDIO               ? stdioClient
                : transport == pipeClientConnection ? std::string_view(pipeClient)
                                                    : std::string_view{};
        }

        [[nodiscard]] bool sameTimelineClient(const WorldEntry& entry) const
        {
            return entry.timelineTransport == transport && entry.timeline.client == agentClient();
        }

        static bool timelineEntity(const WorldEntry& entry, const std::optional<u64> id)
        {
            if (!id || entry.timeline.step == 0)
                return false;
            const auto& provenance = entry.history->provenance();
            const auto row = provenance.find(*id);
            if (row == provenance.end() || row->second.step != entry.timelineEntityStep)
                return false;
            const auto position = entry.history->find(entry.timelineEntityStep);
            return position && *position < entry.history->done() &&
                entry.history->steps()[*position].state == entry.timelineStepState;
        }

        bool activityPendingReview(const WorldEntry& entry, const UndoStep& step) const
        {
            for (const auto& [id, fields] : entry.history->touchedFields(step))
            {
                const auto found = entry.history->provenance().find(id);
                if (found != entry.history->provenance().end() &&
                    std::ranges::any_of(
                        found->second.unreviewed, [&](const auto& field)
                        { return field.firstStep <= step.index && std::ranges::contains(fields, field.name); }))
                    return true;
            }
            return false;
        }

        void updateActivityReviews(const WorldEntry& entry)
        {
            for (const AgentActivity& row : activities)
            {
                if (row.worldSerial != entry.serial ||
                    (row.state != "ready" && row.state != "done" && row.state != "passed"))
                    continue;
                if (const UndoStep* step = activityStep(row); step && !activityPendingReview(entry, *step))
                    updateActivity(row.id, "review", "kept", "review", row.evidence, row.reported);
            }
        }

        void noteAgentReview(const WorldEntry& entry, const std::string_view verb)
        {
            AgentTimeline& timeline = entry.timeline;
            if (timeline.phase != AgentTimelinePhase::Review || timeline.review != verb)
            {
                timeline.phase = AgentTimelinePhase::Review;
                timeline.review = verb;
                ++agentTimelineRevision;
            }
            if (const AgentActivity* row = activity(entry.timelineActivity); row && row->worldSerial == entry.serial)
            {
                bool pending = false;
                if (verb == "review")
                {
                    const UndoStep* step = activityStep(*row);
                    if (!step || row->state == "kept" || row->state == "reverted" || row->state == "acknowledged")
                        return;
                    pending = activityPendingReview(entry, *step);
                }
                const std::string_view status = verb == "review" ? (pending ? std::string_view(row->state) : "kept")
                    : verb == "revertStep"                       ? "reverted"
                    : row->active()                              ? "running"
                                                                 : "ready";
                updateActivity(row->id, "review", status, verb, row->evidence, row->reported);
            }
        }

        void noteAgentRead(const WorldEntry& entry, const std::string_view verb, const ControlArgs& args)
        {
            AgentTimeline& timeline = entry.timeline;
            const bool same =
                sameTimelineClient(entry) && (!runningActivity || runningActivity == entry.timelineActivity);
            if (same)
            {
                if (verb == "entity" && timelineEntity(entry, args.integer("id")))
                {
                    if (timeline.phase == AgentTimelinePhase::Apply)
                        noteAgentReview(entry, verb);
                    return;
                }
                if (timeline.phase == AgentTimelinePhase::Apply ||
                    (timeline.phase == AgentTimelinePhase::Inspect && timeline.inspect == verb))
                    return;
            }
            timeline = {.phase = AgentTimelinePhase::Inspect,
                        .client = std::string(agentClient()),
                        .inspect = std::string(verb)};
            entry.timelineTransport = transport;
            if (runningActivity)
                entry.timelineActivity = runningActivity;
            else if (!same || !activity(entry.timelineActivity) || activity(entry.timelineActivity)->phase != "inspect")
                entry.timelineActivity =
                    (entry.name == "scene" || entry.name == "play") ? startActivity("scene", verb, {}) : 0;
            if (AgentActivity* row = activity(entry.timelineActivity))
            {
                row->inspect = verb;
                updateActivity(row->id, "inspect", runningActivity ? "running" : "inspected", verb, {});
            }
            ++agentTimelineRevision;
        }

        void noteAgentApply(const WorldEntry& entry, const std::string_view operation, const UndoStep& step)
        {
            AgentTimeline& timeline = entry.timeline;
            const bool same =
                sameTimelineClient(entry) && (!runningActivity || runningActivity == entry.timelineActivity);
            const bool fromInspect = same && timeline.phase == AgentTimelinePhase::Inspect;
            if (!fromInspect && !(same && runningActivity))
                timeline = {.client = std::string(agentClient())};
            timeline.phase = AgentTimelinePhase::Apply;
            timeline.operation = operation;
            timeline.step = step.index;
            entry.timelineTransport = transport;
            entry.timelineEntityStep = step.index;
            entry.timelineStepState = step.state;
            if (runningActivity)
                entry.timelineActivity = runningActivity;
            else if (!fromInspect || !activity(entry.timelineActivity))
                entry.timelineActivity = startActivity("scene", operation.substr(0, 120), {});
            if (AgentActivity* row = activity(entry.timelineActivity))
            {
                if (fromInspect && !runningActivity)
                    row->title = operation.substr(0, 120);
                if (row->inspect.empty() && fromInspect)
                    row->inspect = timeline.inspect;
                row->world = entry.name;
                row->worldSerial = entry.serial;
                row->step = step.index;
                row->stepState = step.state;
                std::string evidence;
                for (const CommandRecord& record : step.records)
                {
                    if (record.forward.command != "set" || record.inverse.empty())
                        continue;
                    const auto& forward = record.forward.args;
                    const auto& inverse = record.inverse.front().args;
                    if (!forward.contains("path") || !forward["path"].is_string() || !forward.contains("value") ||
                        !inverse.contains("value"))
                        continue;
                    const std::string before = inverse["value"].dump();
                    const std::string after = forward["value"].dump();
                    if (before.size() <= 64 && after.size() <= 64)
                    {
                        evidence = std::format("{}  {} → {}", forward["path"].get<std::string>(), before, after);
                        break;
                    }
                }
                updateActivity(row->id, "apply", runningActivity ? "running" : "ready", operation, evidence);
                ++activityRevision; // a merged edit can change the step identity without changing the text
            }
            ++agentTimelineRevision;
        }

        [[nodiscard]] u64 elapsedUs() const
        {
            return static_cast<u64>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - runningStart)
                    .count());
        }

#if defined(_WIN32) || !defined(SPITE_SHIPPING)
        std::thread reader;
        std::atomic<bool> inputClosed{false};
        std::atomic<u64> pendingStdioRequests{0};
#if !defined(_WIN32)
        int stdioCancel[2] = {-1, -1};
#endif
#endif
#if defined(_WIN32)
        std::atomic<bool> stopping{false};

        // pipe transport: one client at a time, overlapped I/O throughout so the
        // frame thread's replies never queue behind the pipe thread's pending read
        std::thread pipeThread;
        HANDLE stopEvent = nullptr;
        HANDLE writeEvent = nullptr;
        std::function<void()> receiveQueuedHook;
        std::function<void()> pipeWritePendingHook;
        std::function<void()> pipeGapHook; // set before listenPipe, runs on the pipe thread after each client
        std::function<void()> pipeKeeperHook; // set before listenPipe, runs on the pipe thread once the keeper exists
        std::mutex writeMutex;
        std::mutex pipeMutex; // guards pipe and pipeConnection against the close on disconnect
        HANDLE pipe = INVALID_HANDLE_VALUE;
        u32 pipeConnection = 0; // generation of the connected client; 0 = none
#endif

        // a request naming no world gets the first one, or the play world when preferPlay and one runs
        [[nodiscard]] const WorldEntry* findEntry(const ControlArgs& args, const bool preferPlay = false) const
        {
            if (worlds.empty())
            {
                return nullptr;
            }
            const std::optional<std::string> name = args.string("world");
            if (!name)
            {
                const auto play = preferPlay ? std::ranges::find(worlds, "play", &WorldEntry::name) : worlds.end();
                return play == worlds.end() ? &worlds.front() : &*play;
            }
            const auto it = std::ranges::find(worlds, *name, &WorldEntry::name);
            return it == worlds.end() ? nullptr : &*it;
        }

        [[nodiscard]] std::string unknownWorld(const std::string_view name) const
        {
            std::string message = "unknown world '" + std::string(name) + "'; worlds:";
            for (const WorldEntry& entry : worlds)
            {
                message += (&entry == &worlds.front() ? " " : ", ") + entry.name;
            }
            return message;
        }

        [[nodiscard]] std::string unknownWorld(const ControlArgs& args) const
        {
            return unknownWorld(args.string("world").value_or(std::string{}));
        }

        [[nodiscard]] const World* findWorld(const ControlArgs& args, const bool preferPlay = false) const
        {
            const WorldEntry* entry = findEntry(args, preferPlay);
            return entry ? &entry->world : nullptr;
        }

        // Looked up anew after every command undo, redo and batch run: a
        // command may change the worlds.
        [[nodiscard]] CommandHistory* historyOf(const std::string_view world) const
        {
            const auto it = std::ranges::find(worlds, world, &WorldEntry::name);
            return it == worlds.end() ? nullptr : it->history;
        }

        // Drops the provenance of the entry's destroyed entities once its table doubled since the last sweep; readers
        // filter by liveness themselves
        void sweepProvenance(const WorldEntry& entry) const
        {
            if (entry.history->provenanceSweepDue())
            {
                entry.history->sweepProvenance([&world = entry.world](const u64 id)
                                               { return world.isEntityAlive(Entity(id)); });
            }
        }

        void record(std::string command, nlohmann::json args, std::vector<u64> revives)
        {
            if (capture)
            {
                SASSERTM(capture->undoable, "An inverse '{}' was recorded by a command not registered undoable.",
                         command);
                capture->inverse.push_back({std::move(command), std::move(args), std::move(revives)});
            }
        }

        static void addLayer(std::vector<u32>& layers, const u32 layer)
        {
            if (layer == ALL_LAYERS)
            {
                layers.assign(1, ALL_LAYERS);
            }
            else if (std::ranges::find(layers, layer) == layers.end() &&
                     std::ranges::find(layers, ALL_LAYERS) == layers.end())
            {
                layers.push_back(layer);
            }
        }

        // Adds the layers a recorded command touched to step.layers: those it declared, else its requests' live
        // entities'. A layer an earlier command of the step saved stops being saved when this one touches it.
        void noteLayers(UndoStep& step, const WorldEntry& entry, const Capture& captured, const CommandRecord& record)
        {
            SPITE_ZONE("Control.noteLayers");
            std::vector<u32> touched;
            static const std::vector<u32> NONE;
            noteTouched(touched, entry, captured.layers ? captured.layers->touched : NONE, record);
            std::erase_if(step.savedLayers,
                          [&touched](const u32 layer)
                          {
                              return std::ranges::find(touched, layer) != touched.end() ||
                                  std::ranges::find(touched, ALL_LAYERS) != touched.end();
                          });
            for (const u32 layer : touched)
            {
                addLayer(step.layers, layer);
            }
            if (captured.layers)
            {
                step.savedLayers.insert(step.savedLayers.end(), captured.layers->saved.begin(),
                                        captured.layers->saved.end());
            }
        }

        void noteTouched(std::vector<u32>& into, const WorldEntry& entry, const std::vector<u32>& declared,
                         const CommandRecord& record)
        {
            for (const u32 layer : declared)
            {
                addLayer(into, layer);
            }
            if (!declared.empty())
            {
                return;
            }
            const auto note = [&](const u64 id)
            {
                const Entity entity(id);
                addLayer(into,
                         !entry.world.isEntityAlive(entity) ? ALL_LAYERS
                             : entityLayer                  ? entityLayer(entry.world, entity)
                                                            : 0);
            };
            const auto noteArgs = [&](const nlohmann::json& args)
            {
                for (const char* key : {"id", "parent"})
                {
                    if (const std::optional<u64> id = ControlArgs(&args).integer(key))
                    {
                        note(*id);
                    }
                }
            };
            noteArgs(record.forward.args);
            for (const u64 created : record.forward.created)
            {
                if (created != 0)
                {
                    note(created);
                }
            }
            for (const CommandRequest& inverse : record.inverse)
            {
                noteArgs(inverse.args);
            }
            // a scene edit naming no entity (a clear) touches whatever it touches: a module that knows declares it
            if (into.empty())
            {
                addLayer(into, ALL_LAYERS);
            }
        }

        void warn(std::vector<std::string> warnings)
        {
            if (capture)
            {
                capture->warnings.insert(capture->warnings.end(), std::make_move_iterator(warnings.begin()),
                                         std::make_move_iterator(warnings.end()));
            }
        }

        // An exception out of a handler is that command's error, not the engine's end; into.threw tells the caller
        // the command may have changed state before it threw. A failed assert (a test's signal) is not caught.
        std::string runHandler(const std::string_view name, const Handler& handler, const nlohmann::json& args,
                               IFormatWriter& out, Capture& into)
        {
            SPITE_ZONE("Control.runHandler");
            into.undoable = handler.undoable;
            Capture* outer = std::exchange(capture, &into);
            // one string built in place: a default-constructed one would cost a debug container proxy per call
            std::string error = [&]
            {
                try
                {
                    return handler.fn(ControlArgs(&args), out);
                }
                catch (const AssertionFailure&)
                {
                    capture = outer;
                    throw;
                }
                catch (const std::exception& thrown)
                {
                    return thrownMessage(name, thrown.what(), into);
                }
                catch (...)
                {
                    return thrownMessage(name, "unknown exception", into);
                }
            }();
            capture = outer;
            if (handler.undoable)
            {
                SPITE_COUNTER("control.command.created", into.created.size());
            }
            return error;
        }

        static std::string thrownMessage(const std::string_view name, const char* what, Capture& into)
        {
            into.threw = true;
            SERROR_LOG("control: '{}' threw: {}", name, what);
            return std::format("'{}' threw an exception: {}", name, what);
        }

        static constexpr std::string_view MAY_HAVE_ACTED = "; it may have changed state before it threw";

        // A plain command threw: put its world back with what it recorded before it threw. True when that left the
        // state as it was; otherwise the world's history states are renewed, so the scene counts as changed.
        bool rollBackThrown(const nlohmann::json& args, Capture& thrown, const bool view)
        {
            const WorldEntry* entry = findEntry(ControlArgs(&args));
            if (!entry)
            {
                return false;
            }
            bool restored = !thrown.inverse.empty();
            for (const CommandRequest& inverse : thrown.inverse)
            {
                Capture ignored;
                if (!rerun(entry->name, inverse, ignored).empty())
                {
                    restored = false;
                    break;
                }
            }
            if (!restored && !view)
            {
                entry->history->renewStates();
            }
            return restored;
        }

        // A stored request's args on world, its entity ids forwarded to the live ones.
        nlohmann::json forwardedArgs(const std::string& world, const CommandRequest& request,
                                     const CommandHistory& history)
        {
            nlohmann::json args = request.args;
            args["world"] = world;
            for (const char* key : {"id", "parent"})
            {
                // the handler reads an Entity arg as a number or a digit string
                if (const std::optional<u64> id = ControlArgs(&args).integer(key))
                {
                    if (const u64 live = history.resolve(*id); live != *id)
                    {
                        args[key] = live;
                    }
                }
            }
            forwardValueRefs(request.command, args, history);
            return args;
        }

        // Runs a stored request, an undoable command, on world with its entity
        // ids forwarded; the entities it creates (into.created) take over from
        // those its last run created. request is not read once the command runs.
        std::string rerun(const std::string& world, const CommandRequest& request, Capture& into)
        {
            const auto handler = handlers.find(request.command);
            if (handler == handlers.end() || !handler->second.undoable)
            {
                return "'" + request.command + "' is not a registered undoable command";
            }
            const CommandHistory* history = historyOf(world);
            if (!history)
            {
                return request.command + ": world '" + world + "' is gone";
            }
            const std::string command = request.command;
            const std::vector<u64> previous = request.created;
            const nlohmann::json args = forwardedArgs(world, request, *history);
            JsonFragmentWriter ignored;
            struct ReplayScope
            {
                u32& depth;
                explicit ReplayScope(u32& counter) : depth(counter) { ++depth; }
                ~ReplayScope() { --depth; }
            } scope(replaying);
            if (std::string error = runHandler(command, handler->second, args, ignored, into); !error.empty())
            {
                return command + ": " + error;
            }
            CommandHistory* after = historyOf(world);
            if (!after)
            {
                return command + ": world '" + world + "' is gone";
            }
            // the chain's current id, so ids an earlier undo or redo produced
            // forward too
            for (sizet key = 0; key < std::min(previous.size(), into.created.size()); ++key)
            {
                if (previous[key] != 0 && into.created[key] != 0)
                {
                    after->alias(after->resolve(previous[key]), into.created[key]);
                }
            }
            return {};
        }

        // Re-runs a request of history step position, record r: inverse i
        // (undo) or the forward request (redo), and stores what the run
        // recorded into the step, looked up anew.
        std::string replay(const std::string& world, const sizet position, const sizet r, const std::optional<sizet> i,
                           std::vector<std::string>& warnings)
        {
            Capture into;
            {
                const CommandRecord& record = historyOf(world)->step(position).records[r];
                if (std::string error = rerun(world, i ? record.inverse[*i] : record.forward, into); !error.empty())
                {
                    return error;
                }
            }
            CommandHistory& history = *historyOf(world);
            if (position >= history.steps().size() || r >= history.step(position).records.size())
            {
                return "the undo history changed while it ran";
            }
            CommandRecord& stored = history.step(position).records[r];
            warnings.insert(warnings.end(), std::make_move_iterator(into.warnings.begin()),
                            std::make_move_iterator(into.warnings.end()));
            if (i)
            {
                stored.inverse[*i].created = std::move(into.created);
            }
            else
            {
                stored.forward.created = std::move(into.created);
                stored.inverse = std::move(into.inverse);
                if (into.forward)
                {
                    (*into.forward)["world"] = world;
                    stored.forward.args = std::move(*into.forward);
                }
            }
            return {};
        }

        static nlohmann::json describeStep(const UndoStep& step)
        {
            nlohmann::json commands = nlohmann::json::array();
            for (const CommandRecord& record : step.records)
            {
                commands.push_back({{"command", record.forward.command}, {"args", record.forward.args}});
            }
            return {{"step", step.index}, {"commands", std::move(commands)}};
        }

        // report: "steps", the steps moved so far; a failure adds "failedStep", "historyKept" (false when the world,
        // and its history with it, is gone), "dropped" (the failed step and the steps redo could no longer reach)
        // and "undoable" (the steps still undoable). The step that failed part way is dropped, the rest of the
        // history stays, and its scene tokens are renewed so the scene counts as changed.
        std::string failed(const std::string_view action, const u64 index, const sizet position,
                           const std::string& error, const std::string& world, nlohmann::json& report,
                           nlohmann::json failedStep)
        {
            report["failedStep"] = std::move(failedStep);
            CommandHistory* history = historyOf(world);
            report["historyKept"] = history != nullptr;
            const std::string failure =
                std::string(action) + " of step " + std::to_string(index) + " failed at " + error;
            if (!history)
            {
                return failure + "; world '" + world + "' is gone, and its undo history with it";
            }
            const sizet before = history->steps().size();
            history->abandonFrom(position);
            report["dropped"] = before - history->steps().size();
            report["undoable"] = history->done();
            return failure + "; the step was dropped from the undo history of world '" + world + "' (" +
                std::to_string(history->done()) + " step(s) stay undoable) and the world may differ from it";
        }

        std::string undo(const std::string& world, const u64 steps, IFormatWriter& out, nlohmann::json& report)
        {
            CommandHistory* history = historyOf(world);
            if (history->done() == 0)
            {
                return "nothing to undo in world '" + world + "'";
            }
            std::vector<std::string> warnings;
            u32 count = 0;
            report["steps"] = nlohmann::json::array();
            for (; count < steps && history->done() > 0; ++count)
            {
                const sizet position = history->done() - 1;
                const u64 index = history->step(position).index;
                nlohmann::json described = describeStep(history->step(position));
                for (sizet r = history->step(position).records.size(); r-- > 0;)
                {
                    for (sizet i = 0; i < history->step(position).records[r].inverse.size(); ++i)
                    {
                        if (std::string error = replay(world, position, r, i, warnings); !error.empty())
                        {
                            return failed("undo", index, position, error, world, report, std::move(described));
                        }
                        history = historyOf(world);
                    }
                }
                history->remeasure(position);
                history->setDone(position);
                if (batchStatus.world == world && batchStatus.step == index)
                {
                    const u64 revision = batchStatus.revision + 1;
                    batchStatus = {};
                    batchStatus.revision = revision;
                }
                report["steps"].push_back(std::move(described));
            }
            writeUndoState(*history, "undone", count, warnings, out);
            return {};
        }

        std::string redo(const std::string& world, const u64 steps, IFormatWriter& out, nlohmann::json& report)
        {
            CommandHistory* history = historyOf(world);
            if (history->done() == history->steps().size())
            {
                return "nothing to redo in world '" + world + "'";
            }
            std::vector<std::string> warnings;
            u32 count = 0;
            report["steps"] = nlohmann::json::array();
            for (; count < steps && history->done() < history->steps().size(); ++count)
            {
                const sizet position = history->done();
                const u64 index = history->step(position).index;
                nlohmann::json described = describeStep(history->step(position));
                for (sizet r = 0; r < history->step(position).records.size(); ++r)
                {
                    if (std::string error = replay(world, position, r, std::nullopt, warnings); !error.empty())
                    {
                        return failed("redo", index, position, error, world, report, std::move(described));
                    }
                    history = historyOf(world);
                }
                history->remeasure(position);
                history->setDone(position + 1);
                report["steps"].push_back(std::move(described));
            }
            writeUndoState(*history, "redone", count, warnings, out);
            return {};
        }

        // undo or redo `steps` steps of the world as one request (defined with the reply helpers below)
        std::string walk(const std::string& world, bool redoing, u64 steps, IFormatWriter& out, u64* walked = nullptr);

        static void writeUndoState(const CommandHistory& history, const std::string_view key, const u32 count,
                                   const std::vector<std::string>& warnings, IFormatWriter& out)
        {
            out.writeU32(key, count);
            out.writeU32("undoable", static_cast<u32>(history.done()));
            out.writeU32("redoable", static_cast<u32>(history.steps().size() - history.done()));
            out.beginArray("warnings", static_cast<u32>(warnings.size()));
            for (const std::string& warning : warnings)
            {
                out.writeString("", warning);
            }
            out.endArray();
        }

        // One undo step for every command of the batch, all undoable ones; a
        // failing command rolls back the ones before it. revives[i], when given: the entities command i brings
        // back (what its stored request named), which the entities it creates take over from.
        std::string batch(const ControlArgs& args, IFormatWriter& out,
                          const std::vector<std::vector<u64>>* revives = nullptr)
        {
            batchStatus.client = running.client;
            batchStatus.step = 0;
            batchStatus.source = running.source;
            batchStatus.name = args.string("name").value_or("batch");
            batchStatus.state = "active";
            ++batchStatus.revision;
            struct BatchStatusScope
            {
                BatchStatus& status;
                ~BatchStatusScope()
                {
                    if (status.state == "active")
                        status.state = "refused";
                    ++status.revision;
                }
            } statusScope{batchStatus};
            const WorldEntry* entry = findEntry(args);
            if (!entry)
            {
                return unknownWorld(args);
            }
            const std::string world = entry->name;
            batchStatus.world = world;
            // the ids the commands bring back, as a logged revert writes them (see revertStep): a replay of it
            // aliases the recreations as the revert did
            std::vector<std::vector<u64>> replayed;
            if (const std::optional<std::string> text = args.json("revives"))
            {
                const nlohmann::json list = nlohmann::json::parse(*text, nullptr, false);
                if (!list.is_array())
                {
                    return "batch revives is an array of arrays of entity ids";
                }
                for (const nlohmann::json& ids : list)
                {
                    if (!ids.is_array() ||
                        !std::ranges::all_of(ids, [](const nlohmann::json& id) { return id.is_number_unsigned(); }))
                    {
                        return "batch revives is an array of arrays of entity ids";
                    }
                    replayed.push_back(ids.get<std::vector<u64>>());
                }
                revives = &replayed;
            }
            // a revived id is dead in a real revert and in its replay: a live one would redirect every stored
            // request naming it to the recreation
            if (revives)
            {
                for (const std::vector<u64>& ids : *revives)
                {
                    for (const u64 id : ids)
                    {
                        const u64 live = entry->history->resolve(id);
                        if (id != 0 && entry->world.isEntityAlive(Entity(live)))
                        {
                            return "batch revives names entity " + std::to_string(id) + ", which is alive (as " +
                                std::to_string(live) + "): revives lists entities the commands bring back, dead ones";
                        }
                    }
                }
            }
            const std::optional<std::string> text = args.json("commands");
            nlohmann::json commands = text ? nlohmann::json::parse(*text, nullptr, false) : nlohmann::json();
            if (!commands.is_array() || commands.empty())
            {
                return "batch needs commands: a non-empty array of {command, args}";
            }
            const auto placement = [](const nlohmann::json& commandArgs) -> std::string
            {
                const auto name = commandArgs.find("name");
                return name != commandArgs.end() && name->is_string() ? " '" + name->get<std::string>() + "'" : "";
            };
            // refused up front: a command that is not undoable may change the
            // worlds or the handlers (play, stop, reloadModule) and could not
            // be rolled back
            for (sizet index = 0; index < commands.size(); ++index)
            {
                nlohmann::json& request = commands[index];
                const auto name = request.is_object() ? request.find("command") : request.end();
                if (!request.is_object() || name == request.end() || !name->is_string())
                {
                    return "batch commands are objects {command, args}";
                }
                const std::string& command = name->get_ref<const std::string&>();
                const auto handler = handlers.find(command);
                if (handler == handlers.end() || !handler->second.command)
                {
                    return "batch: unknown command '" + command + "'";
                }
                if (!handler->second.undoable)
                {
                    return "batch: '" + command + "' is not undoable; a batch takes only undoable commands";
                }
                nlohmann::json& commandArgs = request["args"];
                if (commandArgs.is_null())
                {
                    commandArgs = nlohmann::json::object();
                }
                if (!commandArgs.is_object())
                {
                    return "batch: the args of '" + command + "' are not an object";
                }
                if (const auto named = commandArgs.find("world"); named != commandArgs.end() && *named != world)
                {
                    return "batch: every command acts on the batch's world '" + world + "'";
                }
                commandArgs["world"] = world;
                if (handler->second.described)
                {
                    if (nlohmann::json problems = checkArgs(handler->second.args, commandArgs); !problems.empty())
                    {
                        for (nlohmann::json& problem : problems)
                        {
                            problem["command"] = index;
                        }
                        argErrors = problems;
                        return "batch command " + std::to_string(index) + placement(commandArgs) + ": " +
                            argsMessage(command, problems);
                    }
                }
                if (handler->second.preflight)
                {
                    if (const std::string error = handler->second.preflight(ControlArgs(&commandArgs)); !error.empty())
                        return "batch command " + std::to_string(index) + placement(commandArgs) + " (" + command +
                            ") preflight failed: " + error;
                }
            }

            // the inner commands answer through the batch's reply: none may defer (it would take the batch's sink)
            struct SinkScope
            {
                const ReplyFn*& sink;
                const ReplyFn* outer;
                explicit SinkScope(const ReplyFn*& slot) : sink(slot), outer(std::exchange(slot, nullptr)) {}
                ~SinkScope() { sink = outer; }
            } sinkScope(runningSend);

            UndoStep step;
            step.view = true;
            struct BatchScope
            {
                State& state;
                BatchScope(State& owner, const UndoStep& running, const std::string& world) : state(owner)
                {
                    state.batchStep = &running;
                    state.batchWorld = world;
                }
                ~BatchScope() { state.batchStep = nullptr; }
            } batchScope(*this, step, world);
            out.beginArray("results", static_cast<u32>(commands.size()));
            for (sizet i = 0; i < commands.size(); ++i)
            {
                const std::string& command = commands[i]["command"].get_ref<const std::string&>();
                const nlohmann::json& commandArgs = commands[i]["args"];
                Capture captured;
                out.beginObject("");
                const auto handler = handlers.find(command);
                // a revert's commands name entities the commands before them bring back: forwarded when each runs
                nlohmann::json forwarded;
                if (revives && handler != handlers.end())
                {
                    if (const CommandHistory* now = historyOf(world))
                    {
                        forwarded = forwardedArgs(world, {command, commandArgs, {}}, *now);
                    }
                }
                std::string error = handler == handlers.end() || !handler->second.undoable
                    ? "no longer registered"
                    : runHandler(command, handler->second, forwarded.is_null() ? commandArgs : forwarded, out,
                                 captured);
                if (error.empty() && !historyOf(world))
                {
                    error = "world '" + world + "' is gone";
                }
                if (!error.empty())
                {
                    // a command that threw may have acted: what it recorded is rolled back with the rest, and when it
                    // recorded nothing the world's states are renewed
                    const bool unrecorded = captured.threw && captured.inverse.empty();
                    if (unrecorded)
                    {
                        error += MAY_HAVE_ACTED;
                    }
                    else if (captured.threw)
                    {
                        step.records.push_back({{command, commandArgs, {}}, std::move(captured.inverse)});
                    }
                    for (auto record = step.records.rbegin(); record != step.records.rend(); ++record)
                    {
                        for (const CommandRequest& inverse : record->inverse)
                        {
                            Capture ignored;
                            if (std::string undoError = rerun(world, inverse, ignored); !undoError.empty())
                            {
                                batchStatus.state = "rollbackFailed";
                                // the batch left no step: the history stays, its tokens renewed so the scene counts as
                                // changed
                                if (CommandHistory* history = historyOf(world))
                                {
                                    history->renewStates();
                                }
                                return "batch command " + std::to_string(i) + placement(commandArgs) + " (" + command +
                                    ") failed: " + error + "; rolling back failed at " + undoError +
                                    "; the world may differ from the undo history of '" + world + "', which was kept";
                            }
                        }
                    }
                    if (CommandHistory* history = unrecorded ? historyOf(world) : nullptr)
                    {
                        history->renewStates();
                    }
                    batchStatus.state = unrecorded ? "rollbackFailed" : "rolledBack";
                    return "batch command " + std::to_string(i) + placement(commandArgs) + " (" + command +
                        ") failed: " + error + "; the commands before it were rolled back";
                }
                out.endObject();
                if (!forwarded.is_null())
                {
                    commands[i]["args"] = std::move(forwarded);
                }
                if (revives && i < revives->size())
                {
                    CommandHistory& live = *historyOf(world);
                    for (sizet key = 0; key < std::min((*revives)[i].size(), captured.created.size()); ++key)
                    {
                        if ((*revives)[i][key] != 0 && captured.created[key] != 0)
                        {
                            live.alias(live.resolve((*revives)[i][key]), captured.created[key]);
                        }
                    }
                }
                if (captured.forward)
                {
                    (*captured.forward)["world"] = world;
                    commands[i]["args"] = std::move(*captured.forward);
                }
                if (!captured.inverse.empty())
                {
                    step.view = step.view && handler->second.view;
                    CommandRecord record{{command, commands[i]["args"], std::move(captured.created)},
                                         std::move(captured.inverse)};
                    if (const auto entry = std::ranges::find(worlds, world, &WorldEntry::name);
                        !handler->second.view && entry != worlds.end())
                    {
                        noteLayers(step, *entry, captured, record);
                    }
                    step.records.push_back(std::move(record));
                }
            }
            out.endArray();
            CommandHistory& history = *historyOf(world);
            nlohmann::json logged{{"world", world}, {"commands", std::move(commands)}};
            if (const auto label = args.string("name"); label && !label->empty())
            {
                step.label = *label;
                logged["name"] = *label;
            }
            if (revives)
            {
                logged["revives"] = *revives;
            }
            if (step.records.empty())
            {
                batchStatus.state = "completed";
                history.log("batch", logged, running, elapsedUs());
                return {};
            }
            if (std::optional<std::string> merge = args.string("merge"))
            {
                logged["merge"] = *merge;
                step.merge = std::move(*merge);
            }
            step.by = running;
            batchStatus.state = "completed";
            history.record(std::move(step), "batch", logged, elapsedUs());
            if (!history.steps().empty())
                batchStatus.step = history.steps().back().index;
            if (const auto entry = std::ranges::find(worlds, world, &WorldEntry::name); entry != worlds.end())
            {
                sweepProvenance(*entry);
            }
            return {};
        }

        // revertStep: the inverse of a done scene-edit step, as a batch of its stored inverse requests with
        // their entity ids forwarded: one new undoable step carrying the caller's attribution, logged as that batch
        // (a replay of the log needs no step index). Refused, with nothing run, for an unknown, view or undone step
        // and when a later step touched what the step did.
        std::string revertStep(const ControlArgs& args, IFormatWriter& out)
        {
            const WorldEntry* entry = findEntry(args);
            if (!entry)
            {
                return unknownWorld(args);
            }
            const std::string world = entry->name;
            const u64 index = args.integer("index").value_or(0);
            const std::string named = "step " + std::to_string(index);
            CommandHistory& history = *entry->history;
            if (index == 0)
            {
                return "step 0 is no step: a provenance row with step 0 has none left to revert (it left the undo "
                       "history)";
            }
            const std::optional<sizet> position = history.find(index);
            if (!position)
            {
                return named + " is not in the undo history of world '" + world + "': it left it or never existed";
            }
            const UndoStep& target = history.steps()[*position];
            const u64 targetState = target.state;
            if (target.view)
            {
                return named + " is a view step: it changed no scene state, so there is nothing to revert";
            }
            if (*position >= history.done())
            {
                return named + " is undone: redo it first, or leave it";
            }
            const auto parentOf = [this, &world = entry->world](const u64 id) -> u64
            {
                const Entity entity(id);
                if (!hierarchy || !world.isEntityAlive(entity))
                {
                    return 0;
                }
                const Entity parent = hierarchy(world, entity).parent;
                return parent.isValid() ? parent.id() : 0;
            };
            if (const std::optional<RevertConflict> conflict = history.revertConflict(*position, parentOf))
            {
                // a refusal changed nothing: its details ride the error without counting as a state change
                refusal = {
                    {"conflictingStep", conflict->step}, {"entity", conflict->entity}, {"field", conflict->field}};
                return named + " cannot be reverted: later step " + std::to_string(conflict->step) + " touched " +
                    conflict->field + " of entity " + std::to_string(conflict->entity);
            }

            // undo's order: the step's records last to first, each one's inverse requests in order
            nlohmann::json commands = nlohmann::json::array();
            std::vector<std::vector<u64>> revives;
            for (sizet r = target.records.size(); r-- > 0;)
            {
                for (const CommandRequest& inverse : target.records[r].inverse)
                {
                    commands.push_back(
                        {{"command", inverse.command}, {"args", forwardedArgs(world, inverse, history)}});
                    revives.push_back(inverse.created);
                }
            }
            auto restored = history.touchedFields(target);
            const u64 newest = history.steps().empty() ? 0 : history.steps().back().index;

            const nlohmann::json batchArgs{{"world", world}, {"commands", std::move(commands)}};
            JsonFragmentWriter ignored;
            if (std::string error = batch(ControlArgs(&batchArgs), ignored, &revives); !error.empty())
            {
                // commands ran and were rolled back (batch wrote their results): the scene counts as changed, as for
                // a plain batch that fails part way
                std::string written;
                ignored.dumpReply(written);
                if (const nlohmann::json ran = nlohmann::json::parse(written, nullptr, false);
                    ran.is_object() && !ran.empty())
                {
                    ++commandGeneration;
                    ++stateGeneration;
                }
                return "revert of " + named + " failed: " + error;
            }
            CommandHistory* live = historyOf(world);
            if (!live || live->steps().empty() || live->steps().back().index == newest)
            {
                return "revert of " + named + " left no step";
            }
            const UndoStep& made = live->steps().back();
            // the revert is a review of what it undid: the fields of the step and of its own edits
            if (const std::optional<sizet> was = live->find(index))
            {
                live->markReviewed(live->steps()[*was]);
            }
            live->markReviewed(made);
            if (const WorldEntry* current = findEntry(args);
                current && current->timeline.step == index && current->timelineStepState == targetState)
            {
                current->timelineEntityStep = made.index;
                current->timelineStepState = made.state;
                noteAgentReview(*current, "revertStep");
            }
            if (const WorldEntry* current = findEntry(args))
            {
                for (const AgentActivity& row : activities)
                    if (row.worldSerial == current->serial && row.step == index && row.stepState == targetState)
                        updateActivity(row.id, "review", "reverted", "revertStep", row.evidence, row.reported);
                updateActivityReviews(*current);
            }

            out.writeString("world", world);
            out.writeU64("reverted", index);
            out.writeU64("step", made.index);
            std::vector<std::pair<u64, std::vector<std::string>>> rows;
            for (auto& [id, fields] : restored)
            {
                rows.emplace_back(live->resolve(id), std::move(fields));
            }
            std::ranges::sort(rows, {}, &std::pair<u64, std::vector<std::string>>::first);
            out.beginArray("restored", static_cast<u32>(rows.size()));
            for (const auto& [id, fields] : rows)
            {
                out.beginObject("");
                out.writeU64("id", id);
                out.beginArray("fields", static_cast<u32>(fields.size()));
                for (const std::string& field : fields)
                {
                    out.writeString("", field);
                }
                out.endArray();
                out.endObject();
            }
            out.endArray();
            return {};
        }

#if defined(_WIN32) || !defined(SPITE_SHIPPING)
        // Appends bytes to partial and queues every complete non-empty line.
        void receive(std::string& partial, const char* bytes, const sizet count, const u32 source)
        {
            partial.append(bytes, count);
            std::lock_guard lock(inboxMutex);
            for (sizet newline = partial.find('\n'); newline != std::string::npos; newline = partial.find('\n'))
            {
                std::string line = partial.substr(0, newline);
                partial.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                if (!line.empty())
                {
                    if (source == STDIO)
                        pendingStdioRequests.fetch_add(1, std::memory_order_release);
                    inbox.emplace_back(source, std::move(line));
                    hasInput.store(true, std::memory_order_release);
#if defined(_WIN32)
                    if (receiveQueuedHook)
                        receiveQueuedHook();
#endif
                }
            }
        }

        void readStdin()
        {
            std::string partial;
            char buffer[4096];
#if defined(_WIN32)
            const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
            DWORD read = 0;
            while (!stopping.load() && ReadFile(input, buffer, sizeof(buffer), &read, nullptr) && read > 0)
            {
                receive(partial, buffer, read, STDIO);
            }
#else
            pollfd fds[] = {{STDIN_FILENO, POLLIN, 0}, {stdioCancel[0], POLLIN, 0}};
            for (;;)
            {
                const int ready = ::poll(fds, std::size(fds), -1);
                if (ready < 0 && errno == EINTR)
                {
                    continue;
                }
                if (ready < 0 || fds[1].revents != 0)
                {
                    break;
                }
                const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
                if (count > 0)
                {
                    receive(partial, buffer, static_cast<sizet>(count), STDIO);
                }
                else if (count == 0 || (errno != EINTR && errno != EAGAIN))
                {
                    break;
                }
            }
#endif
            inputClosed.store(true, std::memory_order_release);
            attachedEpoch.fetch_add(1, std::memory_order_release);
        }

        static void writeStdio(const std::string& data)
        {
#if defined(_WIN32)
            DWORD written = 0;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
#else
            // A client closing stdout must not terminate the engine or change other threads' signal handling.
            sigset_t sigpipe, previous, pending;
            sigemptyset(&sigpipe);
            sigaddset(&sigpipe, SIGPIPE);
            if (pthread_sigmask(SIG_BLOCK, &sigpipe, &previous) != 0)
            {
                return;
            }
            const bool alreadyPending = sigpending(&pending) != 0 || sigismember(&pending, SIGPIPE) == 1;
            for (sizet offset = 0; offset < data.size();)
            {
                const auto count = ::write(STDOUT_FILENO, data.data() + offset, data.size() - offset);
                if (count > 0)
                {
                    offset += static_cast<sizet>(count);
                }
                else if (count == 0 || errno != EINTR)
                {
                    if (count < 0 && errno == EPIPE && !alreadyPending && sigpending(&pending) == 0 &&
                        sigismember(&pending, SIGPIPE) == 1)
                    {
                        int signal;
                        sigwait(&sigpipe, &signal);
                    }
                    break;
                }
            }
            pthread_sigmask(SIG_SETMASK, &previous, nullptr);
#endif
        }
#endif

#if defined(_WIN32)
        // Waits for an overlapped operation on instance; false when stopping
        // (the operation is cancelled) or when it failed.
        bool await(const HANDLE instance, OVERLAPPED& overlapped, DWORD& outBytes) const
        {
            const HANDLE events[] = {overlapped.hEvent, stopEvent};
            if (WaitForMultipleObjects(2, events, FALSE, INFINITE) != WAIT_OBJECT_0)
            {
                CancelIoEx(instance, &overlapped);
                GetOverlappedResult(instance, &overlapped, &outBytes, TRUE);
                return false;
            }
            return GetOverlappedResult(instance, &overlapped, &outBytes, FALSE) != FALSE;
        }

        void servePipe(const std::wstring name)
        {
            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            u32 generation = 0;
            // between two clients no instance existed for a moment, and a `spite` process landing then
            // read the missing pipe (error 2) as no engine listening. A keeper instance, connected to a dummy client
            // of our own, keeps the name alive: an outsider sees the pipe busy and waits it out, and only a process
            // that is really gone leaves the name unserved. The second slot is the serving instance, so two engines
            // of one checkout still exclude each other.
            const auto createInstance = [&name]
            {
                return CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 2,
                                        65536, 65536, 0, nullptr);
            };
            HANDLE keeper = createInstance();
            HANDLE keeperClient = INVALID_HANDLE_VALUE;
            bool keeperLost = false;
            if (keeper != INVALID_HANDLE_VALUE)
            {
                if (pipeKeeperHook)
                {
                    pipeKeeperHook();
                }
                keeperClient =
                    CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (keeperClient == INVALID_HANDLE_VALUE)
                {
                    // a real client reached the keeper while it still listened (or another engine took the slot): it
                    // would sit on an instance nobody reads. Closing the keeper breaks its pipe instead of hanging it.
                    SERROR_LOG("control: cannot hold the pipe name (error {}); attach is unavailable", GetLastError());
                    CloseHandle(keeper);
                    keeper = INVALID_HANDLE_VALUE;
                    keeperLost = true;
                }
            }
            while (!keeperLost && WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0)
            {
                // a failed keeper fails here the same way and takes the error path below
                const HANDLE instance = createInstance();
                if (instance == INVALID_HANDLE_VALUE)
                {
                    const DWORD error = GetLastError();
                    // another engine of this checkout serves it (shared engine runs, scripts/RunEngine.ps1)
                    if (error == ERROR_ACCESS_DENIED || error == ERROR_PIPE_BUSY)
                    {
                        SWARNING_LOG("control: another engine serves the pipe; attach to this one is unavailable");
                    }
                    else
                    {
                        SERROR_LOG("control: cannot create pipe (error {}); attach is unavailable", error);
                    }
                    break;
                }

                ResetEvent(overlapped.hEvent);
                DWORD bytes = 0;
                bool connected = ConnectNamedPipe(instance, &overlapped) != FALSE;
                if (!connected)
                {
                    const DWORD error = GetLastError();
                    connected = error == ERROR_PIPE_CONNECTED ||
                        (error == ERROR_IO_PENDING && await(instance, overlapped, bytes));
                }
                if (connected)
                {
                    {
                        std::lock_guard lock(pipeMutex);
                        pipe = instance;
                        pipeConnection = ++generation;
                        attachedEpoch.fetch_add(1, std::memory_order_release);
                    }
                    std::string partial;
                    char buffer[4096];
                    while (true)
                    {
                        ResetEvent(overlapped.hEvent);
                        if (!ReadFile(instance, buffer, sizeof(buffer), nullptr, &overlapped) &&
                            GetLastError() != ERROR_IO_PENDING)
                        {
                            break; // client gone
                        }
                        if (!await(instance, overlapped, bytes))
                        {
                            break; // client gone, or stopping
                        }
                        receive(partial, buffer, bytes, generation);
                    }
                    std::lock_guard lock(pipeMutex);
                    pipe = INVALID_HANDLE_VALUE;
                    pipeConnection = 0;
                    attachedEpoch.fetch_add(1, std::memory_order_release);
                    DisconnectNamedPipe(instance);
                }
                CloseHandle(instance);
                if (pipeGapHook)
                {
                    pipeGapHook();
                }
            }
            if (keeper != INVALID_HANDLE_VALUE)
            {
                CloseHandle(keeperClient);
                CloseHandle(keeper);
            }
            CloseHandle(overlapped.hEvent);
        }

        // Replies to a pipe client; dropped when that connection is gone. A
        // client that stops reading costs at most a second of frame time.
        void writePipe(const u32 connection, const std::string& data)
        {
            std::lock_guard writeLock(writeMutex);
            HANDLE instance = INVALID_HANDLE_VALUE;
            {
                std::lock_guard lock(pipeMutex);
                if (connection != pipeConnection || pipe == INVALID_HANDLE_VALUE ||
                    !DuplicateHandle(GetCurrentProcess(), pipe, GetCurrentProcess(), &instance, 0, FALSE,
                                     DUPLICATE_SAME_ACCESS))
                    return;
            }
            OVERLAPPED overlapped{};
            overlapped.hEvent = writeEvent;
            ResetEvent(writeEvent);
            DWORD written = 0;
            const bool immediate =
                WriteFile(instance, data.data(), static_cast<DWORD>(data.size()), nullptr, &overlapped) != FALSE;
            const bool pending = !immediate && GetLastError() == ERROR_IO_PENDING;
            if (pending && pipeWritePendingHook)
                pipeWritePendingHook();
            if (immediate || pending)
            {
                if (WaitForSingleObject(writeEvent, 1000) != WAIT_OBJECT_0)
                    CancelIoEx(instance, &overlapped);
                GetOverlappedResult(instance, &overlapped, &written, TRUE);
            }
            CloseHandle(instance);
        }
#endif
    };

    namespace
    {
        constexpr std::string_view OWNER = "control";
        constexpr u64 MAX_FIXED_STEPS_PER_STEP = 1000;
        constexpr u64 MAX_FRAMES_PER_STEP = 1000;
        constexpr sizet SUMMARY_BYTES = CommandHistory::SUMMARY_BYTES;

        void writeSimulation(const SimulationState& simulation, IFormatWriter& out)
        {
            out.writeBool("paused", simulation.paused);
            out.writeU32("pendingSteps", simulation.pendingSteps);
            out.writeU32("pendingFixedSteps", simulation.pendingFixedSteps);
            out.writeU64("simulatedFrames", simulation.simulatedFrames);
            out.writeU64("fixedSteps", simulation.fixedSteps);
        }

        // tableRows' filter: whether a field of a row, as the schema walk emits it, holds the needle (ignoring case).
        // A field's text is what a viewer shows: strings as they are, floats {:.6g}, float vectors "(a, b)", integers
        // and bools plain; a nested value matches on its leaves. No JSON tree is built per row.
        class RowFilter final : public IFormatWriter
        {
        public:
            RowFilter(const std::string_view needle, const std::string_view column) : m_needle(needle), m_column(column)
            {
            }

            void begin()
            {
                m_hit = false;
                m_depth = 0;
            }

            [[nodiscard]] bool hit() const { return m_hit; }

            void beginObject(const std::string_view name) override { open(name); }
            void endObject() override { --m_depth; }
            void beginArray(const std::string_view name, u32) override { open(name); }
            void endArray() override { --m_depth; }
            void writeBool(const std::string_view name, const bool v) override { leaf(name, v ? "true" : "false"); }
            void writeU8(const std::string_view name, const u8 v) override { integer(name, v); }
            void writeU16(const std::string_view name, const u16 v) override { integer(name, v); }
            void writeU32(const std::string_view name, const u32 v) override { integer(name, v); }
            void writeU64(const std::string_view name, const u64 v) override { integer(name, v); }
            void writeI32(const std::string_view name, const i32 v) override { integer(name, v); }
            void writeF32(const std::string_view name, const f32 v) override { number(name, v); }
            void writeF64(const std::string_view name, const double v) override { number(name, v); }
            void writeFloats(const std::string_view name, const std::span<const f32> v) override
            {
                if (!wanted(name))
                {
                    return;
                }
                m_text = "(";
                for (const f32 component : v)
                {
                    std::format_to(std::back_inserter(m_text), "{}{:.6g}", m_text.size() > 1 ? ", " : "",
                                   static_cast<double>(component));
                }
                m_text += ')';
                test(m_text);
            }
            void writeString(const std::string_view name, const std::string_view v) override { leaf(name, v); }
            void writeEntity(const std::string_view name, const Entity v) override { integer(name, v.id()); }
            void writeNull(const std::string_view name) override { leaf(name, "null"); }

        private:
            std::string_view m_needle;
            std::string_view m_column;
            std::string m_text;
            u32 m_depth = 0;
            bool m_active = false; // the top-level field being walked is one the filter searches
            bool m_hit = false;

            void open(const std::string_view name)
            {
                if (m_depth++ == 0)
                {
                    m_active = m_column.empty() || m_column == name;
                }
            }

            [[nodiscard]] bool wanted(const std::string_view name)
            {
                if (m_depth == 0)
                {
                    m_active = m_column.empty() || m_column == name;
                }
                return m_active && !m_hit;
            }

            void test(const std::string_view text)
            {
                constexpr auto lower = [](const char c)
                { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
                m_hit = !std::ranges::search(text, m_needle, {}, lower, lower).empty();
            }

            void leaf(const std::string_view name, const std::string_view text)
            {
                if (wanted(name))
                {
                    test(text);
                }
            }

            void integer(const std::string_view name, const auto v)
            {
                if (wanted(name))
                {
                    std::array<char, 24> digits;
                    test(std::string_view(digits.data(),
                                          std::to_chars(digits.data(), digits.data() + digits.size(), v).ptr));
                }
            }

            void number(const std::string_view name, const double v)
            {
                if (wanted(name))
                {
                    std::array<char, 48> text;
                    test(std::string_view(text.data(), std::format_to_n(text.data(), text.size(), "{:.6g}", v).out));
                }
            }
        };

        constexpr std::string_view READONLY_HINT =
            "; to change it, removeComponent then addComponent with the new value";

        // Resolves field[.field|[i]]... in schema to the field and its byte
        // offset; an indexed field comes back as a scalar.
        std::string resolveFieldPath(const StructSchema* schema, std::string_view rest, FieldDesc& outField,
                                     u32& outOffset)
        {
            outOffset = 0;
            while (true)
            {
                const sizet next = rest.find('.');
                std::string_view segment = rest.substr(0, next);
                std::optional<u32> index;
                if (const sizet bracket = segment.find('['); bracket != std::string_view::npos)
                {
                    u32 value = 0;
                    const std::string_view digits = segment.substr(bracket + 1, segment.size() - bracket - 2);
                    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
                    if (segment.back() != ']' || error != std::errc{} || end != digits.data() + digits.size())
                    {
                        return "bad index in '" + std::string(segment) + "'";
                    }
                    index = value;
                    segment = segment.substr(0, bracket);
                }
                const auto field = std::ranges::find(schema->fields, segment, &FieldDesc::name);
                if (field == schema->fields.end())
                {
                    return "no field '" + std::string(segment) + "'";
                }
                if (field->flags & fieldFlags::READONLY)
                {
                    return "field '" + std::string(segment) + "' is read-only" + std::string(READONLY_HINT);
                }
                outField = *field;
                outOffset += field->offset;
                if (index)
                {
                    if (*index >= field->arrayCount)
                    {
                        return "index out of range in '" + std::string(segment) + "'";
                    }
                    outOffset += *index * field->elementSize;
                    outField.arrayCount = 0;
                }
                if (next == std::string_view::npos)
                {
                    return {};
                }
                if (outField.kind != FieldKind::Struct || outField.arrayCount != 0)
                {
                    return "'" + std::string(segment) + "' has no fields";
                }
                schema = outField.structSchema;
                rest = rest.substr(next + 1);
            }
        }

        // Resolves Component.field[.field|[i]]... to the component and the
        // field's byte offset in it.
        std::string resolvePath(const std::string_view path, ComponentID& outComponent, FieldDesc& outField,
                                u32& outOffset)
        {
            const sizet dot = path.find('.');
            if (dot == std::string_view::npos)
            {
                return "path needs Component.field";
            }
            const std::string componentName(path.substr(0, dot));
            outComponent = getComponentIdByName(componentName.c_str());
            if (outComponent == INVALID_COMPONENT_ID)
            {
                return "unknown component '" + componentName + "'";
            }
            const StructSchema* schema = getComponentMetadata(outComponent).schema;
            if (!schema)
            {
                return "component '" + componentName + "' has no schema";
            }
            return resolveFieldPath(schema, path.substr(dot + 1), outField, outOffset);
        }

        std::string rangeText(const double min, const double max)
        {
            if (std::isfinite(min) && std::isfinite(max))
            {
                return std::format("{} to {}", min, max);
            }
            return std::isfinite(min) ? std::format(">= {}", min) : std::format("<= {}", max);
        }

        // One number a declared range bounds: its value, and its exact bits (an integer's value, a float's pattern) so
        // a change is told apart and a u64 is compared without rounding.
        struct RangedNumber
        {
            double value = 0.0;
            u64 bits = 0;
        };

        // Where a ranged number sits below the field a request set: a field name, or an array index when name is
        // empty. The walk carries it on the stack and the path text is built from it only for a refusal, so a set of
        // a large array allocates no per-element string.
        struct PathStep
        {
            const PathStep* parent = nullptr;
            std::string_view name;
            u32 index = 0;
        };

        std::string pathText(const std::string& root, const PathStep* step)
        {
            std::vector<const PathStep*> chain;
            for (; step; step = step->parent)
            {
                chain.push_back(step);
            }
            std::string text = root;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            {
                text += (*it)->name.empty() ? std::format("[{}]", (*it)->index) : "." + std::string((*it)->name);
            }
            return text;
        }

        // Calls fn(field, number, step) for every number of the value at data that a declared range bounds: a
        // vector's lanes, an array's elements, a nested struct's own fields; always in the same order. A field of
        // plain numbers with no range is skipped whole, an array of them too.
        template <typename Fn>
        void forRangedNumbers(const FieldDesc& field, const std::byte* data, const PathStep* at, Fn&& fn)
        {
            if (field.kind != FieldKind::Struct && field.min == -std::numeric_limits<double>::infinity() &&
                field.max == std::numeric_limits<double>::infinity())
            {
                return;
            }
            if (field.arrayCount != 0)
            {
                FieldDesc element = field;
                element.arrayCount = 0;
                for (u32 i = 0; i < field.arrayCount; ++i)
                {
                    const PathStep step{at, {}, i};
                    forRangedNumbers(element, data + i * field.elementSize, &step, fn);
                }
                return;
            }
            if (field.kind == FieldKind::Struct)
            {
                for (const FieldDesc& inner : field.structSchema->fields)
                {
                    const PathStep step{at, inner.name, 0};
                    forRangedNumbers(inner, data + inner.offset, &step, fn);
                }
                return;
            }
            const u32 lanes = field.kind == FieldKind::Vec2 ? 2
                : field.kind == FieldKind::Vec3             ? 3
                : field.kind == FieldKind::Vec4             ? 4
                                                            : 1;
            for (u32 lane = 0; lane < lanes; ++lane)
            {
                RangedNumber number;
                switch (field.kind)
                {
                case FieldKind::U8:
                    number.bits = *reinterpret_cast<const u8*>(data);
                    break;
                case FieldKind::U16:
                    number.bits = *reinterpret_cast<const u16*>(data);
                    break;
                case FieldKind::U32:
                    number.bits = *reinterpret_cast<const u32*>(data);
                    break;
                case FieldKind::U64:
                    number.bits = *reinterpret_cast<const u64*>(data);
                    break;
                case FieldKind::I32:
                    number.value = *reinterpret_cast<const i32*>(data);
                    number.bits = static_cast<u64>(static_cast<std::int64_t>(number.value));
                    break;
                case FieldKind::F32:
                case FieldKind::Vec2:
                case FieldKind::Vec3:
                case FieldKind::Vec4:
                    {
                        const f32 lanes32 = reinterpret_cast<const f32*>(data)[lane];
                        number.value = lanes32;
                        number.bits = std::bit_cast<u32>(lanes32);
                    }
                    break;
                case FieldKind::F64:
                    number.value = *reinterpret_cast<const double*>(data);
                    number.bits = std::bit_cast<u64>(number.value);
                    break;
                default:
                    return; // a range means nothing to another kind
                }
                if (field.kind == FieldKind::U8 || field.kind == FieldKind::U16 || field.kind == FieldKind::U32 ||
                    field.kind == FieldKind::U64)
                {
                    number.value = static_cast<double>(number.bits);
                }
                fn(field, number, at);
            }
        }

        // A range bound as the f32 an f32 field is held to. A bound beyond f32's reach is open (+-infinity): converting
        // such a double is undefined, so it is never cast.
        f32 boundAsF32(const double bound)
        {
            constexpr double REACH = std::numeric_limits<f32>::max();
            if (bound > REACH)
            {
                return std::numeric_limits<f32>::infinity();
            }
            return bound < -REACH ? -std::numeric_limits<f32>::infinity() : static_cast<f32>(bound);
        }

        // Whether a number is inside its field's range. An f32 is held to the bounds rounded to f32, so the bound
        // itself (0.1 stored as 0.100000001) passes; a u64 is compared as an integer.
        bool insideRange(const FieldDesc& field, const RangedNumber& number)
        {
            switch (field.kind)
            {
            case FieldKind::U64:
                {
                    constexpr double SPAN = 18446744073709551616.0; // 2^64: every u64 is below it
                    const bool aboveMin =
                        field.min <= 0.0 || (field.min < SPAN && number.bits >= static_cast<u64>(std::ceil(field.min)));
                    const bool belowMax = field.max >= SPAN ||
                        (field.max >= 0.0 && number.bits <= static_cast<u64>(std::floor(field.max)));
                    return aboveMin && belowMax;
                }
            case FieldKind::F32:
            case FieldKind::Vec2:
            case FieldKind::Vec3:
            case FieldKind::Vec4:
                return number.value >= static_cast<double>(boundAsF32(field.min)) &&
                    number.value <= static_cast<double>(boundAsF32(field.max));
            default:
                return number.value >= field.min && number.value <= field.max;
            }
        }

        // set's refusal of a number outside its field's range: the number at its own precision, the range, the path.
        std::string rangeRefusal(const FieldDesc& field, const RangedNumber& number, const std::string& path)
        {
            const std::string shown = field.kind == FieldKind::U64 ? std::to_string(number.bits)
                : field.kind == FieldKind::F32 || field.kind == FieldKind::Vec2 || field.kind == FieldKind::Vec3 ||
                    field.kind == FieldKind::Vec4
                ? std::format("{}", static_cast<f32>(number.value))
                : std::format("{}", number.value);
            return std::format("{} is outside the range {} of {}", shown, rangeText(field.min, field.max), path);
        }
        // Writes valueJson into the field at current (read) / target (written);
        // the result and outPrevious carry the previous value. A refused value
        // leaves the field as it was. checkRange: a number outside a declared
        // range is refused (a client's set; a rerun from the history restores
        // whatever was stored).
        std::string writeField(FieldDesc field, const std::byte* current, std::byte* target,
                               const std::string_view path, const std::string_view valueJson, IFormatWriter& out,
                               nlohmann::json& outPrevious, const bool checkRange)
        {
            // the field alone, as a one-member struct rooted at its address
            field.offset = 0;
            field.flags = fieldFlags::PERSISTED;
            field.name = "value";
            const StructSchema valueSchema{"value", 0, std::span(&field, 1)};
            FieldDesc previousField = field;
            previousField.name = "previous";
            const StructSchema previousSchema{"previous", 0, std::span(&previousField, 1)};

            out.writeString("path", path);
            writeValue(previousSchema, current, out, FieldSet::All);
            JsonFragmentWriter previous;
            writeValue(valueSchema, current, previous, FieldSet::All);
            std::string previousText;
            previous.dump(previousText);

            // the ranged numbers as stored: only one the request changes is held to its range, so a stored value
            // outside it (a scene, code) survives an edit of another lane or field
            const std::string where(path);
            std::vector<u64> stored;
            if (checkRange)
            {
                forRangedNumbers(field, current, nullptr,
                                 [&stored](const FieldDesc&, const RangedNumber& number, const PathStep*)
                                 { stored.push_back(number.bits); });
            }
            std::string error;
            JsonFragmentReader reader(R"({"value":)" + std::string(valueJson) + "}");
            const bool read = readValue(valueSchema, target, reader, error);
            std::string refused;
            if (read && checkRange)
            {
                sizet index = 0;
                forRangedNumbers(field, target, nullptr,
                                 [&](const FieldDesc& ranged, const RangedNumber& number, const PathStep* at)
                                 {
                                     const bool kept = index < stored.size() && stored[index] == number.bits;
                                     ++index;
                                     if (refused.empty() && !kept && !insideRange(ranged, number))
                                     {
                                         refused = rangeRefusal(ranged, number, pathText(where, at));
                                     }
                                 });
            }
            if (!read || !refused.empty())
            {
                // a malformed value may have half-applied: put the old one back
                std::string ignored;
                JsonFragmentReader restore(previousText);
                readValue(valueSchema, target, restore, ignored);
                if (read)
                {
                    return refused;
                }
                if (error.find("expected an integer (named values:") != std::string::npos)
                {
                    return "bad value at " + where + ": " + error;
                }
                return "bad value: " + error;
            }
            outPrevious = nlohmann::json::parse(previousText)["value"];
            return {};
        }

        std::string enumLabelsToNumbers(const FieldDesc& field, nlohmann::json& value, const std::string& path,
                                        bool& changed);

        std::string enumLabelsInStruct(const StructSchema& schema, nlohmann::json& object, const std::string& path,
                                       bool& changed)
        {
            if (!object.is_object())
            {
                return {};
            }
            for (const FieldDesc& member : schema.fields)
            {
                const auto it = object.find(std::string(member.name));
                if (it == object.end())
                {
                    continue;
                }
                if (std::string error =
                        enumLabelsToNumbers(member, *it, path + "." + std::string(member.name), changed);
                    !error.empty())
                {
                    return error;
                }
            }
            return {};
        }

        // Generic set's enum input: an exact registered label of a scalar enum field, at any depth of the value,
        // becomes its number before the strict numeric codec reads it. An unknown label stays a string, so the codec
        // refuses it and lists the named values; an ambiguous one is refused here, before anything is written.
        std::string enumLabelsToNumbers(const FieldDesc& field, nlohmann::json& value, const std::string& path,
                                        bool& changed)
        {
            if (field.arrayCount != 0)
            {
                if (!value.is_array())
                {
                    return {};
                }
                FieldDesc element = field;
                element.arrayCount = 0;
                for (sizet i = 0; i < value.size(); ++i)
                {
                    if (std::string error =
                            enumLabelsToNumbers(element, value[i], std::format("{}[{}]", path, i), changed);
                        !error.empty())
                    {
                        return error;
                    }
                }
                return {};
            }
            switch (field.kind)
            {
            case FieldKind::Struct:
                return enumLabelsInStruct(*field.structSchema, value, path, changed);
            case FieldKind::StructList:
                if (value.is_array())
                {
                    for (sizet i = 0; i < value.size(); ++i)
                    {
                        if (std::string error = enumLabelsInStruct(*field.structSchema, value[i],
                                                                   std::format("{}[{}]", path, i), changed);
                            !error.empty())
                        {
                            return error;
                        }
                    }
                }
                return {};
            case FieldKind::U8:
            case FieldKind::U16:
            case FieldKind::U32:
            case FieldKind::I32:
                break;
            default:
                return {};
            }
            if (field.enumNames.empty() || !value.is_string())
            {
                return {};
            }
            const std::string& label = value.get_ref<const std::string&>();
            const EnumName* match = nullptr;
            for (const EnumName& choice : field.enumNames)
            {
                if (choice.name == label)
                {
                    if (match != nullptr)
                    {
                        return "ambiguous enum label '" + label + "' at " + path;
                    }
                    match = &choice;
                }
            }
            if (match != nullptr)
            {
                value = match->value;
                changed = true;
            }
            return {};
        }

        // Writes valueJson into one field through mutable by-id access, so the
        // component is marked modified; the result and outPrevious carry the
        // previous value.
        std::string setField(const World& world, const Entity entity, const std::string_view path,
                             const std::string& valueJson, IFormatWriter& out, nlohmann::json& outPrevious,
                             const ControlServer::ComponentEditGuardFn& guard, const bool checkRange)
        {
            ComponentID component = INVALID_COMPONENT_ID;
            FieldDesc field;
            u32 offset = 0;
            if (std::string error = resolvePath(path, component, field, offset); !error.empty())
            {
                // the remove + add hint is wrong where the component guard refuses it: name the guard's command
                if (guard && error.ends_with(READONLY_HINT))
                {
                    if (const std::string named = guard(world, entity, component, false); !named.empty())
                    {
                        error.resize(error.size() - READONLY_HINT.size());
                        error += "; " + named;
                    }
                }
                return error;
            }
            if (!world.hasComponentById(entity, component))
            {
                return "entity has no " + std::string(getComponentName(component));
            }
            const auto* current = static_cast<const std::byte*>(world.getConstComponentDataById(entity, component));
            // only a value holding a string can carry a label; any other is read as sent
            std::string converted;
            if (valueJson.find('"') != std::string::npos)
            {
                nlohmann::json value = nlohmann::json::parse(valueJson, nullptr, false);
                bool changed = false;
                if (std::string error = enumLabelsToNumbers(field, value, std::string(path), changed); !error.empty())
                {
                    return error;
                }
                if (changed)
                {
                    converted = value.dump();
                }
            }
            auto* target = static_cast<std::byte*>(world.getComponentDataById(entity, component));
            return writeField(field, current + offset, target + offset, path,
                              converted.empty() ? std::string_view(valueJson) : std::string_view(converted), out,
                              outPrevious, checkRange);
        }

        nlohmann::json summarizeArgs(const nlohmann::json& requestArgs, const bool full)
        {
            return CommandHistory::summarizeArgs(requestArgs, full);
        }

        // The args of every command in an undo/redo report's steps and failedStep, summarized.
        void summarizeStepArgs(nlohmann::json& report)
        {
            const auto summarize = [](nlohmann::json& step)
            {
                for (nlohmann::json& command : step["commands"])
                {
                    command["args"] = summarizeArgs(command["args"], false);
                }
            };
            for (nlohmann::json& step : report["steps"])
            {
                summarize(step);
            }
            if (const auto failedStep = report.find("failedStep"); failedStep != report.end())
            {
                summarize(*failedStep);
            }
        }

        // A JSON value through the format walk, so replies carry it as JSON.
        void writeJson(const std::string_view name, const nlohmann::json& value, IFormatWriter& out)
        {
            switch (value.type())
            {
            case nlohmann::json::value_t::object:
                out.beginObject(name);
                for (const auto& [key, member] : value.items())
                {
                    writeJson(key, member, out);
                }
                out.endObject();
                break;
            case nlohmann::json::value_t::array:
                out.beginArray(name, static_cast<u32>(value.size()));
                for (const nlohmann::json& element : value)
                {
                    writeJson("", element, out);
                }
                out.endArray();
                break;
            case nlohmann::json::value_t::string:
                out.writeString(name, value.get_ref<const std::string&>());
                break;
            case nlohmann::json::value_t::boolean:
                out.writeBool(name, value.get<bool>());
                break;
            case nlohmann::json::value_t::number_unsigned:
                out.writeU64(name, value.get<u64>());
                break;
            case nlohmann::json::value_t::number_integer:
                out.writeF64(name, static_cast<double>(value.get<std::int64_t>()));
                break;
            case nlohmann::json::value_t::number_float:
                out.writeF64(name, value.get<double>());
                break;
            default:
                out.writeNull(name);
                break;
            }
        }

        // A note per entity reference in field (its value at data) that names
        // no live entity (forwardValueRefs covers the ids undo and redo recreated).
        void findDeadReferences(const World& world, const FieldDesc& field, const std::byte* data,
                                const std::string& path, std::vector<std::string>& out)
        {
            const auto check = [&world, &out](const Entity entity, const std::string& at)
            {
                if (entity.isValid() && !world.isEntityAlive(entity))
                {
                    out.push_back(at + " names dead entity " + std::to_string(entity.id()));
                }
            };
            const u32 count = field.arrayCount == 0 ? 1 : field.arrayCount;
            for (u32 i = 0; i < count; ++i)
            {
                const std::byte* element = data + i * field.elementSize;
                const std::string at = field.arrayCount == 0 ? path : path + "[" + std::to_string(i) + "]";
                switch (field.kind)
                {
                case FieldKind::Entity:
                    check(*reinterpret_cast<const Entity*>(element), at);
                    break;
                case FieldKind::EntityList:
                    for (const Entity entity : *reinterpret_cast<const std::vector<Entity>*>(element))
                    {
                        check(entity, at);
                    }
                    break;
                case FieldKind::Struct:
                    for (const FieldDesc& inner : field.structSchema->fields)
                    {
                        findDeadReferences(world, inner, element + inner.offset, at + "." + std::string(inner.name),
                                           out);
                    }
                    break;
                default:
                    break;
                }
            }
        }

        // Rewrites the entity ids in value (a field's JSON) through the
        // history's alias chain, by the field's schema.
        void forwardEntityRefs(const FieldDesc& field, nlohmann::json& value, const CommandHistory& history)
        {
            if (field.arrayCount != 0)
            {
                FieldDesc element = field;
                element.arrayCount = 0;
                if (value.is_array())
                {
                    for (nlohmann::json& item : value)
                    {
                        forwardEntityRefs(element, item, history);
                    }
                }
                return;
            }
            const auto forwardId = [&history](nlohmann::json& id)
            {
                if (id.is_number_unsigned())
                {
                    id = history.resolve(id.get<u64>());
                }
            };
            switch (field.kind)
            {
            case FieldKind::Entity:
                forwardId(value);
                break;
            case FieldKind::EntityList:
                if (value.is_array())
                {
                    for (nlohmann::json& id : value)
                    {
                        forwardId(id);
                    }
                }
                break;
            case FieldKind::Struct:
                if (value.is_object())
                {
                    for (const FieldDesc& inner : field.structSchema->fields)
                    {
                        if (const auto it = value.find(std::string(inner.name)); it != value.end())
                        {
                            forwardEntityRefs(inner, *it, history);
                        }
                    }
                }
                break;
            default:
                break;
            }
        }

        void forwardValueRefs(const std::string_view command, nlohmann::json& args, const CommandHistory& history)
        {
            const auto value = args.find("value");
            if (value == args.end())
            {
                return;
            }
            if (command == "set")
            {
                ComponentID component = INVALID_COMPONENT_ID;
                FieldDesc field;
                u32 offset = 0;
                if (const auto path = args.find("path"); path != args.end() && path->is_string() &&
                    resolvePath(path->get<std::string>(), component, field, offset).empty())
                {
                    forwardEntityRefs(field, *value, history);
                }
            }
            else if (const auto name = args.find("component");
                     command == "addComponent" && name != args.end() && name->is_string())
            {
                const ComponentID component = getComponentIdByName(name->get_ref<const std::string&>().c_str());
                const StructSchema* schema =
                    component == INVALID_COMPONENT_ID ? nullptr : getComponentMetadata(component).schema;
                if (schema && value->is_object())
                {
                    for (const FieldDesc& field : schema->fields)
                    {
                        if (const auto it = value->find(std::string(field.name)); it != value->end())
                        {
                            forwardEntityRefs(field, *it, history);
                        }
                    }
                }
            }
        }

        // Resolves the "id" and "component" args of a component command.
        std::string resolveComponentArgs(const World* world, const ControlArgs& args, Entity& outEntity,
                                         ComponentID& outComponent)
        {
            const std::optional<u64> id = args.integer("id");
            const std::optional<std::string> name = args.string("component");
            if (!id || !name)
            {
                return "needs id and component";
            }
            outEntity = Entity(*id);
            if (!world->isEntityAlive(outEntity))
            {
                return "no live entity " + std::to_string(*id);
            }
            outComponent = getComponentIdByName(name->c_str());
            if (outComponent == INVALID_COMPONENT_ID)
            {
                return "unknown component '" + *name + "'";
            }
            // a component whose module unloaded keeps its id but has no thunks
            if (!getComponentMetadata(outComponent).moveAndDestroy)
            {
                return "component '" + *name + "' is not loaded";
            }
            return {};
        }

        // id, component and, for a schema component, its fields under valueKey
        void writeComponentResult(const World& world, const Entity entity, const ComponentID component,
                                  const std::string_view valueKey, IFormatWriter& out)
        {
            out.writeU64("id", entity.id());
            out.writeString("component", getComponentName(component));
            if (const StructSchema* schema = getComponentMetadata(component).schema)
            {
                out.beginObject(valueKey);
                writeValue(*schema, world.getConstComponentDataById(entity, component), out, FieldSet::All);
                out.endObject();
            }
        }

        void writeEntityComponents(const World& world, const Entity entity, IFormatWriter& out)
        {
            ComponentID ids[256];
            const u32 count = std::min<u32>(world.getEntityAspectIds(entity, ids), 256);
            std::vector<std::string_view> opaque;
            out.beginObject("components");
            for (u32 i = 0; i < count; ++i)
            {
                const ComponentMetadata& metadata = getComponentMetadata(ids[i]);
                if (!metadata.schema)
                {
                    opaque.push_back(metadata.name.toView());
                    continue;
                }
                // const access: an inspection read must not mark the component modified
                const void* data = world.getConstComponentDataById(entity, ids[i]);
                out.beginObject(metadata.name.toView());
                writeValue(*metadata.schema, data, out, FieldSet::All);
                out.endObject();
            }
            out.endObject();
            out.beginArray("opaque", static_cast<u32>(opaque.size()));
            for (const std::string_view name : opaque)
            {
                out.writeString("", name);
            }
            out.endArray();
        }

        // FieldKind order; the schema lattice is closed, so a new kind fails the assert instead of printing nothing
        constexpr std::string_view FIELD_KIND_NAMES[] = {
            "bool",   "u8",         "u16",    "u32",       "u64",
            "i32",    "f32",        "f64",    "vec2",      "vec3",
            "vec4",   "quat",       "mat4",   "string",    "internedString",
            "entity", "entityList", "struct", "structList"};
        static_assert(std::size(FIELD_KIND_NAMES) == static_cast<sizet>(FieldKind::StructList) + 1);

        void writeFieldSchema(const StructSchema& schema, IFormatWriter& out)
        {
            out.beginArray("fields", static_cast<u32>(schema.fields.size()));
            for (const FieldDesc& field : schema.fields)
            {
                out.beginObject("");
                out.writeString("name", field.name);
                out.writeString("kind", FIELD_KIND_NAMES[static_cast<sizet>(field.kind)]);
                if (field.structSchema)
                {
                    out.writeString("type", field.structSchema->typeName);
                }
                if (field.arrayCount != 0)
                {
                    out.writeU32("count", field.arrayCount);
                }
                out.beginArray("flags", 0);
                for (const auto& [bit, name] :
                     {std::pair{fieldFlags::PERSISTED, "persisted"}, std::pair{fieldFlags::OBSERVED, "observed"},
                      std::pair{fieldFlags::READONLY, "readonly"}, std::pair{fieldFlags::COLOR, "color"}})
                {
                    if (field.flags & bit)
                    {
                        out.writeString("", name);
                    }
                }
                out.endArray();
                if (!field.unit.empty())
                {
                    out.writeString("unit", field.unit);
                }
                // an open side of a range is left out (JSON has no infinity)
                if (std::isfinite(field.min))
                {
                    out.writeF64("min", field.min);
                }
                if (std::isfinite(field.max))
                {
                    out.writeF64("max", field.max);
                }
                if (field.step > 0.0)
                {
                    out.writeF64("step", field.step);
                }
                if (!field.enumNames.empty())
                {
                    out.beginArray("enum", static_cast<u32>(field.enumNames.size()));
                    for (const EnumName& named : field.enumNames)
                    {
                        out.beginObject("");
                        if (named.value < 0) // only an i32 enum has negative values
                        {
                            out.writeI32("value", static_cast<i32>(named.value));
                        }
                        else
                        {
                            out.writeU64("value", static_cast<u64>(named.value));
                        }
                        out.writeString("name", named.name);
                        out.endObject();
                    }
                    out.endArray();
                }
                out.endObject();
            }
            out.endArray();
        }

        u32 countEntitiesWith(const World& world, const ComponentID component)
        {
            u64 count = 0;
            for (const Chunk* chunk : world.getChunksContaining(Aspect(component)))
            {
                count += chunk->size();
            }
            return static_cast<u32>(count);
        }
    } // namespace

    // A failure lists what moved before it and what failed; an applied walk is logged as the undo or redo it was, so a
    // replay reaches the same position.
    std::string ControlServer::State::walk(const std::string& world, const bool redoing, const u64 steps,
                                           IFormatWriter& out, u64* walked)
    {
        nlohmann::json report = nlohmann::json::object();
        std::string error = redoing ? redo(world, steps, out, report) : undo(world, steps, out, report);
        if (walked && report.contains("steps"))
        {
            *walked = report["steps"].size();
        }
        if (!report.empty())
        {
            out.writeString("world", world);
            summarizeStepArgs(report);
            for (const auto& [key, value] : report.items())
            {
                writeJson(key, value, out);
            }
        }
        if (CommandHistory* history = historyOf(world); error.empty() && history)
        {
            history->log(redoing ? "redo" : "undo", {{"world", world}, {"steps", steps}}, running, elapsedUs());
        }
        return error;
    }

    ControlServer::ControlServer() : m_state(std::make_unique<State>())
    {
#ifndef SPITE_SHIPPING
        State& state = *m_state;

        registerQuery("list", OWNER, "every query and command by name and owning module", {},
                      [&state](const ControlArgs&, IFormatWriter& out)
                      {
                          for (const bool command : {false, true})
                          {
                              out.beginArray(command ? "commands" : "queries", 0);
                              for (const auto& [name, handler] : state.handlers)
                              {
                                  if (handler.command == command)
                                  {
                                      out.beginObject("");
                                      out.writeString("name", name);
                                      out.writeString("owner", handler.owner);
                                      if (command)
                                      {
                                          out.writeBool("undoable", handler.undoable);
                                      }
                                      out.endObject();
                                  }
                              }
                              out.endArray();
                          }
                          return std::string{};
                      });

        registerQuery("roots", OWNER,
                      "the named roots file args are relative to: each one's directory and use; `host` is "
                      "no root: its args take an OS path as it is",
                      {},
                      [&state](const ControlArgs&, IFormatWriter& out)
                      {
                          out.beginArray("roots", static_cast<u32>(state.roots.size()));
                          for (const auto& [name, root] : state.roots)
                          {
                              out.beginObject("");
                              out.writeString("name", name);
                              out.writeString("directory", utf8Path(root.directory));
                              out.writeString("doc", root.doc);
                              out.endObject();
                          }
                          out.endArray();
                          return std::string{};
                      });

        registerQuery(
            "listDirectory", OWNER,
            "the entries of a directory under a named root: name, kind (file, directory, other), size "
            "in bytes, modified (UTC); directories first, then files by name. path is <root>[/<directory>]; "
            "without it, the named roots. A path under no root is refused with the valid ones",
            {optionalPath("path", ANY_ROOT, "<root>[/<directory>], '/' separated", "the list of roots"),
             optionalArg("extensions", ArgKind::String,
                         "keeps only the files with one of these extensions (comma list, e.g. glb,.gltf); "
                         "directories stay",
                         "every file"),
             optionalArg("foldersOnly", ArgKind::Boolean, "lists directories only", "false"),
             optionalArg("newestFirst", ArgKind::Boolean,
                         "orders the files by modification time, newest first, before the cap of 4096 entries (the "
                         "default is by name)",
                         "false")},
            [this](const ControlArgs& args, IFormatWriter& out)
            {
                DirectoryListing listing;
                if (std::string error =
                        listDirectory(args.string("path").value_or(""), args.string("extensions").value_or(""),
                                      args.boolean("foldersOnly").value_or(false), listing,
                                      args.boolean("newestFirst").value_or(false));
                    !error.empty())
                {
                    return error;
                }
                out.writeString("path", listing.path);
                out.writeString("root", listing.root);
                out.writeString("directory", listing.directory);
                out.writeBool("truncated", listing.truncated);
                out.beginArray("entries", static_cast<u32>(listing.entries.size()));
                for (const DirectoryEntry& entry : listing.entries)
                {
                    out.beginObject("");
                    out.writeString("name", entry.name);
                    out.writeString("kind",
                                    entry.kind == DirectoryEntryKind::File            ? "file"
                                        : entry.kind == DirectoryEntryKind::Directory ? "directory"
                                                                                      : "other");
                    out.writeU64("size", entry.size);
                    out.writeString("modified", entry.modified);
                    out.endObject();
                }
                out.endArray();
                return std::string{};
            });

        registerQuery(
            "searchCapabilities", OWNER,
            "ranks live commands, queries, component schemas and recipes by name and summary. All search words "
            "must match for an exact result. Returns total and bounded matches with name, kind, owner and summary. "
            "When no exact match exists, returns up to three partial suggestions and a hint. Use describe for "
            "commands and queries, or components name= for a component schema",
            {optionalArg("text", ArgKind::String, "space-separated search words, ASCII case-insensitive"),
             optionalArg("owner", ArgKind::String, "owning module or component category, ASCII case-insensitive"),
             optionalArg("kind", ArgKind::String, "query, command, component or recipe"),
             optionalArg("limit", ArgKind::Integer, "maximum matches to return, 1..30", "12")},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                const std::string text = args.string("text").value_or("");
                const std::string owner = args.string("owner").value_or("");
                const std::string kind = args.string("kind").value_or("");
                const u64 limit = args.integer("limit").value_or(12);
                if (text.size() > 120 || limit < 1 || limit > 30 ||
                    (!kind.empty() && kind != "query" && kind != "command" && kind != "component" && kind != "recipe"))
                    return std::string("search text is at most 120 bytes, limit is 1..30, and kind is query, command, "
                                       "component or recipe");
                std::istringstream input(text);
                const std::vector<std::string> words{std::istream_iterator<std::string>(input),
                                                     std::istream_iterator<std::string>()};
                const auto lower = [](const char c)
                { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
                const auto equals = [lower](const std::string_view a, const std::string_view b)
                { return std::ranges::equal(a, b, {}, lower, lower); };
                const auto contains = [lower](const std::string_view a, const std::string_view b)
                { return !std::ranges::search(a, b, {}, lower, lower).empty(); };
                const auto oneEditApart = [equals, lower](std::string_view a, std::string_view b)
                {
                    if (a.size() > b.size())
                        std::swap(a, b);
                    if (a.size() < 4 || b.size() - a.size() > 1)
                        return false;
                    const auto mismatch = std::ranges::mismatch(a, b, {}, lower, lower);
                    const sizet i = static_cast<sizet>(mismatch.in1 - a.begin());
                    if (a.size() != b.size())
                        return equals(a.substr(i), b.substr(i + 1));
                    if (i == a.size())
                        return false;
                    return equals(a.substr(i + 1), b.substr(i + 1)) ||
                        (i + 1 < a.size() && lower(a[i]) == lower(b[i + 1]) && lower(a[i + 1]) == lower(b[i]) &&
                         equals(a.substr(i + 2), b.substr(i + 2)));
                };
                struct Match
                {
                    std::string_view name, kind, owner, doc, steps;
                    sizet matched;
                    u32 score;
                };
                std::vector<Match> matches, partial;
                const auto add = [&](const std::string_view name, const std::string_view type,
                                     const std::string_view owning, const std::string_view doc,
                                     const std::span<const FieldDesc> fields = {}, const std::string_view steps = {})
                {
                    if ((!owner.empty() && !equals(owner, owning)) || (!kind.empty() && kind != type))
                        return;
                    u32 score = 1;
                    sizet matched = 0;
                    bool approximate = false;
                    for (const std::string& word : words)
                    {
                        if (equals(name, word))
                            score += 16;
                        else if (contains(name, word))
                            score += 8;
                        else if (std::ranges::any_of(fields, [&](const FieldDesc& field)
                                                     { return contains(field.name, word); }))
                            score += 6;
                        else if (contains(owning, word))
                            score += 3;
                        else if (contains(doc, word))
                            ++score;
                        else if (std::ranges::any_of(fields, [&](const FieldDesc& field)
                                                     { return oneEditApart(field.name, word); }))
                        {
                            score += 2;
                            approximate = true;
                        }
                        else
                            continue;
                        ++matched;
                    }
                    if (matched == words.size() && !approximate)
                        matches.push_back({name, type, owning, doc, steps, matched, score});
                    else if (matched != 0)
                        partial.push_back({name, type, owning, doc, steps, matched, score});
                };
                for (const auto& [name, handler] : state.handlers)
                    add(name, handler.command ? "command" : "query", handler.owner, handler.doc);
                if (kind.empty() || kind == "component")
                {
                    std::vector<ComponentID> ids(getRegisteredComponentCount());
                    ids.resize(getComponentIds(ids.data(), static_cast<u32>(ids.size())));
                    for (const ComponentID id : ids)
                    {
                        const ComponentMetadata& metadata = getComponentMetadata(id);
                        if (metadata.schema && metadata.moveAndDestroy &&
                            getComponentIdByName(metadata.name.toView().data()) == id)
                            add(metadata.name.toView(), "component", componentCategoryName(metadata.category),
                                "inspect with components name=<name> counts=false", metadata.schema->fields);
                    }
                }
                if (state.handlers.contains("createEntity") && state.handlers.contains("addComponent") &&
                    getComponentIdByName("PointLightComponent") != INVALID_COMPONENT_ID)
                    add("createPointLight", "recipe", OWNER,
                        "createEntity, addComponent PointLightComponent using result.id, verify with entity", {},
                        R"([{"command":"createEntity","args":{"world":"scene","name":"Point Light"}},{"command":"addComponent","args":{"world":"scene","component":"PointLightComponent"},"bind":{"id":{"step":0,"pointer":"/id"}}},{"query":"entity","args":{"world":"scene"},"bind":{"id":{"step":0,"pointer":"/id"}}}])");
                if (state.handlers.contains("batch") && state.handlers.contains("entity") &&
                    state.handlers.contains("undo"))
                    add("verifySceneBatch", "recipe", OWNER, "apply a named batch, verify with entity, undo by world",
                        {},
                        R"([{"command":"batch","args":{"world":"scene"},"inputs":["name","commands"]},{"query":"entity","args":{"world":"scene"},"inputs":["id"]},{"command":"undo","args":{"world":"scene"}}])");
                if (state.handlers.contains("dock"))
                    add("openAgentPanel", "recipe", OWNER, "open the Agent panel with dock kind=show panel=agent", {},
                        R"([{"command":"dock","args":{"kind":"show","panel":"agent"}}])");
                const auto writeMatches =
                    [&out](const std::string_view key, const std::vector<Match>& entries, const sizet maximum)
                {
                    const sizet count = std::min(entries.size(), maximum);
                    out.beginArray(key, static_cast<u32>(count));
                    for (const Match& match : std::span(entries).first(count))
                    {
                        std::string_view summary = match.doc.substr(0, match.doc.find(". "));
                        if (summary.size() > HELP_LINE)
                        {
                            sizet end = HELP_LINE;
                            while ((static_cast<unsigned char>(summary[end]) & 0xc0u) == 0x80u)
                                --end;
                            summary = summary.substr(0, end);
                        }
                        out.beginObject("");
                        out.writeString("name", match.name);
                        out.writeString("kind", match.kind);
                        out.writeString("owner", match.owner);
                        out.writeString("summary", summary);
                        if (!match.steps.empty())
                            writeJson("steps", nlohmann::json::parse(match.steps), out);
                        out.endObject();
                    }
                    out.endArray();
                };
                std::ranges::stable_sort(matches, std::greater{}, &Match::score);
                out.writeU64("total", matches.size());
                writeMatches("matches", matches, static_cast<sizet>(limit));
                if (matches.empty() && !words.empty())
                {
                    std::ranges::stable_sort(partial, std::greater{},
                                             [](const Match& match) { return std::pair{match.matched, match.score}; });
                    writeMatches("suggestions", partial, std::min<sizet>(3, static_cast<sizet>(limit)));
                    out.writeString("hint",
                                    partial.empty() ? "Try one verb, component name or field"
                                                    : "Try fewer words or use a suggested name");
                }
                return std::string{};
            });

        registerQuery("describe", OWNER, "what queries and commands do and the args they take",
                      {optionalArg("name", ArgKind::String, "the query or command", "every one")},
                      [&state](const ControlArgs& args, IFormatWriter& out)
                      {
                          const std::optional<std::string> only = args.string("name");
                          if (only && !state.handlers.contains(*only))
                          {
                              return state.unknownEntry(*only);
                          }
                          state.writeEntries(out, [&](const std::string& name, const State::Handler&)
                                             { return !only || name == *only; });
                          out.beginObject("kinds");
                          for (sizet kind = 0; kind < std::size(ARG_KIND_NAMES); ++kind)
                          {
                              out.writeString(ARG_KIND_NAMES[kind], ARG_KIND_DOCS[kind]);
                          }
                          out.endObject();
                          return std::string{};
                      });

        constexpr static std::string_view ACTIVITY_KINDS[]{"scene", "module", "script"};
        constexpr static std::string_view ACTIVITY_FINISH[]{"done", "failed"};
        constexpr static std::string_view ACTIVITY_REVIEW[]{"keep", "revert", "acknowledge"};
        registerViewCommand("beginAgentActivity", OWNER,
                            "opens one Agent feed operation. Reuse its id in the request's activity field",
                            {requiredArg("kind", ArgKind::String, "operation category", ACTIVITY_KINDS),
                             requiredArg("title", ArgKind::String, "short operation title"),
                             optionalArg("why", ArgKind::String, "the agent's reason for the operation")},
                            [&state](const ControlArgs& args, IFormatWriter& out)
                            {
                                const std::string title = args.string("title").value_or("");
                                const std::string why = args.string("why").value_or("");
                                if (title.empty() || title.size() > 120 || why.size() > 280)
                                    return std::string("activity title must be 1..120 bytes and why at most 280 bytes");
                                const u64 id = state.startActivity(args.string("kind").value_or("script"), title, why);
                                if (!id)
                                    return std::string("the Agent feed has too many active operations");
                                if (state.transport != State::LOCAL)
                                    state.explicitActivityTransports.emplace_back(id, state.transport);
                                out.writeU64("id", id);
                                return std::string{};
                            });
        registerViewCommand(
            "finishAgentActivity", OWNER, "closes an operation with an optional agent-reported result",
            {requiredArg("id", ArgKind::Integer, "activity id"),
             requiredArg("state", ArgKind::String, "completion state", ACTIVITY_FINISH),
             optionalArg("evidence", ArgKind::String, "short agent-reported evidence")},
            [&state](const ControlArgs& args, IFormatWriter&)
            {
                const auto* row = state.activity(args.integer("id").value_or(0));
                const std::string evidence = args.string("evidence").value_or("");
                if (!row || evidence.size() > 512)
                    return std::string("unknown activity or evidence exceeds 512 bytes");
                if (row->state == "kept" || row->state == "reverted")
                    return std::string("the activity's scene step was already reviewed");
                const std::string requested = args.string("state").value_or("done");
                if (requested == "done" &&
                    (row->state == "passed" || row->state == "failed" || row->state == "cancelled" ||
                     row->state == "unchanged"))
                    return std::string{};
                const std::string_view status = requested == "done" && row->kind == "scene" && state.activityStep(*row)
                    ? "ready"
                    : std::string_view(requested);
                state.updateActivity(row->id, row->phase, status, row->action,
                                     evidence.empty() ? std::string_view(row->evidence) : std::string_view(evidence),
                                     row->reported || !evidence.empty());
                return std::string{};
            });
        registerViewCommand(
            "reviewAgentActivity", OWNER, "keeps or reverts the latest scene undo step linked to a feed operation",
            {requiredArg("id", ArgKind::Integer, "activity id"),
             requiredArg("action", ArgKind::String,
                         "keep or revert the latest linked scene step, or acknowledge a failed or cancelled activity",
                         ACTIVITY_REVIEW)},
            [this, &state](const ControlArgs& args, IFormatWriter& out)
            {
                const u64 id = args.integer("id").value_or(0);
                if (args.string("action") == "acknowledge")
                {
                    if (!agentActivityCanAcknowledge(id))
                        return std::string("the activity cannot be acknowledged");
                    const auto* row = state.activity(id);
                    state.updateActivity(id, row->phase, "acknowledged", row->action, row->evidence, row->reported);
                    return std::string{};
                }
                if (!agentActivityCanReview(id))
                    return std::string("the activity has no reviewable scene step");
                const auto* row = state.activity(id);
                const bool keep = args.string("action") == "keep";
                if (keep)
                {
                    const auto world = std::ranges::find(state.worlds, row->worldSerial, &State::WorldEntry::serial);
                    world->history->markReviewed(*state.activityStep(*row));
                    state.updateActivityReviews(*world);
                    if (world->timelineActivity == id)
                        state.noteAgentReview(*world, "review");
                }
                else
                {
                    const nlohmann::json request{{"command", "revertStep"},
                                                 {"args", {{"world", row->world}, {"index", row->step}}}};
                    const auto reply = nlohmann::json::parse(handle(request.dump()));
                    if (!reply.value("ok", false))
                        return reply.value("error", std::string("could not revert the scene step"));
                    for (const auto& [key, value] : reply["result"].items())
                        writeJson(key, value, out);
                }
                state.updateActivity(id, "review", keep ? "kept" : "reverted", keep ? "review" : "revertStep",
                                     row->evidence, row->reported);
                return std::string{};
            });
        registerQuery(
            "agentActivity", OWNER,
            "the latest operation feed, newest first. focus is the active operation or newest result. Rows include "
            "id, kind, title, why, phase, state, action, evidence, reported, inspect, world and step",
            {optionalArg("limit", ArgKind::Integer, "maximum rows, 1..64", "12")},
            [this, &state](const ControlArgs& args, IFormatWriter& out)
            {
                const u64 limit = args.integer("limit").value_or(12);
                if (!limit || limit > 64)
                    return std::string("activity limit must be 1..64");
                const AgentActivity* focus = agentActivityFocus();
                out.writeU64("focus", focus ? focus->id : 0);
                const sizet count = std::min(static_cast<sizet>(limit), state.activities.size());
                out.beginArray("operations", static_cast<u32>(count));
                for (sizet i = 0; i < count; ++i)
                {
                    const AgentActivity& row = state.activities[state.activities.size() - i - 1];
                    out.beginObject("");
                    out.writeU64("id", row.id);
                    out.writeString("kind", row.kind);
                    out.writeString("title", row.title);
                    out.writeString("why", row.why);
                    out.writeString("phase", row.phase);
                    out.writeString("state", row.state);
                    out.writeString("action", row.action);
                    out.writeString("inspect", row.inspect);
                    out.writeString("evidence", row.evidence);
                    out.writeBool("reported", row.reported);
                    out.writeString("world", row.world);
                    out.writeU64("step", row.step);
                    out.endObject();
                }
                out.endArray();
                return std::string{};
            });

        registerQuery(
            "agentTimeline", OWNER,
            "latest observed agent operation in a world: phase (none, inspect, apply, review), client, inspection "
            "query, applied operation, review action and undo step (0 before an apply). Review can be an entity "
            "readback, Keep or Revert; readback means the entity was read, and does not compare values",
            {WORLD_ARG},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                const State::WorldEntry* entry = state.findEntry(args);
                if (!entry)
                    return state.unknownWorld(args);
                const AgentTimeline& timeline = entry->timeline;
                constexpr std::string_view phases[]{"none", "inspect", "apply", "review"};
                out.writeString("world", entry->name);
                out.writeString("phase", phases[static_cast<u8>(timeline.phase)]);
                out.writeString("client", timeline.client);
                out.writeString("inspect", timeline.inspect);
                out.writeString("operation", timeline.operation);
                out.writeString("review", timeline.review);
                out.writeU64("step", timeline.step);
                return std::string{};
            });

        registerQuery(
            "help", OWNER,
            "what is there to run: one line per command and query, the name and the start of its doc; topic is one "
            "entry's name (its whole doc and args) or a word its name or doc contains (ignoring case). describe is "
            "the full table",
            {optionalArg("topic", ArgKind::String, "an entry's name, or a word to search for", "every entry")},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                const std::string topic = args.string("topic").value_or("");
                if (const auto it = state.handlers.find(topic); it != state.handlers.end())
                {
                    const State::Handler& handler = it->second;
                    out.writeString("name", it->first);
                    out.writeString("kind", handler.command ? "command" : "query");
                    out.writeString("owner", handler.owner);
                    out.writeString("doc", handler.described ? handler.doc : "no description registered");
                    out.beginObject("args");
                    for (const Arg& arg : handler.args)
                    {
                        std::string line = std::format("{} {}: {}", arg.required ? "required" : "optional",
                                                       kindName(arg.kind), arg.doc);
                        if (!arg.defaultValue.empty())
                        {
                            line += " (default " + arg.defaultValue + ")";
                        }
                        out.writeString(arg.name, line);
                    }
                    out.endObject();
                    return std::string{};
                }
                const auto lower = [](const char c)
                { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
                const auto has = [&](const std::string_view text)
                { return std::ranges::search(text, topic, {}, lower, lower).begin() != text.end(); };
                u32 matched = 0;
                for (const bool command : {true, false})
                {
                    out.beginObject(command ? "commands" : "queries");
                    for (const auto& [name, handler] : state.handlers)
                    {
                        if (handler.command != command || (!topic.empty() && !has(name) && !has(handler.doc)))
                        {
                            continue;
                        }
                        // the first sentence, at most HELP_LINE characters
                        std::string_view line = handler.doc;
                        line = line.substr(0, std::min(line.find(". "), line.size()));
                        const bool cut = line.size() > HELP_LINE;
                        if (cut)
                        {
                            const sizet space = line.rfind(' ', HELP_LINE);
                            line = line.substr(0, space == std::string_view::npos ? HELP_LINE : space);
                        }
                        out.writeString(name, std::string(line) + (cut ? " ..." : ""));
                        ++matched;
                    }
                    out.endObject();
                }
                return matched == 0 ? "nothing matches '" + topic + "': list names every command and query"
                                    : std::string{};
            });

        // pause-and-inspect: simulation stops, rendering and inspection go on; pause, resume and step are logged
        const auto simulationCommand =
            [&state](std::string (*apply)(const World&, const ControlArgs&), const bool preferPlay = true)
        {
            return [&state, apply, preferPlay](const ControlArgs& args, IFormatWriter& out)
            {
                const State::WorldEntry* entry = state.findEntry(args, preferPlay);
                if (!entry)
                {
                    return state.unknownWorld(args);
                }
                const World* world = &entry->world;
                if (std::string error = apply(*world, args); !error.empty())
                {
                    return error;
                }
                // simulation state is part of what a replay rebuilds: input injected between steps replays in order
                if (state.capture)
                {
                    state.capture->replay = true;
                    // logged in the world it acted on, whichever default picked it
                    state.capture->replayWorld = entry->name;
                    state.capture->replayArgs = nlohmann::json{{"world", entry->name}};
                }
                writeSimulation(world->simulationState(), out);
                return std::string{};
            };
        };
        registerCommand("pause", OWNER,
                        "stops the world's simulation (the play world when none is named and one runs); rendering and "
                        "inspection go on",
                        {WORLD_ARG},
                        simulationCommand(
                            [](const World& world, const ControlArgs&)
                            {
                                world.setSimulationPaused(true);
                                return std::string{};
                            }));
        registerCommand("resume", OWNER,
                        "restarts the world's simulation (the play world when none is named and one runs)", {WORLD_ARG},
                        simulationCommand(
                            [](const World& world, const ControlArgs&)
                            {
                                world.setSimulationPaused(false);
                                return std::string{};
                            }));
        // the reply waits for the steps: it carries the state after they ran (pump), so a client reads what they did
        const HandlerFn step = simulationCommand(
            [](const World& world, const ControlArgs& args)
            {
                if (const std::optional<u64> fixed = args.integer("fixed"))
                {
                    // all N run in one frame, so a huge N would stall the engine
                    if (*fixed == 0 || *fixed > MAX_FIXED_STEPS_PER_STEP)
                    {
                        return "fixed must be 1.." + std::to_string(MAX_FIXED_STEPS_PER_STEP);
                    }
                    world.stepFixedSimulation(static_cast<u32>(*fixed));
                }
                else
                {
                    // the reply waits for the frames, so a huge N would hold a synchronous client for hours
                    const u64 frames = args.integer("frames").value_or(1);
                    if (frames == 0 || frames > MAX_FRAMES_PER_STEP)
                    {
                        return "frames must be 1.." + std::to_string(MAX_FRAMES_PER_STEP);
                    }
                    world.stepSimulation(static_cast<u32>(frames));
                }
                return std::string{};
            },
            true);
        registerCommand(
            "step", OWNER,
            "runs simulation frames, or fixed steps, of a paused world (the play world when none is named and one "
            "runs, else the edit scene); replies with its state once they ran (a pause, "
            "resume or other step that drops them answers too). An in-process request without a reply sink answers "
            "at once, with the steps pending",
            {WORLD_ARG, optionalArg("frames", ArgKind::Integer, "simulation frames (1..1000) to run", "1"),
             optionalArg("fixed", ArgKind::Integer,
                         "fixed steps (1..1000) to run in the next frame, instead of frames")},
            [this, step](const ControlArgs& args, IFormatWriter& out)
            {
                if (std::string error = step(args, out); !error.empty())
                {
                    return error;
                }
                (void)deferUntilStepped(args, true, {}, true);
                return std::string{};
            });

        registerUndoableCommand(
            "set", OWNER,
            "writes one component field; the result carries its previous value; a number outside the field's declared "
            "range (components: min, max) is refused with the range",
            {requiredArg("id", ArgKind::Entity, "the entity"),
             requiredArg("path", ArgKind::String, "Component.field, then .field or [index] steps"),
             requiredArg("value", ArgKind::Json, "the field's new value"),
             optionalArg("merge", ArgKind::String,
                         "a key: a set with the newest undo step's key, setting the same fields, folds into it")},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                const World* world = state.findWorld(args);
                if (!world)
                {
                    return state.unknownWorld(args);
                }
                const std::optional<u64> id = args.integer("id");
                const std::optional<std::string> path = args.string("path");
                const std::optional<std::string> value = args.json("value");
                if (!id || !path || !value)
                {
                    return std::string("set needs id, path and value");
                }
                const Entity entity(*id);
                if (!world->isEntityAlive(entity))
                {
                    return std::string("no live entity ") + std::to_string(*id);
                }
                nlohmann::json previous;
                if (std::string error = setField(*world, entity, *path, *value, out, previous, state.componentGuard,
                                                 state.replaying == 0);
                    !error.empty())
                {
                    return error;
                }
                state.record("set", {{"id", *id}, {"path", *path}, {"value", std::move(previous)}}, {});
                ComponentID component = INVALID_COMPONENT_ID;
                FieldDesc field;
                u32 offset = 0;
                (void)resolvePath(*path, component, field, offset);
                std::vector<std::string> dead;
                findDeadReferences(*world, field,
                                   static_cast<const std::byte*>(world->getConstComponentDataById(entity, component)) +
                                       offset,
                                   "entity " + std::to_string(*id) + " " + *path, dead);
                state.warn(std::move(dead));
                return std::string{};
            });

        // inverse: removeComponent
        registerUndoableCommand(
            "addComponent", OWNER, "adds a component by schema name",
            {requiredArg("id", ArgKind::Entity, "the entity"),
             requiredArg("component", ArgKind::Component, "the component to add"),
             optionalArg("value", ArgKind::Json, "an object of the component's fields", "the component's defaults")},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                const World* world = state.findWorld(args);
                if (!world)
                {
                    return state.unknownWorld(args);
                }
                Entity entity;
                ComponentID component = INVALID_COMPONENT_ID;
                if (std::string error = resolveComponentArgs(world, args, entity, component); !error.empty())
                {
                    return error;
                }
                const std::string name = getComponentName(component);
                if (world->hasComponentById(entity, component))
                {
                    return "entity " + std::to_string(entity.id()) + " already has " + name;
                }
                if (state.componentGuard)
                {
                    if (std::string refused = state.componentGuard(*world, entity, component, true); !refused.empty())
                    {
                        return refused;
                    }
                }
                const StructSchema* schema = getComponentMetadata(component).schema;
                const std::optional<std::string> value = args.json("value");
                if (value && !schema)
                {
                    return "component '" + name + "' has no schema, so it takes no value";
                }
                if (value && !nlohmann::json::parse(*value, nullptr, false).is_object())
                {
                    return std::string("'value' must be an object of fields");
                }
                const ComponentMetadata& metadata = getComponentMetadata(component);
                if (value && metadata.copyPolicy)
                {
                    // built before it joins the entity, so added hooks (asset
                    // bindings) see the value
                    void* staged = ::operator new(metadata.size, std::align_val_t(metadata.alignment));
                    if (metadata.constructionPolicy)
                    {
                        metadata.constructionPolicy(staged);
                    }
                    else
                    {
                        std::memset(staged, 0, metadata.size); // runtime descs may lack ctor/dtor, as in the ECS
                    }
                    std::string error;
                    JsonFragmentReader reader(*value);
                    const bool read = readValue(*schema, staged, reader, error);
                    if (read)
                    {
                        world->addComponentDataById(entity, component, staged);
                    }
                    if (metadata.destructionPolicy)
                    {
                        metadata.destructionPolicy(staged);
                    }
                    ::operator delete(staged, std::align_val_t(metadata.alignment));
                    if (!read)
                    {
                        return "bad value: " + error;
                    }
                }
                else
                {
                    world->addComponentById(entity, component);
                    if (value)
                    {
                        std::string error;
                        JsonFragmentReader reader(*value);
                        if (!readValue(*schema, world->getComponentDataById(entity, component), reader, error))
                        {
                            world->removeComponentById(entity, component);
                            return "bad value: " + error;
                        }
                    }
                }
                writeComponentResult(*world, entity, component, "value", out);
                state.record("removeComponent", {{"id", entity.id()}, {"component", name}}, {});
                if (state.componentAdded)
                {
                    state.componentAdded(*world, entity, component);
                }
                if (value)
                {
                    std::vector<std::string> dead;
                    const auto* data =
                        static_cast<const std::byte*>(world->getConstComponentDataById(entity, component));
                    for (const FieldDesc& field : schema->fields)
                    {
                        findDeadReferences(
                            *world, field, data + field.offset,
                            "entity " + std::to_string(entity.id()) + " " + name + "." + std::string(field.name), dead);
                    }
                    state.warn(std::move(dead));
                }
                return std::string{};
            });

        // the result's removed value is the inverse addComponent's value
        registerUndoableCommand(
            "removeComponent", OWNER, "removes a component; the result carries its removed value",
            {requiredArg("id", ArgKind::Entity, "the entity"),
             requiredArg("component", ArgKind::Component, "the component to remove")},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                const World* world = state.findWorld(args);
                if (!world)
                {
                    return state.unknownWorld(args);
                }
                Entity entity;
                ComponentID component = INVALID_COMPONENT_ID;
                if (std::string error = resolveComponentArgs(world, args, entity, component); !error.empty())
                {
                    return error;
                }
                if (!world->hasComponentById(entity, component))
                {
                    return "entity " + std::to_string(entity.id()) + " has no " + getComponentName(component);
                }
                if (state.componentGuard)
                {
                    if (std::string refused = state.componentGuard(*world, entity, component, false); !refused.empty())
                    {
                        return refused;
                    }
                }
                writeComponentResult(*world, entity, component, "removed", out);
                const std::string name = getComponentName(component);
                nlohmann::json inverse{{"id", entity.id()}, {"component", name}};
                if (const StructSchema* schema = getComponentMetadata(component).schema)
                {
                    JsonFragmentWriter removed;
                    writeValue(*schema, world->getConstComponentDataById(entity, component), removed, FieldSet::All);
                    std::string text;
                    removed.dump(text);
                    inverse["value"] = nlohmann::json::parse(text);
                }
                world->removeComponentById(entity, component);
                state.record("addComponent", std::move(inverse), {});
                return std::string{};
            });

        // undo/redo [steps=1]: revert or reapply the world's last steps
        for (const bool redo : {false, true})
        {
            registerCommand(redo ? "redo" : "undo", OWNER,
                            redo ? "reapplies the world's last undone steps" : "reverts the world's last steps",
                            {WORLD_ARG, optionalArg("steps", ArgKind::Integer, "how many steps", "1")},
                            [&state, redo](const ControlArgs& args, IFormatWriter& out)
                            {
                                const State::WorldEntry* entry = state.findEntry(args);
                                if (!entry)
                                {
                                    return state.unknownWorld(args);
                                }
                                const u64 steps = args.integer("steps").value_or(1);
                                if (steps == 0)
                                {
                                    return std::string("steps must be at least 1");
                                }
                                return state.walk(entry->name, redo, steps, out);
                            });
        }
        // historyJump to=N: undo or redo to a step by its index, the History panel's click. A step is named by its
        // index, not its distance, so a panel's copy of the list that went stale cannot land elsewhere.
        registerCommand(
            "historyJump", OWNER,
            "undoes or redoes the world's steps so that step `to` is the newest done one (0: none is done); refused, "
            "moving nothing, for a step that left the history",
            {WORLD_ARG,
             requiredArg("to", ArgKind::Integer, "the step's index, as history names it; 0 is before them all")},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                const State::WorldEntry* entry = state.findEntry(args);
                if (!entry)
                {
                    return state.unknownWorld(args);
                }
                const CommandHistory& history = *entry->history;
                const u64 to = args.integer("to").value_or(0);
                const std::optional<sizet> at = to == 0 ? std::optional<sizet>() : history.find(to);
                if (to != 0 && !at)
                {
                    const std::string held = history.steps().empty()
                        ? ", which holds no steps"
                        : ", which holds steps " + std::to_string(history.steps().front().index) + " to " +
                            std::to_string(history.steps().back().index);
                    return "step " + std::to_string(to) + " is not in the history of world '" + entry->name + "'" +
                        held;
                }
                const sizet target = at ? *at + 1 : 0;
                const sizet distance = target > history.done() ? target - history.done() : history.done() - target;
                out.writeU64("to", to);
                u64 walked = 0;
                std::string error = distance == 0
                    ? std::string{}
                    : state.walk(entry->name, target > history.done(), distance, out, &walked);
                out.writeU64("moved", walked);
                return error;
            });
        registerCommand("batch", OWNER,
                        "runs undoable commands as one undo step; a failing one rolls back those before it",
                        {requiredArg("commands", ArgKind::Json, "an array of {command, args}"),
                         optionalArg("name", ArgKind::String, "a name shown in History"),
                         optionalArg("merge", ArgKind::String, "as set's merge, for a batch of sets"), WORLD_ARG,
                         optionalArg("revives", ArgKind::Json,
                                     "per command, the entity ids it brings back (a logged revertStep writes it; "
                                     "clients leave it out). Any value, even [], forwards every command's ids as it "
                                     "runs, and a dead id named is redirected to its recreation; a live id is refused",
                                     "none")},
                        [&state](const ControlArgs& args, IFormatWriter& out) { return state.batch(args, out); });

        // revertStep index=N: git revert for the history, since undo is a stack
        registerCommand(
            "revertStep", OWNER,
            "applies the inverse of one done undo step as a new undoable step; refused, naming the later "
            "step, when that step touched the same entity and field",
            {WORLD_ARG, requiredArg("index", ArgKind::Integer, "the step's index, as history and provenance name it")},
            [&state](const ControlArgs& args, IFormatWriter& out) { return state.revertStep(args, out); });

        // history [full]: the undo steps (newest last; canUndo and canRedo say whether undo and redo have a step)
        // and the command log, whose requests replayed in order rebuild the world.
        // Args over SUMMARY_BYTES show
        // as their size unless full.
        registerQuery(
            "history", OWNER,
            "the world's undo steps and command log; each step reports its name and affected entities",
            {WORLD_ARG,
             optionalArg("full", ArgKind::Boolean, "args in full, not shown as their size past 256 bytes", "false")},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                const State::WorldEntry* entry = state.findEntry(args);
                if (!entry)
                {
                    return state.unknownWorld(args);
                }
                const bool full = args.boolean("full").value_or(false);
                const auto writeArgs = [full, &out](const nlohmann::json& requestArgs)
                { writeJson("args", summarizeArgs(requestArgs, full), out); };
                const CommandHistory& history = *entry->history;
                out.writeString("world", entry->name);
                out.writeU32("undoable", static_cast<u32>(history.done()));
                out.writeU32("redoable", static_cast<u32>(history.steps().size() - history.done()));
                out.writeBool("canUndo", history.done() > 0);
                out.writeBool("canRedo", history.done() < history.steps().size());
                out.writeU64("bytes", history.bytes());
                out.writeU32("aliases", static_cast<u32>(history.aliasCount()));
                out.writeU32("longestAliasChain", static_cast<u32>(history.longestAliasChain()));
                out.writeU32("provenanceEntries", static_cast<u32>(history.provenance().size()));
                out.beginArray("steps", static_cast<u32>(history.steps().size()));
                for (sizet i = 0; i < history.steps().size(); ++i)
                {
                    const UndoStep& step = history.steps()[i];
                    out.beginObject("");
                    out.writeU64("step", step.index);
                    out.writeBool("done", i < history.done());
                    out.writeString("name", step.label);
                    writeAttribution(step.by, out);
                    const auto affected = history.touchedFields(step);
                    out.beginArray("entities", static_cast<u32>(affected.size()));
                    for (const auto& entry : affected)
                        out.writeU64("", entry.first);
                    out.endArray();
                    out.beginArray("commands", static_cast<u32>(step.records.size()));
                    for (const CommandRecord& record : step.records)
                    {
                        out.beginObject("");
                        out.writeString("command", record.forward.command);
                        writeArgs(record.forward.args);
                        out.endObject();
                    }
                    out.endArray();
                    out.endObject();
                }
                out.endArray();
                out.writeU64("droppedLogRows", history.droppedLogRows());
                out.writeU64("logBytes", history.logBytes());
                // the log holds every applied request since the world was added
                out.writeBool("complete", history.droppedLogRows() == 0);
                out.beginArray("log", static_cast<u32>(history.logRows().size()));
                for (const CommandLogRow& row : history.logRows())
                {
                    out.beginObject("");
                    out.writeU64("sequence", row.sequence);
                    out.writeString("command", row.command);
                    writeArgs(nlohmann::json::parse(row.args));
                    out.writeString("source", row.source);
                    out.writeString("client", row.client);
                    out.writeString("time", row.time);
                    out.writeString("result", row.result);
                    out.writeU64("durationUs", row.durationUs);
                    out.endObject();
                }
                out.endArray();
                return std::string{};
            });

        for (const bool acquire : {true, false})
        {
            const auto updateLease = [&state, acquire](const ControlArgs& args, IFormatWriter& out)
            {
                state.refreshEditorLease();
                const u32 transport = state.runningTransport;
                if (transport == State::LOCAL || !state.liveLeaseTransport(transport))
                    return std::string("an attached external session is required");
                if (state.editorLease && state.editorLease->transport != transport)
                    return std::string("editor lease is held by another session");
                if (!acquire && !state.editorLease)
                    return std::string("this session does not hold an editor lease");
                u64 ttl = 120000;
                if (const auto rawTtl = args.json("ttlMs"))
                {
                    const auto value = nlohmann::json::parse(*rawTtl);
                    if (!value.is_number_unsigned() || value.get<u64>() < 1000 || value.get<u64>() > 600000)
                        return std::string("ttlMs must be an integer from 1000 to 600000");
                    ttl = value.get<u64>();
                }
                const std::string owner = acquire ? args.string("owner").value_or("") : state.editorLease->owner;
                if (acquire && owner.size() > 64)
                    return std::string("owner is at most 64 bytes");
                const u64 now = state.activityNowMs();
                state.editorLease = State::EditorLease{transport, owner, now, now + ttl};
                state.writeEditorLease(out);
                return std::string{};
            };
            if (acquire)
                registerViewCommand(
                    "acquireEditorLease", OWNER, "acquires or renews the editor lease for this attached session",
                    {requiredArg("owner", ArgKind::String, "owner label, at most 64 bytes"),
                     optionalArg("ttlMs", ArgKind::Integer, "lease duration in ms, 1000..600000", "120000")},
                    updateLease);
            else
                registerViewCommand(
                    "renewEditorLease", OWNER, "renews this attached session's editor lease",
                    {optionalArg("ttlMs", ArgKind::Integer, "lease duration in ms, 1000..600000", "120000")},
                    updateLease);
        }
        registerViewCommand("releaseEditorLease", OWNER,
                            "releases this session's editor lease; local callers may override", {},
                            [&state](const ControlArgs&, IFormatWriter& out)
                            {
                                state.refreshEditorLease();
                                if (state.editorLease && state.runningTransport != State::LOCAL &&
                                    state.editorLease->transport != state.runningTransport)
                                    return std::string("editor lease is held by another session");
                                state.editorLease.reset();
                                state.writeEditorLease(out);
                                return std::string{};
                            });

        // hello client=: names the connection's client for every request it sends after
        // a view command: it changes no scene state, so panels mirroring that state do not re-read it
        registerViewCommand(
            "hello", OWNER, "names this connection's client; history and provenance attribute its requests to it",
            {requiredArg("client", ArgKind::String, "the client's name, at most 64 bytes; empty for none")},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                if (state.runningTransport == State::LOCAL)
                {
                    return "hello names a pipe or stdio client; in-process requests are attributed to '" +
                        std::string(sourceName(state.localSource)) + "'";
                }
                std::string client = args.string("client").value_or(std::string());
                if (client.size() > MAX_CLIENT_NAME)
                {
                    return std::string("client is at most 64 bytes");
                }
                if (state.runningTransport == State::STDIO)
                {
                    state.stdioClient = client;
                    state.attachedEpoch.fetch_add(1, std::memory_order_release);
                }
                else
                {
                    state.pipeClientConnection = state.runningTransport;
                    state.pipeClient = client;
                    state.attachedEpoch.fetch_add(1, std::memory_order_release);
                }
                out.writeString("source", state.runningTransport == State::STDIO ? "stdio" : "pipe");
                out.writeString("client", client);
                return std::string{};
            });

        registerQuery("editorLease", OWNER,
                      "editor lease: held indicates an active lease; owner is its label, transport is local, stdio "
                      "or pipe; heartbeatMs and expiresAtMs are activity-clock milliseconds, remainingMs is time "
                      "until expiry; metadata is empty or zero when unheld",
                      {},
                      [&state](const ControlArgs&, IFormatWriter& out)
                      {
                          state.writeEditorLease(out);
                          return std::string{};
                      });

        // provenance [id]: what the last scene-edit steps did to each entity, and what is unreviewed
        registerQuery("provenance", OWNER,
                      "per edited entity: its last scene-edit step, that step's source, time and fields, and the "
                      "unreviewed fields from applied non-editor edits since the last review",
                      {WORLD_ARG, optionalArg("id", ArgKind::Entity, "one entity", "every edited entity"),
                       optionalArg("previous", ArgKind::Boolean,
                                   "with id: each row also lists `previous`, the value the last step's sets "
                                   "overwrote (field, value; valueBytes past 256 bytes), the first write per "
                                   "field, and `stepEntities`, how many entities that step touched",
                                   "false")},
                      [&state](const ControlArgs& args, IFormatWriter& out)
                      {
                          const State::WorldEntry* entry = state.findEntry(args);
                          if (!entry)
                          {
                              return state.unknownWorld(args);
                          }
                          SPITE_ZONE("Control.provenance");
                          // one entity is one lookup, the all form one walk; neither sweeps
                          const std::optional<u64> only = args.integer("id");
                          std::vector<std::pair<u64, const EntityProvenance*>> rows;
                          const auto& table = entry->history->provenance();
                          {
                              SPITE_ZONE("Control.provenance.collectRows");
                              if (only)
                              {
                                  if (const auto found = table.find(*only);
                                      found != table.end() && entry->world.isEntityAlive(Entity(*only)))
                                  {
                                      rows.emplace_back(*only, &found->second);
                                  }
                              }
                              else
                              {
                                  for (const auto& [id, provenance] : table)
                                  {
                                      // a deleted entity an undo can bring back keeps its entry, unlisted
                                      if (entry->world.isEntityAlive(Entity(id)))
                                      {
                                          rows.emplace_back(id, &provenance);
                                      }
                                  }
                              }
                          }
                          {
                              SPITE_ZONE("Control.provenance.sort");
                              SPITE_COUNTER("control.provenance.rows", rows.size());
                              std::ranges::sort(rows, {}, &std::pair<u64, const EntityProvenance*>::first);
                          }
                          SPITE_ZONE("Control.provenance.writeRows");
                          const auto writeNames =
                              [&out](const std::string_view key, const std::vector<std::string>& names)
                          {
                              out.beginArray(key, static_cast<u32>(names.size()));
                              for (const std::string& name : names)
                              {
                                  out.writeString("", name);
                              }
                              out.endArray();
                          };
                          out.writeString("world", entry->name);
                          out.beginArray("entities", static_cast<u32>(rows.size()));
                          for (const auto& [id, provenance] : rows)
                          {
                              out.beginObject("");
                              out.writeU64("id", id);
                              out.writeU64("step", provenance->step);
                              writeAttribution(provenance->by, out);
                              writeNames("fields", provenance->fields);
                              out.beginArray("unreviewed", static_cast<u32>(provenance->unreviewed.size()));
                              for (const auto& field : provenance->unreviewed)
                              {
                                  out.writeString("", field.name);
                              }
                              out.endArray();
                              out.writeBool("reviewed", provenance->unreviewed.empty());
                              if (only && args.boolean("previous").value_or(false))
                              {
                                  // what the last step overwrote (the value each set's inverse restores), first
                                  // write per path: the all form stays a table of attributions
                                  const CommandHistory& history = *entry->history;
                                  std::vector<std::pair<std::string, const nlohmann::json*>> previous;
                                  if (const std::optional<sizet> at = history.find(provenance->step))
                                  {
                                      for (const CommandRecord& record : history.steps()[*at].records)
                                      {
                                          for (const CommandRequest& inverse : record.inverse)
                                          {
                                              const ControlArgs inverseArgs(&inverse.args);
                                              const std::optional<u64> target = inverseArgs.integer("id");
                                              const std::optional<std::string> path = inverseArgs.string("path");
                                              if (inverse.command != "set" || !target || !path ||
                                                  !inverse.args.contains("value") || history.resolve(*target) != id ||
                                                  std::ranges::any_of(previous, [&](const auto& seen)
                                                                      { return seen.first == *path; }))
                                              {
                                                  continue;
                                              }
                                              previous.emplace_back(*path, &inverse.args["value"]);
                                          }
                                      }
                                  }
                                  const std::optional<sizet> stepAt = history.find(provenance->step);
                                  out.writeU64("stepEntities",
                                               stepAt ? history.touchedFields(history.steps()[*stepAt]).size() : 0);
                                  out.beginArray("previous", static_cast<u32>(previous.size()));
                                  for (const auto& [field, value] : previous)
                                  {
                                      out.beginObject("");
                                      out.writeString("field", field);
                                      // a long value is named by its size, as history's args are
                                      if (const std::string text = value->dump(); text.size() > SUMMARY_BYTES)
                                      {
                                          out.writeU64("valueBytes", text.size());
                                      }
                                      else
                                      {
                                          writeJson("value", *value, out);
                                      }
                                      out.endObject();
                                  }
                                  out.endArray();
                              }
                              out.endObject();
                          }
                          out.endArray();
                          return std::string{};
                      });

        // review [id]: a person saw the non-editor edits; changes no scene state, so neither undone nor logged
        registerViewCommand("review", OWNER, "marks an entity's non-editor edits reviewed (provenance)",
                            {WORLD_ARG, optionalArg("id", ArgKind::Entity, "the entity", "every edited entity")},
                            [&state](const ControlArgs& args, IFormatWriter& out)
                            {
                                const State::WorldEntry* entry = state.findEntry(args);
                                if (!entry)
                                {
                                    return state.unknownWorld(args);
                                }
                                out.writeString("world", entry->name);
                                const std::optional<u64> id = args.integer("id");
                                const u32 reviewed =
                                    entry->history->review(id, [&world = entry->world](const u64 value)
                                                           { return world.isEntityAlive(Entity(value)); });
                                out.writeU32("reviewed", reviewed);
                                if (reviewed != 0 && (!id || State::timelineEntity(*entry, id)))
                                    state.noteAgentReview(*entry, "review");
                                if (reviewed != 0)
                                    state.updateActivityReviews(*entry);
                                return std::string{};
                            });
        // saveLog path=: the world's command log as request lines `spite run`
        // replays into a fresh engine (Command log); refused when
        // rows were dropped, as that replay would diverge. The
        // path is a file under the captures root, like snapshot's.
        registerQuery("saveLog", OWNER, "writes the world's command log as request lines `spite run` replays",
                      {WORLD_ARG, requiredPath("path", "captures", "the file to write, relative to the captures root")},
                      [&state](const ControlArgs& args, IFormatWriter& out)
                      {
                          const State::WorldEntry* entry = state.findEntry(args);
                          if (!entry)
                          {
                              return state.unknownWorld(args);
                          }
                          const std::optional<std::string> path = args.string("path");
                          if (!path)
                          {
                              return std::string("missing arg 'path'");
                          }
                          std::filesystem::path file;
                          if (std::string error = state.resolveRooted("captures", *path, file); !error.empty())
                          {
                              return error;
                          }
                          const CommandHistory& history = *entry->history;
                          if (history.droppedLogRows() != 0)
                          {
                              return "the log of world '" + entry->name + "' dropped " +
                                  std::to_string(history.droppedLogRows()) + " rows; it no longer replays";
                          }
                          // the applied rows, each with its attribution so the replay keeps it
                          std::string text;
                          u32 requests = 0;
                          for (const CommandLogRow& row : history.logRows())
                          {
                              if (row.applied())
                              {
                                  text += R"({"command":")" + row.command + R"(","source":)" +
                                      nlohmann::json(row.source).dump() + R"(,"client":)" +
                                      nlohmann::json(row.client).dump() + R"(,"args":)" + row.args + "}\n";
                                  ++requests;
                              }
                          }
                          if (file.has_parent_path())
                          {
                              if (const Expected<void> made = ensureDirectory(file.parent_path()); !made)
                              {
                                  return std::string(made.error().message.c_str());
                              }
                          }
                          if (const Expected<void> written = writeTextFileAtomically(file, text); !written)
                          {
                              return std::string(written.error().message.c_str());
                          }
                          out.writeString("world", entry->name);
                          out.writeString("path", utf8Path(file));
                          out.writeU32("requests", requests);
                          return std::string{};
                      });

        registerQuery("worlds", OWNER, "the worlds with their simulation state and ECS stats", {},
                      [&state](const ControlArgs&, IFormatWriter& out)
                      {
                          out.beginArray("worlds", static_cast<u32>(state.worlds.size()));
                          for (const State::WorldEntry& entry : state.worlds)
                          {
                              out.beginObject("");
                              out.writeString("name", entry.name);
                              writeSimulation(entry.world.simulationState(), out);
                              const EcsStats stats = entry.world.getStats();
                              out.beginObject("stats");
                              writeValue(EcsStats::spiteSchema(), &stats, out, FieldSet::All);
                              out.endObject();
                              out.endObject();
                          }
                          out.endArray();
                          return std::string{};
                      });

        registerQuery("services", OWNER, "the world's services and their tables, then the root container's",
                      {WORLD_ARG},
                      [&state](const ControlArgs& args, IFormatWriter& out)
                      {
                          const World* world = state.findWorld(args);
                          if (!world)
                          {
                              return state.unknownWorld(args);
                          }
                          // the world's own services, then the root container's
                          const std::vector<ServiceInfo> services = world->services().listServices();
                          out.beginArray("services", static_cast<u32>(services.size()));
                          for (const ServiceInfo& service : services)
                          {
                              out.beginObject("");
                              out.writeString("name", service.name);
                              out.writeU32("depth", service.depth);
                              out.beginArray("tables", static_cast<u32>(service.tables.size()));
                              for (const ServiceTableInfo& table : service.tables)
                              {
                                  out.writeString("", table.name);
                              }
                              out.endArray();
                              out.endObject();
                          }
                          out.endArray();
                          return std::string{};
                      });

        registerQuery("tables", OWNER, "the schema components and the world's service tables", {WORLD_ARG},
                      [&state](const ControlArgs& args, IFormatWriter& out)
                      {
                          std::vector<ComponentID> ids(getRegisteredComponentCount());
                          ids.resize(getComponentIds(ids.data(), static_cast<u32>(ids.size())));
                          std::vector<const ComponentMetadata*> described;
                          for (const ComponentID id : ids)
                          {
                              const ComponentMetadata& metadata = getComponentMetadata(id);
                              if (metadata.schema)
                              {
                                  described.push_back(&metadata);
                              }
                          }
                          out.beginArray("components", static_cast<u32>(described.size()));
                          for (const ComponentMetadata* metadata : described)
                          {
                              out.beginObject("");
                              out.writeString("name", metadata->name.toView());
                              out.writeString("category", componentCategoryName(metadata->category));
                              out.writeU32("fields", static_cast<u32>(metadata->schema->fields.size()));
                              out.endObject();
                          }
                          out.endArray();

                          const World* world = state.findWorld(args);
                          if (!world)
                          {
                              return state.unknownWorld(args);
                          }
                          const std::vector<ServiceInfo> services = world->services().listServices();
                          u32 serviceTableCount = 0;
                          for (const ServiceInfo& service : services)
                          {
                              serviceTableCount += static_cast<u32>(service.tables.size());
                          }
                          out.beginArray("serviceTables", serviceTableCount);
                          for (const ServiceInfo& service : services)
                          {
                              for (const ServiceTableInfo& table : service.tables)
                              {
                                  out.beginObject("");
                                  out.writeString("service", service.name);
                                  out.writeString("table", table.name);
                                  out.endObject();
                              }
                          }
                          out.endArray();
                          return std::string{};
                      });

        registerQuery(
            "tableRows", OWNER,
            "the rows of one service table of a world, each as the table's schema fields (services and tables name "
            "them, e.g. EcsTables archetypes); total counts every row, matched those the filter keeps, fields names "
            "the columns and rows holds up to limit of the matched rows from the first-th. A filter reads every row "
            "of the table once per call and matches the text a viewer shows (floats to 6 digits, vectors as (x, y, z))",
            {WORLD_ARG, requiredArg("service", ArgKind::String, "the service, as services names it"),
             requiredArg("table", ArgKind::String, "the table, as services lists it"),
             optionalArg("limit", ArgKind::Integer, "rows, 0..10000; 0 counts only", "100"),
             optionalArg("first", ArgKind::Integer, "matched rows to skip: the page's offset", "0"),
             optionalArg("filter", ArgKind::String, "keeps the rows with a field whose text holds this, ignoring case",
                         "every row"),
             optionalArg("column", ArgKind::String,
                         "the one field filter searches (refused if the table has no such field)", "every field")},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                const World* world = state.findWorld(args);
                if (!world)
                {
                    return state.unknownWorld(args);
                }
                const std::string service = args.string("service").value_or("");
                const std::string table = args.string("table").value_or("");
                constexpr u64 MAX_ROWS = 10000;
                const std::optional<u64> limitArg = args.integer("limit");
                if ((args.json("limit") && !limitArg) || limitArg.value_or(0) > MAX_ROWS)
                {
                    return "limit must be an integer 0.." + std::to_string(MAX_ROWS);
                }
                const std::optional<u64> firstArg = args.integer("first");
                if (args.json("first") && !firstArg)
                {
                    return std::string("first must be an integer 0 or more");
                }
                const u64 limit = limitArg.value_or(100);
                const u64 first = firstArg.value_or(0);
                const StructSchema* schema = nullptr;
                std::string known;
                u32 listed = 0;
                for (const ServiceInfo& info : world->services().listServices())
                {
                    for (const ServiceTableInfo& candidate : info.tables)
                    {
                        if (info.name == service && candidate.name == table)
                        {
                            schema = candidate.rowSchema;
                        }
                        constexpr u32 MAX_LISTED = 24;
                        if (listed++ < MAX_LISTED)
                        {
                            known += " " + std::string(info.name) + "." + std::string(candidate.name);
                        }
                    }
                }
                if (!schema)
                {
                    return "no table '" + service + "." + table + "'; tables:" + known + (listed > 24 ? " ..." : "");
                }
                const std::string needle = args.string("filter").value_or("");
                const std::string column = args.string("column").value_or("");
                if (!column.empty() &&
                    std::ranges::none_of(schema->fields, [&column](const FieldDesc& f) { return f.name == column; }))
                {
                    std::string fields;
                    for (const FieldDesc& field : schema->fields)
                    {
                        fields += (fields.empty() ? "" : ", ") + std::string(field.name);
                    }
                    return "no field '" + column + "' in " + service + "." + table + "; fields: " + fields;
                }
                RowFilter rowFilter(needle, column);
                const auto keeps = [&](const void* row)
                {
                    if (needle.empty())
                    {
                        return true;
                    }
                    rowFilter.begin();
                    writeValue(*schema, row, rowFilter, FieldSet::All);
                    return rowFilter.hit();
                };
                // one pass counts, filters and picks the page's source rows: a table's rows exist only in its query
                u64 total = 0;
                u64 matched = 0;
                std::vector<u64> page;
                world->services().queryTable(service, table,
                                             [&](const void* row)
                                             {
                                                 const u64 index = total++;
                                                 if (keeps(row))
                                                 {
                                                     if (matched >= first && page.size() < limit)
                                                     {
                                                         page.push_back(index);
                                                     }
                                                     ++matched;
                                                 }
                                             });
                out.writeU64("total", total);
                out.writeU64("matched", matched);
                out.writeU64("first", first);
                out.beginArray("fields", static_cast<u32>(schema->fields.size()));
                for (const FieldDesc& field : schema->fields)
                {
                    out.writeString("", field.name);
                }
                out.endArray();
                out.beginArray("rows", static_cast<u32>(page.size()));
                u64 index = 0;
                sizet next = 0;
                world->services().queryTable(service, table,
                                             [&](const void* row)
                                             {
                                                 if (next < page.size() && page[next] == index++)
                                                 {
                                                     ++next;
                                                     out.beginObject("");
                                                     writeValue(*schema, row, out, FieldSet::All);
                                                     out.endObject();
                                                 }
                                             });
                out.endArray();
                return std::string{};
            });

        registerQuery(
            "components", OWNER,
            "every registered component with its category (game: authored by a game or module; engine: engine-defined "
            "authored data; runtimeDerived: maintained by engine systems; ui: UI-layer and editor panel state), size, "
            "field schema (name, kind, flags; when declared unit, min and max (a range set refuses a number outside "
            "of), step and enum) and how many of the world's entities have it; the Inspector's Add list is game plus "
            "engine",
            {WORLD_ARG,
             optionalArg("category", ArgKind::String, "only components of this category", "every category",
                         COMPONENT_CATEGORY_NAMES),
             optionalArg("name", ArgKind::String, "only this component", "every component"),
             optionalArg("counts", ArgKind::Boolean, "false skips the entity counts, a walk of every chunk each",
                         "true")},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                const World* world = state.findWorld(args);
                if (!world)
                {
                    return state.unknownWorld(args);
                }
                const bool counts = args.boolean("counts").value_or(true);
                // the server already refused a category outside the choices
                const std::optional<std::string> wanted = args.string("category");
                const std::optional<ComponentCategory> category =
                    wanted ? parseComponentCategory(*wanted) : std::nullopt;
                const std::optional<std::string> only = args.string("name");
                std::vector<ComponentID> ids(getRegisteredComponentCount());
                ids.resize(getComponentIds(ids.data(), static_cast<u32>(ids.size())));
                std::erase_if(ids,
                              [&](const ComponentID id)
                              {
                                  const ComponentMetadata& metadata = getComponentMetadata(id);
                                  // an id retired by a layout change shares its name with the live one
                                  return getComponentIdByName(metadata.name.toView().data()) != id ||
                                      (category && metadata.category != *category) ||
                                      (only && metadata.name.toView() != *only);
                              });
                if (only && ids.empty())
                {
                    return "unknown component '" + *only + "'";
                }
                out.beginArray("components", static_cast<u32>(ids.size()));
                for (const ComponentID id : ids)
                {
                    const ComponentMetadata& metadata = getComponentMetadata(id);
                    out.beginObject("");
                    out.writeString("name", metadata.name.toView());
                    out.writeString("category", componentCategoryName(metadata.category));
                    out.writeU64("size", metadata.size);
                    out.writeBool("loaded", metadata.moveAndDestroy != nullptr);
                    if (counts)
                    {
                        out.writeU32("entities", countEntitiesWith(*world, id));
                    }
                    if (metadata.schema)
                    {
                        writeFieldSchema(*metadata.schema, out);
                    }
                    out.endObject();
                }
                out.endArray();
                return std::string{};
            });

        // the EcsTables.systems rows as JSON, so a module reload shows its systems gone and back
        registerQuery("systems", OWNER,
                      "the world's registered systems: module, enabled state, required (the frame cannot run without "
                      "it: setSystemEnabled refuses to turn it off), the phases they have (update, fixed) "
                      "and per phase its stage (number and name), the systems it runs after (Module::Name) and the "
                      "components it reads and writes, and per phase the scheduler's measured cost in ms per frame "
                      "(updateAvgMs, updateMaxMs, fixedAvgMs, fixedMaxMs: wall time over the last 64 frames, zero "
                      "while the system is disabled or the phase skipped)",
                      {WORLD_ARG},
                      [&state](const ControlArgs& args, IFormatWriter& out)
                      {
                          const World* world = state.findWorld(args);
                          if (!world)
                          {
                              return state.unknownWorld(args);
                          }
                          std::vector<SystemRow> rows;
                          world->services().queryTable("EcsTables", "systems", [&rows](const void* row)
                                                       { rows.push_back(*static_cast<const SystemRow*>(row)); });
                          const auto writeList = [&out](const char* key, const std::string& joined)
                          {
                              if (joined.empty())
                              {
                                  out.beginArray(key, 0);
                                  out.endArray();
                                  return;
                              }
                              out.beginArray(key, static_cast<u32>(std::ranges::count(joined, ',') + 1));
                              for (const auto part : std::views::split(joined, ','))
                              {
                                  out.writeString("", std::string_view(part.begin(), part.end()));
                              }
                              out.endArray();
                          };
                          out.beginArray("systems", static_cast<u32>(rows.size()));
                          for (const SystemRow& row : rows)
                          {
                              out.beginObject("");
                              out.writeString("module", row.module);
                              out.writeString("name", row.name);
                              out.writeBool("enabled", row.enabled);
                              out.writeBool("required", row.required);
                              out.writeU64("updateStage", row.updateStage);
                              out.writeU64("fixedStage", row.fixedStage);
                              writeList("phases", row.phases);
                              out.writeString("updateStageName", row.updateStageName);
                              writeList("updateRunsAfter", row.updateRunsAfter);
                              writeList("updateReads", row.updateReads);
                              writeList("updateWrites", row.updateWrites);
                              out.writeString("fixedStageName", row.fixedStageName);
                              writeList("fixedRunsAfter", row.fixedRunsAfter);
                              writeList("fixedReads", row.fixedReads);
                              writeList("fixedWrites", row.fixedWrites);
                              out.writeF32("updateAvgMs", row.updateAvgMs);
                              out.writeF32("updateMaxMs", row.updateMaxMs);
                              out.writeF32("fixedAvgMs", row.fixedAvgMs);
                              out.writeF32("fixedMaxMs", row.fixedMaxMs);
                              out.endObject();
                          }
                          out.endArray();
                          return std::string{};
                      });

        // inverse: setSystemEnabled with the result's previous
        registerUndoableViewCommand(
            "setSystemEnabled", OWNER,
            "enables or disables a registered system of a world: a disabled system is skipped by every stage until "
            "enabled again (a module reload registers it anew, in the state the module declares); a required "
            "system (the systems query's required) is refused, changing nothing, and so is enabling one that would "
            "close a dependency cycle among the active systems (the error names the cycle); the result carries its "
            "previous state",
            {requiredArg("name", ArgKind::String, "the system, as the systems query names it"),
             optionalArg("module", ArgKind::String,
                         "the system's module ('' = the local module); omit it when the name is unique", "any module"),
             requiredArg("enabled", ArgKind::Boolean, "on or off")},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                const World* world = state.findWorld(args);
                if (!world)
                {
                    return state.unknownWorld(args);
                }
                const std::optional<std::string> name = args.string("name");
                const std::optional<bool> enabled = args.boolean("enabled");
                if (!name || !enabled)
                {
                    return std::string("setSystemEnabled needs name and enabled (true or false)");
                }
                const std::optional<std::string> module = args.string("module");
                const std::vector<SystemDescriptor> systems = world->getSystems();
                const auto qualified = [](const SystemDescriptor& system)
                {
                    return system.moduleName && system.moduleName[0]
                        ? std::string(system.moduleName) + "::" + system.name
                        : std::string(system.name);
                };
                std::vector<const SystemDescriptor*> found;
                for (const SystemDescriptor& system : systems)
                {
                    if (*name == system.name && (!module || *module == (system.moduleName ? system.moduleName : "")))
                    {
                        found.push_back(&system);
                    }
                }
                if (found.size() != 1)
                {
                    std::string message = found.empty() ? "no system '" + *name + "'; systems:"
                                                        : "system '" + *name + "' is in several modules, pass module:";
                    if (found.empty())
                    {
                        for (const SystemDescriptor& system : systems)
                        {
                            message += " " + qualified(system);
                        }
                    }
                    for (const SystemDescriptor* system : found)
                    {
                        message += " " + qualified(*system);
                    }
                    return message;
                }
                const SystemDescriptor& system = *found.front();
                if (system.required && !*enabled)
                {
                    std::string message = "system '" + qualified(system) +
                        "' is required: the frame cannot run without it (disabling it would stop every frame and "
                        "this control channel); required systems:";
                    for (const SystemDescriptor& other : systems)
                    {
                        if (other.required)
                        {
                            message += " " + qualified(other);
                        }
                    }
                    return message;
                }
                const SystemName target{system.moduleName, system.name};
                if (*enabled)
                {
                    if (const std::string cycle = world->systemEnableCycle(target); !cycle.empty())
                    {
                        return "system '" + qualified(system) +
                            "' stays disabled: enabling it closes the dependency cycle " + cycle +
                            " (those systems would wait for each other for good)";
                    }
                }
                const bool previous = system.enabled;
                world->setSystemEnabled(target, *enabled);
                state.record("setSystemEnabled",
                             {{"module", system.moduleName ? system.moduleName : ""},
                              {"name", system.name},
                              {"enabled", previous}},
                             {});
                out.writeString("module", system.moduleName ? system.moduleName : "");
                out.writeString("name", system.name);
                out.writeBool("enabled", *enabled);
                out.writeBool("previous", previous);
                return std::string{};
            });

        registerQuery("snapshot", OWNER, "writes the world's component and service tables to a file",
                      {WORLD_ARG, requiredPath("path", "captures", "the file to write, relative to the captures root"),
                       optionalArg("tables", ArgKind::Json, "an array of table names", "every table")},
                      [&state](const ControlArgs& args, IFormatWriter& out)
                      {
                          const World* world = state.findWorld(args);
                          if (!world)
                          {
                              return state.unknownWorld(args);
                          }
                          const std::optional<std::string> path = args.string("path");
                          if (!path)
                          {
                              return std::string("missing arg 'path'");
                          }
                          std::filesystem::path file;
                          if (std::string error = state.resolveRooted("captures", *path, file); !error.empty())
                          {
                              return error;
                          }
                          std::vector<std::string> only;
                          if (const std::optional<std::string> tables = args.json("tables"))
                          {
                              const nlohmann::json list = nlohmann::json::parse(*tables, nullptr, false);
                              if (!list.is_array() || !std::ranges::all_of(list, &nlohmann::json::is_string))
                              {
                                  return std::string("'tables' must be an array of names");
                              }
                              for (const nlohmann::json& name : list)
                              {
                                  only.push_back(name.get<std::string>());
                              }
                          }
                          if (const Expected<void> made = ensureDirectory(file.parent_path()); !made)
                          {
                              return std::string(made.error().message.c_str());
                          }
                          return writeSnapshot(*world, &world->services(), utf8Text(file), only, out);
                      });

        registerQuery("entity", OWNER,
                      "one entity's components with fields, and parent, children, enabled, active, layer (the scene "
                      "layer of its root; 0 is the open scene)",
                      {WORLD_ARG, requiredArg("id", ArgKind::Entity, "the entity")},
                      [&state](const ControlArgs& args, IFormatWriter& out)
                      {
                          const World* world = state.findWorld(args);
                          if (!world)
                          {
                              return state.unknownWorld(args);
                          }
                          const std::optional<u64> id = args.integer("id");
                          if (!id)
                          {
                              return std::string("missing integer arg 'id'");
                          }
                          const Entity entity(*id);
                          if (!world->isEntityAlive(entity))
                          {
                              return std::string("no live entity ") + std::to_string(*id);
                          }
                          out.writeU64("id", entity.id());
                          if (const State::WorldEntry* entry = state.findEntry(args); entry && entry->source)
                          {
                              if (const u64 source = entry->source(entity.id()))
                              {
                                  out.writeU64("source", source);
                              }
                          }
                          state.writeRelation(*world, entity, out);
                          writeEntityComponents(*world, entity, out);
                          return std::string{};
                      });

        registerQuery("find", OWNER,
                      "entities by name (exact, glob, or text, which also matches component names) and component "
                      "presence, paged by id; "
                      "each row has parent (none for a root), "
                      "children (the direct child count), enabled (its own flag), active (enabled and every "
                      "ancestor enabled) and layer (the scene layer of its root; 0 is the open scene); inactive "
                      "entities are listed too",
                      {WORLD_ARG, optionalArg("name", ArgKind::String, "the exact name; not with match"),
                       optionalArg("match", ArgKind::String, "a name glob: * any run, ? any one character"),
                       optionalArg("text", ArgKind::String,
                                   "the entity's name or one of its components' names contains this text (ASCII case "
                                   "ignored); the Hierarchy panel's search"),
                       optionalArg("with", ArgKind::Json, "a component name or array of them the entities have"),
                       optionalArg("without", ArgKind::Json, "a component name or array of them they lack"),
                       optionalArg("values", ArgKind::Json, "components whose fields each row carries"),
                       optionalArg("after", ArgKind::Entity, "the previous page's next: the page starts past it"),
                       optionalArg("limit", ArgKind::Integer, "rows, 0..10000; 0 counts only", "100")},
                      [&state](const ControlArgs& args, IFormatWriter& out)
                      {
                          const State::WorldEntry* entry = state.findEntry(args);
                          if (!entry)
                          {
                              return state.unknownWorld(args);
                          }
                          const std::function<void(Entity, IFormatWriter&)> relation =
                              [&state, &world = entry->world](const Entity row, IFormatWriter& writer)
                          { state.writeRelation(world, row, writer); };
                          return findEntities(entry->world, args, out, &entry->source, &relation);
                      });

        registerQuery("hierarchyAudit", OWNER,
                      "the scene's (non-UI) entities with no name and its transform entities in no tree, for "
                      "cleaning up a module's layout: entities, roots (no parent, with children), unrooted (a "
                      "transform, no parent, no children), unnamed (no EntityNameComponent or an empty name), and "
                      "groups, those with an unrooted or unnamed entity, most first: group (the game components of "
                      "the archetype, joined by +), entities, roots, unrooted, unnamed, unrootedSample and "
                      "unnamedSample (entity ids); groupCount counts all such groups. A layout command's reply "
                      "carries audit and warnings for what it created",
                      {WORLD_ARG, optionalArg("groups", ArgKind::Integer, "groups listed, 0..1000", "20"),
                       optionalArg("samples", ArgKind::Integer, "entity ids per sample, 0..100", "3")},
                      [&state](const ControlArgs& args, IFormatWriter& out)
                      {
                          const World* world = state.findWorld(args);
                          if (!world)
                          {
                              return state.unknownWorld(args);
                          }
                          return auditEntities(*world, args, out, &state.hierarchy);
                      });
#endif
    }

    ControlServer::~ControlServer()
    {
#if defined(_WIN32)
        State& state = *m_state;
        if (state.reader.joinable())
        {
            state.stopping.store(true);
            // A cancel issued before the reader enters ReadFile is lost, so it
            // repeats until the reader has left its loop.
            while (!state.inputClosed.load(std::memory_order_acquire))
            {
                CancelSynchronousIo(state.reader.native_handle());
                std::this_thread::yield();
            }
            state.reader.join();
        }
        if (state.pipeThread.joinable())
        {
            SetEvent(state.stopEvent);
            state.pipeThread.join();
        }
        if (state.stopEvent)
        {
            CloseHandle(state.stopEvent);
            CloseHandle(state.writeEvent);
        }
#elif !defined(SPITE_SHIPPING)
        State& state = *m_state;
        if (state.reader.joinable())
        {
            const char wake = 0;
            while (::write(state.stdioCancel[1], &wake, 1) < 0 && errno == EINTR)
            {
            }
            state.reader.join();
            ::close(state.stdioCancel[0]);
            ::close(state.stdioCancel[1]);
        }
#endif
    }

    void ControlServer::addWorld(const std::string_view name, const World world, SourceFn source)
    {
        CommandHistory& history = CommandHistory::attach(world.services());
        history.setLimits(m_state->undoLimits);
        EcsTables::attach(world, world.services());
        m_state->worlds.push_back({std::string(name), world, &history, m_state->nextWorldSerial++, std::move(source)});
    }

    void ControlServer::removeWorld(const std::string_view name)
    {
        std::erase_if(m_state->worlds,
                      [this, name](const State::WorldEntry& entry)
                      {
                          if (entry.name != name)
                          {
                              return false;
                          }
                          if (entry.timeline.phase != AgentTimelinePhase::None)
                              ++m_state->agentTimelineRevision;
                          CommandHistory::detach(entry.world.services());
                          EcsTables::detach(entry.world.services());
                          return true;
                      });
    }

    const World* ControlServer::world(const ControlArgs& args) const { return m_state->findWorld(args); }

    std::string ControlServer::unknownWorld(const ControlArgs& args) const { return m_state->unknownWorld(args); }

    std::string ControlServer::unknownWorld(const std::string_view name) const { return m_state->unknownWorld(name); }

    const World* ControlServer::world(const std::string_view name) const
    {
        const auto it = std::ranges::find(m_state->worlds, name, &State::WorldEntry::name);
        return it == m_state->worlds.end() ? nullptr : &it->world;
    }

    std::string ControlServer::worldName(const ControlArgs& args) const
    {
        const State::WorldEntry* entry = m_state->findEntry(args);
        return entry ? entry->name : std::string{};
    }

    u64 ControlServer::commandGeneration() const { return m_state->commandGeneration; }
    ControlServer::BatchStatus ControlServer::batchStatus() const { return m_state->batchStatus; }

    u64 ControlServer::agentTimelineRevision() const { return m_state->agentTimelineRevision; }

    ControlServer::AgentTimeline ControlServer::agentTimeline(const std::string_view world) const
    {
        const auto entry = std::ranges::find(m_state->worlds, world, &State::WorldEntry::name);
        return entry == m_state->worlds.end() ? AgentTimeline{} : entry->timeline;
    }

    u64 ControlServer::beginAgentActivity(const std::string_view kind, const std::string_view title,
                                          const std::string_view why)
    {
        return m_state->startActivity(kind, title, why);
    }

    void ControlServer::updateAgentActivity(const u64 id, const std::string_view phase, const std::string_view status,
                                            const std::string_view action, const std::string_view evidence,
                                            const bool reported)
    {
        m_state->updateActivity(id, phase, status, action, evidence, reported);
    }

    u64 ControlServer::currentAgentActivity() const { return m_state->runningActivity; }
    u64 ControlServer::agentActivityRevision() const { return m_state->activityRevision; }
    std::span<const ControlServer::AgentActivity> ControlServer::agentActivities() const { return m_state->activities; }

    const ControlServer::AgentActivity* ControlServer::agentActivityFocus() const
    {
        const auto& rows = m_state->activities;
        const auto active =
            std::find_if(rows.rbegin(), rows.rend(), [](const AgentActivity& row) { return row.active(); });
        return active != rows.rend() ? &*active : rows.empty() ? nullptr : &rows.back();
    }

    bool ControlServer::agentActivityCanReview(const u64 id) const
    {
        const AgentActivity* row = m_state->activity(id);
        return row && (row->state == "ready" || row->state == "done" || row->state == "passed") &&
            m_state->activityStep(*row);
    }

    bool ControlServer::agentActivityCanAcknowledge(const u64 id) const
    {
        const AgentActivity* row = m_state->activity(id);
        if (!row || (row->state != "failed" && row->state != "cancelled"))
            return false;
        const UndoStep* step = m_state->activityStep(*row);
        if (!step)
            return true;
        const auto world = std::ranges::find(m_state->worlds, row->worldSerial, &State::WorldEntry::serial);
        return !m_state->activityPendingReview(*world, *step);
    }

    std::vector<ControlServer::AgentChange> ControlServer::agentActivityChanges(const u64 id) const
    {
        constexpr sizet MAX_ROWS = 64, MAX_TEXT = 64;
        std::vector<AgentChange> rows;
        const AgentActivity* row = m_state->activity(id);
        const UndoStep* step = row ? m_state->activityStep(*row) : nullptr;
        if (!step)
            return rows;
        const CommandHistory& history =
            *std::ranges::find(m_state->worlds, row->worldSerial, &State::WorldEntry::serial)->history;
        const auto text = [](const nlohmann::json& value)
        {
            std::string dumped = value.dump();
            return dumped.size() <= MAX_TEXT ? std::move(dumped) : std::string{};
        };
        for (const CommandRecord& record : step->records)
        {
            const ControlArgs args(&record.forward.args);
            const u64 target = args.integer("id") ? history.resolve(*args.integer("id")) : 0;
            const std::optional<std::string> path = args.string("path");
            if (record.forward.command != "set" || !target || !path || !record.forward.args.contains("value"))
            {
                if (rows.size() < MAX_ROWS)
                    rows.push_back({.entity = target, .field = record.forward.command});
                continue;
            }
            const auto same =
                std::ranges::find_if(rows, [&](const AgentChange& seen)
                                     { return seen.set && seen.entity == target && seen.field == *path; });
            if (same != rows.end())
            {
                same->after = text(record.forward.args["value"]);
            }
            else if (rows.size() < MAX_ROWS)
            {
                AgentChange change{
                    .entity = target, .field = *path, .after = text(record.forward.args["value"]), .set = true};
                if (!record.inverse.empty() && record.inverse.front().args.contains("value"))
                    change.before = text(record.inverse.front().args["value"]);
                rows.push_back(std::move(change));
            }
        }
        return rows;
    }

    u64 ControlServer::agentActivityClockMs() const { return m_state->activityNowMs(); }

    void ControlServer::setAgentActivityClock(std::function<u64()> nowMs) { m_state->activityClock = std::move(nowMs); }

    ControlServer::AgentTraffic ControlServer::agentTraffic() const { return m_state->agentTraffic; }

    std::string ControlServer::handleAsClient(const std::string_view line)
    {
        State& state = *m_state;
        const u32 before = std::exchange(state.transport, State::STDIO);
        std::string reply = handle(line);
        state.transport = before;
        return reply;
    }

    u64 ControlServer::stateGeneration() const { return m_state->stateGeneration; }

    u64 ControlServer::pumpCount() const { return m_state->frames; }

    void ControlServer::suggestArgValues(const std::string_view entry, const std::string_view arg, SuggestFn values)
    {
        const auto handler = m_state->handlers.find(entry);
        if (handler == m_state->handlers.end())
        {
            return;
        }
        for (Arg& declared : handler->second.args)
        {
            if (declared.name == arg)
            {
                declared.suggest = std::move(values);
            }
        }
    }

    void ControlServer::setHierarchyReader(HierarchyReaderFn reader) { m_state->hierarchy = std::move(reader); }

    void ControlServer::auditLayout(const World& world, const std::span<const Entity> created, IFormatWriter& out) const
    {
        SPITE_ZONE("Control.auditLayout");
        SPITE_COUNTER("control.layout.created", created.size());
        auditCreated(world, created, out, &m_state->hierarchy);
    }

    void ControlServer::setComponentEditGuard(ComponentEditGuardFn guard)
    {
        m_state->componentGuard = std::move(guard);
    }

    void ControlServer::setComponentAddedHook(ComponentAddedFn hook) { m_state->componentAdded = std::move(hook); }

    void ControlServer::setRoot(const std::string_view name, std::filesystem::path directory,
                                const std::string_view doc)
    {
        State::Root& root = m_state->roots[std::string(name)];
        root.directory = std::move(directory);
        if (!doc.empty())
        {
            root.doc = doc;
        }
    }

    std::filesystem::path ControlServer::root(const std::string_view name) const
    {
        const auto it = m_state->roots.find(name);
        return it == m_state->roots.end() ? std::filesystem::path{} : it->second.directory;
    }

    std::string ControlServer::resolveFile(const std::string_view root, const std::string_view text,
                                           std::filesystem::path& out) const
    {
        return m_state->resolveRooted(root, text, out);
    }

    std::string ControlServer::resolveDirectory(const std::string_view root, const std::string_view text,
                                                std::filesystem::path& out) const
    {
        return m_state->resolveRooted(root, text, out, true);
    }

    std::vector<std::string> ControlServer::rootNames() const
    {
        std::vector<std::string> names;
        for (const auto& [name, root] : m_state->roots)
        {
            names.push_back(name);
        }
        return names;
    }

    std::string ControlServer::executeUndoableBatch(const std::string_view world, const std::string_view commandsJson,
                                                    IFormatWriter& out)
    {
        return executeUndoableBatchWithRevives(world, commandsJson, {}, out);
    }

    std::string ControlServer::executeUndoableBatchWithRevives(const std::string_view world,
                                                               const std::string_view commandsJson,
                                                               const std::string_view revivesJson, IFormatWriter& out)
    {
        nlohmann::json commands = nlohmann::json::parse(commandsJson, nullptr, false);
        if (!commands.is_array() || commands.empty())
            return "batch needs a non-empty command array";
        nlohmann::json args{{"world", world}, {"commands", std::move(commands)}};
        if (!revivesJson.empty())
        {
            const auto revives = nlohmann::json::parse(revivesJson, nullptr, false);
            if (!revives.is_array())
                return "batch revives must be an array";
            args["revives"] = revives;
        }
        return m_state->batch(ControlArgs(&args), out);
    }

    bool ControlServer::isEntityReferencePath(std::string_view path, bool list) const
    {
        ComponentID component = INVALID_COMPONENT_ID;
        FieldDesc field;
        u32 offset = 0;
        return resolvePath(path, component, field, offset).empty() &&
            field.kind == (list ? FieldKind::EntityList : FieldKind::Entity);
    }

    void ControlServer::recordInverse(const std::string_view command, const std::string_view argsJson,
                                      const std::span<const u64> revives)
    {
        nlohmann::json args = nlohmann::json::parse(argsJson, nullptr, false);
        SASSERTM(args.is_object(), "recordInverse({}) needs a JSON object of args.", command);
        m_state->record(std::string(command), std::move(args), {revives.begin(), revives.end()});
    }

    bool ControlServer::replayingHistory() const { return m_state->replaying > 0; }

    u64 ControlServer::resolveHistoryEntity(const std::string_view world, const u64 id) const
    {
        const CommandHistory* history = m_state->historyOf(world);
        return history ? history->resolve(id) : id;
    }

    void ControlServer::recordForward(const std::string_view argsJson)
    {
        nlohmann::json args = nlohmann::json::parse(argsJson, nullptr, false);
        SASSERTM(args.is_object(), "recordForward needs a JSON object of args.");
        if (State::Capture* capture = m_state->capture)
        {
            SASSERTM(capture->undoable, "recordForward was called by a command not registered undoable.");
            capture->forward = std::move(args);
        }
    }

    void ControlServer::recordReplay(const std::string_view argsJson)
    {
        if (State::Capture* capture = m_state->capture)
        {
            capture->replay = true;
            if (!argsJson.empty())
            {
                capture->replayArgs = nlohmann::json::parse(argsJson, nullptr, false);
                SASSERTM(capture->replayArgs->is_object(), "recordReplay needs a JSON object of args.");
            }
        }
    }

    void ControlServer::recordReplayIn(const std::string_view world, const std::string_view argsJson)
    {
        if (State::Capture* capture = m_state->capture)
        {
            capture->replay = true;
            capture->replayWorld = world;
            if (!argsJson.empty())
            {
                capture->replayArgs = nlohmann::json::parse(argsJson, nullptr, false);
                SASSERTM(capture->replayArgs->is_object(), "recordReplayIn needs a JSON object of args.");
            }
        }
    }

    void ControlServer::recordCreated(const u32 key, const Entity entity)
    {
        if (State::Capture* capture = m_state->capture)
        {
            if (capture->created.size() <= key)
            {
                capture->created.resize(key + 1, 0);
            }
            capture->created[key] = entity.id();
        }
    }

    void ControlServer::setEntityLayerReader(EntityLayerFn reader) { m_state->entityLayer = std::move(reader); }

    void ControlServer::touchLayer(const u32 layer, const bool saved)
    {
        if (State::Capture* capture = m_state->capture)
        {
            if (!capture->layers)
            {
                capture->layers = std::make_unique<State::Capture::LayerNotes>();
            }
            State::addLayer(capture->layers->touched, layer);
            if (saved)
            {
                capture->layers->saved.push_back(layer);
            }
        }
    }

    void ControlServer::markLayerSaved(const std::string_view world, const u32 layer)
    {
        if (CommandHistory* history = m_state->historyOf(world))
        {
            history->markLayerSaved(layer);
        }
    }

    void ControlServer::markLayerUnsaved(const std::string_view world, const u32 layer)
    {
        if (CommandHistory* history = m_state->historyOf(world))
        {
            history->markLayerUnsaved(layer);
        }
    }

    bool ControlServer::layerDirty(const std::string_view world, const u32 layer) const
    {
        if (const UndoStep* batch = m_state->batchStep; batch && m_state->batchWorld == world)
        {
            const auto has = [](const std::vector<u32>& list, const u32 id)
            { return std::ranges::find(list, id) != list.end(); };
            if (has(batch->layers, ALL_LAYERS) || (has(batch->layers, layer) && !has(batch->savedLayers, layer)))
            {
                return true;
            }
        }
        const CommandHistory* history = m_state->historyOf(world);
        return history && history->layerDirty(layer);
    }

    u64 ControlServer::layerState(const std::string_view world, const u32 layer) const
    {
        const CommandHistory* history = m_state->historyOf(world);
        return history ? history->layerState(layer) : 0;
    }

    u64 ControlServer::historyState(const std::string_view world) const
    {
        const CommandHistory* history = m_state->historyOf(world);
        return history ? history->state() : 0;
    }

    ControlServer::UndoDepth ControlServer::undoDepth(const std::string_view world) const
    {
        const CommandHistory* history = m_state->historyOf(world);
        if (!history)
        {
            return {};
        }
        return {static_cast<u32>(history->done()), static_cast<u32>(history->steps().size() - history->done()),
                history->revision()};
    }

    void ControlServer::visitProvenance(const std::string_view world,
                                        const std::function<void(const ProvenanceSummary&)>& visitor) const
    {
        SPITE_ZONE("Control.visitProvenance");
        const auto entry = std::ranges::find(m_state->worlds, world, &State::WorldEntry::name);
        if (entry == m_state->worlds.end())
            return;
        for (const auto& [id, provenance] : entry->history->provenance())
        {
            if (entry->world.isEntityAlive(Entity(id)))
            {
                visitor({id, provenance.by.source, provenance.by.client, provenance.by.time,
                         !provenance.unreviewed.empty()});
            }
        }
    }

    void ControlServer::clearHistory(const std::string_view world)
    {
        if (const auto entry = std::ranges::find(m_state->worlds, world, &State::WorldEntry::name);
            entry != m_state->worlds.end())
        {
            entry->history->clear();
            if (entry->timeline.phase != AgentTimelinePhase::None)
            {
                entry->timeline = {};
                ++m_state->agentTimelineRevision;
            }
        }
    }

    void ControlServer::setUndoLimits(const UndoLimits& limits)
    {
        m_state->undoLimits = limits;
        for (const State::WorldEntry& entry : m_state->worlds)
        {
            entry.history->setLimits(limits);
        }
    }

    namespace
    {
        std::vector<Arg> ownArgs(const std::initializer_list<ArgDesc> args, const bool undoable)
        {
            std::vector<Arg> owned;
            for (const ArgDesc& arg : args)
            {
                owned.push_back({std::string(arg.name),
                                 arg.kind,
                                 arg.required,
                                 std::string(arg.doc),
                                 std::string(arg.defaultValue),
                                 {arg.choices.begin(), arg.choices.end()},
                                 std::string(arg.root)});
            }
            if (undoable && std::ranges::find(owned, WORLD_ARG.name, &Arg::name) == owned.end())
            {
                owned.push_back({std::string(WORLD_ARG.name), WORLD_ARG.kind, false, std::string(WORLD_ARG.doc),
                                 std::string(WORLD_ARG.defaultValue)});
            }
            return owned;
        }
    } // namespace

    std::vector<std::string> ControlServer::takeRefusals(const std::string_view owner)
    {
        std::vector<std::string> taken;
        std::erase_if(m_state->refusals,
                      [&](auto& refusal)
                      {
                          if (refusal.first != owner)
                          {
                              return false;
                          }
                          taken.push_back(std::move(refusal.second));
                          return true;
                      });
        return taken;
    }

    void ControlServer::registerQuery(const std::string_view name, const std::string_view owner,
                                      const std::string_view doc, const std::initializer_list<ArgDesc> args,
                                      HandlerFn fn)
    {
        m_state->place(name,
                       State::Handler{false, false, std::string(owner), std::move(fn), true, std::string(doc),
                                      ownArgs(args, false)});
    }

    void ControlServer::registerCommand(const std::string_view name, const std::string_view owner,
                                        const std::string_view doc, const std::initializer_list<ArgDesc> args,
                                        HandlerFn fn)
    {
        m_state->place(name,
                       State::Handler{true, false, std::string(owner), std::move(fn), true, std::string(doc),
                                      ownArgs(args, false)});
    }

    void ControlServer::registerViewCommand(const std::string_view name, const std::string_view owner,
                                            const std::string_view doc, const std::initializer_list<ArgDesc> args,
                                            HandlerFn fn)
    {
        registerCommand(name, owner, doc, args, std::move(fn));
        if (const auto entry = m_state->handlers.find(name); entry->second.owner == owner)
        {
            entry->second.view = true;
        }
    }

    void ControlServer::registerUndoableCommand(const std::string_view name, const std::string_view owner,
                                                const std::string_view doc, const std::initializer_list<ArgDesc> args,
                                                HandlerFn fn)
    {
        m_state->place(
            name,
            State::Handler{true, true, std::string(owner), std::move(fn), true, std::string(doc), ownArgs(args, true)});
    }

    void ControlServer::setCommandPreflight(const std::string_view name, const std::string_view owner, PreflightFn fn)
    {
        const auto entry = m_state->handlers.find(name);
        if (entry != m_state->handlers.end() && entry->second.owner == owner && entry->second.undoable)
            entry->second.preflight = std::move(fn);
    }

    void ControlServer::registerUndoableViewCommand(const std::string_view name, const std::string_view owner,
                                                    const std::string_view doc,
                                                    const std::initializer_list<ArgDesc> args, HandlerFn fn)
    {
        registerUndoableCommand(name, owner, doc, args, std::move(fn));
        if (const auto entry = m_state->handlers.find(name); entry->second.owner == owner)
        {
            entry->second.view = true;
        }
    }

    void ControlServer::registerQuery(const std::string_view name, const std::string_view owner, HandlerFn fn)
    {
        m_state->place(name, State::Handler{false, false, std::string(owner), std::move(fn)});
    }

    void ControlServer::registerCommand(const std::string_view name, const std::string_view owner, HandlerFn fn)
    {
        m_state->place(name, State::Handler{true, false, std::string(owner), std::move(fn)});
    }

    void ControlServer::registerUndoableCommand(const std::string_view name, const std::string_view owner, HandlerFn fn)
    {
        m_state->place(name, State::Handler{true, true, std::string(owner), std::move(fn)});
    }

    void ControlServer::markUnavailable(const std::string_view name, const std::string_view reason)
    {
        m_state->unavailable.insert_or_assign(std::string(name), std::string(reason));
    }

    void ControlServer::withdraw(const std::string_view name, const std::string_view reason)
    {
        if (const auto it = m_state->handlers.find(name); it != m_state->handlers.end())
        {
            m_state->handlers.erase(it);
        }
        markUnavailable(name, reason);
    }

    void ControlServer::unregisterOwner(const std::string_view owner)
    {
        std::erase_if(m_state->handlers, [owner](const auto& entry) { return entry.second.owner == owner; });
        std::erase_if(m_state->frameStart,
                      [owner](const State::FrameStartRequest& item) { return item.owner == owner; });
        const auto owned = [owner](const State::FrameCallback& item) { return item.owner == owner; };
        std::erase_if(m_state->frameCallbacks, owned);
        std::erase_if(m_state->runningFrameCallbacks, owned);
        // its pending deferred replies would never complete: the client gets an error instead of waiting forever
        std::vector<State::Deferred> orphaned;
        for (auto it = m_state->deferred.begin(); it != m_state->deferred.end();)
        {
            if (it->second.owner == owner)
            {
                orphaned.push_back(std::move(it->second));
                it = m_state->deferred.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (const State::Deferred& deferred : orphaned)
        {
            const std::string error = std::format("{} was unregistered before it replied", owner);
            deferred.send(replyText(nlohmann::json{{"id", deferred.requestId}, {"ok", false}, {"error", error}}));
        }
    }

    void ControlServer::describeOwner(const std::string_view owner, IFormatWriter& out) const
    {
        m_state->writeEntries(out, [owner](const std::string&, const State::Handler& handler)
                              { return !owner.empty() && handler.owner == owner; });
    }

    bool ControlServer::hasOwner(const std::string_view owner) const
    {
        return std::ranges::any_of(m_state->handlers,
                                   [owner](const auto& entry) { return entry.second.owner == owner; });
    }

#if defined(_WIN32) || !defined(SPITE_SHIPPING)
    void ControlServer::listenStdio()
    {
        State& state = *m_state;
        if (state.reader.joinable())
        {
            return;
        }
#if defined(_WIN32)
        state.reader = std::thread([&state] { state.readStdin(); });
#else
#if defined(__linux__)
        if (::pipe2(state.stdioCancel, O_CLOEXEC) != 0)
#else
        if (::pipe(state.stdioCancel) != 0)
#endif
        {
            throw std::system_error(errno, std::generic_category(), "control: cannot create stdio cancellation pipe");
        }
        try
        {
#if !defined(__linux__)
            if (fcntl(state.stdioCancel[0], F_SETFD, FD_CLOEXEC) < 0 ||
                fcntl(state.stdioCancel[1], F_SETFD, FD_CLOEXEC) < 0)
            {
                throw std::system_error(errno, std::generic_category(),
                                        "control: cannot secure stdio cancellation pipe");
            }
#endif
            state.reader = std::thread([&state] { state.readStdin(); });
        }
        catch (...)
        {
            ::close(state.stdioCancel[0]);
            ::close(state.stdioCancel[1]);
            throw;
        }
#endif
        state.attachedEpoch.fetch_add(1, std::memory_order_release);
    }
#endif

#if defined(_WIN32)
    void ControlServer::listenPipe(const std::string_view name, const std::string_view session)
    {
        State& state = *m_state;
        if (state.pipeThread.joinable())
        {
            return;
        }
        state.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        state.writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::string pipeName(name);
        if (pipeName.empty())
        {
            pipeName = controlPipeName(platform::executablePath());
        }
        if (!session.empty())
        {
            pipeName += std::string(SESSION_PIPE_INFIX) + std::string(session);
        }
        SINFO_LOG("control: serving the pipe {}", pipeName);
        std::wstring wideName(pipeName.begin(), pipeName.end());
        state.pipeThread = std::thread([&state, wideName = std::move(wideName)] { state.servePipe(wideName); });
    }

    void ControlServer::setPipeWritePendingHook(std::function<void()> hook)
    {
        m_state->pipeWritePendingHook = std::move(hook);
    }

    void ControlServer::setPipeGapHook(std::function<void()> hook) { m_state->pipeGapHook = std::move(hook); }

    void ControlServer::setPipeKeeperHook(std::function<void()> hook) { m_state->pipeKeeperHook = std::move(hook); }

    void ControlServer::setReceiveQueuedHook(std::function<void()> hook)
    {
        m_state->receiveQueuedHook = std::move(hook);
    }
#endif

    bool ControlServer::pipeConnected() const
    {
#if defined(_WIN32)
        std::lock_guard lock(m_state->pipeMutex);
        return m_state->pipeConnection != 0;
#else
        return false;
#endif
    }

    u64 ControlServer::attachedEpoch() const { return m_state->attachedEpoch.load(std::memory_order_acquire); }

    std::optional<ControlServer::AttachedClient> ControlServer::attachedClient() const
    {
#if defined(_WIN32) || !defined(SPITE_SHIPPING)
        State& state = *m_state;
#if defined(_WIN32)
        {
            std::lock_guard lock(state.pipeMutex);
            if (state.pipeConnection != 0)
            {
                // a name belongs to the connection that announced it, not to a later one
                return AttachedClient{RequestSource::Pipe,
                                      state.pipeClientConnection == state.pipeConnection ? state.pipeClient
                                                                                         : std::string{}};
            }
        }
#endif
        if (state.reader.joinable() && !stdioClosed())
        {
            return AttachedClient{RequestSource::Stdio, state.stdioClient};
        }
#endif
        return std::nullopt;
    }

    bool ControlServer::stdioInputClosedForTest() const
    {
#if defined(_WIN32) || !defined(SPITE_SHIPPING)
        return m_state->inputClosed.load(std::memory_order_acquire);
#else
        return false;
#endif
    }

    bool ControlServer::stdioClosed() const
    {
#if defined(_WIN32) || !defined(SPITE_SHIPPING)
        return m_state->inputClosed.load(std::memory_order_acquire) &&
            !m_state->hasInput.load(std::memory_order_acquire);
#else
        return false;
#endif
    }

    DeferredReply ControlServer::deferReply()
    {
        State& state = *m_state;
        if (!state.runningSend || !*state.runningSend || state.deferredNow != 0)
        {
            return {};
        }
        state.deferredNow = state.nextDeferred++;
        state.deferred.emplace(state.deferredNow,
                               State::Deferred{*state.runningId, *state.runningSend, *state.runningOwner});
        return {state.deferredNow};
    }

    ControlServer::AsyncReplyFn ControlServer::deferReplyAsync()
    {
        const DeferredReply reply = deferReply();
        if (!reply.isValid())
        {
            return {};
        }
        return [box = m_state->mailbox, reply](ResultFn result)
        {
            {
                const std::lock_guard lock(box->mutex);
                box->items.emplace_back(reply, std::move(result));
            }
            box->any.store(true, std::memory_order_release);
        };
    }

    DeferredReply ControlServer::deferUntilStepped(const ControlArgs& args, const bool armed,
                                                   std::vector<std::pair<std::string, u32>> counts,
                                                   const bool preferPlay)
    {
        const State::WorldEntry* entry = m_state->findEntry(args, preferPlay);
        const DeferredReply reply = entry ? deferReply() : DeferredReply{};
        if (reply.isValid())
        {
            const u64 activity = m_state->runningActivity;
            m_state->pendingSteps.push_back({reply, entry->name, entry->serial, armed, std::move(counts), activity});
            if (activity)
                m_state->updateActivity(activity, "review", "waiting", "step", {});
        }
        return reply;
    }

    void ControlServer::armStepReply(const DeferredReply reply)
    {
        for (State::PendingStep& pending : m_state->pendingSteps)
        {
            if (pending.reply.id == reply.id)
            {
                pending.armed = true;
            }
        }
    }

    std::string ControlServer::replyAfterFrames(const u32 frames, ResultFn fill, IFormatWriter& out)
    {
        // inside a batch, undo, redo or replay the running request's sink is the outer command's: deferring would
        // take its reply
        const DeferredReply reply = m_state->replaying > 0 ? DeferredReply{} : deferReply();
        if (!reply.isValid())
        {
            std::string error = fill(out);
            if (error.empty())
            {
                out.writeBool("settled", false);
            }
            return error;
        }
        m_state->frameWaits.push_back({reply, std::max(frames, 1u), std::move(fill)});
        return {};
    }

    void ControlServer::completeReply(const DeferredReply reply, const ResultFn& result)
    {
        const auto it = m_state->deferred.find(reply.id);
        if (it == m_state->deferred.end())
        {
            return;
        }
        const State::Deferred deferred = std::move(it->second);
        m_state->deferred.erase(it);
        JsonFragmentWriter out;
        if (const std::string error = result(out); !error.empty())
        {
            deferred.send(replyText(nlohmann::json{{"id", deferred.requestId}, {"ok", false}, {"error", error}}));
            return;
        }
        std::string resultText;
        out.dumpReply(resultText);
        deferred.send(R"({"id":)" + deferred.requestId.dump() + R"(,"ok":true,"result":)" + resultText + "}");
    }

    void ControlServer::runAtFrameStart(const std::string_view owner, std::function<void()> callback)
    {
        m_state->frameCallbacks.push_back({std::string(owner), std::move(callback)});
    }

    bool ControlServer::runningFrameCallback(const std::string_view owner) const
    {
        return m_state->frameCallbackOwner == owner;
    }

    void ControlServer::runAtFrameStart(const std::string_view owner, std::string request,
                                        std::function<void(const std::string&)> done)
    {
        m_state->frameStart.push_back({std::string(owner), std::move(request), std::move(done)});
    }

    void ControlServer::setLocalSource(const RequestSource source) { m_state->localSource = source; }

    u64 ControlServer::attributedRequests() const { return m_state->attributed; }

    u64 ControlServer::clientCommandCount() const { return m_state->clientCommands; }

    void ControlServer::pump()
    {
        State& state = *m_state;
        if (state.frameCallbacksRunning)
            return;
        ++state.frames;
        if (state.mailbox->any.exchange(false, std::memory_order_acq_rel))
        {
            std::vector<std::pair<DeferredReply, ResultFn>> mail;
            {
                const std::lock_guard lock(state.mailbox->mutex);
                mail.swap(state.mailbox->items);
            }
            for (const auto& [reply, result] : mail)
            {
                completeReply(reply, result);
            }
        }
        if (!state.frameWaits.empty())
        {
            std::vector<State::FrameWait> due;
            std::erase_if(state.frameWaits,
                          [&due](State::FrameWait& wait)
                          {
                              if (--wait.left > 0)
                              {
                                  return false;
                              }
                              due.push_back(std::move(wait));
                              return true;
                          });
            for (const State::FrameWait& wait : due)
            {
                completeReply(wait.reply,
                              [&wait](IFormatWriter& out)
                              {
                                  std::string error = wait.fill(out);
                                  if (error.empty())
                                  {
                                      out.writeBool("settled", true);
                                  }
                                  return error;
                              });
            }
        }
        state.frameCallbacksRunning = true;
        state.runningFrameCallbacks.swap(state.frameCallbacks);
        std::ranges::reverse(state.runningFrameCallbacks);
        while (!state.runningFrameCallbacks.empty())
        {
            State::FrameCallback item = std::move(state.runningFrameCallbacks.back());
            state.runningFrameCallbacks.pop_back();
            state.frameCallbackOwner = item.owner;
            try
            {
                item.fn();
            }
            catch (...)
            {
                item.fn = {};
                state.runningFrameCallbacks.clear();
                state.frameCallbackOwner.clear();
                state.frameCallbacksRunning = false;
                throw;
            }
            item.fn = {};
            state.frameCallbackOwner.clear();
        }
        state.frameCallbacksRunning = false;
        if (!state.frameStart.empty())
        {
            std::vector<State::FrameStartRequest> todo;
            todo.swap(state.frameStart);
            for (State::FrameStartRequest& item : todo)
            {
                const std::string reply = handle(item.request);
                if (item.done)
                {
                    item.done(reply);
                }
            }
        }
        if (!state.pendingSteps.empty())
        {
            std::erase_if(
                state.pendingSteps,
                [this, &state](const State::PendingStep& pending)
                {
                    const auto entry = std::ranges::find(state.worlds, pending.worldSerial, &State::WorldEntry::serial);
                    const World* stepped = entry == state.worlds.end() ? nullptr : &entry->world;
                    if (stepped &&
                        (!pending.armed || stepped->simulationState().pendingSteps > 0 ||
                         stepped->simulationState().pendingFixedSteps > 0))
                    {
                        return false;
                    }
                    if (const auto* activity = state.activity(pending.activity); activity && activity->active())
                    {
                        if (stepped)
                            state.updateActivity(pending.activity, "review", "running", "step",
                                                 std::format("Fixed steps: {}", stepped->simulationState().fixedSteps));
                        else
                            state.updateActivity(pending.activity, "review", "failed", "step",
                                                 "World removed before step completion");
                    }
                    completeReply(pending.reply,
                                  [stepped, &pending](IFormatWriter& out)
                                  {
                                      if (!stepped)
                                      {
                                          return "world '" + pending.world + "' was removed before its steps ran";
                                      }
                                      for (const auto& [name, count] : pending.counts)
                                      {
                                          out.writeU32(name, count);
                                      }
                                      writeSimulation(stepped->simulationState(), out);
                                      return std::string{};
                                  });
                    return true;
                });
        }
#if defined(_WIN32) || !defined(SPITE_SHIPPING)
        if (!state.hasInput.load(std::memory_order_acquire))
        {
            return;
        }
        std::vector<std::pair<u32, std::string>> lines;
        {
            std::lock_guard lock(state.inboxMutex);
            lines.swap(state.inbox);
            state.hasInput.store(false, std::memory_order_release);
            if (state.inputClosed.load(std::memory_order_acquire))
            {
                state.attachedEpoch.fetch_add(1, std::memory_order_release); // the last line is consumed
            }
        }
        for (sizet index = 0; index < lines.size(); ++index)
        {
            const auto& [source, line] = lines[index];
            if (source != State::STDIO && !state.liveLeaseTransport(source))
            {
                if (state.batchReady && state.batchReadyTransport == source)
                {
                    state.batchReady = false;
                    state.batchStatus.state = "refused";
                    ++state.batchStatus.revision;
                }
                continue;
            }
            const bool heldBatch = state.batchReady;
            if (!state.batchReady)
            {
                const auto request = nlohmann::json::parse(line, nullptr, false);
                if (request.is_object() && request.contains("command") && request["command"].is_string() &&
                    request["command"] == "batch")
                {
                    const Attribution by = state.attributionOf(source);
                    state.batchStatus.client = by.client;
                    state.batchStatus.source = by.source;
                    if (request.contains("client") && request["client"].is_string())
                        state.batchStatus.client = request["client"].get<std::string>();
                    if (request.contains("source") && request["source"].is_string())
                        state.batchStatus.source = request["source"].get<std::string>();
                    state.batchStatus.name = "batch";
                    state.batchStatus.world.clear();
                    if (request.contains("args") && request["args"].is_object() && request["args"].contains("name") &&
                        request["args"]["name"].is_string())
                        state.batchStatus.name = request["args"]["name"].get<std::string>();
                    state.batchStatus.step = 0;
                    state.batchStatus.state = "active";
                    ++state.batchStatus.revision;
                    state.batchReady = true;
                    state.batchReadyTransport = source;
                    const std::lock_guard lock(state.inboxMutex);
                    state.inbox.insert(state.inbox.begin(), std::make_move_iterator(lines.begin() + index),
                                       std::make_move_iterator(lines.end()));
                    state.hasInput.store(true, std::memory_order_release);
                    break;
                }
            }
            else
                state.batchReady = false;
            const ReplyFn send = [&state, source](const std::string_view reply)
            {
                const std::string text = std::string(reply) + '\n';
                if (source == State::STDIO)
                {
                    State::writeStdio(text);
                }
#if defined(_WIN32)
                else
                {
                    state.writePipe(source, text);
                }
#endif
            };
            state.transport = source;
            std::string reply;
            try
            {
                reply = handle(line, send);
            }
            catch (...)
            {
                state.transport = State::LOCAL;
                if (heldBatch && state.batchStatus.state == "active")
                {
                    state.batchStatus.state = "refused";
                    ++state.batchStatus.revision;
                }
                if (source == State::STDIO)
                    state.pendingStdioRequests.fetch_sub(1, std::memory_order_release);
                const std::lock_guard lock(state.inboxMutex);
                state.inbox.insert(state.inbox.begin(), std::make_move_iterator(lines.begin() + index + 1),
                                   std::make_move_iterator(lines.end()));
                state.hasInput.store(!state.inbox.empty(), std::memory_order_release);
                throw;
            }
            state.transport = State::LOCAL;
            if (heldBatch && state.batchStatus.state == "active")
            {
                state.batchStatus.state = "refused";
                ++state.batchStatus.revision;
            }
            if (!reply.empty())
            {
                send(reply);
            }
            if (source == State::STDIO)
                state.pendingStdioRequests.fetch_sub(1, std::memory_order_release);
        }
#endif
    }

    std::string ControlServer::query(const std::string_view name, IFormatWriter& result)
    {
        const auto handler = m_state->handlers.find(name);
        if (handler == m_state->handlers.end() || handler->second.command)
        {
            return "unknown query '" + std::string(name) + "'";
        }
        // a deferring query would take the running request's deferReply slot
        if (m_state->runningId)
        {
            return "query '" + std::string(name) + "' cannot run inside a running handler";
        }
        static const nlohmann::json noArgs = nlohmann::json::object();
        // empty args can only miss a required one; checkArgs allocates, and the editor's panels query every refresh
        if (handler->second.described && std::ranges::any_of(handler->second.args, &Arg::required))
        {
            if (const nlohmann::json problems = checkArgs(handler->second.args, noArgs); !problems.empty())
            {
                return argsMessage(name, problems);
            }
        }
        State::Capture captured;
        return m_state->runHandler(name, handler->second, noArgs, result, captured);
    }

    std::string ControlServer::handle(const std::string_view line, const ReplyFn& deferred)
    {
        SPITE_ZONE("Control.handle");
        crashRecordCommand(line);
        const nlohmann::json request = nlohmann::json::parse(line, nullptr, false);
        const nlohmann::json id = request.is_object() ? request.value("id", nlohmann::json()) : nlohmann::json();
        const auto fail =
            [&id](const std::string_view error, nlohmann::json argErrors = {}, nlohmann::json details = {})
        {
            nlohmann::json reply{{"id", id}, {"ok", false}, {"error", error}};
            if (!argErrors.is_null())
            {
                reply["argErrors"] = std::move(argErrors);
            }
            if (!details.is_null())
            {
                reply["details"] = std::move(details);
            }
            return replyText(reply);
        };

        if (!request.is_object())
        {
            return fail("request is not a JSON object");
        }
        const bool isCommand = request.contains("command");
        const auto nameIt = request.find(isCommand ? "command" : "query");
        if (nameIt == request.end() || !nameIt->is_string())
        {
            return fail("missing string member 'query' or 'command'");
        }
        const std::string& name = nameIt->get_ref<const std::string&>();
        const auto handler = m_state->handlers.find(name);
        if (handler == m_state->handlers.end())
        {
            return fail(m_state->unknownEntry(name));
        }
        if (handler->second.command != isCommand)
        {
            return fail("'" + name + "' is a " + (isCommand ? "query" : "command"));
        }

        static const nlohmann::json noArgs = nlohmann::json::object();
        const auto argsIt = request.find("args");
        const nlohmann::json& args = argsIt != request.end() && argsIt->is_object() ? *argsIt : noArgs;

        State& state = *m_state;

        // a request run from inside a handler is its caller's; one from a connection or in process is its transport's
        // only a command is attributed (it may write a step or a row); a query reads no attribution and pays none,
        // so a command a query handler runs takes its transport's
        const bool inherits = state.runningId != nullptr && state.runningCommand;
        state.refreshEditorLease();
        const u32 origin = inherits ? state.runningTransport : state.transport;
        if (isCommand && origin != State::LOCAL && state.editorLease && state.editorLease->transport != origin &&
            name != "hello" && name != "acquireEditorLease" && name != "renewEditorLease" &&
            name != "releaseEditorLease")
            return fail("editor lease is held by another session");
        const bool agentRequest = state.runningId == nullptr && state.transport != State::LOCAL;
        bool quiet = false;
        if (const auto value = request.find("quiet"); value != request.end())
        {
            if (isCommand || !value->is_boolean())
                return fail("'quiet' is a boolean for queries");
            quiet = value->get<bool>();
        }
        if (agentRequest && !quiet)
            state.agentTraffic = {state.agentTraffic.count + 1, state.activityNowMs(), isCommand};
        u64 activityId = state.runningActivity;
        if (const auto value = request.find("activity"); value != request.end())
        {
            if (!value->is_number_unsigned() || !state.activity(value->get<u64>()))
                return fail("unknown activity id");
            activityId = value->get<u64>();
        }
        struct ActivityScope
        {
            u64& current;
            u64 previous;
            ~ActivityScope() { current = previous; }
        } activityScope{state.runningActivity, std::exchange(state.runningActivity, activityId)};
        Attribution by;
        if (isCommand)
        {
            by = inherits ? state.running : state.attributionOf(state.transport);
        }
        for (const char* key : {"source", "client"})
        {
            const auto member = request.find(key);
            if (member == request.end())
            {
                continue;
            }
            const bool isSource = *key == 's';
            if (!member->is_string() ||
                (isSource &&
                 std::ranges::find(SOURCE_NAMES, member->get_ref<const std::string&>()) == std::end(SOURCE_NAMES)) ||
                (!isSource && member->get_ref<const std::string&>().size() > MAX_CLIENT_NAME))
            {
                return fail(
                    std::format("request member '{}' must be {}", key,
                                isSource ? "one of host, editor, stdio, pipe" : "a string of at most 64 bytes"));
            }
            if (isCommand)
            {
                (isSource ? by.source : by.client) = member->get<std::string>();
            }
        }
        struct RunningScope
        {
            State& state;
            Attribution by;
            std::chrono::steady_clock::time_point start;
            u32 transport;
            RunningScope(State& owner, Attribution next, const u32 nextTransport) :
                state(owner), by(std::exchange(owner.running, std::move(next))),
                start(std::exchange(owner.runningStart, std::chrono::steady_clock::now())),
                transport(std::exchange(owner.runningTransport, nextTransport))
            {
            }
            ~RunningScope()
            {
                state.running = std::move(by);
                state.runningStart = start;
                state.runningTransport = transport;
            }
        };
        std::optional<RunningScope> runningScope;
        if (isCommand)
        {
            by.time = utcNow();
            by.frame = state.frames;
            ++state.attributed;
            state.clientCommands += by.source == "pipe" || by.source == "stdio" ? 1 : 0;
            runningScope.emplace(state, std::move(by), inherits ? state.runningTransport : state.transport);
        }

        // a failed edit leaves a row with its error, refused args included; none when no world is named
        const auto logFailure = [&](const std::string_view error)
        {
            if (isCommand && logsFailure(name, handler->second.undoable))
            {
                if (const State::WorldEntry* entry = state.findEntry(ControlArgs(&args)))
                {
                    nlohmann::json logged = args;
                    logged["world"] = entry->name;
                    entry->history->log(name, logged, state.running, state.elapsedUs(), error);
                }
            }
        };
        if (handler->second.described)
        {
            if (nlohmann::json problems = checkArgs(handler->second.args, args); !problems.empty())
            {
                const std::string message = argsMessage(name, problems);
                logFailure(message);
                return fail(message, std::move(problems));
            }
        }
        JsonFragmentWriter result;
        State::Capture captured;
        m_state->argErrors = {};
        m_state->refusal = {};
        // a handler may run requests itself (the console): each sees its own
        const nlohmann::json* outerId = std::exchange(m_state->runningId, &id);
        const ReplyFn* outerSend = std::exchange(m_state->runningSend, &deferred);
        const std::string* outerOwner = std::exchange(m_state->runningOwner, &handler->second.owner);
        const u64 outerDeferred = std::exchange(m_state->deferredNow, 0);
        // a request run from inside a replayed handler is still a request, not a replay
        const u32 outerReplaying = std::exchange(m_state->replaying, 0);
        const bool outerCommand = std::exchange(m_state->runningCommand, isCommand);
        std::string error;
        try
        {
            error = m_state->runHandler(name, handler->second, args, result, captured);
        }
        catch (...)
        {
            m_state->deferred.erase(m_state->deferredNow);
            m_state->deferredNow = outerDeferred;
            m_state->runningCommand = outerCommand;
            m_state->replaying = outerReplaying;
            m_state->runningSend = outerSend;
            m_state->runningOwner = outerOwner;
            m_state->runningId = outerId;
            throw;
        }
        m_state->runningCommand = outerCommand;
        m_state->replaying = outerReplaying;
        const u64 deferredId = std::exchange(m_state->deferredNow, outerDeferred);
        m_state->runningSend = outerSend;
        m_state->runningOwner = outerOwner;
        m_state->runningId = outerId;
        if (!error.empty())
        {
            m_state->deferred.erase(deferredId);
            // a command that threw may have acted: undone with what it recorded, or its state counts as changed
            const bool acted =
                captured.threw && isCommand && !m_state->rollBackThrown(args, captured, handler->second.view);
            if (acted)
            {
                error += State::MAY_HAVE_ACTED;
            }
            logFailure(error);
            if (agentRequest && isCommand && logsFailure(name, handler->second.undoable))
            {
                const std::string label = ControlArgs(&args).string("name").value_or(name);
                const u64 id = activityId ? activityId : state.startActivity("scene", label.substr(0, 120), {});
                state.updateActivity(id, "apply", "failed", name, error.substr(0, 512));
            }
            const auto countChange = [&]
            {
                ++m_state->commandGeneration;
                m_state->stateGeneration += handler->second.view ? 0 : 1;
            };
            if (acted)
            {
                countChange();
            }

            if (!m_state->refusal.empty())
            {
                return fail(error, std::exchange(m_state->argErrors, {}), std::exchange(m_state->refusal, {}));
            }
            std::string written;
            result.dumpReply(written);
            nlohmann::json details = nlohmann::json::parse(written, nullptr, false);
            if (!details.is_object() || details.empty())
            {
                return fail(error, std::exchange(m_state->argErrors, {}));
            }
            // members written before failing say what the handler did: state may have changed
            if (isCommand && !acted)
            {
                countChange();
            }
            return fail(error, std::exchange(m_state->argErrors, {}), std::move(details));
        }
        if (isCommand)
        {
            ++m_state->commandGeneration;
            if (!handler->second.view)
            {
                ++m_state->stateGeneration;
            }
        }
        if (!captured.inverse.empty())
        {
            if (const State::WorldEntry* entry = m_state->findEntry(ControlArgs(&args)))
            {
                nlohmann::json forward = captured.forward ? std::move(*captured.forward) : args;
                forward["world"] = entry->name;
                UndoStep step;
                step.view = handler->second.view;
                if (const auto merge = args.find("merge"); merge != args.end() && merge->is_string())
                {
                    step.merge = merge->get<std::string>();
                }
                CommandRecord record{{name, forward, std::move(captured.created)}, std::move(captured.inverse)};
                if (!step.view)
                {
                    m_state->noteLayers(step, *entry, captured, record);
                }
                step.records.push_back(std::move(record));
                step.by = m_state->running;
                entry->history->record(std::move(step), name, forward, m_state->elapsedUs());
                m_state->sweepProvenance(*entry);
                if (agentRequest && !handler->second.view)
                    state.noteAgentApply(*entry, name, entry->history->steps().back());
            }
        }
        if (captured.replay)
        {
            const State::WorldEntry* entry = nullptr;
            if (captured.replayWorld.empty())
            {
                entry = m_state->findEntry(ControlArgs(&args));
            }
            else if (const auto it = std::ranges::find(m_state->worlds, captured.replayWorld, &State::WorldEntry::name);
                     it != m_state->worlds.end())
            {
                entry = &*it;
            }
            if (entry)
            {
                nlohmann::json logged = args;
                if (captured.replayArgs)
                {
                    logged.update(*captured.replayArgs);
                }
                if (captured.replayWorld.empty())
                {
                    logged["world"] = entry->name;
                }
                if (name == "step")
                {
                    static_assert(MAX_FRAMES_PER_STEP == MAX_FIXED_STEPS_PER_STEP);
                    entry->history->logStep(logged, MAX_FRAMES_PER_STEP, m_state->running, m_state->elapsedUs());
                }
                else
                {
                    entry->history->log(name, logged, m_state->running, m_state->elapsedUs());
                }
            }
        }
        if (deferredId != 0)
        {
            return {};
        }
        if (agentRequest && isCommand && name == "batch" && state.batchStatus.step != 0)
        {
            if (const State::WorldEntry* entry = state.findEntry(ControlArgs(&args)))
            {
                const UndoStep& step = entry->history->steps().back();
                const std::string_view operation = state.batchStatus.name;
                if (!step.view)
                    state.noteAgentApply(*entry, operation.empty() ? "batch" : operation, step);
            }
        }
        else if (agentRequest && !quiet && !isCommand && (name == "entity" || name == "find" || name == "components"))
        {
            const ControlArgs controlArgs(&args);
            if (const State::WorldEntry* entry = state.findEntry(controlArgs))
                state.noteAgentRead(*entry, name, controlArgs);
        }
        if (isCommand && (name == "undo" || name == "redo" || name == "historyJump"))
            if (const State::WorldEntry* entry = state.findEntry(ControlArgs(&args)))
                state.updateActivityUndo(*entry);
        std::string resultText;
        SPITE_ZONE("Control.serializeReply");
        result.dumpReply(resultText);
        return R"({"id":)" + id.dump() + R"(,"ok":true,"result":)" + resultText + "}";
    }

    std::string plainRelativePathProblem(const std::string_view text, const bool directory)
    {
        return pathProblem(text, directory);
    }

    std::string setSchemaField(const StructSchema& schema, void* root, const std::string_view path,
                               const std::string_view valueJson, IFormatWriter& out)
    {
        FieldDesc field;
        u32 offset = 0;
        if (std::string error = resolveFieldPath(&schema, path, field, offset); !error.empty())
        {
            return error;
        }
        auto* target = static_cast<std::byte*>(root) + offset;
        nlohmann::json previous;
        // config sections gate their values through ConfigFieldMeta, and restore a stored value through here too
        return writeField(field, target, target, path, valueJson, out, previous, false);
    }
} // namespace spite
