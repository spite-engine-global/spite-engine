#pragma once
#include "Aspect.hpp"
#include "base/Platform.hpp"
#include "base/Schema.hpp"

namespace spite
{
    struct ArchetypeStats
    {
        Aspect aspect;
        sizet componentCount;
        sizet entityCount;
        sizet chunkCount; // Active chunks
        sizet wastedMemoryUnusedSlots; // Memory in partially filled chunks (unused slots)
        sizet memoryBytes; // Total memory for chunks (storage blocks only)
        float chunkUtilization; // Average entities per chunk / capacity (0.0-1.0)
        SPITE_OBSERVED_SCHEMA(ArchetypeStats, componentCount, entityCount, chunkCount, wastedMemoryUnusedSlots,
                              memoryBytes, chunkUtilization)
    };

    /// Aggregate ECS statistics
    struct EcsStats
    {
        // Entity stats
        sizet activeEntityCount; // Currently alive entities
        sizet maxEntityIdEverUsed; // Size of generations array
        sizet recycledSlotCount; // Available for reuse

        // Archetype stats
        sizet archetypeCount; // Unique component compositions
        sizet totalChunkCount; // All active chunks across archetypes
        sizet totalChunkMemoryBytes; // Sum of all chunk storage blocks
        float averageChunkUtilization; // Weighted average across all archetypes
        sizet totalWastedMemoryUnusedSlots;
        sizet entityMoves; // entities moved between archetypes since the world was made (one per entity per move)

        // Component stats
        sizet registeredComponentCount; // component ids issued, retired ids included (getRegisteredComponentCount)

        // Query cache stats
        sizet cachedQueryCount; // Unique cached query shapes
        sizet queriesRebuiltLastPass; // Queries re-walked by the last stage-boundary rebuild pass (0 = cache clean)
        SPITE_OBSERVED_SCHEMA(EcsStats, activeEntityCount, maxEntityIdEverUsed, recycledSlotCount, archetypeCount,
                              totalChunkCount, totalChunkMemoryBytes, averageChunkUtilization,
                              totalWastedMemoryUnusedSlots, entityMoves, registeredComponentCount, cachedQueryCount,
                              queriesRebuiltLastPass)
    };
} // namespace spite
