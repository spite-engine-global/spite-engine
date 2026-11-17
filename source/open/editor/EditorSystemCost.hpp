#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include "ecs/SystemDescriptor.hpp"

// What the editor's cost readouts share (the inspector's strip and the hierarchy's subtree costs):
// which phases touch an entity's components, and how a shown cost follows the scheduler's average without flickering.
namespace spite::systemcost
{
    // the readouts read the scheduler again this many frames after the last read (a few times a second), never every
    // frame
    constexpr u32 REFRESH_FRAMES = 30;
    // a shown cost moves only when the average moved this far, so a steady system rewrites nothing
    constexpr float STEP_MS = 0.05f;
    // a cost leaves the warning only when it falls this far below the budget, so one hovering around the budget does
    // not flip at every read
    constexpr float WARN_RELEASE = 0.9f;

    // The components a phase touches: what it writes, else (a phase that writes nothing) what it reads.
    [[nodiscard]] inline const std::vector<ComponentID>& touchedOf(const SystemPhaseDescriptor& phase)
    {
        return phase.writes.empty() ? phase.reads : phase.writes;
    }

    // Whether a phase that touches `touched` touches an entity of this aspect: the entity has every one of them.
    // Queries are built by the systems at run time, so the declared access is all the schedule knows of what a system
    // touches; a system that writes components of several shapes in one phase matches none of them.
    [[nodiscard]] inline bool covers(const std::span<const ComponentID> touched,
                                     const std::span<const ComponentID> aspect)
    {
        return !touched.empty() &&
            std::ranges::all_of(touched, [aspect](const ComponentID id)
                                { return std::ranges::find(aspect, id) != aspect.end(); });
    }

    [[nodiscard]] inline bool touches(const SystemPhaseDescriptor& phase, const std::span<const ComponentID> aspect)
    {
        return covers(touchedOf(phase), aspect);
    }

    // Takes the scheduler's reading avgMs of a phase into the shown cost heldMs (-1: none yet): it moves by STEP_MS, a
    // warning flip, or the average becoming or stopping being exactly 0 (a disabled system or a skipped phase). warn is
    // the average over budgetMs (0: never), released at WARN_RELEASE of it. True when what shows changed.
    inline bool hold(float& heldMs, bool& warn, const float avgMs, const float budgetMs)
    {
        const bool over =
            budgetMs > 0.0f && (warn && heldMs >= 0.0f ? avgMs >= budgetMs * WARN_RELEASE : avgMs > budgetMs);
        if (heldMs >= 0.0f && warn == over && (heldMs == 0.0f) == (avgMs == 0.0f) && std::abs(avgMs - heldMs) < STEP_MS)
        {
            return false;
        }
        const bool moved = heldMs != avgMs || warn != over;
        heldMs = avgMs;
        warn = over;
        return moved;
    }
} // namespace spite::systemcost
