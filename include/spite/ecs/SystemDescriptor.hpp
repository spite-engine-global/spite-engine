#pragma once

// System registration vocabulary. Stage values are plain u64s; the canonical
// stage constants live here in CoreExecutionStages.

#include <optional>
#include <vector>

#include "ComponentMetadata.hpp"
#include "EcsExport.hpp"

namespace spite
{
    class SystemContext;

    using ExecutionStage = uint64_t;

    namespace CoreExecutionStages
    {
        inline constexpr ExecutionStage EARLIEST_UPDATE = 5000;
        inline constexpr ExecutionStage PRE_UPDATE = 10000;
        inline constexpr ExecutionStage UPDATE = 20000;
        inline constexpr ExecutionStage POST_UPDATE = 30000;
        inline constexpr ExecutionStage BEGIN_FRAME = 35000;
        inline constexpr ExecutionStage PRE_RENDER = 40000;
        inline constexpr ExecutionStage RESOURCE_TRANSFER = 45000;
        inline constexpr ExecutionStage RENDER = 50000;
        inline constexpr ExecutionStage POST_RENDER = 60000;
        inline constexpr ExecutionStage CLEANUP = 90000;

        // the constant's identifier, or null for a custom stage value
        [[nodiscard]] constexpr const char* constantName(const ExecutionStage stage)
        {
            switch (stage)
            {
            case EARLIEST_UPDATE:
                return "EARLIEST_UPDATE";
            case PRE_UPDATE:
                return "PRE_UPDATE";
            case UPDATE:
                return "UPDATE";
            case POST_UPDATE:
                return "POST_UPDATE";
            case BEGIN_FRAME:
                return "BEGIN_FRAME";
            case PRE_RENDER:
                return "PRE_RENDER";
            case RESOURCE_TRANSFER:
                return "RESOURCE_TRANSFER";
            case RENDER:
                return "RENDER";
            case POST_RENDER:
                return "POST_RENDER";
            case CLEANUP:
                return "CLEANUP";
            default:
                return nullptr;
            }
        }
    } // namespace CoreExecutionStages

    // Fixed steps one stepped frame of a paused world runs (step frames=N), whatever time the frame took: a stepped
    // frame is one nominal fixed step long.
    inline constexpr u32 STEPPED_FRAME_FIXED_STEPS = 1;

    // Pause-and-inspect state of a world: while paused, update stages before
    // BEGIN_FRAME and the fixed phase are skipped; render stages keep running.
    struct SimulationState
    {
        bool paused = false;
        u32 pendingSteps = 0; // simulated frames still to run before pausing again
        u32 pendingFixedSteps = 0; // fixed steps the next frame runs (step fixed=N)
        u64 simulatedFrames = 0; // update frames that ran their simulation stages
        u64 fixedSteps = 0; // fixed steps that ran the fixed phase
    };

    // system entry point: runs on the world's task scheduler; only touch the
    // reads/writes the phase declares
    using SystemFn = void (*)(SystemContext&);

    struct SystemName
    {
        const char* moduleName = nullptr; // null = the local/unnamed module
        const char* systemName = nullptr;
    };

    struct SystemPhaseDescriptor
    {
        std::vector<ComponentID> reads;
        std::vector<ComponentID> writes;
        ExecutionStage stage = CoreExecutionStages::UPDATE;
        std::vector<SystemName> runsAfter;
    };

    // What the scheduler measured of one phase of a system, over its last SYSTEM_TIMING_WINDOW frames: milliseconds
    // per frame, the phase's runs of a frame summed (a fixed phase can run several times or not at all in one;
    // wall time, so a system that fans work out counts until its own task returns, and a task another system's
    // worker runs while it waits counts in both: rows can sum above the frame's real time). Zero for a disabled system
    // and for a phase the world skipped (paused simulation), not the last value it had.
    struct SystemPhaseTiming
    {
        float avgMs = 0.0f;
        float maxMs = 0.0f;
    };

    inline constexpr unsigned SYSTEM_TIMING_WINDOW = 64;

    // nanoseconds from any fixed origin; callable from every scheduler thread
    using SystemClockFn = uint64_t (*)();

    // The single vocabulary for system registration AND readback:
    // - registration through World::registerSystem; the strings are copied into
    //   the string interner, so callers may free them afterwards.
    // - readback through World::getSystems(); the strings are interner-owned and
    //   stay valid for the process lifetime. fn/fixedUpdateFn then report the
    //   effective per-phase functions (the resolved fixed-phase override).
    struct SystemDescriptor
    {
        const char* moduleName = nullptr; // null = the local/unnamed module
        const char* name = nullptr;
        bool enabled = true; // registration: false registers the system disabled
        // the frame cannot run without it (the renderer's fence chain): always enabled, World::setSystemEnabled
        // refuses to disable it
        bool required = false;

        SystemFn fn = nullptr;
        SystemFn fixedUpdateFn = nullptr; // overrides fn for the fixed phase
        std::optional<SystemPhaseDescriptor> update;
        std::optional<SystemPhaseDescriptor> fixedUpdate;
        // readback only (registration ignores them)
        SystemPhaseTiming updateTiming;
        SystemPhaseTiming fixedTiming;
    };
} // namespace spite

// The engine writes the ECS vocabulary as `ecs::X`; publish the expected aliases.
namespace spite::ecs
{
    using spite::SystemDescriptor;
    using spite::SystemName;
    using spite::SystemPhaseDescriptor;
} // namespace spite::ecs
