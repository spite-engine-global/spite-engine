#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "base/MathFunctions.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include "rendering/GpuSharedTypes.hpp"
#include "rendering/GraphicsDescs.hpp"

// Camera jitter and its per-view controller: the first step of temporal anti-aliasing. A view that
// jitters adds a sub-pixel Halton offset to its projection while its picture changes and for a full cycle after, then
// freezes (no jitter, no uploads). The pure functions and the controller here are what the render runtime's upload
// runs each frame; shaders/source/shared/bindless.slang TemporalData is the record it writes for the GPU.

namespace spite
{
    // A view's override of graphics.aa: Auto follows it (and a tool's view, the editor's Scene view, stays off), On
    // jitters every frame whatever the config says and never freezes, Off never jitters.
    enum class JitterMode : u32
    {
        Auto,
        On,
        Off
    };
    SPITE_ENUM_NAMES(JitterMode, Auto, On, Off)

    // Where a view's jitter stands, for the temporal table.
    enum class TemporalState : u32
    {
        Off, // the view does not jitter: nothing here is written
        Active, // the picture changed: this frame jitters
        Converging, // still, jittering through the rest of the cycle
        Frozen, // still and converged: no jitter, no writes
        Forced // paused, stepped, pinned by the oracle or a fixed capture: no jitter this frame
    };
    SPITE_ENUM_NAMES(TemporalState, Off, Active, Converging, Frozen, Forced)

    // why the view's cut generation moved, a bitmask
    enum TemporalCut : u32
    {
        TEMPORAL_CUT_FIRST = 1, // the view's first frame, or jitter switched on
        TEMPORAL_CUT_CAMERA = 2, // another camera entity
        TEMPORAL_CUT_SCENE = 4, // another scene (Play enter or exit)
        TEMPORAL_CUT_RESIZE = 8, // the render or display extent changed
        TEMPORAL_CUT_GRAPH = 16, // the render graph was asked to rebuild
        TEMPORAL_CUT_TELEPORT = 32, // the camera moved further than TEMPORAL_TELEPORT_FRACTION of its far plane
        TEMPORAL_CUT_RESET = 64, // an explicit reset
        // the first frame after forced ones: what the view showed meanwhile (pause, step, the oracle pin, a fixed
        // capture) was not accumulated, so the history restarts
        TEMPORAL_CUT_RESUME = 128
    };

    // a camera that moved further than this share of its far plane in one frame did not travel: its history is stale
    inline constexpr float TEMPORAL_TELEPORT_FRACTION = 0.1f;

    // the n-th element (n >= 1) of the Halton sequence of a base, in [0, 1)
    inline float halton(u32 index, const u32 base)
    {
        float result = 0.f;
        float fraction = 1.f;
        while (index > 0)
        {
            fraction /= static_cast<float>(base);
            result += fraction * static_cast<float>(index % base);
            index /= base;
        }
        return result;
    }

    // ceil(8 (display / render)^2) samples, 8 at scale 1: the length of the jitter cycle (what DLSS and FSR ask for)
    inline u32 temporalPhaseCount(const Extent2D render, const Extent2D display)
    {
        if (render.width == 0 || display.width == 0)
        {
            return 8;
        }
        const double ratio = static_cast<double>(display.width) / static_cast<double>(render.width);
        return std::max(8u, static_cast<u32>(std::ceil(8.0 * ratio * ratio - 1e-6)));
    }

    // The jitter of the frameIndex-th sample of a cycle, in render pixels (x right, y down), each in (-0.5, 0.5);
    // Halton(2, 3) from the first element on, so it is never both zero.
    inline Vec2 jitterPixels(const u32 frameIndex, const u32 phaseCount)
    {
        const u32 element = frameIndex % std::max(phaseCount, 1u) + 1;
        return {halton(element, 2) - 0.5f, halton(element, 3) - 0.5f};
    }

    // The same offset in clip space (NDC: x right, y up, so a pixel offset down is a negative clip y), which the
    // projection is shifted by.
    inline Vec2 jitterClip(const Vec2 pixels, const Extent2D render)
    {
        return {2.f * pixels.x / static_cast<float>(std::max(render.width, 1u)),
                -2.f * pixels.y / static_cast<float>(std::max(render.height, 1u))};
    }

    // projection with the picture shifted by clip.x and clip.y in NDC: clip' = T * clip with T translating NDC, i.e. a
    // multiple of the w row added to the x and y rows. A perspective and an orthographic projection alike.
    inline Mat4 jitteredProjection(const Mat4& projection, const Vec2 clip)
    {
        Mat4 result = projection;
        for (u32 column = 0; column < 4; ++column)
        {
            result[column][0] += clip.x * projection[column][3];
            result[column][1] += clip.y * projection[column][3];
        }
        return result;
    }

    // One row of the temporal table (the render runtime's `temporal` table): a view's jitter as the controller left it.
    struct TemporalRow
    {
        u32 view = 0;
        bool enabled = false;
        TemporalState state = TemporalState::Off;
        u32 frameIndex = 0; // the sample of the cycle the last jittered frame used
        u32 phaseCount = 0;
        float jitterX = 0.f; // this frame's offset, render pixels
        float jitterY = 0.f;
        u32 convergeLeft = 0; // still frames left before the view freezes
        u32 cutGeneration = 0;
        u32 cutReasons = 0; // TemporalCut bits of the last cut
        u64 jitteredFrames = 0;
        u64 uploads = 0; // frame-slot writes of the temporal record: flat once the view froze
        u64 taaFrames = 0; // resolves the TAA history holds since it last restarted: counts while converging
        SPITE_OBSERVED_SCHEMA(TemporalRow, view, enabled, state, frameIndex, phaseCount, jitterX, jitterY, convergeLeft,
                              cutGeneration, cutReasons, jitteredFrames, uploads, taaFrames)
    };

    // What the controller reads of a view each frame.
    struct TemporalInputs
    {
        bool enabled = false; // the view jitters at all (graphics.aa or its override)
        bool continuous = false; // JitterMode::On: no converge-and-freeze
        bool forcedOff = false; // paused, stepped, pinned or a fixed capture
        Entity camera = Entity::undefined();
        u32 scene = 0;
        Extent2D render{};
        Extent2D display{};
        u32 phaseCount = 0; // the upscaler's jitter cycle; 0: ceil(8 (display / render)^2)
        u64 graphGeneration = 0; // RenderGraphGenerationSingleton
        Mat4 viewProjection{1.f}; // unjittered
        Mat4 inverseProjection{1.f}; // unjittered
        Vec3 position{};
        float farPlane = 0.f;
        bool sceneChanged = false; // object update records reached the GPU
        u64 lightUploads = 0;
        u64 pictureContentVersion = 0;
        u64 contentUploads = 0; // the transfer manager's staged bytes plus in-frame writes (textures, materials)
        float exposure = 0.f;
    };

    // A view's controller memory and the record it last wrote.
    struct TemporalView
    {
        JitterMode mode = JitterMode::Auto; // the view's override
        bool resetRequested = false; // temporalReset: the next frame cuts
        // the texture LOD bias in octaves the resolve samples with while the view jitters (TemporalData::mipBias); the
        // upscaler writes log2(render / display) - 1 here, 0 while none runs
        float mipBias = 0.f;

        bool seen = false; // the controller has run for the view since jitter was last off
        Entity camera = Entity::undefined();
        u32 scene = 0;
        Extent2D render{};
        Extent2D display{};
        u64 graphGeneration = 0;
        Mat4 viewProjection{1.f};
        Vec3 position{};
        u64 lightUploads = 0;
        u64 pictureContentVersion = 0;
        u64 contentUploads = 0;
        float exposure = 0.f;

        TemporalState state = TemporalState::Off;
        u32 convergeLeft = 0;
        u32 frameIndex = 0;
        u32 phaseCount = 8;
        Vec2 jitter{};
        u32 cutGeneration = 0;
        u32 cutReasons = 0;
        u64 jitteredFrames = 0;

        // this frame's record; published is the last one that moved generation (a slot's buffer rewrites when it holds
        // an older generation), recorded is false until the first
        TemporalData record{};
        TemporalData published{};
        bool recorded = false;
        u64 generation = 0;
        std::vector<u64> slotGenerations;
        u64 uploads = 0;

        // the TAA resolve's history pair: the texture the last resolve wrote, whether it holds the
        // accumulation of the current cut, how many samples it averages and how many resolves it holds
        u32 historyIndex = 0;
        bool historyValid = false;
        u32 historyCut = 0;
        float historySamples = 0.f;
        u64 taaFrames = 0;
    };

    // the least weight of each new frame while the view changes: the history stays at most nine frames deep
    inline constexpr float TAA_MIN_BLEND = 0.1f;

    // What the TAA resolve does for a view this frame (planTaaResolve).
    struct TaaPlan
    {
        bool run = false; // the resolve records
        bool reset = false; // the history is dropped: the output is this frame's colour
        float blend = 1.f; // this frame's weight against the history
        u32 read = 0; // the texture of the pair the history is read from
        u32 write = 1; // and the one the output goes to
        float samples = 1.f; // the samples the output averages
    };

    // Plans the frame's resolve once the view stepped. A switched-off or forced frame drops the history (Composite
    // shows the scene colour, unjittered, as with TAA off), a frozen view records nothing (Composite keeps the
    // converged history), and a cut or a pair the graph recreated resets it. A changing view gives each frame at least
    // TAA_MIN_BLEND; the first still frame counts the history as one sample and averages the cycle's samples evenly on
    // top, so a converged view holds its jitter cycle's mean.
    inline TaaPlan planTaaResolve(TemporalView& view, const bool recreated)
    {
        TaaPlan plan;
        if (recreated || view.state == TemporalState::Off || view.state == TemporalState::Forced)
        {
            view.historyValid = false;
        }
        if (view.state != TemporalState::Active && view.state != TemporalState::Converging)
        {
            return plan;
        }
        plan.run = true;
        plan.read = view.historyIndex;
        plan.write = 1 - view.historyIndex;
        plan.reset = !view.historyValid || view.historyCut != view.cutGeneration;
        float samples = 0.f;
        if (!plan.reset && view.state == TemporalState::Active)
        {
            samples = std::min(view.historySamples, 1.f / TAA_MIN_BLEND - 1.f);
        }
        else if (!plan.reset)
        {
            samples = view.convergeLeft + 1 == view.phaseCount
                ? 1.f
                : std::min(view.historySamples, static_cast<float>(view.phaseCount));
        }
        plan.blend = 1.f / (samples + 1.f);
        plan.samples = samples + 1.f;
        return plan;
    }

    // The resolve recorded the plan: the texture it wrote holds the view's history now.
    inline void commitTaaResolve(TemporalView& view, const TaaPlan& plan)
    {
        view.historyIndex = plan.write;
        view.historyValid = true;
        view.historyCut = view.cutGeneration;
        view.historySamples = plan.samples;
        view.taaFrames = plan.reset ? 1 : view.taaFrames + 1;
    }

    // Advances the view by one frame: finds a cut, decides whether this frame jitters and with which sample. The
    // frame number is the view's own count of jittered frames: no clock, so a replayed frame sequence reproduces it.
    // Returns the clip-space offset to add to the projection (zero when the frame does not jitter).
    inline Vec2 stepTemporal(TemporalView& view, const TemporalInputs& inputs)
    {
        if (!inputs.enabled)
        {
            view.state = TemporalState::Off;
            view.seen = false;
            view.recorded = false;
            view.jitter = {};
            view.convergeLeft = 0;
            return {};
        }

        u32 cut = view.resetRequested ? TEMPORAL_CUT_RESET : 0u;
        view.resetRequested = false;
        const bool first = !view.seen;
        if (first)
        {
            cut |= TEMPORAL_CUT_FIRST;
        }
        else
        {
            cut |= (inputs.camera != view.camera ? TEMPORAL_CUT_CAMERA : 0u) |
                (inputs.scene != view.scene ? TEMPORAL_CUT_SCENE : 0u) |
                (inputs.render.width != view.render.width || inputs.render.height != view.render.height ||
                         inputs.display.width != view.display.width || inputs.display.height != view.display.height
                     ? TEMPORAL_CUT_RESIZE
                     : 0u) |
                (inputs.graphGeneration != view.graphGeneration ? TEMPORAL_CUT_GRAPH : 0u) |
                (length(inputs.position - view.position) > TEMPORAL_TELEPORT_FRACTION * inputs.farPlane
                     ? TEMPORAL_CUT_TELEPORT
                     : 0u) |
                (view.state == TemporalState::Forced && !inputs.forcedOff ? TEMPORAL_CUT_RESUME : 0u);
        }
        const bool cameraMoved = std::memcmp(&inputs.viewProjection, &view.viewProjection, sizeof(Mat4)) != 0;
        const bool changed = cut != 0 || cameraMoved || inputs.sceneChanged ||
            inputs.lightUploads != view.lightUploads || inputs.contentUploads != view.contentUploads ||
            inputs.pictureContentVersion != view.pictureContentVersion || inputs.exposure != view.exposure;
        if (cut != 0)
        {
            ++view.cutGeneration;
            view.cutReasons = cut;
            view.frameIndex = 0;
        }

        view.phaseCount = inputs.phaseCount > 0 ? inputs.phaseCount : temporalPhaseCount(inputs.render, inputs.display);
        bool jitter = false;
        if (inputs.forcedOff)
        {
            view.state = TemporalState::Forced;
            view.convergeLeft = 0;
        }
        else if (changed)
        {
            view.state = TemporalState::Active;
            view.convergeLeft = view.phaseCount;
            jitter = true;
        }
        else if (view.convergeLeft > 0 || inputs.continuous)
        {
            view.convergeLeft = view.convergeLeft > 0 ? view.convergeLeft - 1 : 0;
            view.state = TemporalState::Converging;
            jitter = true;
        }
        else
        {
            view.state = TemporalState::Frozen;
        }

        view.jitter = {};
        if (jitter)
        {
            view.jitter = jitterPixels(view.frameIndex, view.phaseCount);
            ++view.jitteredFrames;
        }

        TemporalData record{};
        record.viewProjectionUnjittered = inputs.viewProjection;
        record.previousViewProjectionUnjittered = first ? inputs.viewProjection : view.viewProjection;
        record.inverseProjectionUnjittered = inputs.inverseProjection;
        record.jitterPixels = view.jitter;
        record.jitterClip = jitterClip(view.jitter, inputs.render);
        record.renderSize = Vec2(static_cast<float>(inputs.render.width), static_cast<float>(inputs.render.height));
        record.displaySize = Vec2(static_cast<float>(inputs.display.width), static_cast<float>(inputs.display.height));
        record.frameIndex = view.frameIndex;
        record.phaseCount = view.phaseCount;
        record.cutGeneration = view.cutGeneration;
        record.mipBias = view.mipBias;
        view.record = record;
        if (jitter)
        {
            ++view.frameIndex;
        }

        view.seen = true;
        view.camera = inputs.camera;
        view.scene = inputs.scene;
        view.render = inputs.render;
        view.display = inputs.display;
        view.graphGeneration = inputs.graphGeneration;
        view.viewProjection = inputs.viewProjection;
        view.position = inputs.position;
        view.lightUploads = inputs.lightUploads;
        view.contentUploads = inputs.contentUploads;
        view.pictureContentVersion = inputs.pictureContentVersion;
        view.exposure = inputs.exposure;
        return record.jitterClip;
    }

    // Per render view. The render runtime's upload steps each view; the table and the capture commands read them.
    struct TemporalSingleton
    {
        std::vector<TemporalView> views;
        // the oracle pins it: no view jitters, so a capture does not depend on the frames before it
        bool pinned = false;
        // set by the screenshot tool for the frame that records a `jitter=fixed` capture: that frame renders unjittered
        bool captureFixed = false;

        [[nodiscard]] TemporalRow row(const u32 viewIndex) const
        {
            const TemporalView& view = views[viewIndex];
            return {.view = viewIndex,
                    .enabled = view.state != TemporalState::Off,
                    .state = view.state,
                    .frameIndex = view.frameIndex,
                    .phaseCount = view.phaseCount,
                    .jitterX = view.jitter.x,
                    .jitterY = view.jitter.y,
                    .convergeLeft = view.convergeLeft,
                    .cutGeneration = view.cutGeneration,
                    .cutReasons = view.cutReasons,
                    .jitteredFrames = view.jitteredFrames,
                    .uploads = view.uploads,
                    .taaFrames = view.taaFrames};
        }
    };
} // namespace spite
