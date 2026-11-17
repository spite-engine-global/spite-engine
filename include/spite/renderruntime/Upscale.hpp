#pragma once

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

#include "base/InternedString.hpp"
#include "base/Schema.hpp"
#include "rendering/GraphicsDescs.hpp"
#include "renderpipeline/UpscalerBackend.hpp"
#include "renderruntime/TemporalAa.hpp"

// A render view's upscaler: what it asked for, what the upload step resolved it to through
// the UpscalerRegistry, and the counters the `upscale` table shows. An upscaler needs the view's jitter and motion
// vectors, so a view that upscales is a jittering view: TemporalSingleton steps it and the pass module
// (SpiteRenderPipeline) replaces the TAA resolve with the upscale.

namespace spite
{
    // the backend names the engine knows: "native" is the temporal upscaler of SpiteRenderPipeline, "off" asks for none
    // and "auto" for the first available provider, else native
    inline constexpr std::string_view UPSCALER_NATIVE = "native";
    inline constexpr std::string_view UPSCALER_OFF = "off";
    inline constexpr std::string_view UPSCALER_AUTO = "auto";

    // The texture LOD bias in octaves that keeps the detail of a view rendered below its display size,
    // log2(render / display) - 1 (what DLSS and FSR ask of the engine); 0 for no extent
    inline float upscaleMipBias(const Extent2D render, const Extent2D display)
    {
        if (render.width == 0 || display.width == 0)
        {
            return 0.f;
        }
        return std::log2(static_cast<float>(render.width) / static_cast<float>(display.width)) - 1.f;
    }

    // a jitter state as the protocol spells it: its SPITE_ENUM_NAMES name with a lower-case first letter
    inline std::string temporalStateName(const TemporalState state)
    {
        for (const EnumName& name : spiteEnumNames(state))
        {
            if (name.value == static_cast<std::int64_t>(state))
            {
                std::string text(name.name);
                text[0] = static_cast<char>(text[0] | 0x20);
                return text;
            }
        }
        return {};
    }

    // bytes of the display-resolution RGBA16F textures the graph holds for a view's upscaler: the output (every
    // backend), the history pair of a graph history (the native backend) and the sharpened picture. What a provider
    // keeps inside its own context is not counted.
    inline u64 upscaleHistoryBytes(const Extent2D display, const bool graphHistory, const bool sharpen)
    {
        return u64{display.width} * display.height * 8 * ((graphHistory ? 3u : 1u) + (sharpen ? 1u : 0u));
    }

    struct UpscaleView
    {
        // the view's request: set from graphics.upscaler for the game view, by setUpscale for any view
        InternedString backend = "off";
        float sharpness = 0.f;

        // what the last upload step resolved: the graph declares the upscale exactly then (a change asks for one
        // rebuild), `reason` says why a request is not honoured and `lastError` what failed a backend asked for
        std::string active = std::string(UPSCALER_OFF);
        bool graphHistory = false; // the active backend accumulates in the graph's history pair
        bool sharpen = false;
        std::string reason;
        std::string lastError;

        // the request and registry state the registry resolved it at, and what it came to: resolved again only when
        // one of them moves, so a steady view resolves nothing
        InternedString resolvedFor = "off";
        u64 resolvedGeneration = 0;
        std::string resolvedName = std::string(UPSCALER_OFF);
        std::string resolvedReason;
        // the context the active backend was created for (a new extent or backend makes a new one)
        UpscalerContext context{};
        bool hasContext = false;
        std::string contextBackend;
        u64 contextSerial = 0; // which registration of that backend (UpscalerRegistry::serial)
        u64 contextId = 0; // the backend's name for it (IUpscalerBackend::create)

        u64 dispatches = 0; // upscales recorded, flat while the view is frozen
        u64 resets = 0; // of them, the ones that restarted the history
        u64 exposureWrites = 0; // writes of the exposure texture, flat while the exposure is
    };

    // One row of the `upscale` table: a view's render scale and extents, its request, the backend that runs, the state
    // of its jitter cycle and what the upscale cost. Rows of a view that does not upscale have state off and no bytes.
    struct UpscaleRow
    {
        u32 view = 0;
        bool scalable = true; // false: a tool's view, which stays at scale 1 with no upscaler
        float scale = 1.f;
        u32 renderWidth = 0;
        u32 renderHeight = 0;
        u32 displayWidth = 0;
        u32 displayHeight = 0;
        std::string backend; // the request
        std::string active; // what runs: off while none does
        std::string reason; // why the request is not what runs, empty when it is
        std::string lastError; // what failed the backend asked for this session, empty when nothing did
        std::string state = "off"; // off, or the view's jitter state as TemporalState names it (lower case)
        float sharpness = 0.f;
        float mipBias = 0.f;
        u32 jitterIndex = 0;
        u32 phaseCount = 0;
        u64 dispatches = 0;
        u64 resets = 0;
        u64 exposureWrites = 0;
        u64 historyBytes = 0;
        double gpuMs = 0.0; // the view's Upscale_* passes in the last timed frame, 0 while none was timed
        SPITE_OBSERVED_SCHEMA(UpscaleRow, view, scalable, scale, renderWidth, renderHeight, displayWidth, displayHeight,
                              backend, active, reason, lastError, state, sharpness, mipBias, jitterIndex, phaseCount,
                              dispatches, resets, exposureWrites, historyBytes, gpuMs)
    };

    // Per render view; the upload step resolves it, the Upscale pass module and the table read it.
    struct UpscaleSingleton
    {
        std::vector<UpscaleView> views;
        u64 frames = 0; // upload steps of the primary scene: the clock contexts retire by
    };
} // namespace spite
