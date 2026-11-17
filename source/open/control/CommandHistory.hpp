#pragma once

#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/Platform.hpp"
#include "base/Schema.hpp"
#include "control/CommandLogRow.hpp"
#include "control/ControlServer.hpp"

namespace spite
{
    class ServiceLocator;

    // A stored command request. created[key] is the entity the last run of the
    // request created for key (see ControlServer::recordCreated); 0 = none.
    struct CommandRequest
    {
        std::string command;
        nlohmann::json args;
        std::vector<u64> created;
    };

    // One mutating command and the requests that revert it, in run order.
    struct CommandRecord
    {
        CommandRequest forward;
        std::vector<CommandRequest> inverse;
    };

    // Who sent a request and when: never read by what the request does.
    struct Attribution
    {
        std::string source; // editor, host, stdio or pipe
        std::string client; // the name the connection announced; empty when none
        std::string time; // UTC, "YYYY-MM-DDTHH:MM:SS.mmmZ"
        u64 frame = 0; // the control server's pump count
    };

    // One undo step: a command, or every command of a batch. bytes is
    // measured by CommandHistory (push, remeasure). merge: the request's
    // "merge" arg (see push).
    struct UndoStep
    {
        u64 index = 0;
        u64 state = 0; // the scene token after this step (CommandHistory::state)
        std::vector<CommandRecord> records;
        sizet bytes = 0;
        std::string merge;
        std::string label;
        bool view = false; // every command of the step is a view command: undoable, but no scene edit
        // the scene layers the step touched (ControlServer::touchLayer): few, usually one; ControlServer::ALL_LAYERS
        // stands for every layer; empty for a view step and a step that touches no scene entity
        std::vector<u32> layers;
        // the layers that equal their saved file when the step is done (a load): record takes the step's token as
        // their saved one. Always a subset of layers.
        std::vector<u32> savedLayers;
        Attribution by;
    };

    // What the last scene-edit steps did to an entity. fields: what the last step touched (a set's path,
    // an add/removeComponent's component, else the command); unreviewed: what non-editor steps touched since the
    // last review, empty when reviewed.
    struct EntityProvenance
    {
        u64 step = 0; // 0 once that step and every earlier one touching the entity fell out of the history: only an
                      // entry with unreviewed fields outlives it
        Attribution by;
        std::vector<std::string> fields;
        struct UnreviewedField
        {
            std::string name;
            u64 firstStep;
        };
        std::vector<UnreviewedField> unreviewed;
    };

    // The first later step that stops a step from being reverted (revertConflict): it touched the same field of
    // entity (a field the step's requests or the inverse requests the revert runs write), or entity itself, which
    // the reverted step created (or a descendant of it) or the later one destroyed.
    struct RevertConflict
    {
        u64 step = 0;
        u64 entity = 0;
        std::string field;
    };

    // A world's undo history and command log, a service of that
    // world so its `log` table lands in the world's tables and snapshots.
    // Touched only on the frame thread, inside the control pump.
    class CommandHistory
    {
    public:
        static constexpr const char* SPITE_SERVICE_NAME = "CommandHistory";

        // The world's history: registered on services (with its `log` table)
        // when they hold none yet.
        static CommandHistory& attach(ServiceLocator& services);
        static void detach(ServiceLocator& services);

        // A request's args with the values over SUMMARY_BYTES replaced by their size, unless full.
        static constexpr sizet SUMMARY_BYTES = 256;
        [[nodiscard]] static nlohmann::json summarizeArgs(const nlohmann::json& requestArgs, bool full);

        // Pushes the step of an applied request and logs the request. A new
        // step drops every undone one (redo) and the oldest past the limits.
        // A step with a merge key whose sets name the same fields as the newest
        // step, done and under the same key, folds into it instead: that step
        // keeps its inverses and takes the new values, so a number-field drag
        // is one undo step; while the step's request is still the newest log
        // row, the row takes the new request too, so a drag of any length is
        // one row and the log replays to the same state.
        // A scene-edit step (not view) updates the provenance of every entity its requests name or create; a step
        // that leaves the history (past the limits, a redo a new step discards, abandoned) hands the entries naming
        // it to the entity's newest earlier step still in the history; with none the entry goes, but one with
        // unreviewed fields stays so the review is not lost, with step 0.
        void record(UndoStep step, const std::string& command, const nlohmann::json& args, u64 durationUs);
        // Re-measures the step at position after undo or redo rewrote its requests.
        void remeasure(sizet position);
        // Replay order: every successfully applied recorded command, undo, redo
        // and batch (record logs the ones that push a step).
        // error: empty for an applied request, else the row records a failed edit command, which replay skips.
        void log(const std::string& command, const nlohmann::json& args, const Attribution& by, u64 durationUs,
                 std::string_view error = {});
        // log() for a `step`: while the newest row is an applied step of the same world, kind (frames or fixed) and
        // source with the same other args, its count grows by the request's instead of a new row, up to maxCount (the
        // most one request may run), so a client stepping one frame at a time does not exhaust the log; the folded
        // row replays identically.
        void logStep(const nlohmann::json& args, u64 maxCount, const Attribution& by, u64 durationUs);
        // Empties the steps and the provenance; the present state gets a new token, as the world may have changed
        // behind the history.
        void clear();
        [[nodiscard]] const std::unordered_map<u64, EntityProvenance>& provenance() const { return m_provenance; }
        // Marks the non-editor edits of a live entity reviewed (of every live entity without id); returns how many
        // were unreviewed. An id's read costs one lookup, the all form walks the table.
        u32 review(std::optional<u64> id, const std::function<bool(u64)>& alive);
        // Whether the provenance table doubled since its last sweep: the cue for the caller to pass sweepProvenance
        // its world's liveness test, so the table of a long session stays within twice its live entities.
        [[nodiscard]] bool provenanceSweepDue() const { return m_provenance.size() > m_provenanceSweepAt; }
        // Drops the entries of entities alive says are gone, but not of one a step in the history can bring back
        // (undo of its delete, redo of its create): its marks wait for the revival.
        void sweepProvenance(const std::function<bool(u64)>& alive);
        // The scene token of the present position: the one of the newest done step, or the base token with none
        // done. A step with a scene edit (a folded merge included) mints a token and undo and redo move between
        // them; a view step (UndoStep::view) keeps the token before it, so equal tokens mean no recorded scene edit
        // separates the two moments, whatever view steps lie between. A token whose step was dropped (redo discarded
        // by a new step) is never seen again, and one whose step was evicted becomes the base token.
        [[nodiscard]] u64 state() const { return m_done == 0 ? m_baseState : m_steps[m_done - 1].state; }
        // state() for one scene layer: the token of the newest done step that touched it, so undoing back to a save
        // restores the token and the layer reads clean again; with no such step the token of the newest evicted one
        // that did, or the base of the history. Walks the done steps (at most the step limit); a cold-path reader.
        [[nodiscard]] u64 layerState(u32 layer) const;
        // A layer's saved token (layerState at its last save or load) and whether the layer has left it. A layer
        // without a saved token is clean at the history's first position, dirty after clear() and renewStates().
        // markLayerUnsaved makes it dirty until the next save.
        void markLayerSaved(u32 layer) { m_layerSaved[layer] = layerState(layer); }
        void markLayerUnsaved(u32 layer) { m_layerSaved[layer] = 0; }
        [[nodiscard]] bool layerDirty(u32 layer) const
        {
            const auto saved = m_layerSaved.find(layer);
            return layerState(layer) != (saved == m_layerSaved.end() ? FIRST_STATE : saved->second);
        }
        // Drops the undone steps and the steps from position on (a step whose undo or redo failed part way, so the
        // world is in no state the history names) and mints a token for every remaining step, so the present
        // position never equals a token taken earlier. The steps below position stay.
        void abandonFrom(sizet position);
        // Mints new tokens for the base and every step (the view steps sharing one as before): the world changed
        // behind the history, which stays.
        void renewStates();

        // The position of the step with that index, none once it left the history.
        [[nodiscard]] std::optional<sizet> find(u64 index) const;
        // The fields a scene-edit step touched, by the live id of each entity.
        [[nodiscard]] std::unordered_map<u64, std::vector<std::string>> touchedFields(const UndoStep& step) const;
        // Why the done step at position cannot be reverted: the first done step after it that touched a field of an
        // entity the step or its inverse requests write (a field is the same when equal or one names a part of the
        // other), any entity it created or a descendant of one (parentOf gives an entity's live parent, 0 for none),
        // or one it touched and the later one destroyed.
        [[nodiscard]] std::optional<RevertConflict> revertConflict(sizet position,
                                                                   const std::function<u64(u64)>& parentOf) const;
        // Marks the fields the step touched reviewed on their entities.
        void markReviewed(const UndoStep& step);

        // The live entity a stored id names now: ids of entities that undo or
        // redo recreated forward to the recreation.
        [[nodiscard]] u64 resolve(u64 id) const;
        // dead is a chain end (resolve's result); every alias onto it is repointed, so chains stay one hop. The
        // entity's provenance entry moves to live.
        void alias(u64 dead, u64 live);
        [[nodiscard]] sizet aliasCount() const { return m_aliases.size(); }
        [[nodiscard]] sizet longestAliasChain() const;

        [[nodiscard]] UndoStep& step(sizet position) { return m_steps[position]; }
        [[nodiscard]] const std::deque<UndoStep>& steps() const { return m_steps; }
        [[nodiscard]] const std::deque<CommandLogRow>& logRows() const { return m_log; }
        // steps [0, done) are applied, [done, size) undone
        [[nodiscard]] sizet done() const { return m_done; }
        void setDone(sizet done);
        // Changes whenever the steps, their position, a step's requests or provenance review do, and is never shared
        // by two histories (a play world made again has a new one).
        [[nodiscard]] u64 revision() const { return m_revision; }
        void setLimits(const UndoLimits& limits) { m_limits = limits; }
        [[nodiscard]] sizet bytes() const { return m_bytes; }
        // the applied rows' bytes (UndoLimits::logBytes)
        [[nodiscard]] sizet logBytes() const { return m_logBytes; }
        [[nodiscard]] u64 droppedLogRows() const { return m_droppedLogRows; }

    private:
        void touch() { m_revision = ++s_revisionClock; }
        // Whether the step folded into the newest one.
        bool push(UndoStep step);
        // Drops the oldest log rows past the limits.
        void trimLog();
        static sizet measure(const UndoStep& step);
        // Keeps the aliases of the ids the remaining steps name, each pointing
        // straight at its live id.
        void pruneAliases();
        void noteProvenance(const UndoStep& step);
        // Called as the oldest step is evicted: its layers' tokens survive as their bases.
        void retireLayers(const UndoStep& step);
        // Called as a step leaves the history (still in m_steps): an entry naming it points at the entity's newest
        // earlier step, else goes, or loses its step when it holds unreviewed fields.
        void forgetProvenance(const UndoStep& step);

        static inline u64 s_revisionClock = 0;
        u64 m_revision = ++s_revisionClock;
        UndoLimits m_limits;
        std::unordered_map<u64, EntityProvenance> m_provenance;
        std::deque<UndoStep> m_steps;
        sizet m_done = 0;
        sizet m_bytes = 0;
        u64 m_nextStep = 1;
        static constexpr u64 FIRST_STATE = 1; // tokens start at 1: 0 means no history
        u64 m_baseState = FIRST_STATE;
        u64 m_layerEpoch = FIRST_STATE; // layerState with no step to name: changes only when the tokens are renewed
        std::unordered_map<u32, u64> m_layerSaved;
        std::unordered_map<u32, u64> m_layerBases; // a layer's token once its newest touching step was evicted
        u64 m_nextState = 1;
        std::deque<CommandLogRow> m_log;
        sizet m_logBytes = 0; // applied rows only
        // the failed rows' sequences, oldest first, and their bytes: bounded apart from the applied rows
        std::deque<u64> m_failedSequences;
        sizet m_failedBytes = 0;
        u64 m_nextSequence = 1;
        u64 m_droppedLogRows = 0;
        // the step whose request the newest log row is (a merge step), 0 = none
        u64 m_logTail = 0;
        std::unordered_map<u64, u64> m_aliases;
        // live id -> the ids aliased onto it, so alias() repoints them without a scan
        std::unordered_map<u64, std::vector<u64>> m_aliasedOnto;
        static constexpr sizet MIN_PRUNE_AT = 64;
        sizet m_pruneAt = MIN_PRUNE_AT; // alias() prunes when the map outgrows it
        sizet m_provenanceSweepAt = MIN_PRUNE_AT;
    };
} // namespace spite
