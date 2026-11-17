#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "asset/AssetExport.hpp"
#include "asset/AssetHandle.hpp"
#include "asset/AssetRegistry.hpp"
#include "base/Schema.hpp"
#include "rendering/GpuBudget.hpp"

namespace spite
{
    // CPU-known GPU footprint of one asset (bake-manifest counts x engine
    // element sizes for meshes; estimated texel bytes for textures).
    struct AssetMemStats
    {
        sizet bytes = 0;
        SPITE_OBSERVED_SCHEMA(AssetMemStats, bytes)
    };

    // Aggregate policy telemetry (perf-gate inputs).
    struct ResidencyStats
    {
        u32 trackedAssets = 0;
        u32 pending = 0;
        u32 resident = 0;
        u32 evicting = 0;
        u64 evictions = 0; // destroys executed by the policy
        u64 prefetchAdmitted = 0; // loads kicked by budget admission
        u32 prefetchQueued = 0; // demands waiting for budget
        u64 resurrected = 0; // grace-window resurrections (no reallocation)
        u64 overflowReports = 0;
        SPITE_OBSERVED_SCHEMA(ResidencyStats, trackedAssets, pending, resident, evicting, evictions, prefetchAdmitted,
                              prefetchQueued, resurrected, overflowReports)
    };

    // One tracked asset as a table row; state is pending, resident or evicting.
    struct AssetResidencyRow
    {
        std::string name;
        std::string type;
        u32 subAsset = 0;
        std::string state;
        u32 pins = 0;
        u64 bytes = 0;
        u64 residentAtFrame = 0;
        SPITE_OBSERVED_SCHEMA(AssetResidencyRow, name, type, subAsset, state, pins, bytes, residentAtFrame)
    };

    // Policy tier of the streaming architecture (gpu-streaming spec): owns
    // WHEN asset GPU data is resident, never HOW it is created or freed.
    //
    // Per-asset state machine, transitions only inside update():
    //
    //   (prefetch/pin) -> Pending -> Resident --release--> Evicting --> destroyed
    //                                       |                ^
    //                                       +----------------+
    //                          re-demand inside the grace window resurrects the
    //                          held record without reallocation (tryRegister)
    //
    // Registry demotion: the release sink hands refcount-zero records here
    // (onRecordReleased) instead of queueing destruction; the manager decides
    // when the destroy actually runs. Records for assets the manager never
    // tracked keep the old semantics (destroy immediately, uploads still
    // deferred by the backend).
    //
    // Threading: intent calls (pin/unpin/prefetch/reportOverflow), queries,
    // and onRecordReleased are safe from any thread; update() runs once per
    // frame on the render thread.
    class SPITE_ASSET_API ResidencyManager
    {
    public:
        // Mechanism hooks, implemented over GpuAssetFactory. No Vulkan types.
        struct Backend
        {
            virtual ~Backend() = default;
            // True when the asset's GPU uploads settled on the device.
            virtual bool isGpuReady(AssetHandle handle) const = 0;
            // Kick async load/GPU creation for a prefetched asset.
            virtual void requestLoad(AssetHandle handle) = 0;
            // Null-patch + deferred free of GPU data for a record handed over
            // at refcount zero (uploads-in-flight deferral is the backend's job).
            virtual void destroyRecord(AssetHandle handle, void* data, AssetRegistry::AssetDeleter deleter,
                                       void* context) = 0;
        };

        struct Config
        {
            // hysteresis: a released asset is never destroyed younger than this
            u32 minResidentFrames = 60;
            // Evicting grace window: re-demand inside it resurrects for free
            u32 evictGraceFrames = 120;
            // pressure: while budget headroom is below target, evicting records
            // older than minResidentFrames destroy immediately (skip the rest
            // of the grace), oldest first
            u64 evictHeadroomTargetBytes = 256ull << 20;
            u32 maxEvictionsPerFrame = 8;
        };

        ResidencyManager(AssetRegistry& registry, Backend& backend);
        ResidencyManager(AssetRegistry& registry, Backend& backend, const Config& config);

        ResidencyManager(const ResidencyManager&) = delete;
        ResidencyManager& operator=(const ResidencyManager&) = delete;

        // ---- demand (cheap, order-free, any thread) ----
        // Must be resident for the frame being built: forces admission when
        // unloaded, resurrects when evicting.
        void pin(AssetHandle handle);
        void unpin(AssetHandle handle);
        // Streaming demand: admitted inside update() while the budget allows.
        void prefetch(AssetHandle handle, u32 priority, sizet expectedBytes);
        // A pass clamped writes for this asset (growth decision is phase 4).
        void reportOverflow(AssetHandle handle);

        // ---- queries: what is true for the frame about to render ----
        bool resident(AssetHandle handle) const;
        AssetMemStats stats(AssetHandle handle) const;
        ResidencyStats residencyStats() const;
        // One row per tracked asset, by name (the assets table).
        [[nodiscard]] std::vector<AssetResidencyRow> rows() const;
        // Load references that are still pinned. Used when rebuilding the GPU registry.
        [[nodiscard]] std::vector<AssetHandle> pinnedAssets() const;
        // Exact CPU-known cost once the load parsed the payload (prefetch
        // admissions use the estimate passed to prefetch()).
        void setStats(AssetHandle handle, AssetMemStats stats);
        u64 overflowReports() const { return m_overflowReports; }

        // ---- registry demotion ----
        // The demoted release sink hands the record here; tracked assets enter
        // Evicting (grace), untracked ones destroy immediately (old behavior).
        void onRecordReleased(AssetHandle handle, void* data, AssetRegistry::AssetDeleter deleter, void* context);
        // Load-path hook: re-registers a held record the way a finished load does,
        // with the load reference (refcount 1) and one pin, so the caller's
        // release retires it; false when nothing is held or the asset is registered.
        bool resurrect(AssetHandle handle);
        // Destroys the record held in the grace window and forgets the asset (its bake changed, so the held
        // copy must not resurrect). Nothing when the asset is not held.
        void discardHeld(AssetHandle handle);
        // Frame boundary with workers stopped: release grace-window records before the GPU backend is replaced.
        void clearHeld();

        // ---- the one tick (render thread) ----
        void update(const GpuBudget& budget);

    private:
        enum class State : u8
        {
            Pending,
            Resident,
            Evicting,
        };

        struct Record
        {
            State state = State::Pending;
            u32 pins = 0;
            AssetMemStats stats{};
            u64 residentAtFrame = 0;
            u64 releasedAtFrame = 0;
            u64 evictAtFrame = 0;
            // record payload held during Evicting for resurrection
            void* data = nullptr;
            AssetRegistry::AssetDeleter deleter = nullptr;
            void* context = nullptr;
        };

        struct PrefetchDemand
        {
            AssetHandle handle;
            u32 priority = 0;
            sizet expectedBytes = 0;
        };

        AssetRegistry& m_registry;
        Backend& m_backend;
        Config m_config;
        u64 m_frame = 0;
        mutable std::mutex m_mutex;
        std::unordered_map<AssetHandle, Record, AssetHandle::Hash> m_records;
        std::vector<PrefetchDemand> m_prefetchQueue;
        std::atomic<u64> m_overflowReports{0};
        u64 m_evictions = 0;
        u64 m_prefetchAdmitted = 0;
        u64 m_resurrected = 0;
    };
} // namespace spite
