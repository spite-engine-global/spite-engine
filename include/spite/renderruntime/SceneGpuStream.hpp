#pragma once

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "base/Handle.hpp"
#include "ecs/EcsTypes.hpp"
#include "rendering/GpuDataTypes.hpp"
#include "rendering/GpuSharedTypes.hpp"
#include "rendering/RenderResourceHandles.hpp"
#include "renderruntime/LightShadows.hpp"
#include "renderruntime/RenderRuntimeExport.hpp"

namespace spite
{
    class World;
    class IRenderResourceManager;
    class ServiceLocator;
    struct CameraMatricesData;
    struct RenderViews;
    struct SceneGpuStatsSingleton;
    struct ShadowCascadeCountsSingleton;
    struct SunShadows;
    struct SkinPaletteBlockOwner;
    struct SubmeshSkin;

    // Stable object-buffer slots of renderables (GpuInstanceSlotComponent),
    // shared by every world the render runtime draws; each slot records its
    // owner's scene. Each slot carries at most one scene update record per
    // frame: a slot cleared this frame is reused this frame only by
    // overwriting its clear record, otherwise from the next frame on.
    class SPITE_RENDER_RUNTIME_API InstanceSlots
    {
    public:
        // acquire's record for a slot no record of this frame names yet: append one
        static constexpr u32 NEW_RECORD = ~0u;

        explicit InstanceSlots(u32 capacity) :
            m_owners(capacity), m_grantEpochs(capacity), m_meshletCounts(capacity), m_spawnPending(capacity),
            m_capacity(capacity)
        {
        }

        // false when every slot is taken; record is the index of the frame's clear record the grant must overwrite,
        // or NEW_RECORD
        bool acquire(u32 scene, Entity owner, u32& slot, u32& record);
        // Between frames: a slot the owner is drawn through from the next frame, whose upload writes its spawn record
        // (spawnPending until spawned); a released slot is handed over in place, its clear never written. False when
        // every slot is taken.
        bool grantForNextFrame(u32 scene, Entity owner, u32& slot);
        [[nodiscard]] bool spawnPending(const u32 slot) const { return m_spawnPending[slot] != 0; }
        void spawned(const u32 slot) { m_spawnPending[slot] = 0; }
        [[nodiscard]] u32 capacity() const { return m_capacity; }
        // more slots, between frames
        void grow(u32 capacity);
        [[nodiscard]] bool isOwnedBy(u32 slot, u32 scene, Entity owner) const
        {
            return m_owners[slot].entity == owner && m_owners[slot].scene == scene;
        }
        // the slot's owner in the scene (a picked object id); undefined when free, out of range or another scene's
        [[nodiscard]] Entity ownerIn(const u32 slot, const u32 scene) const
        {
            return slot < m_capacity && m_owners[slot].scene == scene ? m_owners[slot].entity : Entity::undefined();
        }
        // counts frames (endFrame); an owner granted its slot in epoch e is drawn through it from that frame on
        [[nodiscard]] u64 epoch() const { return m_epoch; }
        // the epoch the slot's current owner was granted it in
        [[nodiscard]] u64 grantEpoch(const u32 slot) const { return m_grantEpochs[slot]; }
        // records the slot's meshlet count, returning the change to the scene
        // total for addMeshletTotal; safe across threads for distinct slots
        [[nodiscard]] std::int64_t exchangeMeshlets(u32 slot, u32 meshletCount)
        {
            const std::int64_t delta = std::int64_t{meshletCount} - std::int64_t{m_meshletCounts[slot]};
            m_meshletCounts[slot] = meshletCount;
            return delta;
        }
        void addMeshletTotal(std::int64_t delta)
        {
            m_meshletTotal.fetch_add(static_cast<u64>(delta), std::memory_order_relaxed);
        }
        // the next upload writes the slot's clear record
        void release(u32 slot);
        // the caller already wrote the slot's clear record, at index record of the frame's records
        void retire(u32 slot, u32 record);
        // every slot of a scene that leaves the runtime or is suspended
        void releaseScene(u32 scene);
        // released slots awaiting their clear record; retireReleased once written, released()[i] at firstRecord + i
        std::span<const u32> released() const { return m_released; }
        void retireReleased(u32 firstRecord);
        // this frame's retired slots become free
        void endFrame();

        // The last update record written for the slot (zero: none or cleared), the shadow tracker's old state; safe
        // across threads for distinct slots. Grown with the high-water mark.
        [[nodiscard]] SceneObjectUpdate& lastRecord(const u32 slot) { return m_lastRecords[slot]; }

        // How many consecutive frames, up to the epoch it names, the slot's record changed (the probe volumes' dynamic
        // rule); beside the record because the record has no room for it. Same threading as lastRecord.
        // `listed`: the slot is in dynamicSlots.
        struct MoveStreak
        {
            u64 epoch = 0;
            u32 frames = 0;
            bool listed = false;
        };
        [[nodiscard]] MoveStreak& moveStreak(const u32 slot) { return m_moveStreaks[slot]; }
        // The slots whose streak reached the dynamic length and that have not been seen stopping, so that the frame
        // after the last record of one can mark where it stopped. Touched by the upload thread alone.
        [[nodiscard]] std::vector<u32>& dynamicSlots() { return m_dynamicSlots; }

        u32 highWater() const { return m_highWater; }
        u32 occupied() const { return m_occupied; }
        u64 meshletTotal() const { return m_meshletTotal.load(std::memory_order_relaxed); }

    private:
        void vacate(u32 slot);
        void take(u32 scene, Entity owner, u32 slot, u64 grantEpoch);

        struct Owner
        {
            u32 scene = 0;
            Entity entity = Entity::undefined();
        };

        struct Retired
        {
            u32 slot = 0;
            u32 record = 0; // its clear record this frame
        };

        std::vector<u32> m_free;
        std::vector<u32> m_released;
        std::vector<Retired> m_retired;
        std::vector<Owner> m_owners;
        std::vector<u64> m_grantEpochs;
        std::vector<u32> m_meshletCounts;
        std::vector<u8> m_spawnPending;
        std::vector<SceneObjectUpdate> m_lastRecords;
        std::vector<MoveStreak> m_moveStreaks;
        std::vector<u32> m_dynamicSlots;
        std::atomic<u64> m_meshletTotal = 0;
        u64 m_epoch = 0;
        u32 m_capacity = 0;
        u32 m_highWater = 0;
        u32 m_occupied = 0;
    };

    // The scene-to-GPU streaming state every world shares: the instance slots
    // and the frame's update-record count, which each world's upload appends
    // to. The published object/meshlet counts live in the rendering module's
    // SceneGpuStatsSingleton — the single stats channel.
    class SPITE_RENDER_RUNTIME_API SceneGpuStream
    {
    public:
        struct LightLimits
        {
            u32 pointLights = 0;
            u32 directionalLights = 0;
            u32 spotLights = 0;
            u32 areaLights = 0;
        };

        SceneGpuStream(LightLimits limits, u32 instanceCapacity) : m_limits(limits), m_slots(instanceCapacity) {}

        const LightLimits& lightLimits() const { return m_limits; }
        const InstanceSlots& instanceSlots() const { return m_slots; }

        // --- upload-system surface (module-internal) ---
        InstanceSlots& instanceSlots() { return m_slots; }
        // starts a frame's uploads: last frame's retired slots free and the
        // frame's counts restart; the object buffers grew first (growObjectCapacity)
        void beginFrame();
        // The frame records no graph (RenderingManager::skippedFrame): its object step consumes nothing (releases,
        // slot candidates and grants wait) and the next recorded frame rewrites every slotted row, as the changes of
        // the held frames are no longer visible to change detection.
        void holdFrame() { m_frameHeld = true; }
        [[nodiscard]] bool frameHeld() const { return m_frameHeld; }
        [[nodiscard]] bool rewritesObjects() const { return m_rewriteObjects; }
        void requireFadeShadows() { m_fadeShadows = true; }
        [[nodiscard]] bool fadeShadowsRequired() const { return m_fadeShadows; }
        u32& frameUpdateCount() { return m_frameUpdateCount; }
        // move records the frame's worlds have written so far, packed down from the end of the record buffer
        u32& frameMoveCount() { return m_frameMoveCount; }
        // renderables left without a slot this frame, summed over the worlds
        u32& frameOverflowed() { return m_frameOverflowed; }
        u32 overflowedLastFrame() const { return m_overflowedLastFrame; }
        // object capacity growths so far
        u32& growths() { return m_growths; }
        u32 growths() const { return m_growths; }
        // frames of overflow until a growth the GPU budget refused is tried again; 0: not refused
        u32& growthBackoff() { return m_growthBackoff; }
        [[nodiscard]] bool growthDenied() const { return m_growthBackoff > 0; }
        // the overflow that cannot grow away was reported; cleared by a frame without overflow
        bool& overflowWarned() { return m_overflowWarned; }

        // --- skinning ---
        // palette delta records the frame's worlds have written so far, and how many the update buffer holds (it
        // grows between frames to skinUpdateWanted: the most a frame wanted and could not fit)
        u32& skinUpdateCount() { return m_skinUpdateCount; }
        u32& skinUpdateCapacity() { return m_skinUpdateCapacity; }
        u32& skinUpdateWanted() { return m_skinUpdateWanted; }
        // joint 0 of the palette heap was taken (paletteBase 0 means no palette)
        bool& paletteReserved() { return m_paletteReserved; }

    private:
        LightLimits m_limits{};
        InstanceSlots m_slots;
        u32 m_frameUpdateCount = 0;
        u32 m_frameMoveCount = 0;
        u32 m_frameOverflowed = 0;
        u32 m_overflowedLastFrame = 0;
        u32 m_growths = 0;
        u32 m_growthBackoff = 0;
        bool m_overflowWarned = false;
        u32 m_skinUpdateCount = 0;
        u32 m_skinUpdateCapacity = INITIAL_SKIN_UPDATE_RECORDS;
        u32 m_skinUpdateWanted = 0;
        bool m_paletteReserved = false;
        bool m_frameHeld = false;
        bool m_fadeShadows = false;
        bool m_rewriteObjects = false;
    };

    // One world the render runtime draws (RenderRuntime::attachWorld), on that
    // world's services: its scene id, the shared state its upload writes, and
    // its own slot bookkeeping and per-frame dynamic light buffers.
    struct SPITE_RENDER_RUNTIME_API WorldGpuScene
    {
        struct FrameLightBuffers
        {
            BufferHandle pointLightsBuffer{};
            BufferHandle directionalLightsBuffer{};
            BufferHandle spotLightsBuffer{};
            // allocated by the first area light, never before
            BufferHandle areaLightsBuffer{};

            u32 pointLightCapacity = 0;
            u32 directionalLightCapacity = 0;
            u32 spotLightCapacity = 0;
            u32 areaLightCapacity = 0;

            // what this frame slot's buffers and light headers hold
            u64 lightGeneration = 0;
            u32 viewMask = 0;
        };

        // the light step's per-frame gather, kept so a frame does not allocate
        struct LightStaging
        {
            std::vector<PointLight> pointLights;
            std::vector<DirectionalLight> directionalLights;
            std::vector<SpotLight> spotLights;
            std::vector<AreaLight> areaLights;
            // the point and spot lights with castShadows among them, as the shadow maps' selection ranks them
            std::vector<ShadowLightCandidate> shadowLights;
            // the entities behind pointLights, spotLights and areaLights, row for row
            std::vector<u64> pointEntities;
            std::vector<u64> spotEntities;
            std::vector<u64> areaEntities;
        };

        u32 scene = 0;
        // the host's handle: it may destroy the world before the runtime
        const World* world = nullptr;
        // the render runtime's own world services, which hold the per-view singletons (environment, probe volume,
        // light clusters, temporal, ...): the upload reads them here, whatever locator the host parented this
        // world's services on
        ServiceLocator* runtime = nullptr;
        SceneGpuStream* stream = nullptr;
        RenderViews* views = nullptr;
        SceneGpuStatsSingleton* stats = nullptr;
        // the swapchain view's matrices for CPU consumers
        CameraMatricesData* primaryCamera = nullptr;
        // every view's cascades; this scene fits those of its views
        SunShadows* shadows = nullptr;
        // the pass modules' copy of every view's cascade count
        ShadowCascadeCountsSingleton* shadowCounts = nullptr;
        // the scene's sun , chosen at each light gather: undefined for none
        Entity sun = Entity::undefined();
        Vec3 sunDirection{};
        // the scene's sun light is tinted by the atmosphere's transmittance: the active environment is
        // procedural and there is a sun. The environment step sets it; a change re-uploads the lights.
        bool skySun = false;

        // slots granted last frame, verified next frame: an entity destroyed
        // before its component add committed never fires the removal that
        // frees the slot
        std::vector<std::pair<Entity, u32>> pendingSlotGrants;
        // entities that may need a slot assigned in place: created with the
        // slot component, or given it, meshlets or world bounds since
        std::vector<Entity> slotCandidates;
        // the material edits (MaterialClassCounts::shadowEdits and lightEdits) this world's trackers have applied
        u64 materialEditsSeen = 0;
        u64 lightEditsSeen = 0;
        // entities that went inactive since the last upload; it frees the slot of each still inactive
        std::vector<Entity> deactivated;
        std::vector<FrameLightBuffers> frameLightBuffers;
        LightStaging lightStaging;
        // bumped when a light changes; a frame slot behind it re-uploads
        u64 lightGeneration = 1;
        // point, directional, spot, area lights matched at the last check: a drop is a removal
        std::array<u32, 4> lightCounts{};
        bool warnedPointOverflow = false;
        bool warnedDirectionalOverflow = false;
        bool warnedSpotOverflow = false;
        bool warnedAreaOverflow = false;
        // RenderRuntime::suspendScene: the scene holds no slots and its upload writes no objects
        bool suspended = false;
        // the suspension's first upload reset the renderables' slot components
        bool slotsReset = false;
        // renderables RenderRuntime::resumeScene granted slots: the next upload writes their spawn records
        std::vector<Entity> respawns;
        // the change tick at the BoundingBoxes system's last run in this world
        u64 boundsSeenTick = 0;
        // the change tick at the object upload's last changed-row walk
        u64 objectsSeenTick = 0;
        // the skin step: the change tick of its last walk of the skin roots, the roots whose palettes
        // did not all fit the update buffer last frame, the heap-full warning
        u64 skinSeenTick = 0;
        // the palette heap growths this world's skin step has seen published
        // (GlobalResourceManager::heapSwapsPublished)
        u64 paletteSwapsSeen = 0;
        std::vector<Entity> skinDeferred;
        // renderables the palette heap refused a block of `count` joints, by entity id; they wait for
        // GpuAllocationManager::heapReleases to move past `skinReleasesSeen` (empty in the common case)
        struct SkinRefused
        {
            Entity entity;
            u32 count = 0;
        };
        std::vector<SkinRefused> skinRefused;
        u64 skinReleasesSeen = 0;
        // one warning per refusal episode: cleared when nothing waits any more
        bool warnedSkinHeap = false;
        // the palette blocks of shared poses, by the pose's id and the submesh skin: one block, written
        // once, for every renderable of that skin on an animator showing the pose; an entry whose owner or skin is
        // gone is stale. A heap growth clears it: the blocks are written anew
        struct SharedPaletteBlock
        {
            std::weak_ptr<SkinPaletteBlockOwner> owner;
            std::weak_ptr<const SubmeshSkin> skin;
            u32 base = 0;
        };
        std::map<std::pair<u32, const SubmeshSkin*>, SharedPaletteBlock> sharedPalettes;
        sizet sharedPalettesPruned = 0; // the size after the last sweep of stale entries
        Handle subscriptions[8]{};

        // Subscribes the slot lifecycle hooks on the world.
        void subscribe();
        // Unsubscribes, frees the scene's slots and releases the light buffers
        // through the resource manager.
        void shutdown(IRenderResourceManager& resourceManager);
    };
} // namespace spite
