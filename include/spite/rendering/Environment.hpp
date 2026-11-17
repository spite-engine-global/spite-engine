#pragma once

#include <cstring>
#include <vector>

#include "base/InternedString.hpp"
#include "base/Schema.hpp"
#include "rendering/GpuSharedTypes.hpp"

// The environment of a render view: the scene's active EnvironmentComponent resolved to the record the GPU
// reads. The render runtime's upload system writes it, the pass modules and the host's source resolver read it.

namespace spite
{
    // What the background of a view is made of. The values are ENVIRONMENT_MODE_* in shared/bindless.slang.
    enum class EnvironmentMode : u32
    {
        None,
        Color,
        Gradient,
        Procedural,
        Hdri
    };
    SPITE_ENUM_NAMES(EnvironmentMode, None, Color, Gradient, Procedural, Hdri)

    static_assert(static_cast<u32>(EnvironmentMode::None) == ENVIRONMENT_MODE_NONE);
    static_assert(static_cast<u32>(EnvironmentMode::Color) == ENVIRONMENT_MODE_COLOR);
    static_assert(static_cast<u32>(EnvironmentMode::Gradient) == ENVIRONMENT_MODE_GRADIENT);
    static_assert(static_cast<u32>(EnvironmentMode::Procedural) == ENVIRONMENT_MODE_PROCEDURAL);
    static_assert(static_cast<u32>(EnvironmentMode::Hdri) == ENVIRONMENT_MODE_HDRI);

    // What the bake pass remembers of the cube it last wrote.
    struct EnvironmentBakeMemory
    {
        bool baked = false; // the cube holds a bake of the source and rotation below, or of the procedural key
        u64 source = 0; // EnvironmentView::sourceGeneration of it
        float rotation = 0.f;
        bool lutBuilt = false; // the BRDF lookup table holds its integration (it depends on nothing in the scene)
        u32 mode = 0; // the mode (ENVIRONMENT_MODE_*) of that bake: a cube holds one mode's picture at a time
        EnvironmentData procedural{}; // environmentProceduralKey of the record the procedural bake drew
        bool atmosphereBuilt = false; // the atmosphere's multiple-scattering table holds the albedo below
        Vec3 atmosphereAlbedo{0.f};
    };

    struct EnvironmentView
    {
        // the active environment as of this frame (all zero: none), the copy the GPU holds, and the entity it came from
        EnvironmentData record{};
        EnvironmentData published{};
        u64 entity = 0; // Entity::id() of the active EnvironmentComponent, 0 when the scene has none
        InternedString hdri{}; // the environment asset the record names (hdri mode only)
        // moves with every change of the record; a frame slot's buffer holds the one of its last write
        u64 generation = 0;
        std::vector<u64> slotGenerations;
        u64 uploads = 0; // buffer writes since the start

        // The environment cube: the bindless index of the equirect texture the hdri names, valid once its
        // asset is ready (sourceReady; index 0 is a slot like any other), written by the host's resolver;
        // sourceGeneration moves whenever the source changes.
        u32 sourceTexture = 0;
        bool sourceReady = false;
        u64 sourceGeneration = 0;
        // The graph's shape for this view: which of the environment's resources and passes the last compile declared
        // (ENVIRONMENT_SHAPE_*), and the shape a recompile was last requested for.
        u32 compiledShape = 0;
        u32 requestedShape = 0;
        u64 bakes = 0; // cube bakes recorded since the start
        u64 shUpdates = 0; // diffuse SH reductions recorded since the start (with every bake)
        u64 specularBakes = 0; // prefilters of the specular cube recorded since the start (with every bake)
        u64 lutBuilds = 0; // BRDF lookup table builds recorded since the start (one per compile with the cube)
        u64 atmosphereBuilds = 0; // atmosphere multiple-scattering table builds recorded since the start
        // what the view's last compile's bake pass wrote (reset by the compile: a new cube holds nothing); the Sky
        // pass, declared after Light, reads it
        EnvironmentBakeMemory bakeMemory{};
    };

    struct EnvironmentSingleton
    {
        std::vector<EnvironmentView> views;
    };

    // The graph declares the sky pass for the modes that draw one and the cube with its bake for the hdri and
    // procedural modes: a view without them holds no cube and records nothing. A change of shape recompiles the graph.
    inline constexpr u32 ENVIRONMENT_SHAPE_SKY = 1;
    inline constexpr u32 ENVIRONMENT_SHAPE_CUBE = 2;

    inline u32 environmentShape(const EnvironmentData& record)
    {
        switch (record.mode)
        {
        case ENVIRONMENT_MODE_COLOR:
        case ENVIRONMENT_MODE_GRADIENT:
            return ENVIRONMENT_SHAPE_SKY;
        case ENVIRONMENT_MODE_PROCEDURAL:
        case ENVIRONMENT_MODE_HDRI:
            return ENVIRONMENT_SHAPE_SKY | ENVIRONMENT_SHAPE_CUBE;
        default:
            return 0;
        }
    }

    // The baked cube's faces, square, with a full mip chain
    inline constexpr u32 ENVIRONMENT_CUBE_EXTENT = 512;
    // The mip of that cube the diffuse SH reduction reads (64 squared faces): the term has no use for more detail
    inline constexpr u32 ENVIRONMENT_SH_SOURCE_LEVEL = 3;
    // GGX importance samples per prefiltered texel and per lookup table texel
    inline constexpr u32 ENVIRONMENT_SPECULAR_SAMPLES = 256;
    inline constexpr u32 ENVIRONMENT_LUT_SAMPLES = 512;

    inline EnvironmentMode environmentMode(const EnvironmentData& record)
    {
        return static_cast<EnvironmentMode>(record.mode);
    }

    // The part of a procedural record the cube is drawn from: the colours, the sun, its irradiance and the disc's size.
    // Tint, intensity, ambient intensity, rotation and affectsLighting are not in it: the sky and Light apply them as
    // they read the cube.
    inline EnvironmentData environmentProceduralKey(const EnvironmentData& record)
    {
        EnvironmentData key{};
        key.mode = record.mode;
        key.zenith = record.zenith;
        key.horizon = record.horizon;
        key.ground = record.ground;
        key.parameters.z = record.parameters.z;
        key.sun = record.sun;
        key.sunIrradiance = record.sunIrradiance;
        return key;
    }

    // Whether the view's cube has what it is drawn from: the source loaded for the hdri mode, nothing for the
    // procedural mode (the record is all it needs).
    inline bool environmentCubeReady(const EnvironmentView& view)
    {
        switch (environmentMode(view.record))
        {
        case EnvironmentMode::Procedural:
            return true;
        case EnvironmentMode::Hdri:
            return view.sourceReady;
        default:
            return false;
        }
    }

    // Whether the view's bake records this frame: its graph has the cube, its environment is in a mode that draws one
    // (the hdri with the source loaded, or the procedural) and the cube does not hold a bake of what it would draw now:
    // the same mode and, for the hdri, source and rotation, for the procedural the same key. Nothing else moves it: not
    // the intensity, tint or any other field of the record, which the sky applies as it draws. The sun is in the
    // record, so a sun that moved is a new key.
    inline bool environmentBakeRuns(const EnvironmentView& view, const EnvironmentBakeMemory& memory)
    {
        if ((view.compiledShape & ENVIRONMENT_SHAPE_CUBE) == 0 || !environmentCubeReady(view))
        {
            return false;
        }
        if (!memory.baked || memory.mode != view.record.mode)
        {
            return true;
        }
        if (environmentMode(view.record) == EnvironmentMode::Procedural)
        {
            const EnvironmentData key = environmentProceduralKey(view.record);
            return std::memcmp(&key, &memory.procedural, sizeof(EnvironmentData)) != 0;
        }
        return memory.source != view.sourceGeneration || memory.rotation != view.record.parameters.x;
    }

    // The view's graph was compiled for its environment: a compile makes a new cube (and a new table), so whatever the
    // memory said the cube held is forgotten.
    inline void environmentBakeCompiled(EnvironmentView& view)
    {
        view.compiledShape = environmentShape(view.record);
        view.bakeMemory = {};
    }

    // Whether the view's BRDF table build records this frame: its graph has the cube (the table's home) and the
    // table is not built yet. A recompile makes a new table; nothing else does.
    inline bool environmentLutRuns(const EnvironmentView& view, const EnvironmentBakeMemory& memory)
    {
        return (view.compiledShape & ENVIRONMENT_SHAPE_CUBE) != 0 && !memory.lutBuilt;
    }

    // Whether the view's atmosphere table build records this frame: its graph has the cube (the table's
    // home) and the table is not built yet, or the procedural mode's ground albedo, which the table's ground bounce
    // reads, is not the one it holds. A recompile makes a new table; nothing else does, in particular not the sun.
    inline bool environmentAtmosphereRuns(const EnvironmentView& view, const EnvironmentBakeMemory& memory)
    {
        if ((view.compiledShape & ENVIRONMENT_SHAPE_CUBE) == 0)
        {
            return false;
        }
        return !memory.atmosphereBuilt ||
            (environmentMode(view.record) == EnvironmentMode::Procedural &&
             Vec3(view.record.ground) != memory.atmosphereAlbedo);
    }

    // Whether the view's Light pass shades its ambient term from the baked environment this frame: the environment is
    // in the hdri or procedural mode with the graph that has the cube (a mode change waits for its recompile), lights
    // the scene, and the bake of that mode and the table have run (the passes record before Light in the same frame).
    // Inspection views show the G-buffer, not a lit scene. Otherwise Light keeps the constant ambient and reads no cube
    // slot.
    inline bool environmentLights(const EnvironmentView& view, const EnvironmentBakeMemory& memory,
                                  const bool inspecting)
    {
        const EnvironmentMode mode = environmentMode(view.record);
        return !inspecting && (mode == EnvironmentMode::Hdri || mode == EnvironmentMode::Procedural) &&
            view.record.parameters.w > 0.5f && view.compiledShape == environmentShape(view.record) && memory.baked &&
            memory.mode == view.record.mode && memory.lutBuilt;
    }

    // Whether the view's Sky pass records this frame: its graph has the pass for the environment the view shows now (a
    // mode change waits for its recompile), the view is not an inspection view (those show the G-buffer, not a lit
    // scene) and, for the cube, the source is loaded and the cube holds a bake or is baked this frame (the bake runs
    // first).
    inline bool environmentSkyRuns(const EnvironmentView& view, const EnvironmentBakeMemory& memory,
                                   const bool inspecting)
    {
        const u32 shape = environmentShape(view.record);
        if ((shape & ENVIRONMENT_SHAPE_SKY) == 0 || view.compiledShape != shape || inspecting)
        {
            return false;
        }
        return (shape & ENVIRONMENT_SHAPE_CUBE) == 0 ||
            (environmentCubeReady(view) && (memory.baked || environmentBakeRuns(view, memory)));
    }
} // namespace spite
