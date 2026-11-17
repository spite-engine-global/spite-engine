#include "ApplicationConfigFields.hpp"

#include "application/input/InputActionMap.hpp"
#include "base/ApplicationConfig.hpp"

namespace spite
{
    namespace
    {
        // What a command may set, tighter than the validators that accept a file: SDL takes the window's extent as
        // an int and no display has a texture past 16384; every fixed step is an event each frame, and the time
        // scale multiplies the time a frame feeds them.
        constexpr double MAX_WINDOW_EXTENT = 16384;
        constexpr double MAX_FIXED_STEPS = 1000;
        constexpr double MAX_TIME_SCALE = 1000;

        constexpr ConfigChoice WINDOW_STATES[] = {{static_cast<i32>(WindowState::Normal), "normal"},
                                                  {static_cast<i32>(WindowState::Minimized), "minimized"},
                                                  {static_cast<i32>(WindowState::Maximized), "maximized"},
                                                  {static_cast<i32>(WindowState::Hidden), "hidden"}};

        constexpr ConfigChoice TONEMAPS[] = {{static_cast<i32>(Tonemap::Neutral), "neutral"},
                                             {static_cast<i32>(Tonemap::Aces), "aces"},
                                             {static_cast<i32>(Tonemap::Agx), "agx"}};

        constexpr ConfigChoice ANTI_ALIASING[] = {{static_cast<i32>(AntiAliasing::Off), "off"},
                                                  {static_cast<i32>(AntiAliasing::Taa), "taa"}};

        constexpr ConfigChoice AMBIENT_OCCLUSION[] = {{static_cast<i32>(AmbientOcclusion::Off), "off"},
                                                      {static_cast<i32>(AmbientOcclusion::Gtao), "gtao"}};
        constexpr ConfigChoice UPSCALE_QUALITIES[] = {
            {static_cast<i32>(UpscaleQuality::Custom), "custom"},
            {static_cast<i32>(UpscaleQuality::Native), "native"},
            {static_cast<i32>(UpscaleQuality::Quality), "quality"},
            {static_cast<i32>(UpscaleQuality::Balanced), "balanced"},
            {static_cast<i32>(UpscaleQuality::Performance), "performance"},
            {static_cast<i32>(UpscaleQuality::UltraPerformance), "ultraPerformance"}};

        constexpr ConfigFieldMeta WINDOW[] = {
            {.path = "width",
             .doc = "Width of the window's client area; the restore size while maximized or fullscreen.",
             .unit = "px",
             .min = 1,
             .max = MAX_WINDOW_EXTENT},
            {.path = "height",
             .doc = "Height of the window's client area; the restore size while maximized or fullscreen.",
             .unit = "px",
             .min = 1,
             .max = MAX_WINDOW_EXTENT},
            {.path = "title", .doc = "The window's title bar text."},
            {.path = "resizable", .doc = "Whether the user can resize the window."},
            {.path = "fullscreen", .doc = "Cover the display: borderless at the desktop resolution."},
            {.path = "borderless", .doc = "Draw the window without a frame."},
            {.path = "state", .doc = "The window's state.", .choices = WINDOW_STATES},
            {.path = "highDpi",
             .doc = "Create the window with a high-DPI backing store; read when the window is created.",
             .apply = ConfigFieldApply::Startup},
        };

        constexpr ConfigFieldMeta GRAPHICS[] = {
            {.path = "maxFramesInFlight",
             .doc = "Frames the CPU may record ahead of the GPU; sizes every per-frame resource.",
             .min = 1},
            {.path = "enableMeshShaders",
             .doc = "Use mesh shading when the device supports it; false forces the fallback pipeline."},
            {.path = "meshletCulling",
             .doc = "Per-meshlet frustum and normal-cone culling; false passes every candidate through."},
            {.path = "occlusionCulling",
             .doc = "Two-phase Hi-Z occlusion culling; false draws every frustum-visible object."},
            {.path = "lod",
             .doc =
                 "Cluster LOD: the cull draws the clusters of a baked mesh whose projected error is within the pixel "
                 "budget while their parent group's is not; false draws level 0 only (as does a mesh baked before "
                 "format 3).",
             .apply = ConfigFieldApply::Live},
            {.path = "lodBias",
             .doc =
                 "Cluster LOD error budget in stops: a cluster may add 2^lodBias display pixels of projected error (0: "
                 "one pixel; +1 coarser, -1 finer). The default of every render view's own bias (setLod). Only read "
                 "while lod is on.",
             .min = -16,
             .max = 16,
             .apply = ConfigFieldApply::Live},
            {.path = "gpuPassTimers",
             .doc = "Per-pass GPU timestamps for the zones report; false keeps only the frame pair."},
            {.path = "gpuCalibratedTimestamps",
             .doc = "Place GPU passes on the CPU clock with calibrated timestamps; false: frame-relative."},
            {.path = "maxLights", .doc = "Capacity of the light buffers."},
            {.path = "renderViewCount",
             .doc = "Views rendered per frame: the swapchain view and offscreen ones; the editor needs 2.",
             .min = 1},
            {.path = "probeCaptureView",
             .doc =
                 "The view reserved for light probe capture, 0 for none: a view index below renderViewCount that the "
                 "host does not show (the editor moves it past its own two views; a game view cannot be set on it). "
                 "While a probe volume bakes it renders one cube face of one probe per frame at 64 x 64, its sun "
                 "shadowed by one sun map fitted around the volume; any graph recompile (a resize, bloom, jitter or "
                 "material-class switch) restarts the whole bake; without it a scene's probe volumes stay unbaked. "
                 "probeCaptureViews draws more faces a frame."},
            {.path = "probeCaptureViews",
             .doc =
                 "Capture views from probeCaptureView on, each drawing one cube face of a different probe per frame "
                 "(1: one face a frame; 4 bakes a volume about four times faster for four times the capture passes "
                 "while it bakes): they share the first one's sun map, and the host must create them (renderViewCount "
                 ">= probeCaptureView + probeCaptureViews; views past it are not created); the editor and game views "
                 "never sit on them. Read at startup.",
             .min = 1,
             .max = GraphicsConfig::MAX_PROBE_CAPTURE_VIEWS,
             .apply = ConfigFieldApply::Startup},
            {.path = "tonemap",
             .doc = "The operator Composite maps the exposed scene light to the display with: neutral (Khronos PBR "
                    "Neutral, base colours stay as authored), aces (a fit of ACES filmic) or agx.",
             .choices = TONEMAPS,
             .apply = ConfigFieldApply::Live},
            {.path = "aa",
             .doc =
                 "Temporal anti-aliasing: off draws every view as before; taa jitters the projection of a view "
                 "while its picture changes and for one cycle after, then stops (a still view costs nothing). A "
                 "view's own jitter (setJitter) wins; pause, step and the render oracle never jitter. A resolve "
                 "accumulates the frames into the picture, so the view stays antialiased. A view that asks for an "
                 "upscaler (upscaler.backend, setUpscale) jitters whatever aa says, and its upscaler takes the place "
                 "of the resolve.",
             .choices = ANTI_ALIASING,
             .apply = ConfigFieldApply::Live},
            {.path = "ao",
             .doc = "Ambient occlusion: off draws every view as before; gtao darkens the indirect light (the "
                    "environment's ambient term) where nearby geometry blocks it, from the view's depth and normals. "
                    "It is recomputed only on frames where the view's depth changed (a camera move or a scene update), "
                    "so a still view costs nothing; the ao view mode shows it.",
             .choices = AMBIENT_OCCLUSION,
             .apply = ConfigFieldApply::Live},
            {.path = "aoRadius",
             .doc = "Reach of the ambient occlusion in world units: geometry farther than this from a surface does not "
                    "occlude it (its weight fades over the last 60%). Only read while ao is on.",
             .unit = "m",
             .min = 0.001,
             .max = GraphicsConfig::MAX_AO_RADIUS,
             .apply = ConfigFieldApply::Live},
            {.path = "aoIntensity",
             .doc = "Power the ambient occlusion visibility is raised to: 1 as computed, above 1 darker, 0 no effect. "
                    "Only read while ao is on.",
             .min = 0,
             .max = GraphicsConfig::MAX_AO_INTENSITY,
             .apply = ConfigFieldApply::Live},
            {.path = "exposureMinEv",
             .doc = "Lowest exposure auto exposure may ask for, in EV (a bright scene is not darkened below it).",
             .min = -24,
             .max = 24,
             .apply = ConfigFieldApply::Live},
            {.path = "exposureMaxEv",
             .doc = "Highest exposure auto exposure may ask for, in EV (a dark scene is not lifted above it); a pair "
                    "set the wrong way round is taken in order.",
             .min = -24,
             .max = 24,
             .apply = ConfigFieldApply::Live},
            {.path = "exposureAdaptSeconds",
             .doc = "Time constant of auto exposure's adaptation, in seconds: it closes 63% of a luminance change in "
                    "this time and 95% in three times it; 0 follows at once. Frozen while the world is paused.",
             .min = 0,
             .max = 3600,
             .apply = ConfigFieldApply::Live},
            {.path = "pickAlphaThreshold",
             .doc = "Coverage (albedo alpha times tint and texture alpha) from which a blended see-through surface is "
                    "pickable: click picking, pixelInfo and the entityId and meshletId views take the nearest blended "
                    "surface at or above it and look through the ones below; 0 takes any coverage above 0.",
             .min = 0,
             .max = 1,
             .apply = ConfigFieldApply::Live},
            {.path = "maxBindlessTextures", .doc = "Capacity of the bindless texture table.", .min = 1},
            {.path = "maxBindlessMaterials", .doc = "Capacity of the bindless material table."},
            {.path = "maxBindlessInstances",
             .doc = "Initial instance slots of the object buffers; they grow on demand up to objectCapacityCeiling.",
             .min = 1},
            {.path = "objectCapacityCeiling",
             .doc = "Most instance slots the object buffers may grow to, 0 for unlimited (the GPU budget bounds it); "
                    "renderables past it stay non-resident.",
             .apply = ConfigFieldApply::Live},
            {.path = "maxVisibleMeshlets",
             .doc = "Capacity of the visible-meshlet list; 0 derives 4x maxBindlessInstances."},
            {.path = "globalVertexBufferSize",
             .doc = "Initial global vertex heap size when manifest seeding is disabled; grows with demand.",
             .unit = "bytes"},
            {.path = "globalIndexBufferSize",
             .doc = "Initial meshlet index heap size when manifest seeding is disabled; grows with demand.",
             .unit = "bytes"},
            {.path = "globalDrawIndexBufferSize",
             .doc = "Initial expanded index heap size when manifest seeding is disabled; grows with demand.",
             .unit = "bytes"},
            {.path = "maxMeshlets",
             .doc = "Initial meshlet table capacity when manifest seeding is disabled; grows with demand."},
            {.path = "gpuMemoryBudgetMB",
             .doc = "Soft ceiling of streaming residency; 0 is the device-local heap budget minus a safety margin.",
             .unit = "MB"},
            {.path = "readbackStagingMB", .doc = "GPU to CPU readback staging per frame slot.", .unit = "MB", .min = 1},
            {.path = "gpuHangTimeoutMs",
             .doc = "A frame fence not signaled this long is a GPU hang: frames are skipped and the window query "
                    "reports gpuHang.",
             .unit = "ms",
             .min = 1},
            {.path = "gpuHangFatalMs",
             .doc =
                 "A GPU hang this old (counted from its start, so a value up to gpuHangTimeoutMs ends it at the first "
                 "poll) ends the run with a crash dump; 0 keeps it hung for inspection.",
             .unit = "ms"},
            {.path = "shadows",
             .doc = "Sun shadow cascades and point and spot light shadow maps.",
             .apply = ConfigFieldApply::Live},
            {.path = "shadows.cascadeCount",
             .doc = "Cascades of the sun's shadow map.",
             .min = 1,
             .max = ShadowConfig::MAX_CASCADES,
             .apply = ConfigFieldApply::Live},
            {.path = "shadows.resolution",
             .doc = "Texels per side of each cascade's map.",
             .unit = "px",
             .min = 64,
             .max = ShadowConfig::MAX_RESOLUTION,
             .apply = ConfigFieldApply::Live},
            {.path = "shadows.maxDistance",
             .doc = "Distance at which the last cascade ends, or the camera's far plane when nearer.",
             .unit = "m",
             .min = 0.01,
             .apply = ConfigFieldApply::Live},
            {.path = "shadows.splitLambda",
             .doc = "Cascade split scheme: 0 uniform, 1 logarithmic.",
             .min = 0,
             .max = 1,
             .apply = ConfigFieldApply::Live},
            {.path = "shadows.depthBiasConstant",
             .doc = "Constant depth bias of the shadow rasterization.",
             .apply = ConfigFieldApply::Live},
            {.path = "shadows.depthBiasSlope",
             .doc = "Slope-scaled depth bias of the shadow rasterization.",
             .apply = ConfigFieldApply::Live},
            {.path = "shadows.normalOffset",
             .doc = "Receiver offset along the normal, in texels of the sampled cascade.",
             .unit = "texels",
             .min = 0,
             .apply = ConfigFieldApply::Live},
            {.path = "shadows.pcfRadius",
             .doc = "Filter radius: (2r+1)^2 taps per lookup.",
             .unit = "texels",
             .min = 0,
             .max = ShadowConfig::MAX_PCF_RADIUS,
             .apply = ConfigFieldApply::Live},
            {.path = "shadows.blendBand",
             .doc = "Fraction of a cascade's split range blended into the next cascade.",
             .min = 0,
             .max = 0.5,
             .apply = ConfigFieldApply::Live},
            {.path = "shadows.lightMaps",
             .doc = "Shadow map slots of each view's point and spot lights (a spot light takes one, a point light six "
                    "and holds all or none), cut to what shadows.lightBudgetMiB holds; 0 turns light shadows off.",
             .min = 0,
             .max = ShadowConfig::MAX_LIGHT_MAPS,
             .apply = ConfigFieldApply::Live},
            {.path = "shadows.lightResolution",
             .doc = "Texels per side of each light shadow map.",
             .unit = "px",
             .min = 64,
             .max = ShadowConfig::MAX_LIGHT_RESOLUTION,
             .apply = ConfigFieldApply::Live},
            {.path = "shadows.lightBudgetMiB",
             .doc = "Hard cap on one view's light shadow maps: a 1024-squared map takes 2 MiB, and the array has two "
                    "layers at least, so a budget under two maps holds none.",
             .unit = "MiB",
             .min = 0,
             .max = ShadowConfig::MAX_LIGHT_BUDGET_MIB,
             .apply = ConfigFieldApply::Live},
            {.path = "lighting", .doc = "Light binning: the froxel grid of per-cluster light lists."},
            {.path = "lighting.gridX",
             .doc = "Screen tiles across each view's light cluster grid; read at startup.",
             .min = 1,
             .max = LightingConfig::MAX_GRID},
            {.path = "lighting.gridY",
             .doc = "Screen tiles down each view's light cluster grid; read at startup.",
             .min = 1,
             .max = LightingConfig::MAX_GRID},
            {.path = "lighting.gridZ",
             .doc = "Depth slices of each view's light cluster grid, exponential for a perspective view and linear for "
                    "an orthographic one; read at startup.",
             .min = 1,
             .max = LightingConfig::MAX_GRID},
            {.path = "lighting.maxLightsPerCluster",
             .doc = "Lights one cluster lists; lights past it are counted as overflow and left out of that cluster's "
                    "list. Read at startup.",
             .min = 1,
             .max = LightingConfig::MAX_LIGHTS_PER_CLUSTER},
            {.path = "upscaler", .doc = "The game view's upscaler.", .apply = ConfigFieldApply::Live},
            {.path = "upscaler.scale",
             .doc = "Render scale of the game view while quality is custom: its scene renders at this share of its "
                    "output size on each axis and the backend upscales it; 1 renders at the output size.",
             .min = UpscalerConfig::MIN_SCALE,
             .max = 1,
             .apply = ConfigFieldApply::Live},
            {.path = "upscaler.backend",
             .doc = "What turns the game view's render-resolution picture into its output, by name: off (the default) "
                    "Composite's bilinear filter, native the render pipeline's temporal upscaler (it jitters the "
                    "view and accumulates a display-resolution history while it changes, then freezes), auto the "
                    "first provider that is available, else native, or a provider's own name. Any name is a valid "
                    "request: one that is not there, not supported or failed falls back to native and then to off "
                    "(the upscale table says why).",
             .apply = ConfigFieldApply::Live},
            {.path = "upscaler.quality",
             .doc = "The game view's render scale preset: custom takes upscaler.scale, the others render at 1, 1/1.5, "
                    "1/1.7, 1/2 or 1/3 of the output size per axis.",
             .choices = UPSCALE_QUALITIES,
             .apply = ConfigFieldApply::Live},
            {.path = "upscaler.sharpness",
             .doc = "Contrast-adaptive sharpening of the upscaled picture, 0 (off, no pass) to 1.",
             .min = 0,
             .max = 1,
             .apply = ConfigFieldApply::Live},
            {.path = "bloom", .doc = "Bloom on every view's shown picture.", .apply = ConfigFieldApply::Live},
            {.path = "bloom.intensity",
             .doc = "Share of the glow added to the scene light before the tone map: the glow holds the light above "
                    "the threshold, so 1 adds as much light as it took. 0 (the default) is off: no pass runs and no "
                    "texture exists; a change between 0 and above rebuilds the render graph, any other is applied at "
                    "once.",
             .min = 0,
             .max = 16,
             .apply = ConfigFieldApply::Live},
            {.path = "bloom.threshold",
             .doc = "Scene-referred light (before exposure) below which a texel adds nothing to the glow, by its "
                    "brightest channel.",
             .min = 0,
             .max = 65504,
             .apply = ConfigFieldApply::Live},
            {.path = "bloom.knee",
             .doc = "Width of the soft threshold as a share of the threshold: 0 is a hard edge, 1 starts to pass "
                    "light from half the threshold.",
             .min = 0,
             .max = 1,
             .apply = ConfigFieldApply::Live},
            {.path = "bloom.radius",
             .doc = "How far the glow spreads, 0 to 1: each smaller mip of the pyramid joins with this weight against "
                    "the mip above it, so 0 keeps the narrowest blur and 1 the widest.",
             .min = 0,
             .max = 1,
             .apply = ConfigFieldApply::Live},
            {.path = "bloom.mips",
             .doc = "Levels of the bloom pyramid; each halves the picture again, and a small picture has fewer.",
             .min = 1,
             .max = BloomConfig::MAX_MIPS,
             .apply = ConfigFieldApply::Live},
            {.path = "shaderSourceDirectory",
             .doc = "Directory of the .slang sources shader hot reload compiles; missing or empty turns it off.",
             .root = "content",
             .directory = true},
            {.path = "shaderBinaryDirectory",
             .doc = "Directory the pipelines load compiled shaders from; hot reload compiles into it.",
             .root = "content",
             .directory = true},
        };

        constexpr ConfigFieldMeta INPUT[] = {
            {.path = "mappingsFile",
             .doc = "Input mapping file: the actions and their bindings.",
             .root = "content",
             .fileProblem = inputMappingsFileProblem},
            {.path = "mouseSensitivity", .doc = "Scale of mouse movement deltas."},
            {.path = "invertYAxis", .doc = "Invert the vertical mouse movement."},
            {.path = "enableController", .doc = "Read gamepads."},
            {.path = "enableTouch", .doc = "Read touch input."},
            {.path = "controllerDeadzone",
             .doc = "Stick magnitude below which an axis reads zero.",
             .min = 0,
             .max = 0.99},
            {.path = "controllerSensitivity", .doc = "Scale of stick values after the deadzone.", .min = 0},
        };

        constexpr ConfigFieldMeta TIMING[] = {
            {.path = "targetFPS",
             .doc = "Frame rate the loop paces to while limitFPS is on and vsync is off.",
             .unit = "fps"},
            {.path = "timeScale",
             .doc = "Multiplier of the simulation's time; 0 freezes it.",
             .min = 0,
             .max = MAX_TIME_SCALE},
            {.path = "limitFPS", .doc = "Pace the loop to targetFPS while vsync is off; needs targetFPS above 0."},
            {.path = "maxFrameTime",
             .doc = "Longest frame time one frame advances the simulation by; a slower frame is cut; 0 for no limit.",
             .unit = "ms"},
            {.path = "fixedDeltaTime", .doc = "Length of one fixed simulation step.", .unit = "s", .min = 0.001},
            {.path = "maxFixedSteps",
             .doc = "Fixed steps one frame may run; the time beyond them is dropped.",
             .min = 1,
             .max = MAX_FIXED_STEPS},
            {.path = "vsyncEnabled",
             .doc = "Present with FIFO, waiting for the display's vertical blank each frame; off presents with Mailbox "
                    "(Immediate where it is not offered) and the frame limit applies. The swapchain is rebuilt on a "
                    "change; a headless run never waits, and a bounded run (RunEngine, spite run) starts with it off."},
        };
    } // namespace

    std::span<const ConfigFieldMeta> windowConfigFields() { return WINDOW; }
    std::span<const ConfigFieldMeta> graphicsConfigFields() { return GRAPHICS; }
    std::span<const ConfigFieldMeta> inputConfigFields() { return INPUT; }
    std::span<const ConfigFieldMeta> timingConfigFields() { return TIMING; }
} // namespace spite
