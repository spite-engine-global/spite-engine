#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ecs/EcsTypes.hpp"

namespace spite
{
    class Engine;
    class EditorSettingsService;
    class Widget;

    // The agent layer (Agent layer): what a control client changed, shown in the editor, read from
    // the edit attribution (provenance, the attached client). Everything here is what clients REPORTED
    // (`source` is advisory bookkeeping, not a trust boundary), so no mark is a guarantee; the chip's tooltip says so.
    //
    //   - the header's Agent button (chip()): an anchored menu with current presence, recent activity and an action
    //     to open the full Agent panel
    //   - marks: AGENT on the entity being edited (the one whose last scene-edit step is the newest non-editor one
    //     and within EDITING_WINDOW_MS), EDITED on every entity with unreviewed non-editor changes
    //   - changeOf: one entity's unreviewed change with the values its last step overwrote, for the inspector
    //
    // `editorSettings agentLayer=off` hides the marks and clears changeOf; the neutral Agent button remains available.
    // Cost: per frame three counters (the command generation, the attached-client epoch, and the clock while an
    // entity is being edited); the presence is rebuilt only when one moves; the provenance table (the all-entities
    // form) is read once per command generation or world switch, never per frame, and one entity's change (the id
    // form) once per generation while the inspector shows it. Query `agentLayer` (owner editorAgentLayer): the
    // state, the marks and the read counters.
    class EditorAgentLayer
    {
    public:
        enum class Mark : std::uint8_t
        {
            None,
            Agent, // the entity being edited by a non-editor client now
            Edited, // unreviewed non-editor changes
        };

        struct Previous
        {
            std::string field;
            std::string value; // JSON text, empty when the value was too large to carry (valueBytes)
            bool operator==(const Previous&) const = default;
        };

        // One entity's change, as the inspector shows it.
        struct Change
        {
            u64 step = 0; // the newest scene-edit step that touched the entity; 0: it left the undo history
            u32 stepEntities = 0; // how many entities that step touched: Revert undoes all of them
            bool byEditor = false; // that step is the editor's: an agent change is behind it, Revert would undo it
            std::string source;
            std::string client;
            std::string time; // as recorded, UTC
            std::int64_t timeMs = 0; // time as epoch milliseconds, 0 when unreadable
            std::vector<std::string> unreviewed; // fields non-editor edits touched since the last review
            std::vector<Previous> previous; // what that step overwrote (empty for a step that is not the agent's)
            bool operator==(const Change&) const = default;
        };

        // One entity's last scene-edit step, whoever made it and whether or not it was reviewed.
        struct LastEdit
        {
            std::string source; // editor, host, stdio or pipe, as the request reported it
            std::string client;
            std::int64_t timeMs = 0; // epoch milliseconds, 0 when unreadable
            bool operator==(const LastEdit&) const = default;
        };

        // What the Scene view's callout says: the entity being edited now (the one markOf calls Agent), the
        // client, and the first field its newest step overwrote with the value it had and the one it holds, JSON text
        // (empty: too large to carry, or not read). `fields` counts what the step overwrote; 0: it set no value.
        struct Callout
        {
            Entity entity = Entity::undefined();
            std::string client;
            std::int64_t timeMs = 0;
            u64 step = 0;
            u32 fields = 0;
            std::string field;
            std::string was;
            std::string now;
            bool operator==(const Callout&) const = default;
        };

        // How long after its last edit a client counts as editing an entity.
        static constexpr std::int64_t EDITING_WINDOW_MS = 5000;

        EditorAgentLayer(Engine& engine, const EditorSettingsService& settings);
        ~EditorAgentLayer();
        EditorAgentLayer(const EditorAgentLayer&) = delete;
        EditorAgentLayer& operator=(const EditorAgentLayer&) = delete;

        // The top-right status bar opens the full Agent panel when activated.
        [[nodiscard]] Widget chip() const;
        void onOpen(std::function<void()> action);
        // The Agent panel reports whether it is on screen: the button pulses once when a change starts awaiting review
        // while it is not.
        void setPanelShown(bool shown);
        // Once per frame, before the panels that read the marks.
        void update();

        [[nodiscard]] bool enabled() const;
        // Bumped whenever a mark or the layer's switch changed (not the chip's client or visibility): a reader of the
        // marks redraws on a new value.
        [[nodiscard]] u64 revision() const;
        // Whether any entity carries a mark now.
        [[nodiscard]] bool hasMarks() const;
        // The mark of an entity of the displayed world (the play world while playing, else the scene).
        [[nodiscard]] Mark markOf(Entity entity) const;
        // The change of an entity of `world` ("scene" or "play"): null when the layer is off or it has no
        // unreviewed change. Valid until the next call; reads the provenance of that one entity when a command ran
        // since the last read of it.
        [[nodiscard]] const Change* changeOf(std::string_view world, Entity entity);
        // Bumped when changeOf's answer for the entity it last read differs from the one before (a read that finds
        // the same change, or none again, bumps nothing): the inspector rebuilds its rows on a new value.
        [[nodiscard]] u64 changeRevision() const;
        // The last edit of an entity of `world`: null when it was never edited. Not switched off by agentLayer (it is
        // the entity's provenance, not a mark); shares changeOf's one-entity read, so asking for both costs one query
        // per command generation. Valid until the next call.
        [[nodiscard]] const LastEdit* lastEditOf(std::string_view world, Entity entity);
        // Bumped when lastEditOf's answer for the entity it last read differs from the one before.
        [[nodiscard]] u64 lastEditRevision() const;

        // The callout, null when no entity is being edited or the layer is off. Reads the provenance and the entity
        // once per new editing entity or newest edit, never per call or frame.
        [[nodiscard]] const Callout* callout();
        // Bumped when callout()'s answer may differ: the editing entity, its newest edit or the layer's switch moved.
        [[nodiscard]] u64 calloutRevision() const;

        // Replaces the clock the editing window reads (epoch milliseconds, UTC): a test steps time with it.
        void setClock(std::function<std::int64_t()> nowMs);

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
