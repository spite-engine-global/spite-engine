#pragma once

#include <array>
#include <bitset>
#include <string>
#include <string_view>
#include <vector>

#include "application/Window.hpp"
#include "base/ServiceLocator.hpp"
#include "base/Schema.hpp"
#include "rendering/GpuDataTypes.hpp"
#include "rendering/IRenderer.hpp"
#include "rendering/IRenderResourceManager.hpp"
#include "rendering/LightClusters.hpp"
#include "rendering/LodSelection.hpp"
#include "rendering/NamedBufferRegistry.hpp"

namespace spite
{
    struct OceanProfileResources
    {
        std::vector<BufferHandle> buffers;
    };
    struct CameraMatricesData
    {
        Mat4 view = Mat4(1);
        Mat4 projection = Mat4(1);
        Vec3 pos{};
        Mat4 viewProjection{};
    };

    struct RenderGraphGenerationSingleton
    {
    private:
        u64 m_generation = 0;
        u64 m_sourceGeneration = 0;
        std::string m_source;

    public:
        u64& get() { return m_generation; }
        const u64& get() const { return m_generation; }
        // Requests a rebuild naming its requester (copied), which the rebuild log reports.
        void bump(std::string_view source)
        {
            m_sourceGeneration = ++m_generation;
            m_source = source;
        }
        // The requester of the latest generation; empty when an unnamed get()++ advanced it. Valid until the next bump.
        [[nodiscard]] std::string_view source() const
        {
            return m_sourceGeneration == m_generation ? std::string_view(m_source) : std::string_view{};
        }
    };

    // The primary camera matrices the pass modules sample (camPos, VP).
    // The render runtime's upload system writes it; pass modules read it.
    struct CameraMatricesSingleton
    {
    private:
        CameraMatricesData m_matrices{};

    public:
        CameraMatricesSingleton(CameraMatricesData matrices) : m_matrices(matrices) {}

        CameraMatricesSingleton() = default;

        CameraMatricesData& get() { return m_matrices; }
        const CameraMatricesData& get() const { return m_matrices; }

        // Per render view: the eye the view draws from, for per-view passes (get() is the primary view's).
        std::vector<Vec3> viewPositions;
        // Per render view: the way it looks, and whether its projection is orthographic (parallel rays: view-dependent
        // terms use -forward everywhere instead of the direction to the eye).
        struct ViewDirection
        {
            Vec3 forward{0.0f, 0.0f, -1.0f};
            bool orthographic = false;
        };
        std::vector<ViewDirection> viewDirections;
    };

    // Per-frame scene stats the pass modules consume for culling dispatches,
    // plus cumulative scene-upload telemetry written by the render runtime's
    // upload system.
    struct SceneGpuStatsSingleton
    {
    private:
        u32 m_cullableObjects = 0;
        u32 m_objectSlots = 0;
        u32 m_objectUpdates = 0;
        u32 m_objectMoves = 0;
        u32 m_overflowedObjects = 0;
        u32 m_objectCapacity = 0;
        u32 m_objectGrowths = 0;
        u32 m_meshlets = 0; // every resident cluster: level 0 and the coarser clusters of the LOD hierarchy
        u32 m_materialClasses = 0;
        u32 m_materialClassesBuilt = 0;
        double m_uploadTotalMs = 0.0;
        double m_lastUploadMs = 0.0;
        u64 m_uploadFrames = 0;
        u64 m_lightUploads = 0;
        u32 m_skinPaletteUpdates = 0;

    public:
        u32& cullableObjects() { return m_cullableObjects; }
        u32 cullableObjects() const { return m_cullableObjects; }
        // Instance-slot high-water mark: the range the cull dispatches over
        // (free slots inside it carry no meshlets).
        u32& objectSlots() { return m_objectSlots; }
        u32 objectSlots() const { return m_objectSlots; }
        // Scene update records written this frame for the scatter pass.
        u32& objectUpdates() { return m_objectUpdates; }
        u32 objectUpdates() const { return m_objectUpdates; }
        // Of those, the move records (SceneObjectMove), packed at the end of the record buffer: they end where
        // objectCapacity full records would.
        u32& objectMoves() { return m_objectMoves; }
        u32 objectMoves() const { return m_objectMoves; }
        // Renderables without an instance slot because the object buffer is full
        // (instance-capacity overflow; the object buffers' growth signal).
        u32& overflowedObjects() { return m_overflowedObjects; }
        u32 overflowedObjects() const { return m_overflowedObjects; }
        // Instance slots the object buffers hold now (cullableObjects of them are used), and how often they grew.
        u32& objectCapacity() { return m_objectCapacity; }
        u32 objectCapacity() const { return m_objectCapacity; }
        u32& objectGrowths() { return m_objectGrowths; }
        u32 objectGrowths() const { return m_objectGrowths; }
        u32& meshlets() { return m_meshlets; }
        u32 meshlets() const { return m_meshlets; }
        // The non-opaque material classes some resident material has and the cull routes (MaterialClassCounts::present,
        // read once a frame by the upload): bit 0 the mask class, bit 1 the blend class.
        // materialClassesBuilt is what the render graph was last asked to be built with: the upload sets it and
        // requests a rebuild when the live set differs, so a graph without a class declares no list, pass or draw for
        // it, and a scene with none costs nothing.
        u32& materialClasses() { return m_materialClasses; }
        u32 materialClasses() const { return m_materialClasses; }
        u32& materialClassesBuilt() { return m_materialClassesBuilt; }
        u32 materialClassesBuilt() const { return m_materialClassesBuilt; }
        double& uploadTotalMs() { return m_uploadTotalMs; }
        double uploadTotalMs() const { return m_uploadTotalMs; }
        double& lastUploadMs() { return m_lastUploadMs; }
        double lastUploadMs() const { return m_lastUploadMs; }
        u64& uploadFrames() { return m_uploadFrames; }
        u64 uploadFrames() const { return m_uploadFrames; }
        // frame slots whose light buffers and headers were rewritten: flat on a quiescent scene
        u64& lightUploads() { return m_lightUploads; }
        u64 lightUploads() const { return m_lightUploads; }
        // Palette joint records written this frame for the scatter (48 bytes of matrix each).
        u32& skinPaletteUpdates() { return m_skinPaletteUpdates; }
        u32 skinPaletteUpdates() const { return m_skinPaletteUpdates; }

        SPITE_OBSERVED_SCHEMA(SceneGpuStatsSingleton, m_cullableObjects, m_objectSlots, m_objectUpdates, m_objectMoves,
                              m_overflowedObjects, m_objectCapacity, m_objectGrowths, m_meshlets, m_materialClasses,
                              m_materialClassesBuilt, m_uploadTotalMs, m_lastUploadMs, m_uploadFrames, m_lightUploads,
                              m_skinPaletteUpdates)
    };

    // Per render view: the sun cascades the render runtime fitted this frame (0 without a sun or for an inactive
    // view). The scene upload writes it before Render; the shadow pass groups record that many cascades.
    struct ShadowCascadeCountsSingleton
    {
        std::vector<u32> counts;
        // Per view and cascade: the ShadowDirtyReason bits accumulated since the cascade last rendered. The scene
        // upload and the graph compile set them; the cascade's group renders exactly while one is set and its Stats
        // pass clears them, so a mark survives a frame without a render (an inactive view).
        std::vector<std::array<u32, MAX_SHADOW_CASCADES>> pending;
        // Point and spot light shadow maps, per view: the map slots the graph compiles (0 while no scene
        // light casts shadows), the bit mask of the slots a light holds this frame, and per slot the dirty reasons
        // pending as for a cascade. A slot no light holds never has one: it never renders.
        std::vector<u32> mapSlots;
        std::vector<std::bitset<MAX_LIGHT_SHADOW_MAPS>> mapOccupied;
        std::vector<std::array<u32, MAX_LIGHT_SHADOW_MAPS>> mapPending;

        // every per-view vector sized for the views at once: the passes index them all by view
        ShadowCascadeCountsSingleton() = default;
        explicit ShadowCascadeCountsSingleton(const u32 views) :
            counts(views), pending(views), mapSlots(views), mapOccupied(views), mapPending(views)
        {
        }
    };

    // Per render view: how the Light pass shows the frame (the viewMode command). NONE lights it; the others show one
    // input of it unlit (shaders/source/light.frag.slang DEBUG_*): the G-buffer, the linear depth, the visibility
    // buffer's ids, the sun cascade, the shadow mask, the material's occlusion and its emitted radiance, the lights
    // listed by the pixel's light cluster, the texture mip level the material resolve sampled (MIP: the resolve writes
    // it as a grey into the base colour target, 16 bytes to a level), the cluster LOD level of the pixel's meshlet, the
    // material class of the pixel's meshlet (opaque grey, mask green, blend orange).
    // A new mode goes at the end: the values are the shader's and the toolbar's.
    enum class ShadingDebug : u32
    {
        NONE = 0,
        ALBEDO = 1,
        NORMALS = 2,
        DEPTH = 3,
        ROUGHNESS = 4,
        METALLIC = 5,
        ENTITY_ID = 6,
        MESHLET_ID = 7,
        CASCADE = 8,
        SHADOW_MASK = 9,
        OCCLUSION = 10,
        EMISSIVE = 11,
        LIGHTS_PER_CLUSTER = 12,
        MIP = 13,
        LOD = 14,
        AO = 15,
        MATERIAL_CLASS = 16
    };

    struct ShadingDebugSingleton
    {
        std::vector<ShadingDebug> modes;
    };

    // Why a cascade renders , a bitmask per cascade.
    enum ShadowDirtyReason : u32
    {
        SHADOW_DIRTY_CAMERA = 1, // the cascade's fitted record changed
        SHADOW_DIRTY_LIGHT = 2, // the sun or its direction changed
        SHADOW_DIRTY_CASTERS = 4, // a scene update record reaches the cascade's caster volume
        SHADOW_DIRTY_GRAPH = 8, // a compile, a new cascade, a bias change
        SHADOW_DIRTY_FORCED = 16 // shadowInvalidate
    };

    // One cascade's results from the shadow passes . The pass group counts the frames it rendered; the two
    // GPU columns are read back a few frames late, so they describe the cascade's latest render that has landed.
    struct ShadowCascadeStats
    {
        u32 visibleMeshlets = 0;
        u32 overflowed = 0; // meshlets the cull dropped for lack of list capacity
        u64 measuredFrame = 0; // the render the two columns above describe (as frame); 0: none has landed yet
        u64 renderedFrames = 0;
        u64 lastRenderedFrame = 0; // as ShadowCascadeStatsSingleton::frame
    };

    struct ShadowCascadeStatsSingleton
    {
        u64 frame = 0; // bumped once per frame by the scene upload
        // The cull counts cost a copy pass and a readback per rendered cascade, so they are produced only while a
        // consumer wants them: the oracle pins them, a reader of the cascade rows asks for WANT_FRAMES frames. The
        // renders that follow an ask produce them; a cached cascade does not re-render for the ask.
        static constexpr u64 WANT_FRAMES = 32;
        u64 wantedUntil = 0;
        bool pinned = false;
        [[nodiscard]] bool wanted() const { return pinned || (wantedUntil != 0 && frame <= wantedUntil); }
        void want() { wantedUntil = frame + WANT_FRAMES; }
        void pin() { pinned = true; }
        std::vector<std::array<ShadowCascadeStats, MAX_SHADOW_CASCADES>> views;
        // the light shadow maps' render counts per view and slot (their cull counts are not read back)
        std::vector<std::array<ShadowCascadeStats, MAX_LIGHT_SHADOW_MAPS>> maps;
    };

    // One row of the clusters table: a view's froxel light grid, how often its build recorded, and what
    // the last build's stats readback measured (a few frames after it recorded; measuredBuild is the build they
    // describe, 0 until one lands).
    struct LightClusterRow
    {
        u32 view = 0;
        u32 scene = 0;
        bool orthographic = false;
        u32 gridX = 0;
        u32 gridY = 0;
        u32 gridZ = 0;
        u32 clusters = 0;
        u32 maxLightsPerCluster = 0;
        // the lights the view's scene uploaded last, by kind: the lists index the points, then the spots, then the
        // areas
        u32 pointLights = 0;
        u32 spotLights = 0;
        u32 areaLights = 0;
        u64 builds = 0;
        u64 measuredBuild = 0;
        u32 assigned = 0; // light entries stored over every cluster
        u32 maxLights = 0; // the most lights one cluster wanted, stored or not
        float meanLights = 0.0f; // assigned / clusters
        u32 overflowedClusters = 0; // clusters that wanted more than maxLightsPerCluster
        u32 droppedLights = 0; // entries left out for that

        SPITE_OBSERVED_SCHEMA(LightClusterRow, view, scene, orthographic, gridX, gridY, gridZ, clusters,
                              maxLightsPerCluster, pointLights, spotLights, areaLights, builds, measuredBuild, assigned,
                              maxLights, meanLights, overflowedClusters, droppedLights)
    };

    // Per render view: what its light clusters build from (the scene upload writes inputs every frame) and the build
    // passes' counts. The light cluster module records a view's build only when the inputs differ from the last
    // build's (LightClusterBuildGate).
    struct LightClustersSingleton
    {
        struct View
        {
            LightClusterInputs inputs;
            // the scene's light counts at its last light upload (not an input of the build: the key covers them)
            u32 pointLights = 0;
            u32 spotLights = 0;
            u32 areaLights = 0;
            u64 builds = 0;
            u64 measuredBuild = 0;
            LightClusterStats gpu{};
            // the grid the last recorded build used (count() 0 until one did): what the Light pass and pixelInfo
            // read the lists with
            ClusterGrid grid{0, 0, 0, 0};
        };
        std::vector<View> views;
        // debug reference: Light loops every point and spot light instead of reading the cluster lists (the
        // clustered shading is compared against it)
        bool bruteForce = false;

        [[nodiscard]] LightClusterRow row(const u32 view, const LightingConfig& lighting) const
        {
            const View& shown = views[view];
            const ClusterGrid grid{lighting.gridX, lighting.gridY, lighting.gridZ, lighting.maxLightsPerCluster};
            return {.view = view,
                    .scene = shown.inputs.scene,
                    .orthographic = shown.inputs.projection[3][3] > 0.5f,
                    .gridX = grid.x,
                    .gridY = grid.y,
                    .gridZ = grid.z,
                    .clusters = grid.count(),
                    .maxLightsPerCluster = grid.maxLights,
                    .pointLights = shown.pointLights,
                    .spotLights = shown.spotLights,
                    .areaLights = shown.areaLights,
                    .builds = shown.builds,
                    .measuredBuild = shown.measuredBuild,
                    .assigned = shown.gpu.assigned,
                    .maxLights = shown.gpu.maxWanted,
                    .meanLights = static_cast<float>(shown.gpu.assigned) / static_cast<float>(grid.count()),
                    .overflowedClusters = shown.gpu.overflowedClusters,
                    .droppedLights = shown.gpu.droppedLights};
        }
    };

    // The active renderer's frame index; headless stacks have no renderer and
    // read 0.
    inline u32 currentFrameIndex(ServiceLocator& services)
    {
        return services.contains<IRenderer>() ? services.read<IRenderer>()->getCurrentFrameIndex() : 0;
    }
} // namespace spite
