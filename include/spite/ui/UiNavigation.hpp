#pragma once

#include <span>
#include <vector>

#include "base/MathTypes.hpp"
#include "ui/UiExport.hpp"
#include "ui/UiInputSnapshot.hpp"
#include "ui/UiLayout.hpp"

namespace spite
{
    // Thresholds on the value of an intent's input action (1 for a button or key, 0..1 for a stick past the input
    // deadzone) and the timing of a held direction, in seconds of the frame time the caller passes.
    struct UiNavConfig
    {
        float engage = 0.5f; // an intent goes down at this value
        float release = 0.35f; // and up below this one, so a stick on the edge does not flutter
        float delay = 0.4f; // a held direction repeats after this long ...
        float interval = 0.12f; // ... and then every interval
    };

    // Turns the values of the intent actions, frame by frame, into the transitions the router navigates by: a press
    // on the frame an intent goes down, repeats of a held direction. One direction is live at a time (a stick held
    // diagonally acts on its stronger axis, the one already held keeps it), so two held buttons never move twice.
    // Pure and deterministic: the same values and frame times give the same transitions; at most one repeat per
    // frame, and a long frame skips the repeats it missed instead of bursting.
    class SPITE_UI_API UiNavRepeater
    {
    public:
        explicit UiNavRepeater(const UiNavConfig& config = {}) : m_config(config) {}

        // Appends this frame's transitions (sourceKey 0: the caller fills it).
        void step(float deltaTime, std::span<const float, UI_NAV_INTENT_COUNT> values,
                  std::vector<UiNavTransition>& out);
        // Bit per intent held after the last step.
        [[nodiscard]] u32 held() const { return m_held; }
        // Nothing held: the caller may skip stepping until an input action is active.
        [[nodiscard]] bool idle() const { return m_held == 0; }
        void reset();
        [[nodiscard]] const UiNavConfig& config() const { return m_config; }

    private:
        UiNavConfig m_config;
        u32 m_held = 0;
        i32 m_direction = -1; // the live direction's intent, -1 none
        float m_heldTime = 0.0f;
        u32 m_repeats = 0;
    };

    // The candidate nearest to `from` in the direction (Up, Down, Left or Right), as an index into `candidates`;
    // -1 when none lies that way. Candidates must lie ahead of the origin's centre; those within a 45 degree cone
    // of the direction win over the rest, and among them the one with the least gap along the direction plus twice
    // the sideways offset of the centres. Ties go to the earlier candidate, so paint order decides.
    [[nodiscard]] SPITE_UI_API i32 pickNavigationTarget(UiNavIntent direction, const UiRect& from,
                                                        std::span<const UiRect> candidates);
} // namespace spite
