#pragma once
#include <string>

#include "base/Platform.hpp"
#include "base/Schema.hpp"
#include "ecs/World.hpp"

namespace spite
{
    class ServiceLocator;

    // One archetype of a world; chunk rows name it by the same `components` key.
    struct ArchetypeRow
    {
        std::string components; // component names in id order, comma separated
        u64 componentCount = 0;
        u64 entityCount = 0;
        u64 chunkCount = 0;
        u64 memoryBytes = 0;
        u64 wastedBytes = 0;
        f32 utilization = 0.0f;
        SPITE_OBSERVED_SCHEMA(ArchetypeRow, components, componentCount, entityCount, chunkCount, memoryBytes,
                              wastedBytes, utilization)
    };

    struct ChunkRow
    {
        std::string archetype; // ArchetypeRow::components
        u64 index = 0;
        u64 entityCount = 0;
        u64 capacity = 0;
        u64 memoryBytes = 0;
        u64 wastedBytes = 0;
        SPITE_OBSERVED_SCHEMA(ChunkRow, archetype, index, entityCount, capacity, memoryBytes, wastedBytes)
    };

    // One registered system; a stage of 0 means the system has no such phase. Per phase: the stage's
    // CoreExecutionStages identifier (the number for a custom stage), the systems it runs after
    // (`Module::Name`, `Name` for the local module) and the component names it reads and writes, comma separated.
    // The Ms columns are the scheduler's measured wall time per frame over the last SYSTEM_TIMING_WINDOW frames
    // (SystemPhaseTiming), zero for a disabled system and a skipped phase; a phase the system lacks reads zero.
    struct SystemRow
    {
        std::string module; // "" = the local/unnamed module
        std::string name;
        bool enabled = true;
        bool required = false; // SystemDescriptor::required: setSystemEnabled cannot turn it off
        u64 updateStage = 0;
        u64 fixedStage = 0;
        std::string phases; // "update", "fixed" or "update,fixed"; a registered system has at least one phase
        std::string updateStageName;
        std::string updateRunsAfter;
        std::string updateReads;
        std::string updateWrites;
        std::string fixedStageName;
        std::string fixedRunsAfter;
        std::string fixedReads;
        std::string fixedWrites;
        f32 updateAvgMs = 0.0f;
        f32 updateMaxMs = 0.0f;
        f32 fixedAvgMs = 0.0f;
        f32 fixedMaxMs = 0.0f;
        SPITE_OBSERVED_SCHEMA(SystemRow, module, name, enabled, required, updateStage, fixedStage, phases,
                              updateStageName, updateRunsAfter, updateReads, updateWrites, fixedStageName,
                              fixedRunsAfter, fixedReads, fixedWrites, updateAvgMs, updateMaxMs, fixedAvgMs, fixedMaxMs)
    };

    // A world's archetype, chunk and system tables, a service of that world's locator
    // so they land in its tables and snapshots. Rows are produced only inside
    // a query, on the frame thread.
    struct EcsTables
    {
        World world;

        static void attach(const World& world, ServiceLocator& services);
        static void detach(ServiceLocator& services);
    };
} // namespace spite
