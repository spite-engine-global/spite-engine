#pragma once

#include "base/Platform.hpp"
#include "base/Schema.hpp"

namespace spite
{
    // What the engine's last finished frame changed (Quiescent frame): published at the end of every
    // Engine::update as the service table FrameQuiescence.stats and as the Frame rows of the stats panel. A frame is
    // quiescent when every counter below but the totals is zero; the counters name what broke it.
    struct FrameQuiescence
    {
        bool quiescent = true;
        u64 stagedBytes = 0; // bytes the frame copied through the transfer staging ring
        // scene object update records the frame wrote, plus light buffer rewrites; a held frame writes none and
        // leaves the last count standing
        u32 sceneUpdateRecords = 0;
        u32 columnsWritten =
            0; // component columns (one component of one chunk) written, plus entities destroyed, in every world
        u32 uiRelayouts = 0; // layouts of every UI (a destroyed UI counts one, a created one its own layouts)
        u32 uiRepaints = 0; // repaints, likewise
        u64 frames = 0; // frames measured since the engine started
        u64 busyFrames = 0; // of them, the ones that were not quiescent
        SPITE_OBSERVED_SCHEMA(FrameQuiescence, quiescent, stagedBytes, sceneUpdateRecords, columnsWritten, uiRelayouts,
                              uiRepaints, frames, busyFrames)
    };
} // namespace spite
