#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "base/ApplicationConfig.hpp"
#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "rendering/GraphicsDescs.hpp"
#include "rendering/RenderPassModule.hpp"

// What an upscaler backend is: the native temporal upscaler of SpiteRenderPipeline, and the provider
// DLLs (DLSS, FSR) that follow. Engine types and graph names only: no Vulkan or vendor type, so a provider is written
// against include/spite and never sees the device but through the accessor of. The UpscalerRegistry
// (renderruntime) owns the backends; the Upscale pass module (SpiteRenderPipeline) finds the view's through the
// ServiceLocator and calls dispatch from a compute pass whose graph usages declare every texture it names.

namespace spite
{
    // What the backend asks the engine to prepare around its dispatch
    struct UpscalerRequirements
    {
        // a 1x1 R32_SFLOAT texture (UpscaleDispatch::exposure) holding the view's exposure, written only when it
        // changes
        bool exposureTexture = false;
        // The three below are inputs the engine does not produce yet (transparency is not built): the registry refuses
        // a backend that requires one instead of running it without.
        bool reactiveMask = false;
        bool transparencyMask = false;
        // its vectors carry the jitter: ours never do
        bool jitteredVectors = false;
        // it accumulates in the graph's display-resolution history pair (UpscaleDispatch::historyRead and
        // historyWrite, which the pass alternates each dispatch): the native backend. Without it the backend keeps its
        // own history and its pass is one.
        bool graphHistory = false;
    };

    // The extents and camera a view's context is created for; a change of any is a new context
    struct UpscalerContext
    {
        u32 view = 0;
        Extent2D render{};
        Extent2D display{};
        bool hdr = true; // linear scene colour before the tone map: always, today
        bool depthInverted = true; // reverse Z: always, today
        float nearPlane = 0.f;
        float farPlane = 0.f;
        float verticalFov = 0.f;
        friend bool operator==(const UpscalerContext& a, const UpscalerContext& b)
        {
            return a.view == b.view && a.render.width == b.render.width && a.render.height == b.render.height &&
                a.display.width == b.display.width && a.display.height == b.display.height && a.hdr == b.hdr &&
                a.depthInverted == b.depthInverted && a.nearPlane == b.nearPlane && a.farPlane == b.farPlane &&
                a.verticalFov == b.verticalFov;
        }
    };

    // One dispatch: graph texture names inside the executing pass's view (RenderPassExecutionContext resolves them to
    // bindless indices or, through the accessor, images) and the frame's values
    struct UpscaleDispatch
    {
        u64 context = 0; // the view's, as create answered
        InternedString colour;
        InternedString depth;
        InternedString motionVectors;
        InternedString exposure; // empty without requirements().exposureTexture
        InternedString output; // UpscaledColor, display extent
        InternedString historyRead; // the pair, empty without requirements().graphHistory
        InternedString historyWrite;
        Extent2D render{};
        Extent2D display{};
        Vec2 jitter{}; // render pixels, the picture is shifted by it; never both zero in a jittered frame
        bool reset = false; // the history is dropped
        float blend = 1.f; // this frame's weight against the history (TaaPlan::blend), for a graph history
        float deltaMs = 0.f; // reserved: 0 until a backend that reads the frame time (FSR) is added,
    };

    class IUpscalerBackend
    {
    public:
        virtual ~IUpscalerBackend() = default;

        // the name a view requests it by ("native", "fsr", "dlss")
        [[nodiscard]] virtual std::string_view name() const = 0;
        // Empty: usable on this device; else why not (shown in the upscale table).
        [[nodiscard]] virtual std::string supported() const = 0;
        // the render extent for a display extent and quality (DLSS answers from its optimal-settings query)
        [[nodiscard]] virtual Extent2D renderSize(const Extent2D display, const UpscaleQuality quality) const
        {
            const float preset = upscaleQualityScale(quality);
            return renderExtentFor(display, preset > 0.0f ? preset : 1.0f);
        }
        // the jitter cycle's length and the texture LOD bias in octaves for an extent pair; empty: the engine's
        // (ceil(8 (display / render)^2) samples, log2(render / display) - 1), which every backend asks for today
        [[nodiscard]] virtual std::optional<u32> phaseCount(const Extent2D, const Extent2D) const
        {
            return std::nullopt;
        }
        [[nodiscard]] virtual std::optional<float> mipBias(const Extent2D, const Extent2D) const
        {
            return std::nullopt;
        }
        [[nodiscard]] virtual UpscalerRequirements requirements() const { return {}; }

        // A context for a view at these extents, before its first dispatch: its id (never 0), or 0 to refuse it (the
        // backend then counts as failed for the session). A view has one at a time; a change of extent creates the
        // new one before the old one is destroyed.
        [[nodiscard]] virtual u64 create(const UpscalerContext& context) = 0;
        // Frees a context; the registry calls it once the frames that used it finished. Contexts a backend still holds
        // when it is itself destroyed (replaced, or its owner dropped) go with it.
        virtual void destroy(u64 context) = 0;
        // Records the upscale into the executing pass; empty on success, else the error (the backend then counts as
        // failed for the session, and the view falls back next frame).
        [[nodiscard]] virtual std::string dispatch(RenderPassExecutionContext& context,
                                                   const UpscaleDispatch& inputs) = 0;
    };
} // namespace spite
