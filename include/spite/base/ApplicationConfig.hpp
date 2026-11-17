#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "base/BaseExport.hpp"
#include "base/InternedString.hpp"
#include "base/Schema.hpp"

namespace spite
{
    /**
     * @brief Enum representing different window states.
     */
    enum class WindowState
    {
        Normal,
        Minimized,
        Maximized,
        Hidden
    };

    /**
     * @brief Configuration structure for window settings.
     */
    struct WindowConfig
    {
        unsigned int width = 1600;
        unsigned int height = 1200;
        InternedString title = "SPITE Engine";
        bool resizable = true;
        bool fullscreen = false;
        bool borderless = false;
        WindowState state = WindowState::Normal;
        bool highDpi = true;

        SPITE_SCHEMA(WindowConfig, width, height, title, resizable, fullscreen, borderless, state, highDpi)
    };

    // Sun shadow settings . cascadeCount and resolution shape the
    // render graph; the rest reach the GPU through uploads or push constants.
    struct ShadowConfig
    {
        static constexpr unsigned int MAX_CASCADES = 4;

        // Each cascade is a D16 layer of resolution^2 texels: the largest set (MAX_CASCADES layers of MAX_RESOLUTION^2
        // x 2 B) is 512 MiB per rendered view, what a file or a command may ask the device for; a larger value
        // cannot allocate on most cards and would fail at every start.
        static constexpr unsigned int MAX_RESOLUTION = 8192;
        static_assert(static_cast<unsigned long long>(MAX_CASCADES) * MAX_RESOLUTION * MAX_RESOLUTION * 2 <= 1ull
                          << 29);

        unsigned int cascadeCount = 4; // 1..MAX_CASCADES
        unsigned int resolution = 2048; // texels per side of each cascade's map, 64..MAX_RESOLUTION
        float maxDistance = 100.0f; // the last cascade ends at min(camera far, maxDistance)
        float splitLambda = 0.75f; // 0: uniform splits, 1: logarithmic
        float depthBiasConstant = -2.0f;
        float depthBiasSlope = -4.0f;
        float normalOffset = 1.0f; // in texels of the sampled cascade
        unsigned int pcfRadius = 1; // (2r+1)^2 taps, 0..MAX_PCF_RADIUS
        float blendBand = 0.1f; // fraction of a cascade's split blended into the next one

        static constexpr unsigned int MAX_PCF_RADIUS = 4;

        // Point and spot light shadow maps: per rendered view a D16 array of at most MAX_LIGHT_MAPS map
        // slots (a spot light takes one, a point light six) of lightResolution^2 texels, cut to what lightBudgetMiB
        // holds. The memory budget caps allocation independently of the slot limit.
        static constexpr unsigned int MAX_LIGHT_MAPS = 144;
        static constexpr unsigned int MAX_LIGHT_RESOLUTION = 2048;
        static constexpr unsigned int MAX_LIGHT_BUDGET_MIB = 512;
        static_assert(static_cast<unsigned long long>(MAX_LIGHT_MAPS) * MAX_LIGHT_RESOLUTION * MAX_LIGHT_RESOLUTION *
                          2 <=
                      1ull << 31);

        unsigned int lightMaps = 8; // map slots per view, 0..MAX_LIGHT_MAPS: 0 turns light shadows off
        unsigned int lightResolution = 1024; // texels per side of each map, 64..MAX_LIGHT_RESOLUTION
        unsigned int lightBudgetMiB = 16; // hard cap on a view's light shadow maps, 0..MAX_LIGHT_BUDGET_MIB

        // the map slots a view gets: lightMaps, within the hard cap and the budget. A layered texture has two layers at
        // least, so the budget must hold two maps for any slot, and one slot is charged for two
        [[nodiscard]] unsigned int lightMapSlots() const
        {
            const unsigned long long mapBytes = 2ull * lightResolution * lightResolution;
            const unsigned long long affordable = (static_cast<unsigned long long>(lightBudgetMiB) << 20) / mapBytes;
            return affordable < 2
                ? 0u
                : static_cast<unsigned int>(std::min<unsigned long long>({lightMaps, MAX_LIGHT_MAPS, affordable}));
        }

        [[nodiscard]] bool valid() const
        {
            return lightMaps <= MAX_LIGHT_MAPS && lightResolution >= 64 && lightResolution <= MAX_LIGHT_RESOLUTION &&
                lightBudgetMiB <= MAX_LIGHT_BUDGET_MIB && cascadeCount >= 1 && cascadeCount <= MAX_CASCADES &&
                resolution >= 64 && resolution <= MAX_RESOLUTION && std::isfinite(maxDistance) && maxDistance > 0.0f &&
                splitLambda >= 0.0f && splitLambda <= 1.0f && blendBand >= 0.0f && blendBand <= 0.5f &&
                std::isfinite(normalOffset) && normalOffset >= 0.0f && std::isfinite(depthBiasConstant) &&
                std::isfinite(depthBiasSlope) && pcfRadius <= MAX_PCF_RADIUS;
        }

        SPITE_SCHEMA(ShadowConfig, cascadeCount, resolution, maxDistance, splitLambda, depthBiasConstant,
                     depthBiasSlope, normalOffset, pcfRadius, blendBand, lightMaps, lightResolution, lightBudgetMiB)
    };

    // The froxel grid light binning sorts point and spot lights into: gridX by gridY screen tiles and
    // gridZ depth slices per view, each cluster listing at most maxLightsPerCluster lights (more are counted as
    // overflow, not stored). The buffers are sized from it at startup.
    struct LightingConfig
    {
        static constexpr unsigned int MAX_GRID = 64;
        static constexpr unsigned int MAX_LIGHTS_PER_CLUSTER = 1024;
        // a view's cluster lists stay under 64 MiB
        static constexpr unsigned long long MAX_LIST_BYTES = 1ull << 26;

        unsigned int gridX = 16;
        unsigned int gridY = 9;
        unsigned int gridZ = 24;
        unsigned int maxLightsPerCluster = 128;

        [[nodiscard]] unsigned int clusterCount() const { return gridX * gridY * gridZ; }

        [[nodiscard]] bool valid() const
        {
            return gridX >= 1 && gridX <= MAX_GRID && gridY >= 1 && gridY <= MAX_GRID && gridZ >= 1 &&
                gridZ <= MAX_GRID && maxLightsPerCluster >= 1 && maxLightsPerCluster <= MAX_LIGHTS_PER_CLUSTER &&
                static_cast<unsigned long long>(clusterCount()) * (maxLightsPerCluster + 1) * sizeof(unsigned int) <=
                MAX_LIST_BYTES;
        }

        SPITE_SCHEMA(LightingConfig, gridX, gridY, gridZ, maxLightsPerCluster)
    };

    // How far below its output a view renders: Custom takes UpscalerConfig::scale, the others the presets
    // DLSS and FSR share: Native 1x, Quality 1.5x, Balanced 1.7x, Performance 2x and UltraPerformance 3x fewer pixels
    // per axis.
    enum class UpscaleQuality : unsigned int
    {
        Custom,
        Native,
        Quality,
        Balanced,
        Performance,
        UltraPerformance
    };
    SPITE_ENUM_NAMES(UpscaleQuality, Custom, Native, Quality, Balanced, Performance, UltraPerformance)
    inline constexpr bool validUpscaleQuality(const UpscaleQuality quality)
    {
        return quality <= UpscaleQuality::UltraPerformance;
    }
    // The share of its output a preset renders at on each axis; 0 for Custom (and a value that is no preset).
    inline constexpr float upscaleQualityScale(const UpscaleQuality quality)
    {
        constexpr float PRESETS[] = {0.0f, 1.0f, 1.0f / 1.5f, 1.0f / 1.7f, 0.5f, 1.0f / 3.0f};
        return validUpscaleQuality(quality) ? PRESETS[static_cast<unsigned int>(quality)] : 0.0f;
    }

    // The game view's upscaler: its scene renders at scale times its output extent on each
    // axis and the backend turns it into the output: off (the default) Composite's bilinear filter, native the
    // temporal upscaler of the render pipeline. Other views keep what the setUpscale command gave them (the Scene
    // view: off at scale 1).
    struct UpscalerConfig
    {
        static constexpr float MIN_SCALE = 0.25f;

        float scale = 1.0f; // MIN_SCALE..1, read while quality is custom
        // the backend by name: off, native, ... (a name nothing provides falls back, with the reason in the upscale
        // table)
        InternedString backend = "off";
        UpscaleQuality quality = UpscaleQuality::Custom;
        float sharpness = 0.0f; // 0..1: the contrast-adaptive sharpening after the backend, 0 off

        // the render scale the config asks the game view for
        [[nodiscard]] float effectiveScale() const
        {
            const float preset = upscaleQualityScale(quality);
            return preset > 0.0f ? preset : scale;
        }

        [[nodiscard]] bool valid() const
        {
            return std::isfinite(scale) && scale >= MIN_SCALE && scale <= 1.0f && validUpscaleQuality(quality) &&
                std::isfinite(sharpness) && sharpness >= 0.0f && sharpness <= 1.0f && !backend.toView().empty();
        }

        SPITE_SCHEMA(UpscalerConfig, scale, backend, quality, sharpness)
    };

    // Bloom: the light of the shown picture above a soft threshold, blurred through a pyramid of mips,
    // added to the scene light before the tone map. Off by default: an intensity of 0 declares no pass and allocates no
    // texture. Every field is live; the graph rebuilds only when the intensity crosses 0.
    struct BloomConfig
    {
        static constexpr unsigned int MAX_MIPS = 8;

        // the share of the glow added to the scene light; 0 is off. The glow holds the light the threshold let
        // through, so an intensity of 1 adds as much light as it took.
        float intensity = 0.0f;
        // scene-referred light (before exposure): a texel's brightest channel under threshold - knee adds nothing
        float threshold = 1.0f;
        // the soft threshold's width as a share of threshold, 0 (a hard edge) to 1
        float knee = 0.5f;
        // 0..1: how far the glow spreads; each smaller mip joins with this weight against the mip above, 0 keeps the
        // narrowest blur
        float radius = 0.7f;
        // levels of the pyramid, 1..MAX_MIPS (fewer when the picture is too small for them)
        unsigned int mips = 6;

        [[nodiscard]] bool valid() const
        {
            return std::isfinite(intensity) && intensity >= 0.0f && intensity <= 16.0f && std::isfinite(threshold) &&
                threshold >= 0.0f && threshold <= 65504.0f && std::isfinite(knee) && knee >= 0.0f && knee <= 1.0f &&
                std::isfinite(radius) && radius >= 0.0f && radius <= 1.0f && mips >= 1 && mips <= MAX_MIPS;
        }

        SPITE_SCHEMA(BloomConfig, intensity, threshold, knee, radius, mips)
    };

    // The display transform Composite applies to the exposed scene light: Khronos PBR Neutral (keeps a
    // base colour as authored), a fit of ACES filmic, or AgX.
    enum class Tonemap : unsigned int
    {
        Neutral,
        Aces,
        Agx
    };
    SPITE_ENUM_NAMES(Tonemap, Neutral, Aces, Agx)
    inline constexpr bool validTonemap(const Tonemap tonemap) { return tonemap <= Tonemap::Agx; }

    // Temporal anti-aliasing: Off draws every view as before; Taa jitters the projection of the views
    // that change (a view's own override, setJitter, wins).
    enum class AntiAliasing : unsigned int
    {
        Off,
        Taa
    };
    SPITE_ENUM_NAMES(AntiAliasing, Off, Taa)
    inline constexpr bool validAntiAliasing(const AntiAliasing aa) { return aa <= AntiAliasing::Taa; }

    // Ambient occlusion: Off draws every view as before; Gtao darkens the indirect light where nearby
    // geometry blocks it (ground-truth AO over the depth and normals, shaders/source/gtao.comp.slang).
    enum class AmbientOcclusion : unsigned int
    {
        Off,
        Gtao
    };
    SPITE_ENUM_NAMES(AmbientOcclusion, Off, Gtao)
    inline constexpr bool validAmbientOcclusion(const AmbientOcclusion ao) { return ao <= AmbientOcclusion::Gtao; }

    // a named constant: GraphicsConfig holds InternedStrings, so it is not constant-evaluable
    inline constexpr unsigned int DEFAULT_READBACK_STAGING_MB = 64;

    /**
     * @brief Configuration structure for graphics settings.
     */
    struct GraphicsConfig
    {
        unsigned int maxFramesInFlight = 2;
        bool enableMeshShaders =
            true; // Advertise mesh shading only if supported AND enabled; false forces the fallback pipeline
        bool meshletCulling = true; // per-meshlet frustum and normal-cone culling; false passes candidates through
        bool occlusionCulling = true; // two-phase Hi-Z occlusion culling; false draws every frustum-visible object
        // Live: cluster LOD. The cull draws the clusters of a baked mesh's DAG whose projected error is
        // within 2^lodBias display pixels while their parent group's is not; false draws level 0 only (the oracle's and
        // the reference for every comparison), as does a mesh baked before version 3. lodBias in stops: +1 doubles the
        // error a cluster may add; it is the default of every render view's own bias (the setLod command).
        bool lod = true; // on since the LOD-on against LOD-off check (--oracle --lod-check) passes
        float lodBias = 0.f;
        bool gpuPassTimers = true; // Per-pass GPU timestamps for the zones report; false keeps only the frame pair
        bool gpuCalibratedTimestamps =
            true; // GPU passes on the CPU clock via calibrated timestamps; false: frame-relative
        unsigned int maxLights = 1024;
        // Views rendered per frame: the swapchain view (RenderViews, default
        // 0) to the swapchain, the others into offscreen graph textures
        // (viewTargetName); each runs its own pass chain.
        unsigned int renderViewCount = 1;
        // The view reserved for light probe capture, 0: none (view 0 is the primary view); a view of
        // the range above the views the host shows. While a probe volume bakes it renders one cube face of one probe
        // per frame, at 64 x 64, with only the passes that light and draw the sky; it is never shown. Without one a
        // scene's probe volumes stay unbaked and nothing lights from them.
        unsigned int probeCaptureView = 0;
        // The views reserved from probeCaptureView on, 1 to MAX_PROBE_CAPTURE_VIEWS: the faces the bake
        // draws a frame, one per view, each on a different probe; views past renderViewCount are not created, so the
        // host raises renderViewCount to probeCaptureView + probeCaptureViews. 1 draws one face a frame.
        unsigned int probeCaptureViews = 1;
        static constexpr unsigned int MAX_PROBE_CAPTURE_VIEWS = 8;
        // Live: the operator of every view's Composite pass
        Tonemap tonemap = Tonemap::Neutral;
        // Live: Taa jitters the projection of each view while its picture changes and for a cycle after
        AntiAliasing aa = AntiAliasing::Off;
        // Live: Gtao computes each view's ambient occlusion (recomputed only when its depth changed) and Light
        // multiplies the indirect light by it; aoRadius is its reach in world units, aoIntensity the power the
        // visibility is raised to (1: as computed, 0: no effect)
        AmbientOcclusion ao = AmbientOcclusion::Off;
        float aoRadius = 0.5f;
        float aoIntensity = 1.f;
        // Live: what auto exposure (CameraComponent::autoExposure) adapts within: the EV bounds of the exposure the
        // histogram asks for, and the time constant in seconds of the adaptation toward it (0: at once)
        float exposureMinEv = -6.f;
        float exposureMaxEv = 8.f;
        float exposureAdaptSeconds = 1.f;
        // Live: the coverage (the material's albedo alpha, tint and texture alpha) from which a blended surface is
        // pickable: click picking, pixelInfo and the id views take the nearest blended surface at or above
        // it and look through the ones below. 0 takes every surface with any coverage; a fully transparent one is
        // never pickable.
        float pickAlphaThreshold = 0.1f;

        // Bindless rendering configuration
        unsigned int maxBindlessTextures = 8192;
        unsigned int maxBindlessMaterials = 4096;
        // The object buffers' initial instance-slot capacity; they grow on demand up to objectCapacityCeiling.
        unsigned int maxBindlessInstances = 4096;
        // Live (setConfig): the instance-slot capacity the object buffers may grow to; 0 = unlimited (the GPU budget
        // bounds it). Renderables past it stay non-resident, with a warning.
        unsigned int objectCapacityCeiling = 0;
        // Visible-meshlet list capacity — deliberately separate from the
        // instance count: meshlets per instance vary by model (hundreds each
        // is normal), so sizing the cull output from the instance count
        // under-provisions. 0 = derive as 4x maxBindlessInstances.
        unsigned int maxVisibleMeshlets = 0;
        unsigned int globalVertexBufferSize = 256 * 1024 * 1024; // 256 MB
        unsigned int globalIndexBufferSize = 128 * 1024 * 1024; // 128 MB
        unsigned int globalDrawIndexBufferSize = 512 * 1024 * 1024; // 512 MB, expanded u32 indices (3 per triangle)
        unsigned int maxMeshlets = 1024 * 1024 * 2;
        // Soft streaming budget ceiling; 0 = auto (device-local heap budget minus safety margin)
        unsigned int gpuMemoryBudgetMB = 0;
        // GPU -> CPU readback staging per frame slot (ReadbackManager)
        unsigned int readbackStagingMB = DEFAULT_READBACK_STAGING_MB;
        // A frame fence not signaled in gpuHangTimeoutMs is a GPU hang (IRenderer::gpuHang): frames are skipped and
        // the control channel keeps answering; one gpuHangFatalMs old (0: never; counted from the hang's start, which
        // includes the timed-out wait) ends the run with a crash dump.
        unsigned int gpuHangTimeoutMs = 5000;
        unsigned int gpuHangFatalMs = 30000;
        ShadowConfig shadows;
        // Read at startup: sizes the light cluster buffers
        LightingConfig lighting;
        // Live: the game view's render scale
        UpscalerConfig upscaler;
        // Live: bloom on every view's shown picture (off at intensity 0)
        BloomConfig bloom;
        // Test only, outside the schema: the named render-graph pass records no
        // barriers, a known hazard proving a sync-validation run can see one.
        // Also "<pass>:exit" (its group's brackets), "graph:init", "graph:frameEnd".
        std::string dropBarriersBeforePass;
        // Outside the schema: VK_EXT_debug_utils (pass labels, object names) in Release too; --renderdoc sets it.
        bool debugLabels = false;
        // Shader hot reload compiles <source>/*.slang into the binary directory pipelines load from
        // (SPITE_SHADER_PATH); a missing or empty source directory turns it off. Tests point both at a private
        // directory.
        InternedString shaderSourceDirectory = "shaders/source";
        InternedString shaderBinaryDirectory = "shaders/bin";

        // each exposure field inside its range (a bound pair set the wrong way round is taken in order: each bound
        // is valid whatever the other is, so a setConfig of one never depends on the other)
        bool exposureValid() const
        {
            return std::isfinite(exposureMinEv) && std::isfinite(exposureMaxEv) &&
                std::isfinite(exposureAdaptSeconds) && exposureMinEv >= -24.f && exposureMinEv <= 24.f &&
                exposureMaxEv >= -24.f && exposureMaxEv <= 24.f && exposureAdaptSeconds >= 0.f &&
                exposureAdaptSeconds <= 3600.f;
        }

        bool pickValid() const
        {
            return std::isfinite(pickAlphaThreshold) && pickAlphaThreshold >= 0.f && pickAlphaThreshold <= 1.f;
        }

        bool aoValid() const
        {
            return validAmbientOcclusion(ao) && std::isfinite(aoRadius) && aoRadius > 0.f &&
                aoRadius <= MAX_AO_RADIUS && std::isfinite(aoIntensity) && aoIntensity >= 0.f &&
                aoIntensity <= MAX_AO_INTENSITY;
        }
        static constexpr float MAX_AO_RADIUS = 100.f;
        static constexpr float MAX_AO_INTENSITY = 8.f;

        // The first view of the probe capture range: probeCaptureView when it names a view (above 0, below
        // renderViewCount), else ~0u
        unsigned int captureView() const
        {
            return probeCaptureView > 0 && probeCaptureView < renderViewCount ? probeCaptureView : ~0u;
        }

        // The views of the range [captureView(), captureView() + probeCaptureViews) that exist (the faces a frame
        // bakes); 0 without a capture view
        unsigned int captureViewCount() const
        {
            const unsigned int first = captureView();
            if (first == ~0u)
            {
                return 0;
            }
            const unsigned int wanted = probeCaptureViews < 1 ? 1u
                : probeCaptureViews > MAX_PROBE_CAPTURE_VIEWS ? MAX_PROBE_CAPTURE_VIEWS
                                                              : probeCaptureViews;
            return renderViewCount - first < wanted ? renderViewCount - first : wanted;
        }

        bool isCaptureView(const unsigned int view) const
        {
            const unsigned int first = captureView();
            return first != ~0u && view >= first && view - first < captureViewCount();
        }

        // The cull-output capacity: which of the two knobs actually clamps.
        unsigned int visibleMeshletCapacity() const
        {
            return maxVisibleMeshlets ? maxVisibleMeshlets : maxBindlessInstances * 4;
        }

        SPITE_SCHEMA(GraphicsConfig, maxFramesInFlight, enableMeshShaders, meshletCulling, occlusionCulling, lod,
                     lodBias, gpuPassTimers, gpuCalibratedTimestamps, maxLights, renderViewCount, probeCaptureView,
                     probeCaptureViews, tonemap, aa, ao, aoRadius, aoIntensity, exposureMinEv, exposureMaxEv,
                     exposureAdaptSeconds, pickAlphaThreshold, maxBindlessTextures, maxBindlessMaterials,
                     maxBindlessInstances, objectCapacityCeiling, maxVisibleMeshlets, globalVertexBufferSize,
                     globalIndexBufferSize, globalDrawIndexBufferSize, maxMeshlets, gpuMemoryBudgetMB,
                     readbackStagingMB, gpuHangTimeoutMs, gpuHangFatalMs, shadows, upscaler, bloom, lighting,
                     shaderSourceDirectory, shaderBinaryDirectory)
    };

    /**
     * @brief Configuration structure for input settings.
     */
    struct InputConfig
    {
        InternedString mappingsFile = "input_mappings.json";
        float mouseSensitivity = 1.0f;
        bool invertYAxis = false;
        bool enableController = true;
        bool enableTouch = false;
        float controllerDeadzone = 0.1f;
        float controllerSensitivity = 1.0f;

        SPITE_SCHEMA(InputConfig, mappingsFile, mouseSensitivity, invertYAxis, enableController, enableTouch,
                     controllerDeadzone, controllerSensitivity)
    };

    /**
     * @brief Configuration structure for performance settings.
     */
    struct TimingConfig
    {
        unsigned int targetFPS = 0;
        float timeScale = 1.0f;
        bool limitFPS = false;
        unsigned int maxFrameTime = 1000; // milliseconds
        float fixedDeltaTime = 1.0f / 60.0f;
        unsigned int maxFixedSteps = 4;
        bool vsyncEnabled = true;

        SPITE_SCHEMA(TimingConfig, targetFPS, timeScale, limitFPS, maxFrameTime, fixedDeltaTime, maxFixedSteps,
                     vsyncEnabled)
    };
} // namespace spite

namespace spite
{
    /**
     * @brief Main application configuration structure.
     */
    struct ApplicationConfig
    {
        WindowConfig window;
        GraphicsConfig graphics;
        InputConfig input;
        TimingConfig timing;

        std::string configPath = "config/";

        // Runtime modules the engine loads at startup, in order: DLL
        // paths, relative ones against the executable's directory, ".dll"
        // optional. Read from application/modules.json; outside the schema,
        // which has no string list.
        std::vector<std::string> modules{};
        // Optional authoritative project layer; configPath/project then supplies read-only defaults.
        std::string projectConfigPath{};

        /**
         * @brief Validate the configuration values.
         * @return true if configuration is valid, false otherwise.
         */
        SPITE_BASE_API bool validate() const;

        SPITE_SCHEMA(ApplicationConfig, window, graphics, input, timing, configPath, projectConfigPath)
    };
} // namespace spite
