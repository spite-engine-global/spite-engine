#include "ui/UiNavigation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace spite
{
    namespace
    {
        constexpr u32 DIRECTIONS = 4; // Up..Right are the first four intents

        constexpr u32 bit(const u32 intent) { return 1u << intent; }
    } // namespace

    void UiNavRepeater::reset()
    {
        m_held = 0;
        m_direction = -1;
        m_heldTime = 0.0f;
        m_repeats = 0;
    }

    void UiNavRepeater::step(const float deltaTime, const std::span<const float, UI_NAV_INTENT_COUNT> values,
                             std::vector<UiNavTransition>& out)
    {
        u32 down = 0;
        for (u32 intent = 0; intent < UI_NAV_INTENT_COUNT; ++intent)
        {
            const float threshold = (m_held & bit(intent)) != 0 ? m_config.release : m_config.engage;
            if (values[intent] > 0.0f && values[intent] >= threshold)
            {
                down |= bit(intent);
            }
        }
        // the live direction stays while it is down; else the strongest down direction, the earlier on a tie
        i32 direction = m_direction >= 0 && (down & bit(static_cast<u32>(m_direction))) != 0 ? m_direction : -1;
        if (direction < 0)
        {
            for (u32 intent = 0; intent < DIRECTIONS; ++intent)
            {
                if ((down & bit(intent)) != 0 &&
                    (direction < 0 || values[intent] > values[static_cast<u32>(direction)]))
                {
                    direction = static_cast<i32>(intent);
                }
            }
        }
        // Shift+F6 holds both landmark intents (F6 binds with any modifiers): the one needing the extra key wins
        if ((down & bit(static_cast<u32>(UiNavIntent::PreviousLandmark))) != 0)
        {
            down &= ~bit(static_cast<u32>(UiNavIntent::NextLandmark));
        }
        for (u32 intent = DIRECTIONS; intent < UI_NAV_INTENT_COUNT; ++intent)
        {
            if ((down & bit(intent)) != 0 && (m_held & bit(intent)) == 0)
            {
                out.push_back({.intent = static_cast<UiNavIntent>(intent)});
            }
        }
        if (direction != m_direction)
        {
            m_direction = direction;
            m_heldTime = 0.0f;
            m_repeats = 0;
            if (direction >= 0)
            {
                out.push_back({.intent = static_cast<UiNavIntent>(direction)});
            }
        }
        else if (direction >= 0)
        {
            m_heldTime += std::max(deltaTime, 0.0f);
            if (m_heldTime >= m_config.delay + static_cast<float>(m_repeats) * m_config.interval)
            {
                out.push_back({.intent = static_cast<UiNavIntent>(direction), .repeat = true});
                // the repeats a long frame spanned are skipped, not replayed
                m_repeats = 1 +
                    (m_config.interval > 0.0f ? static_cast<u32>((m_heldTime - m_config.delay) / m_config.interval)
                                              : 0u);
            }
        }
        // a direction that lost to the live one is not held, so its release shows
        m_held = (down & ~((1u << DIRECTIONS) - 1u)) | (direction >= 0 ? bit(static_cast<u32>(direction)) : 0u);
    }

    i32 pickNavigationTarget(const UiNavIntent direction, const UiRect& from, const std::span<const UiRect> candidates)
    {
        // unit direction of travel in UI space (y grows downward)
        Vec2 axis{0.0f, 0.0f};
        switch (direction)
        {
        case UiNavIntent::Up:
            axis = Vec2{0.0f, -1.0f};
            break;
        case UiNavIntent::Down:
            axis = Vec2{0.0f, 1.0f};
            break;
        case UiNavIntent::Left:
            axis = Vec2{-1.0f, 0.0f};
            break;
        case UiNavIntent::Right:
            axis = Vec2{1.0f, 0.0f};
            break;
        default:
            return -1;
        }
        const bool horizontal = axis.x != 0.0f;
        const float travel = horizontal ? axis.x : axis.y;
        const Vec2 origin = from.position + from.size * 0.5f;
        const float fromLead = horizontal ? (travel > 0.0f ? from.position.x + from.size.x : from.position.x)
                                          : (travel > 0.0f ? from.position.y + from.size.y : from.position.y);
        i32 best = -1;
        float bestScore = std::numeric_limits<float>::max();
        bool bestInCone = false;
        for (sizet i = 0; i < candidates.size(); ++i)
        {
            const UiRect& rect = candidates[i];
            const Vec2 delta = rect.position + rect.size * 0.5f - origin;
            const float ahead = (horizontal ? delta.x : delta.y) * travel;
            if (ahead <= 0.0f)
            {
                continue;
            }
            const float side = std::abs(horizontal ? delta.y : delta.x);
            const float nearEdge = horizontal ? (travel > 0.0f ? rect.position.x : rect.position.x + rect.size.x)
                                              : (travel > 0.0f ? rect.position.y : rect.position.y + rect.size.y);
            const float gap = std::max((nearEdge - fromLead) * travel, 0.0f);
            const float score = gap + 2.0f * side;
            const bool inCone = side <= ahead;
            if ((inCone && !bestInCone) || (inCone == bestInCone && score < bestScore))
            {
                best = static_cast<i32>(i);
                bestScore = score;
                bestInCone = inCone;
            }
        }
        return best;
    }
} // namespace spite
