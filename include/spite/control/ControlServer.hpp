#pragma once

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/Platform.hpp"
#include "control/ControlExport.hpp"
#include "ecs/ComponentMetadata.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    class World;
    class IFormatWriter;
    class ServiceLocator;
    struct StructSchema;

    // The "args" object of a request; valid only during the handler call.
    class SPITE_CONTROL_API ControlArgs
    {
    public:
        explicit ControlArgs(const void* json) : m_json(json) {}
        // A string member, or an integer member as its decimal text.
        [[nodiscard]] std::optional<std::string> string(std::string_view key) const;
        // An unsigned integer member, or a string member holding one.
        [[nodiscard]] std::optional<u64> integer(std::string_view key) const;
        [[nodiscard]] std::optional<bool> boolean(std::string_view key) const;
        // Any member as JSON text.
        [[nodiscard]] std::optional<std::string> json(std::string_view key) const;

    private:
        const void* m_json;
    };

    // The value an argument takes (schema-described arguments).
    enum class ArgKind : u8
    {
        String, // a string; an integer is taken as its decimal text
        Integer, // an unsigned integer, or a string holding one
        Number, // any number
        Boolean,
        Entity, // an entity id, as Integer
        Component, // a registered component's name
        Json, // any JSON value: objects, arrays, lists of names
    };

    // One argument of a query or command, as describe reports it and the
    // server checks it before the handler runs. defaultValue documents what an
    // absent optional argument means; the handler applies it. choices, for a
    // String argument, are its only allowed values: describe lists them, the
    // server refuses any other (problem "choice") and the console completes
    // them. root, for a String argument that names a file: the named root (ControlServer::setRoot) it is
    // relative to, or HOST_ROOT for a path the OS takes as it is. The server refuses a rooted argument that is
    // absolute, has "..", a ':' part, a device name or NUL before the handler runs. Registration copies them.
    struct ArgDesc
    {
        std::string_view name;
        ArgKind kind = ArgKind::String;
        bool required = false;
        std::string_view doc;
        std::string_view defaultValue;
        std::span<const std::string_view> choices;
        std::string_view root;
    };

    // ArgDesc::root of a trusted path: a module DLL or a directory that finds one, the OS path as given
    // The host validates this trusted path before loading the module.
    inline constexpr std::string_view HOST_ROOT = "host";
    // ArgDesc::root of a directory path whose first segment names the root: "<root>[/<directory>]", empty for the
    // list of roots (listDirectory). The same refusals apply to the rest of it as to a rooted file.
    inline constexpr std::string_view ANY_ROOT = "any";

    [[nodiscard]] constexpr ArgDesc requiredArg(const std::string_view name, const ArgKind kind,
                                                const std::string_view doc,
                                                const std::span<const std::string_view> choices = {})
    {
        return {name, kind, true, doc, {}, choices};
    }

    [[nodiscard]] constexpr ArgDesc optionalArg(const std::string_view name, const ArgKind kind,
                                                const std::string_view doc, const std::string_view defaultValue = {},
                                                const std::span<const std::string_view> choices = {})
    {
        return {name, kind, false, doc, defaultValue, choices};
    }

    [[nodiscard]] constexpr ArgDesc requiredPath(const std::string_view name, const std::string_view root,
                                                 const std::string_view doc)
    {
        return {name, ArgKind::String, true, doc, {}, {}, root};
    }

    [[nodiscard]] constexpr ArgDesc optionalPath(const std::string_view name, const std::string_view root,
                                                 const std::string_view doc, const std::string_view defaultValue = {})
    {
        return {name, ArgKind::String, false, doc, defaultValue, {}, root};
    }

    enum class DirectoryEntryKind : u8
    {
        File,
        Directory,
        Other, // a link, a device: listed, never entered or chosen
    };

    // One entry of a directory under a named root (ControlServer::listDirectory).
    struct DirectoryEntry
    {
        std::string name; // UTF-8
        DirectoryEntryKind kind = DirectoryEntryKind::File;
        u64 size = 0; // bytes; 0 for a directory
        std::string modified; // UTC, "YYYY-MM-DDTHH:MM:SSZ"; empty for a root
    };

    struct DirectoryListing
    {
        std::string path; // "<root>[/<directory>]" as listed, '/' separated; empty for the list of roots
        std::string root; // the named root; empty for the list of roots
        std::string directory; // the OS directory, UTF-8; empty for the list of roots
        std::vector<DirectoryEntry> entries; // directories first, then files by name (ASCII case-insensitive) or age
        bool truncated = false; // more than ControlServer::MAX_DIRECTORY_ENTRIES matched
    };

    // Whether a file name's extension is one of a comma list ("glb, .gltf": a dot is optional, ASCII
    // case-insensitive); an empty list takes every name. The rule listDirectory's extensions apply.
    [[nodiscard]] SPITE_CONTROL_API bool fileNameMatchesExtensions(std::string_view fileName,
                                                                   std::string_view extensions);

    // The "world" arg of a handler that acts on one (ControlServer::world);
    // undoable commands take it implicitly.
    inline constexpr ArgDesc WORLD_ARG =
        optionalArg("world", ArgKind::String, "the world to act on", "the first world added");

    // What each world's undo history keeps: the newest steps within steps and
    // stepBytes of stored requests (JSON size), the newest command log rows
    // within logRows and logBytes. The newest step and row always stay. The rows of failed edits are counted
    // apart, within failedLogRows and failedLogBytes: a client failing in a loop never pushes applied rows out.
    struct UndoLimits
    {
        sizet steps = 1000;
        sizet stepBytes = sizet{64} << 20;
        sizet logRows = 4096;
        sizet logBytes = sizet{16} << 20;
        sizet failedLogRows = 256;
        sizet failedLogBytes = sizet{1} << 20;
    };

    // Where a request came from: the kind of its source; a connection adds the client name it announced.
    enum class RequestSource : u8
    {
        Host, // in process, not the editor
        Editor, // in process while the editor host runs
        Stdio,
        Pipe,
    };

    // A request whose handler answers it later (ControlServer::deferReply).
    struct DeferredReply
    {
        u64 id = 0;

        [[nodiscard]] bool isValid() const { return id != 0; }
    };

    // The engine control server: a registry of named queries (reads)
    // and commands (writes) over newline-delimited JSON. A request {"id": n,
    // "query" or "command": name, "args": {...}} gets {"id": n, "ok": true,
    // "result": {...}} or {"id": n, "ok": false, "error": "..."}; a request whose
    // args break the entry's declared ones is refused before its handler runs,
    // with "argErrors": [{"arg", "problem", "expected"}]. A handler that fails
    // after writing result members (it acted, then hit what it reports) gets
    // them back as "details" beside "error", and a command counts as one that
    // may have changed state (the generations). Handlers run on
    // the frame thread inside pump(); with no client attached pump() is one
    // atomic load. Every entry has an owning module and unregisters with it.
    class SPITE_CONTROL_API ControlServer
    {
    public:
        // Writes the result object's members; returns an error message, empty on success.
        using HandlerFn = std::function<std::string(const ControlArgs& args, IFormatWriter& result)>;
        using PreflightFn = std::function<std::string(const ControlArgs& args)>;
        // Read-only validation of stable references before any command in a batch runs.
        void setCommandPreflight(std::string_view name, std::string_view owner, PreflightFn fn);
        // Vets addComponent (adding) and removeComponent before they act;
        // returns a refusal, empty to allow.
        using ComponentEditGuardFn =
            std::function<std::string(const World& world, Entity entity, ComponentID component, bool adding)>;
        // Runs after addComponent added a component, inside the command: it may record more inverses (recordInverse)
        // for what the component's systems add later.
        using ComponentAddedFn = std::function<void(const World& world, Entity entity, ComponentID component)>;
        // Takes the reply line of a request answered later.
        using ReplyFn = std::function<void(std::string_view reply)>;
        // Fills a deferred request's result; returns an error message, empty on success.
        using ResultFn = std::function<std::string(IFormatWriter& result)>;

        ControlServer();
        ~ControlServer();
        ControlServer(const ControlServer&) = delete;
        ControlServer& operator=(const ControlServer&) = delete;

        // The generic queries read worlds by name (the first added is the default)
        // and each world's own services.
        // The source maps a world's entity id to the id it was cloned from in another world (0 = none); entity and
        // find report it as `source`.
        using SourceFn = std::function<u64(u64 id)>;
        void addWorld(std::string_view name, World world, SourceFn source = {});
        // Before the world is destroyed.
        void removeWorld(std::string_view name);
        // The world a request names in its "world" arg, the default without one;
        // null when unknown. For handlers that act on a world.
        [[nodiscard]] const World* world(const ControlArgs& args) const;
        [[nodiscard]] const World* world(std::string_view name) const;
        // The refusal for a request naming no world: the unknown name and the valid ones.
        [[nodiscard]] std::string unknownWorld(const ControlArgs& args) const;
        [[nodiscard]] std::string unknownWorld(std::string_view name) const;
        // The name of the world a request names, the default one without; empty when unknown. For a handler that
        // finds its world again later (the world may be gone by then).
        [[nodiscard]] std::string worldName(const ControlArgs& args) const;
        // Bumped by every command that succeeds (batch, undo and redo once):
        // a client polling it per frame learns that state may have changed.
        [[nodiscard]] u64 commandGeneration() const;
        struct BatchStatus
        {
            u64 revision = 0;
            u64 step = 0;
            std::string client;
            std::string source;
            std::string world;
            std::string name;
            std::string state; // active, completed, refused, rolledBack or rollbackFailed
        };
        [[nodiscard]] BatchStatus batchStatus() const;
        enum class AgentTimelinePhase : u8
        {
            None,
            Inspect,
            Apply,
            Review,
        };
        struct AgentTimeline
        {
            AgentTimelinePhase phase = AgentTimelinePhase::None;
            std::string client;
            std::string inspect;
            std::string operation;
            std::string review;
            u64 step = 0;
        };
        // Frame-thread view of the latest operation in a world. Stage details are empty until observed.
        [[nodiscard]] u64 agentTimelineRevision() const;
        [[nodiscard]] AgentTimeline agentTimeline(std::string_view world) const;

        struct AgentActivity
        {
            u64 id = 0, startedMs = 0, step = 0;
            std::string kind, title, why, phase, state, action, evidence, world, inspect;
            bool reported = false;
            // Undo identity, including worlds that were removed and later recreated.
            u64 worldSerial = 0, stepState = 0;
            [[nodiscard]] bool active() const { return state == "running" || state == "waiting"; }
        };
        // Frame-thread operation feed. A request can opt into an existing id with top-level "activity".
        u64 beginAgentActivity(std::string_view kind, std::string_view title, std::string_view why = {});
        void updateAgentActivity(u64 id, std::string_view phase, std::string_view state, std::string_view action,
                                 std::string_view evidence = {}, bool reported = false);
        [[nodiscard]] u64 currentAgentActivity() const;
        [[nodiscard]] u64 agentActivityRevision() const;
        [[nodiscard]] std::span<const AgentActivity> agentActivities() const;
        [[nodiscard]] const AgentActivity* agentActivityFocus() const;
        [[nodiscard]] bool agentActivityCanReview(u64 id) const;
        [[nodiscard]] bool agentActivityCanAcknowledge(u64 id) const;
        // One thing the operation's undo step did: a `set` as the entity, field, and the value it overwrote and the one
        // it left (JSON text, first before and last after per entity and field; empty when over 64 bytes), any other
        // command as its name in `field` with no values. entity is 0 when the command names none.
        struct AgentChange
        {
            u64 entity = 0;
            std::string field, before, after;
            bool set = false;
        };
        // The changes of the operation's linked undo step, at most 64; empty once the step left the history or was
        // reshaped. Reads the step: call per revision of the feed, never per frame.
        [[nodiscard]] std::vector<AgentChange> agentActivityChanges(u64 id) const;
        // The clock the feed stamps with (milliseconds, steady): the panel measures age and holds against it. A test
        // replaces it (null restores the real one).
        [[nodiscard]] u64 agentActivityClockMs() const;
        void setAgentActivityClock(std::function<u64()> nowMs);
        // The newest request a control client (a pipe or stdio connection, not the editor's own and not `quiet`) made:
        // how many so far, when (agentActivityClockMs) and whether it was a command. count 0: none yet. Moves per
        // request, so a reader compares the count and acts when it changed.
        struct AgentTraffic
        {
            u64 count = 0, atMs = 0;
            bool command = false;
        };
        [[nodiscard]] AgentTraffic agentTraffic() const;
        // Runs one request outside the pump as a stdio client's: its attribution, feed rows and traffic. How a test
        // or an in-process host stands in for an agent without a pipe.
        std::string handleAsClient(std::string_view line);
        // commandGeneration without the view commands (registerViewCommand): bumped only by commands that may have
        // changed scene or component state, for readers that mirror that state.
        [[nodiscard]] u64 stateGeneration() const;
        // Number of pump calls since construction; frame thread only.
        [[nodiscard]] u64 pumpCount() const;
        // owner: the registering module's name, as its systems use. doc and
        // args describe the entry (the describe query); a request naming an
        // arg outside args, missing a required one or of another kind is
        // refused. An entry registered without them takes any args unchecked.
        void registerQuery(std::string_view name, std::string_view owner, std::string_view doc,
                           std::initializer_list<ArgDesc> args, HandlerFn fn);
        void registerCommand(std::string_view name, std::string_view owner, std::string_view doc,
                             std::initializer_list<ArgDesc> args, HandlerFn fn);
        // A command that changes only editor view state (camera, layout, filters), never scene or component state:
        // it bumps commandGeneration but not stateGeneration, and describe reports "view": true.
        void registerViewCommand(std::string_view name, std::string_view owner, std::string_view doc,
                                 std::initializer_list<ArgDesc> args, HandlerFn fn);
        // A command that records its inverse (recordInverse) on success. Only
        // these run inside a batch, undo or redo, so such a command must change
        // nothing but its world's state: no worlds or handlers added or removed.
        // It takes WORLD_ARG without declaring it.
        void registerUndoableCommand(std::string_view name, std::string_view owner, std::string_view doc,
                                     std::initializer_list<ArgDesc> args, HandlerFn fn);
        // An undoable command that changes only state a scene file does not hold (a view's camera, a render pass
        // toggle, the asset library, a UI document), never scene or component state: it is a view command
        // (registerViewCommand) whose undo step leaves historyState alone, so it never makes the open scene dirty.
        // A batch of such commands only is one such step; any other command in it makes the step a scene edit.
        void registerUndoableViewCommand(std::string_view name, std::string_view owner, std::string_view doc,
                                         std::initializer_list<ArgDesc> args, HandlerFn fn);
        void registerQuery(std::string_view name, std::string_view owner, HandlerFn fn);
        void registerCommand(std::string_view name, std::string_view owner, HandlerFn fn);
        void registerUndoableCommand(std::string_view name, std::string_view owner, HandlerFn fn);
        // A query or command this server does not serve in its configuration (a windowed-only verb on a headless
        // engine): requests and describe answer "'<name>' is unavailable: <reason>" instead of unknown. A registered
        // entry of that name takes precedence.
        void markUnavailable(std::string_view name, std::string_view reason);
        // Drops a registered entry of that name, if any, and marks it unavailable: for a host that registers its
        // standard set first, then takes out what its configuration cannot serve (an engine without a GPU device).
        void withdraw(std::string_view name, std::string_view reason);
        // Lists, in describe's `choices` with `open: true`, the values an arg usually takes, read when describe runs
        // (a theme file, a loaded locale, a key name). Not enforced: the handler refuses what it does not know and
        // names the valid values. A no-op for an unregistered entry or arg.
        using SuggestFn = std::function<std::vector<std::string>()>;
        void suggestArgValues(std::string_view entry, std::string_view arg, SuggestFn values);
        // Rules from modules control sits below (hierarchy, UI ownership).
        void setComponentEditGuard(ComponentEditGuardFn guard);
        void setComponentAddedHook(ComponentAddedFn hook);
        // An entity's place in its world's hierarchy, which control sits below: entity and find rows carry `parent`
        // (omitted for a root), `children` (the direct child count), `enabled` (its own flag) and `active` (enabled
        // and every ancestor enabled) from it, and `layer` from the entity-layer reader.
        struct EntityRelation
        {
            Entity parent = Entity::undefined();
            u32 children = 0;
            bool enabled = true;
            bool active = true;
        };
        using HierarchyReaderFn = std::function<EntityRelation(const World& world, Entity entity)>;
        void setHierarchyReader(HierarchyReaderFn reader);
        // A layout command's last step: writes `audit` (entities, unrooted, unnamed) for the entities it created and,
        // when some are unnamed or unrooted (a transform with no parent and no children), `warnings` saying so; the
        // hierarchyAudit query lists them. Never fails the command. Without a hierarchy reader nothing is unrooted.
        void auditLayout(const World& world, std::span<const Entity> created, IFormatWriter& out) const;
        // Names a directory the files of rooted args (ArgDesc::root) live under; the `roots` query lists them. Only
        // host code adds one: no command does. A second call with the name replaces its directory, and its
        // doc unless doc is empty.
        void setRoot(std::string_view name, std::filesystem::path directory, std::string_view doc = {});
        // A root's directory; empty when it is not set.
        [[nodiscard]] std::filesystem::path root(std::string_view name) const;
        // The file a rooted arg's text names under the root's directory; an error message (empty on success) when
        // the root is not set, the text is not a plain relative file path (absolute, "..", a ':' part, a device
        // name, NUL, no file name) or a link inside the root leads out of it.
        [[nodiscard]] std::string resolveFile(std::string_view root, std::string_view text,
                                              std::filesystem::path& out) const;
        // The named roots, in name order.
        [[nodiscard]] std::vector<std::string> rootNames() const;
        // resolveFile for a directory: the same refusals, and empty text is the root's own directory.
        [[nodiscard]] std::string resolveDirectory(std::string_view root, std::string_view text,
                                                   std::filesystem::path& out) const;
        static constexpr sizet MAX_DIRECTORY_ENTRIES = 4096;
        // What a directory under a root holds (the listDirectory query's data, which the editor's file picker reads
        // too): path is "<root>[/<directory>]" (empty: one entry per root, as directories). extensions, a comma
        // list ("glb, .gltf", ASCII case-insensitive), keeps only the files with one of them; directories always
        // stay. foldersOnly drops everything else. An error message (empty on success) for a refused path (the
        // refusal of a root that is not set lists the valid ones), a directory that does not exist or is not one.
        // A root whose own directory does not exist yet lists as empty: writers create it. The entries are the
        // first MAX_DIRECTORY_ENTRIES in sorted order (directories, then names), `truncated` set when more matched;
        // the reply is bounded, but the enumeration and sort cover the whole directory (O(n log n), one call).
        // newestFirst orders the files by modification time, newest first (name breaks a tie), before the cap: a
        // directory of thousands of captures keeps the newest ones.
        [[nodiscard]] std::string listDirectory(std::string_view path, std::string_view extensions, bool foldersOnly,
                                                DirectoryListing& out, bool newestFirst = false) const;

        // Undo. Each world control serves keeps an undo history and a
        // command log (the CommandHistory service's `log` table). An undoable
        // command's handler records, while it runs, the requests that revert
        // it, in the order undo runs them (each an undoable command): argsJson
        // is the args object; undo adds the world and forwards the entity ids
        // in "id" and "parent" to entities undo or redo recreated since.
        // revives: the ids of the deleted entities the request recreates, by
        // recordCreated key. A run that records nothing stays out of the
        // history and the log. A set (or a batch of sets) with a string arg
        // "merge" equal to the newest step's, setting the same fields (or a seekAnimation or setAnimationSpeed of the
        // same animator, or an attachToBone of the same entity), folds into that step: one undo step (and one log row)
        // for an editor drag.
        void recordInverse(std::string_view command, std::string_view argsJson, std::span<const u64> revives = {});
        // Execute an already prepared array of undoable requests as one rollback-capable history step.
        std::string executeUndoableBatch(std::string_view world, std::string_view commandsJson, IFormatWriter& out);
        // Also forward the given old entity IDs to each request's created IDs while executing the batch.
        std::string executeUndoableBatchWithRevives(std::string_view world, std::string_view commandsJson,
                                                    std::string_view revivesJson, IFormatWriter& out);
        // True only for a writable entity or entity-list field matching the supplied value shape.
        [[nodiscard]] bool isEntityReferencePath(std::string_view path, bool list) const;
        // Whether the running command is a request stored in the undo history, run by undo, redo or a batch's
        // rollback; never true for a request a client sent, whatever its args. For an inverse that takes values
        // only the server held (a client has no way to name them), which it refuses from a request.
        [[nodiscard]] bool replayingHistory() const;
        // Resolve an entity ID stored inside an internal history payload to its current incarnation.
        [[nodiscard]] u64 resolveHistoryEntity(std::string_view world, u64 id) const;
        // For an undoable command whose repeat must run differently from the
        // request: argsJson replaces the request's args in the undo history
        // and the command log, so redo and replay run those instead.
        void recordForward(std::string_view argsJson);
        // For a command that is not undoable but must replay (input injection): on success the request is logged in
        // its world's command log with no undo step. argsJson, when given, is a JSON object whose members replace
        // the request's in the log (the arguments the handler resolved), so the replay does not depend on the state
        // it ran in.
        void recordReplay(std::string_view argsJson = {});
        // Like recordReplay, but the row goes to the named world's log with the request's args as they are, for a
        // command whose request does not name that world (`play`, which creates it: the log begins with the play).
        // argsJson, when given, is a JSON object whose members replace the request's in the log (as recordReplay's).
        void recordReplayIn(std::string_view world, std::string_view argsJson = {});
        // For a handler that creates entities: entity is the one created for
        // key (0 for a single entity, the document index for a scene document),
        // so undo and redo, which create them anew, can forward stored ids.
        void recordCreated(u32 key, Entity entity);
        // A token of the world's present scene state in its undo history: equal tokens mean no recorded scene edit
        // separates the two moments (undoing back to a position restores its token; a new edit, a merged drag step
        // included, mints one; a step of view commands, registerUndoableViewCommand, mints none), so "changed since
        // a save" is a token comparison. A failed undo or redo drops the failed step and renews every token. 0: no
        // such world. Edits that bypass the control commands are not seen.
        [[nodiscard]] u64 historyState(std::string_view world) const;
        // Scene layers as control sees them: a number (0 = the base) the module that owns layers gives an
        // entity. Each undo step records the layers its commands touched, so layerState is historyState for one
        // layer: it moves only for a step that touched the layer, and undoing back to a save restores it.
        // A command's layers are those it declared (touchLayer, for what it destroys or moves out of a layer) or, with
        // none declared, those of the live entities its requests name (id, parent, created); a request naming an
        // entity gone by then, or none at all (a clear), touches ALL_LAYERS unless the command declares. A view
        // command touches none. Without a reader every entity is layer 0. layerDirty also counts the commands a
        // running batch has done.
        static constexpr u32 ALL_LAYERS = UINT32_MAX;
        using EntityLayerFn = std::function<u32(const World& world, Entity entity)>;
        void setEntityLayerReader(EntityLayerFn reader);
        // saved: the layer equals its saved file once the command is done (a load), unless a later command of the
        // same step touches it again.
        void touchLayer(u32 layer, bool saved = false);
        [[nodiscard]] u64 layerState(std::string_view world, u32 layer) const;
        // A layer's save: markLayerSaved takes its present state as the saved one, markLayerUnsaved leaves it dirty
        // until the next save. layerDirty: its state is not its saved one; a layer never saved is clean at the first
        // position of the history and dirty after the history is cleared or renewed. 0 / false: no such world.
        void markLayerSaved(std::string_view world, u32 layer);
        void markLayerUnsaved(std::string_view world, u32 layer);
        [[nodiscard]] bool layerDirty(std::string_view world, u32 layer) const;
        // How many steps of the world's undo history undo and redo can walk now (the history query's undoable and
        // redoable); both 0 for an unknown world.
        struct UndoDepth
        {
            u32 undoable = 0;
            u32 redoable = 0;
            // Moves whenever the world's steps or provenance review change; 0 for
            // an unknown world. A reader that shows the steps rereads them when it moves.
            u64 revision = 0;
        };
        [[nodiscard]] UndoDepth undoDepth(std::string_view world) const;
        struct ProvenanceSummary
        {
            u64 id = 0;
            std::string_view source;
            std::string_view client;
            std::string_view time;
            bool unreviewed = false;
        };
        // Visits live edited entities without sorting or serializing. Host-serialized context only;
        // string views are borrowed for the callback, which must not mutate the world or history.
        // An unknown world visits nothing; ordering is unspecified.
        void visitProvenance(std::string_view world,
                             const std::function<void(const ProvenanceSummary&)>& visitor) const;
        // Empties the world's undo history, for a boundary after which stored inverses no longer apply (another scene
        // opened). The present state gets a new token. The command log stays.
        void clearHistory(std::string_view world);
        // For every world, served now or added later; applies at the next push.
        void setUndoLimits(const UndoLimits& limits);
        // A registration naming an entry of another owner or kind is refused, never replacing it: an error is
        // logged and kept for the registering owner. Returns and clears those messages (a module load fails on them).
        [[nodiscard]] std::vector<std::string> takeRefusals(std::string_view owner);
        // Drops every query and command the module registered (module unload).
        void unregisterOwner(std::string_view owner);
        // Whether any query or command is registered under owner.
        [[nodiscard]] bool hasOwner(std::string_view owner) const;
        // Writes the "entries" array of the describe query, limited to what owner registered (nothing for an empty
        // owner).
        void describeOwner(std::string_view owner, IFormatWriter& out) const;

        // Serves requests from stdin and replies on stdout.
        void listenStdio();
        // Serves one local client at a time on a named pipe; clients attach and
        // detach while the engine runs. Empty: this checkout's pipe (controlPipeName of the executable); a session
        // names a second engine of one checkout (`spite run --session`), reached by `spite request --session`.
        void listenPipe(std::string_view name = {}, std::string_view session = {});
        // Test seam: runs on the pipe thread each time a client has left and the next instance is not yet created (the
        // gap a late client lands in). Set before listenPipe.
        void setPipeGapHook(std::function<void()> hook);
        // Test seam: runs on the pipe thread after the keeper instance exists, before the dummy client connects to it.
        void setPipeKeeperHook(std::function<void()> hook);
        void setReceiveQueuedHook(std::function<void()> hook);
        void setPipeWritePendingHook(std::function<void()> hook);
        [[nodiscard]] bool stdioInputClosedForTest() const;
        [[nodiscard]] bool pipeConnected() const;
        // True once the stdio client closed its input.
        [[nodiscard]] bool stdioClosed() const;
        // The client attached now (the connected pipe client, else an open stdio one) and the name it announced with
        // hello (empty: none); none when no client is attached. Frame thread only. Advisory, as every source:
        // it names what the client said.
        struct AttachedClient
        {
            RequestSource source = RequestSource::Pipe;
            std::string name;
        };
        [[nodiscard]] std::optional<AttachedClient> attachedClient() const;
        // A counter that moves whenever attachedClient() may answer differently (a pipe connects or drops, stdio
        // opens or closes, a client announces a name): a reader polls it per frame and asks attachedClient() on a
        // change, so a quiescent frame takes no lock. Any thread.
        [[nodiscard]] u64 attachedEpoch() const;

        // For a handler whose result needs later frames (a GPU readback): takes
        // over the running request's reply, which completeReply sends; the
        // handler then returns success and writes nothing. Invalid when the
        // request came through handle() without a ReplyFn: it has nowhere to
        // go later. Not for undoable commands (batch, undo and redo answer at once).
        [[nodiscard]] DeferredReply deferReply();
        // Answers a deferred request on the frame thread: result fills the
        // result object, a non-empty return fails the request with it. A reply
        // to a client that detached since is dropped.
        void completeReply(DeferredReply reply, const ResultFn& result);
        // deferReply for a result computed on another thread: the returned function (any thread, once) queues its
        // ResultFn, which the next pump() runs and sends on the frame thread. Empty where deferReply is invalid.
        // Calling it after the server is gone does nothing.
        using AsyncReplyFn = std::function<void(ResultFn)>;
        [[nodiscard]] AsyncReplyFn deferReplyAsync();
        // deferReply for a request that steps the world it names (preferPlay: the play world when it names none and
        // one runs, as step does): answered with that world's simulation state (and
        // counts, written as u32 members first) at the first pump after the world has no pending steps. Unarmed, it
        // waits for armStepReply too, for steps a later frame requests (injected input with frames). Invalid as
        // deferReply is: the caller then answers at once.
        [[nodiscard]] DeferredReply deferUntilStepped(const ControlArgs& args, bool armed = true,
                                                      std::vector<std::pair<std::string, u32>> counts = {},
                                                      bool preferPlay = false);
        void armStepReply(DeferredReply reply);
        // For a command whose effect shows only after frames ran (a window change, a restyle): answers the running
        // request at the pump `frames` frames on (min 1) with what fill writes then, plus `settled: true`. Where
        // deferReply is invalid (no reply sink) or the command runs inside a batch, undo, redo or replay, fill runs
        // into out now and `settled` is false. Returns fill's error in that case; a deferred fill's error fails the
        // request.
        [[nodiscard]] std::string replyAfterFrames(u32 frames, ResultFn fill, IFormatWriter& out);

        // Executes queued requests and sends their replies, deferred ones on
        // the transport the request came from.
        void pump();
        // Runs the request in the next pump, before the queued client lines, as a client's request would run at the
        // frame boundary, then calls done with the reply. For a panel whose action changes what other panels observe
        // (a module image): it must not run inside the frame. unregisterOwner(owner) drops it unrun.
        void runAtFrameStart(std::string_view owner, std::string request,
                             std::function<void(const std::string& reply)> done);
        // Frame-thread only. Owner callbacks queued here are discarded on unregisterOwner.
        void runAtFrameStart(std::string_view owner, std::function<void()> callback);
        [[nodiscard]] bool runningFrameCallback(std::string_view owner) const;
        // The source of in-process requests (handle and runAtFrameStart outside a running request): Host until the
        // editor host sets Editor. A request run from inside a handler takes its caller's source.
        void setLocalSource(RequestSource source);
        // How many requests handle() gave an attribution (source, client, time): what a request run costs it, so a
        // test can show a kind of request pays none.
        [[nodiscard]] u64 attributedRequests() const;
        // How many commands reported a pipe or stdio source (a client's, as it said), run or refused after their args
        // were read, since the server started: the editor's status bar turns it into commands per minute. Frame thread.
        [[nodiscard]] u64 clientCommandCount() const;
        // One request line in, one reply line out (no trailing newline). A
        // deferred request returns an empty line; its reply goes to deferred.
        [[nodiscard]] std::string handle(std::string_view line, const ReplyFn& deferred = {});
        // Runs the named query with no args in process, its result written to `result`: the handler a request
        // runs, without the request and reply JSON (an editor view refreshing from a query). Returns an error
        // message, empty on success.
        [[nodiscard]] std::string query(std::string_view name, IFormatWriter& result);

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };

    // Why text is not a plain relative file or directory path (absolute, '..', a ':' part, a device name,
    // NUL; with no named root to resolve it under): the reason, empty when it is one. For a value a client sets that
    // the host resolves itself, such as a config path field. directory: empty text and a trailing separator pass.
    [[nodiscard]] SPITE_CONTROL_API std::string plainRelativePathProblem(std::string_view text, bool directory);

    // Writes valueJson into the field at path (field[.field|[i]]...) of root, a value schema describes, as set
    // does for a component field: the result carries path and the previous value. Returns an error, empty on
    // success; a refused value leaves the field as it was.
    [[nodiscard]] SPITE_CONTROL_API std::string setSchemaField(const StructSchema& schema, void* root,
                                                               std::string_view path, std::string_view valueJson,
                                                               IFormatWriter& out);
} // namespace spite
