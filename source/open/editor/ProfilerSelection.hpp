#pragma once

#include "base/Platform.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    // The Profiler GUI's one selection model: which capture the views show, the frame range
    // selected in it, the zone selected and the GPU pass. It lives on the Profiler panel's root element so
    // `entity`, `find` and uiTree-adjacent tools read it; only the profilerSelect command writes it (every field is
    // READONLY to `set`), which checks the values against the store, so a view never holds a range outside its capture.
    // `generation` grows with every change, for views that redraw on it.
    struct ProfilerSelectionComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "ProfilerSelection";
        u32 capture = 0; // CaptureStore id, 0: none
        bool hasFrames = false;
        u64 firstFrame = 0; // inclusive
        u64 lastFrame = 0; // inclusive
        bool hasZone = false;
        u32 zoneLane = 0;
        u64 zoneIndex = 0; // within its lane, as profileZones reports it
        bool hasGpuPass = false;
        u64 gpuFrame = 0; // the GPU frame's index, as profileGpu reports
        u32 gpuPass = 0; // the pass's position in its frame
        u64 generation = 0;
        SPITE_SCHEMA(ProfilerSelectionComponent, SPITE_FIELD(capture, READONLY), SPITE_FIELD(hasFrames, READONLY),
                     SPITE_FIELD(firstFrame, READONLY), SPITE_FIELD(lastFrame, READONLY),
                     SPITE_FIELD(hasZone, READONLY), SPITE_FIELD(zoneLane, READONLY), SPITE_FIELD(zoneIndex, READONLY),
                     SPITE_FIELD(hasGpuPass, READONLY), SPITE_FIELD(gpuFrame, READONLY), SPITE_FIELD(gpuPass, READONLY),
                     SPITE_FIELD(generation, READONLY))
    };
} // namespace spite
