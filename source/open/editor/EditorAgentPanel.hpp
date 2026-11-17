#pragma once

#include <memory>
#include <string>

#include "control/ControlServer.hpp"

namespace spite
{
    class Engine;
    class EditorAgentLayer;
    class EditorFeedback;
    class Localization;
    class UiContext;
    class UiLayoutTokenTable;
    class Widget;

    std::string agentActivityCaption(const ControlServer::AgentActivity& activity, const Localization& messages);
    // Operations waiting for the person's review: the Agent panel's count and the title-bar button's trigger.
    [[nodiscard]] sizet agentPendingReviews(const ControlServer& control);

    // The motion durations of the agent surfaces, read from the base sheet's `motion.*` tokens (seconds).
    struct AgentMotion
    {
        float fast = 0.19f; // a crossfade, a pulse
        float normal = 0.32f; // a slide, a pop, a shake
        float slow = 0.45f; // one beat of the working dots
        float loop = 0.7f; // one beat of the breathing icon
        [[nodiscard]] static AgentMotion read(const UiLayoutTokenTable& tokens);
    };
    // One-shot pulse of an element: it flashes in and slides a few pixels into place, after `delay` seconds.
    void pulseAgentActivity(UiContext& ui, Entity element, float seconds, float delay = 0.0f);

    // The Agent panel (Agent layer): what control clients are doing and what they
    // changed, one scrolling list grouped Now, Needs review and Earlier. A card is the operation's title, a fixed
    // status pill (Working and Your review in the agent colour, Needs attention in the error colour, the rest muted,
    // each with a tooltip saying what it means) and a muted line with its age and kind, kept current once a second;
    // its details hold the live step, the agent's result or reason, what the undo step changed (field, before ->
    // after), chips that select and frame the changed entities, and Keep / Revert naming the step they act on; K
    // keeps the expanded card. With two or more changes awaiting review the header offers Keep all.
    //
    // Live: a client's request keeps the header live for a short window (the lamp breathes, a strip under it slides,
    // the caption says Reading the scene or Applying changes), and the latest read shows as a transient card while it
    // lasts. A new operation shows as Working for a minimum time even when it finished within one frame, so its
    // animation is seen; that hold is presentation only, the feed keeps its own state.
    //
    // The cards are pooled by operation (grown on demand up to the feed's 64) and reordered in place; the details are
    // one panel moved under the expanded card. Cost: while shown, an integer compare per frame for the feed revision,
    // the state generation, the age bucket and the live operation's second; the step's changes are read once per
    // feed revision while a card with a step is expanded, never per frame. Motion: a new card slides in, a status
    // change crossfades its pill, a kept result pops its check, a reverted one slides back, a failed one flashes and
    // shakes once, the pending count bumps once; only the focused live operation loops. Reduced motion snaps and
    // hiding the panel cancels. Query `agentPanelView` (owner editorAgentPanel): the groups, the rows with their
    // status, kind and age, the expanded row and the pending and failed counts, read from the feed, not the widgets.
    class EditorAgentPanel
    {
    public:
        EditorAgentPanel(Engine& engine, EditorFeedback& feedback, EditorAgentLayer& agent);
        ~EditorAgentPanel();
        EditorAgentPanel(const EditorAgentPanel&) = delete;
        EditorAgentPanel& operator=(const EditorAgentPanel&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
