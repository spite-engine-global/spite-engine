#pragma once

#include <source_location>

#include "Profiling.hpp"

namespace spite::prof
{
    // Names must have static storage, just like the engine's SPITE_ZONE names.
    class SPITE_PROFILING_API GameZoneScope
    {
    public:
        explicit GameZoneScope(const char* name, std::source_location location = std::source_location::current());
        ~GameZoneScope();
        GameZoneScope(const GameZoneScope&) = delete;
        GameZoneScope& operator=(const GameZoneScope&) = delete;

    private:
        ZoneScope m_collector;
    };
} // namespace spite::prof

#define SPITE_GAME_CONCAT_INNER(a, b) a##b
#define SPITE_GAME_CONCAT(a, b) SPITE_GAME_CONCAT_INNER(a, b)
#define SPITE_GAME_ZONE(name) spite::prof::GameZoneScope SPITE_GAME_CONCAT(spiteGameZone_, __LINE__)(name)
// Counters are collector frame sums.
#define SPITE_GAME_COUNTER(name, value) spite::prof::addCounter(name, value)
