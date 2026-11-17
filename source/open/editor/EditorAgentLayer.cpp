#include "EditorAgentLayer.hpp"
#include "EditorAgentPanel.hpp"
#include "EditorContext.hpp"
#include "EditorEntityLabel.hpp"
#include "EditorPanelUi.hpp"
#include "EditorSettings.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "engine/PlayWorld.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "serialization/FormatWalk.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiScope.hpp"
#include "ui/UiTween.hpp"
#include "uiview/UiViewContext.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using Mark = EditorAgentLayer::Mark;

        constexpr std::string_view OWNER = "editorAgentLayer";
        constexpr std::string_view EDITOR_SOURCE = "editor";
        constexpr std::string_view SCENE_WORLD = "scene";
        constexpr std::string_view PLAY_WORLD = "play";

        // "YYYY-MM-DDTHH:MM:SS.mmmZ" (the attribution's time) as epoch milliseconds
        std::optional<std::int64_t> parseUtcMs(const std::string_view text)
        {
            if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' ||
                text[16] != ':')
            {
                return std::nullopt;
            }
            const auto number = [&text](const sizet at, const sizet count) -> int
            {
                int value = 0;
                for (sizet i = at; i < at + count; ++i)
                {
                    if (i >= text.size() || text[i] < '0' || text[i] > '9')
                    {
                        return -1;
                    }
                    value = value * 10 + (text[i] - '0');
                }
                return value;
            };
            const int year = number(0, 4);
            const int month = number(5, 2);
            const int day = number(8, 2);
            const int hour = number(11, 2);
            const int minute = number(14, 2);
            const int second = number(17, 2);
            const int milli = text.size() >= 24 && text[19] == '.' ? number(20, 3) : 0;
            if (year < 0 || month < 0 || day < 0 || hour < 0 || minute < 0 || second < 0 || milli < 0)
            {
                return std::nullopt;
            }
            using namespace std::chrono;
            const year_month_day date{std::chrono::year{year}, std::chrono::month{static_cast<unsigned>(month)},
                                      std::chrono::day{static_cast<unsigned>(day)}};
            if (!date.ok())
            {
                return std::nullopt;
            }
            const auto days = duration_cast<milliseconds>(sys_days{date}.time_since_epoch()).count();
            return days + ((static_cast<std::int64_t>(hour) * 60 + minute) * 60 + second) * 1000 + milli;
        }

        std::int64_t systemNowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }
    } // namespace

    struct EditorAgentLayer::State
    {
        State(Engine& engine, const EditorSettingsService& settings) :
            engine(&engine), control(&engine.control()), settings(&settings), scope(editorUi(engine).ui()),
            clock(systemNowMs)
        {
        }

        // who edited an entity last among the non-editor sources, for the editing window
        struct Candidate
        {
            u64 id = 0;
            std::int64_t timeMs = 0;
            std::string client;
            std::string source;
        };

        struct Presence
        {
            bool shown = false;
            bool editing = false;
            u64 editingId = 0;
            std::int64_t editingMs = 0; // when its newest edit was made
            std::string client; // empty: the attached client announced no name
            std::string entity; // the edited entity's label while editing
            std::string operation;
            std::string batchState;
            bool operator==(const Presence&) const = default;
        };

        Engine* engine;
        ControlServer* control;
        const EditorSettingsService* settings;
        UiScope scope;
        std::function<std::int64_t()> clock;
        Button chip;
        std::function<void()> openPanel;
        Panel lamp;
        Label text;
        bool nameStale = true;
        u64 nameRevision = ~0ull;
        u64 activityRevision = ~0ull;
        sizet pendingReviews = 0;
        bool panelShown = false;
        std::string nameShown;

        bool seenEnabled = false;
        std::string seenWorld;
        u64 seenGeneration = ~0ull;
        std::unordered_map<u64, Mark> edited; // by entity id: the unreviewed ones
        Candidate candidate;
        Presence presence;
        // presenceNow's inputs, so a quiescent frame recomputes nothing: the attached client changes with the control
        // server's epoch, the candidate with a provenance read, the editing window with the clock
        std::optional<ControlServer::AttachedClient> attached;
        u64 seenEpoch = ~0ull;
        u64 seenBatchRevision = 0;
        ControlServer::BatchStatus batchStatus;
        std::int64_t batchAtMs = 0;
        bool presenceStale = true;
        std::string attachedSource;
        u64 revision = 0;
        u64 tableReads = 0; // provenance reads of every entity
        u64 changeReads = 0; // provenance reads of one
        u64 attachedReads = 0; // attachedClient() asks
        u64 presenceBuilds = 0; // presence recomputed (its label built while editing)

        // changeOf's cache: one entity of one world at one history revision
        std::string changeWorld;
        u64 changeId = 0;
        u64 changeGeneration = ~0ull;
        std::optional<Change> change;
        u64 changeRevision = 0;
        std::optional<LastEdit> last;
        u64 lastRevision = 0;

        void readEntity(std::string_view world, Entity entity);

        // callout(): the editing entity's newest step, read when the editing entity or its newest edit changed
        std::optional<Callout> calloutRead;
        u64 calloutRevision = 0;
        bool calloutStale = false;
        void noteCallout()
        {
            const bool editing = presence.editing && enabled();
            if (editing != (calloutKeyId != 0) ||
                (editing && (presence.editingId != calloutKeyId || presence.editingMs != calloutKeyMs)))
            {
                calloutKeyId = editing ? presence.editingId : 0;
                calloutKeyMs = editing ? presence.editingMs : 0;
                calloutStale = true;
                ++calloutRevision;
            }
        }
        u64 calloutKeyId = 0;
        std::int64_t calloutKeyMs = 0;
        void readCallout();

        [[nodiscard]] bool enabled() const { return settings->settings().agentLayer; }
        [[nodiscard]] std::string_view displayedWorld() const { return engine->playWorld() ? PLAY_WORLD : SCENE_WORLD; }
        [[nodiscard]] const World& worldOf() const
        {
            PlayWorld* play = engine->playWorld();
            return play ? play->world() : engine->world();
        }

        Json query(const std::string_view world, Json args) const
        {
            SPITE_ZONE("EditorAgentLayer.query");
            args["world"] = world;
            const std::string request = Json{{"query", "provenance"}, {"args", std::move(args)}}.dump();
            const std::string text = [&]
            {
                SPITE_ZONE("EditorAgentLayer.provenanceRequest");
                return control->handle(request);
            }();
            SPITE_COUNTER("editor.agentLayer.replyBytes", text.size());
            const Json reply = [&]
            {
                SPITE_ZONE("EditorAgentLayer.parseReply");
                return Json::parse(text, nullptr, false);
            }();
            SPITE_ZONE("EditorAgentLayer.copyResult");
            return reply.is_object() && reply.value("ok", false) ? reply["result"] : Json::object();
        }

        // Reads the provenance of every entity of the displayed world: the unreviewed ones and the newest
        // non-editor editor
        void readTable(const std::string_view world)
        {
            SPITE_ZONE("EditorAgentLayer.read");
            ++tableReads;
            std::unordered_map<u64, Mark> next;
            Candidate newest;
            {
                SPITE_ZONE("EditorAgentLayer.scanProvenance");
                u64 rows = 0;
                control->visitProvenance(world,
                                         [&](const ControlServer::ProvenanceSummary& row)
                                         {
                                             ++rows;
                                             if (row.unreviewed)
                                             {
                                                 next.emplace(row.id, Mark::Edited);
                                             }
                                             if (row.source != "pipe" && row.source != "stdio")
                                             {
                                                 return;
                                             }
                                             const std::int64_t at = parseUtcMs(row.time).value_or(0);
                                             if (at > newest.timeMs || (at == newest.timeMs && row.id > newest.id))
                                             {
                                                 newest = {row.id, at, std::string(row.client),
                                                           std::string(row.source)};
                                             }
                                         });
                SPITE_COUNTER("editor.agentLayer.rows", rows);
            }
            SPITE_ZONE("EditorAgentLayer.publishMarks");
            if (next != edited)
            {
                edited = std::move(next);
                ++revision;
            }
            candidate = std::move(newest);
        }

        [[nodiscard]] Presence presenceNow()
        {
            ++presenceBuilds;
            Presence next;
            const bool recentBatch = (batchStatus.source == "pipe" || batchStatus.source == "stdio") &&
                clock() - batchAtMs <= EDITING_WINDOW_MS;
            const bool editing = candidate.id != 0 && candidate.timeMs > 0 &&
                clock() - candidate.timeMs <= EDITING_WINDOW_MS && worldOf().isEntityAlive(Entity(candidate.id));
            next.shown = attached.has_value() || editing || recentBatch;
            next.client = attached ? attached->name : candidate.client;
            if (recentBatch)
            {
                next.client = batchStatus.client;
                next.operation = batchStatus.name;
                next.batchState = batchStatus.state;
            }
            if (attached && attached->name.empty() && editing && candidate.source == attachedSource)
            {
                next.client = candidate.client;
            }
            if (editing)
            {
                next.editing = true;
                next.editingId = candidate.id;
                next.editingMs = candidate.timeMs;
                std::string buffer;
                next.entity = std::string(entityLabel(buffer, worldOf(), Entity(candidate.id)));
            }
            return next;
        }

        // the attached client again when the control server says it may differ
        void readAttached()
        {
            const u64 epoch = control->attachedEpoch();
            if (epoch == seenEpoch)
            {
                return;
            }
            seenEpoch = epoch;
            ++attachedReads;
            attached = control->attachedClient();
            attachedSource =
                attached ? std::string(attached->source == RequestSource::Stdio ? "stdio" : "pipe") : std::string{};
            presenceStale = true;
        }

        [[nodiscard]] const ControlServer::AgentActivity* shownActivity() const
        {
            const auto* row = control->agentActivityFocus();
            return enabled() && row && (row->active() || presence.batchState != "active") ? row : nullptr;
        }

        void showChip()
        {
            const auto* row = shownActivity();
            const bool highlighted = enabled() && (row || presence.editing || !presence.batchState.empty());
            chip.setStyleClass(InternedString(highlighted ? "agent.bar.highlight" : "agent.bar"));
            const bool active = row ? row->active() : presence.batchState == "active" || presence.editing;
            lamp.setStyleClass(InternedString(active ? "agent.chip.lamp.editing" : "agent.bar.lamp"));
            nameStale = true;
        }

        [[nodiscard]] const char* messageKey() const
        {
            if (shownActivity())
                return "editor.agent.chip.activity";
            if (!enabled() || !presence.shown)
                return "editor.agent.chip.disconnected";
            if (!presence.batchState.empty())
            {
                if (presence.batchState == "active")
                    return "editor.agent.chip.batchActive";
                if (presence.batchState == "completed")
                    return "editor.agent.chip.batchComplete";
                if (presence.batchState == "rolledBack")
                    return "editor.agent.chip.batchRolledBack";
                if (presence.batchState == "rollbackFailed")
                    return "editor.agent.chip.batchRollbackFailed";
                return "editor.agent.chip.batchRefused";
            }
            const bool named = !presence.client.empty();
            return presence.editing ? (named ? "editor.agent.chip.editing" : "editor.agent.chip.unnamedEditing")
                                    : (named ? "editor.agent.chip.idle" : "editor.agent.chip.unnamed");
        }

        [[nodiscard]] const Localization* localization() const
        {
            return engine->services().contains<Localization>() ? &engine->services().get<Localization>() : nullptr;
        }

        void showFullText()
        {
            const Localization* messages = localization();
            if (!messages || !chip.isVisible() || (!nameStale && messages->revision() == nameRevision))
            {
                return;
            }
            nameStale = false;
            nameRevision = messages->revision();
            std::string scratch;
            const auto* row = shownActivity();
            const std::string phase = row ? agentActivityCaption(*row, *messages) : std::string{};
            const MessageArg args[]{{"client", std::string_view(presence.client)},
                                    {"entity", std::string_view(presence.entity)},
                                    {"operation", std::string_view(presence.operation)},
                                    {"activity", row ? std::string_view(row->title) : std::string_view{}},
                                    {"phase", std::string_view(phase)}};
            const std::string full(messages->resolve(InternedString(messageKey()), args, scratch));
            if (full != nameShown)
            {
                nameShown = full;
                text.setText(full);
                panelui::setTip(chip, full);
            }
        }

        void registerQuery()
        {
            control->registerQuery(
                "agentLayer", OWNER,
                "the agent layer the editor shows: whether it is on (editorSettings agentLayer), the displayed "
                "world, the attached control client and the entity it is editing now (its last non-editor edit "
                "within a few seconds), the marks (agent: being edited, edited: unreviewed non-editor changes) and "
                "the reads it made (provenance, attached client, presence), plus operation and batchState when active",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    out.writeBool("enabled", enabled());
                    out.writeString("world", displayedWorld());
                    out.writeU64("revision", revision);
                    out.writeBool("shown", presence.shown && enabled());
                    out.writeString("source", attachedSource);
                    out.writeString("client", presence.client);
                    out.writeBool("editing", presence.editing && enabled());
                    out.writeU64("editingId", enabled() ? presence.editingId : 0);
                    out.writeString("entity", presence.entity);
                    out.writeString("operation", presence.operation);
                    out.writeString("batchState", presence.batchState);
                    std::vector<std::pair<u64, Mark>> rows(edited.begin(), edited.end());
                    if (presence.editing && !edited.contains(presence.editingId))
                    {
                        rows.emplace_back(presence.editingId, Mark::Edited);
                    }
                    std::ranges::sort(rows, {}, &std::pair<u64, Mark>::first);
                    out.beginArray("marks", enabled() ? static_cast<u32>(rows.size()) : 0u);
                    for (const auto& [id, mark] : rows)
                    {
                        if (!enabled())
                        {
                            break;
                        }
                        out.beginObject("");
                        out.writeU64("id", id);
                        out.writeString("mark", presence.editing && id == presence.editingId ? "agent" : "edited");
                        out.endObject();
                    }
                    out.endArray();
                    out.writeU64("tableReads", tableReads);
                    out.writeU64("changeReads", changeReads);
                    out.writeU64("attachedReads", attachedReads);
                    out.writeU64("presenceBuilds", presenceBuilds);
                    return std::string{};
                });
        }
    };

    EditorAgentLayer::EditorAgentLayer(Engine& engine, const EditorSettingsService& settings) :
        m_state(std::make_unique<State>(engine, settings))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        using namespace panelui;
        UiRectDesc chipDesc =
            barDesc(Vec2{280.0f, 0.0f}, stack(UiLayoutDirection::Row, 0.0f, {}, UiLayoutAlign::Center));
        state.chip = scope.createButton("editor.bar.agent", "", chipDesc);
        bindGap(state.chip, SPACE_SM);
        bindPadding(state.chip, SPACE_MD, SPACE_NONE, SPACE_MD, SPACE_NONE);
        state.chip.bindLayoutToken(UiLayoutField::MinHeight, InternedString("size.control"));
        state.chip.setStyleClass(InternedString("agent.bar"));
        state.chip.setAccessibleNameKey(InternedString("editor.agent.button"));

        state.lamp = scope.createPanel("editor.bar.agent.lamp", barDesc(Vec2{}));
        bindBasis(state.lamp, SPACE_SM);
        state.lamp.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SPACE_SM));
        state.lamp.setParent(state.chip);
        state.lamp.setInteractable(false);
        state.lamp.setStyleClass(InternedString("agent.bar.lamp"));

        UiRectDesc titleDesc = barDesc(Vec2{});
        titleDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
        Label title = scope.createLabel("editor.bar.agent.title", "", titleDesc);
        title.setLocalizedText(InternedString("editor.agent.chip.connected"));
        title.setStyleClass(InternedString("agent.surface.section"));
        title.setParent(state.chip);
        title.setInteractable(false);

        UiRectDesc textDesc = barDesc(Vec2{});
        textDesc.layoutItem->grow = 1.0f;
        textDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        state.text = scope.createLabel("editor.bar.agent.text", "", textDesc);
        state.text.setParent(state.chip);
        state.text.setInteractable(false);
        state.text.setStyleClass(InternedString("agent.chip.text"));
        state.chip.onClick(
            [&state]
            {
                if (state.openPanel)
                    state.openPanel();
            });
        setTip(state.chip);
        state.registerQuery();
        scope.commit();
    }

    EditorAgentLayer::~EditorAgentLayer() { m_state->control->unregisterOwner(OWNER); }

    Widget EditorAgentLayer::chip() const { return m_state->chip; }

    void EditorAgentLayer::onOpen(std::function<void()> action) { m_state->openPanel = std::move(action); }

    void EditorAgentLayer::setPanelShown(const bool shown) { m_state->panelShown = shown; }

    bool EditorAgentLayer::enabled() const { return m_state->enabled(); }

    u64 EditorAgentLayer::revision() const { return m_state->revision; }

    bool EditorAgentLayer::hasMarks() const
    {
        const State& state = *m_state;
        return state.enabled() && (!state.edited.empty() || state.presence.editing);
    }

    void EditorAgentLayer::setClock(std::function<std::int64_t()> nowMs)
    {
        m_state->clock = nowMs ? std::move(nowMs) : std::function<std::int64_t()>(systemNowMs);
        m_state->presenceStale = true;
    }

    Mark EditorAgentLayer::markOf(const Entity entity) const
    {
        const State& state = *m_state;
        if (!state.enabled())
        {
            return Mark::None;
        }
        if (state.presence.editing && entity.id() == state.presence.editingId)
        {
            return Mark::Agent;
        }
        return state.edited.contains(entity.id()) ? Mark::Edited : Mark::None;
    }

    void EditorAgentLayer::update()
    {
        State& state = *m_state;
        if (!state.enabled())
        {
            // off: the marks are gone at once and nothing is read until it is switched on
            if (state.seenEnabled)
            {
                state.seenEnabled = false;
                state.edited.clear();
                state.candidate = {};
                state.presence = {};
                state.noteCallout();
                if (state.change)
                {
                    state.change.reset();
                    ++state.changeRevision;
                }
                state.changeGeneration = ~0ull;
                state.seenGeneration = ~0ull;
                ++state.revision;
                state.showChip();
            }
            state.showFullText();
            return;
        }
        const std::string_view world = state.displayedWorld();
        const u64 generation = state.control->undoDepth(world).revision;
        const bool read = !state.seenEnabled || generation != state.seenGeneration || world != state.seenWorld;
        if (read)
        {
            state.seenEnabled = true;
            state.seenGeneration = generation;
            state.seenWorld = std::string(world);
            state.readTable(world);
            state.presenceStale = true;
        }
        state.readAttached();
        if (const auto status = state.control->batchStatus(); status.revision != state.seenBatchRevision)
        {
            state.seenBatchRevision = status.revision;
            state.batchStatus = status;
            state.batchAtMs = state.clock();
            state.presenceStale = true;
        }
        if (!state.presence.batchState.empty() && state.clock() - state.batchAtMs > EDITING_WINDOW_MS)
            state.presenceStale = true;
        if (state.presence.editing && state.clock() - state.candidate.timeMs > EDITING_WINDOW_MS)
        {
            state.presenceStale = true; // the window ended
        }
        if (state.presenceStale)
        {
            state.presenceStale = false;
            State::Presence next = state.presenceNow();
            if (read || next != state.presence)
            {
                if (next != state.presence)
                {
                    // only a change of what the marks show redraws a reader: a client attaching or detaching is the
                    // chip's alone (a pipe request per query would otherwise redraw the hierarchy at every one)
                    const bool marks =
                        next.editing != state.presence.editing || next.editingId != state.presence.editingId;
                    state.presence = std::move(next);
                    state.revision += marks ? 1 : 0;
                    state.noteCallout();
                }
                state.showChip();
            }
        }
        if (state.activityRevision != state.control->agentActivityRevision())
        {
            state.activityRevision = state.control->agentActivityRevision();
            state.showChip();
            UiContext& ui = editorUi(*state.engine).ui();
            const AgentMotion motion = AgentMotion::read(ui.layoutTokens());
            if (state.shownActivity())
                pulseAgentActivity(ui, state.lamp.entity(), motion.fast);
            const sizet pending = agentPendingReviews(*state.control);
            if (pending > state.pendingReviews && !state.panelShown && state.enabled() && !ui.tweens().reducedMotion())
            {
                pulseAgentActivity(ui, state.chip.entity(), motion.fast);
                (void)ui.tweens().start({.element = state.chip.entity(),
                                         .component = "UiVisual",
                                         .field = "scale",
                                         .to = Vec4{1.0f, 1.0f, 0.0f, 0.0f},
                                         .from = Vec4{1.06f, 1.06f, 0.0f, 0.0f},
                                         .duration = motion.normal,
                                         .ease = UiEase::OutBack});
            }
            state.pendingReviews = pending;
        }
        state.showFullText();
    }

    // Reads the provenance of one entity when its history changed (or another entity or world is
    // asked): the change the inspector's agent block shows and the last edit its readout strip shows, from one query.
    void EditorAgentLayer::State::readEntity(const std::string_view world, const Entity entity)
    {
        const u64 generation = control->undoDepth(world).revision;
        if (changeWorld == world && changeId == entity.id() && changeGeneration == generation)
        {
            return;
        }
        changeWorld = std::string(world);
        changeId = entity.id();
        changeGeneration = generation;
        std::optional<Change> read;
        std::optional<LastEdit> readLast;
        ++changeReads;
        const Json result = query(world, Json{{"id", entity.id()}, {"previous", true}});
        if (result.contains("entities") && !result["entities"].empty())
        {
            const Json& row = result["entities"][0];
            Change next;
            next.step = row.value("step", u64{0});
            next.stepEntities = row.value("stepEntities", u32{0});
            next.source = row.value("source", std::string{});
            next.client = row.value("client", std::string{});
            next.time = row.value("time", std::string{});
            next.timeMs = parseUtcMs(next.time).value_or(0);
            next.byEditor = next.source == EDITOR_SOURCE;
            readLast = LastEdit{next.source, next.client, next.timeMs};
            for (const Json& field : row.value("unreviewed", Json::array()))
            {
                next.unreviewed.push_back(field.get<std::string>());
            }
            if (!next.byEditor)
            {
                for (const Json& was : row.value("previous", Json::array()))
                {
                    next.previous.push_back(
                        {was.value("field", std::string{}), was.contains("value") ? was["value"].dump() : ""});
                }
            }
            if (!next.unreviewed.empty())
            {
                read = std::move(next);
            }
        }
        if (read != change)
        {
            change = std::move(read);
            ++changeRevision;
        }
        if (readLast != last)
        {
            last = std::move(readLast);
            ++lastRevision;
        }
    }

    // The step that made the editing window: its first overwritten field with the value it had, and the value now.
    void EditorAgentLayer::State::readCallout()
    {
        calloutStale = false;
        calloutRead.reset();
        if (calloutKeyId == 0)
        {
            return;
        }
        const std::string_view world = displayedWorld();
        const Json result = query(world, Json{{"id", calloutKeyId}, {"previous", true}});
        Callout next{.entity = Entity(calloutKeyId), .client = presence.client, .timeMs = presence.editingMs};
        if (result.contains("entities") && !result["entities"].empty())
        {
            const Json& row = result["entities"][0];
            next.step = row.value("step", u64{0});
            const Json& previous = row.contains("previous") ? row["previous"] : Json::array();
            next.fields = static_cast<u32>(previous.size());
            if (!previous.empty())
            {
                next.field = previous[0].value("field", std::string{});
                next.was = previous[0].contains("value") ? previous[0]["value"].dump() : std::string{};
                const Json entity = Json::parse(
                    control->handle(
                        Json{{"query", "entity"}, {"args", {{"world", world}, {"id", calloutKeyId}}}}.dump()),
                    nullptr, false);
                const Json* value =
                    entity.is_object() && entity.value("ok", false) && entity["result"].contains("components")
                    ? &entity["result"]["components"]
                    : nullptr;
                for (sizet at = 0; value && at < next.field.size();)
                {
                    const sizet end = std::min(next.field.find('.', at), next.field.size());
                    const std::string key = next.field.substr(at, end - at);
                    if (value->is_object() && value->contains(key))
                    {
                        value = &(*value)[key];
                    }
                    else if (value->is_array() && !key.empty() &&
                             std::ranges::all_of(key, [](const char c) { return c >= '0' && c <= '9'; }) &&
                             std::stoul(key) < value->size())
                    {
                        value = &(*value)[std::stoul(key)];
                    }
                    else
                    {
                        value = nullptr;
                    }
                    at = end + 1;
                }
                next.now = value ? value->dump() : std::string{};
            }
        }
        calloutRead = std::move(next);
    }

    const EditorAgentLayer::Callout* EditorAgentLayer::callout()
    {
        State& state = *m_state;
        if (state.calloutStale)
        {
            state.readCallout();
        }
        return state.calloutRead ? &*state.calloutRead : nullptr;
    }

    u64 EditorAgentLayer::calloutRevision() const { return m_state->calloutRevision; }

    const EditorAgentLayer::Change* EditorAgentLayer::changeOf(const std::string_view world, const Entity entity)
    {
        State& state = *m_state;
        if (!state.enabled() || !entity.isValid())
        {
            return nullptr;
        }
        state.readEntity(world, entity);
        return state.change ? &*state.change : nullptr;
    }

    const EditorAgentLayer::LastEdit* EditorAgentLayer::lastEditOf(const std::string_view world, const Entity entity)
    {
        State& state = *m_state;
        if (!entity.isValid())
        {
            return nullptr;
        }
        state.readEntity(world, entity);
        return state.last ? &*state.last : nullptr;
    }

    u64 EditorAgentLayer::lastEditRevision() const { return m_state->lastRevision; }

    u64 EditorAgentLayer::changeRevision() const { return m_state->changeRevision; }
} // namespace spite
