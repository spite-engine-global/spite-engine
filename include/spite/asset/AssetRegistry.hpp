#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <span>
#include <unordered_map>

#include "asset/AssetExport.hpp"
#include "asset/AssetHandle.hpp"
#include "base/Assert.hpp"

namespace spite
{
    class SPITE_ASSET_API AssetRegistry
    {
    public:
        // context is passed back to the deleter; GPU-aware destroyers use it to
        // reach their owner without the record holding a back-reference.
        using AssetDeleter = void (*)(void* data, void* context);

        // Invoked instead of inline destruction when a ref count reaches zero.
        // Lets an owner defer the destroy until pending GPU work completes.
        using ReleaseSink = std::function<void(AssetHandle handle, void* data, AssetDeleter deleter, void* context)>;

    private:
        struct AssetRecord
        {
            void* data = nullptr;
            AssetDeleter deleter = nullptr;
            void* context = nullptr;
            u32 refCount = 0;
        };

        std::unordered_map<AssetHandle, AssetRecord, AssetHandle::Hash> m_registry;
        ReleaseSink m_releaseSink;
        mutable std::mutex m_mutex;
        std::atomic<u64> m_generation = 1;

        void destroyRecord(const AssetRecord& record) const;

    public:
        AssetRegistry() = default;
        ~AssetRegistry();

        AssetRegistry(const AssetRegistry&) = delete;
        AssetRegistry(AssetRegistry&&) = delete;
        AssetRegistry& operator=(const AssetRegistry&) = delete;
        AssetRegistry& operator=(AssetRegistry&&) = delete;

        // Gets the runtime pointer for a handle. Returns nullptr if not loaded.
        void* getAssetData(AssetHandle handle) const;

        // Check if an asset is registered (regardless of ref count)
        bool isAssetRegistered(AssetHandle handle) const;

        // Get current reference count for an asset (0 if not registered)
        u32 getRefCount(AssetHandle handle) const;

        // Atomic find-or-insert: registers the asset with a 0 reference count,
        // or reports the existing registration. Concurrent creators of the same
        // handle get exactly one winner; the loser destroys its own data.
        bool tryRegister(AssetHandle handle, void* data, AssetDeleter deleter, void* context = nullptr);

        // Swaps a registered asset's data in place; its reference count and holders stay. Returns the old data for
        // the caller to retire (the deleter and context given stay those of the new data), or nullptr and no change
        // when the handle is not registered.
        void* replaceAsset(AssetHandle handle, void* data, AssetDeleter deleter, void* context = nullptr);

        struct Replacement
        {
            AssetHandle handle;
            void* data = nullptr;
            AssetDeleter deleter = nullptr;
            void* context = nullptr;
        };
        // Validate every distinct registered handle before replacing any. The caller supplies equally sized output
        // storage, receives every old pointer, and owns its retirement. Reference counts are preserved.
        bool replaceAssets(std::span<const Replacement> replacements, std::span<void*> previous);
        // Installs the refcount-zero sink. Without one, records are destroyed
        // inline on the releasing thread (the default, for tests and tools).
        void setReleaseSink(ReleaseSink sink);

        // Call when a component/system begins using an asset
        void acquireAsset(AssetHandle handle);
        bool tryAcquireAsset(AssetHandle handle);

        // Call when a component/system stops using an asset. If the ref count
        // drops to 0 the record is destroyed inline, or handed to the release
        // sink when one is installed.
        void releaseAsset(AssetHandle handle);

        // Force destroy an asset regardless of ref count
        void forceDestroy(AssetHandle handle);

        void cleanup();

        sizet getAssetCount() const;
        u64 getGeneration() const { return m_generation.load(std::memory_order_relaxed); }
    };

    // RAII wrapper for automatic acquire/release
    class AssetRef
    {
    public:
        AssetRef() = default;

        AssetRef(AssetRegistry* registry, AssetHandle handle) : m_handle(handle), m_registry(registry)
        {
            SASSERT(m_handle.isValid());
            if (registry)
                registry->acquireAsset(m_handle);
        }

        AssetRef(AssetHandle handle) : m_handle(handle), m_registry(nullptr) { SASSERT(m_handle.isValid()); }

        ~AssetRef() { release(); }

        AssetRef(const AssetRef& other) : m_handle(other.m_handle), m_registry(other.m_registry)
        {
            if (m_registry)
                m_registry->acquireAsset(m_handle);
        }

        AssetRef(AssetRef&& other) noexcept : m_handle(other.m_handle), m_registry(other.m_registry)
        {
            other.m_handle = {};
            other.m_registry = nullptr;
        }

        AssetRef& operator=(const AssetRef& other)
        {
            if (this != &other)
            {
                release();
                m_handle = other.m_handle;
                m_registry = other.m_registry;
                SASSERT(m_handle.isValid() && m_registry);
                m_registry->acquireAsset(m_handle);
            }
            return *this;
        }

        AssetRef& operator=(AssetRef&& other) noexcept
        {
            if (this != &other)
            {
                release();
                m_handle = other.m_handle;
                m_registry = other.m_registry;
                other.m_handle = {};
            }
            return *this;
        }

        void setHandle(AssetHandle handle)
        {
            if (m_handle.isValid() && m_registry)
            {
                m_registry->releaseAsset(m_handle);
                m_handle = {};
            }
            m_handle = handle;
        }

        void release()
        {
            if (m_handle.isValid() && m_registry)
            {
                m_registry->releaseAsset(m_handle);
                m_handle = {};
                m_registry = nullptr;
            }
        }

        bool isValid() const { return m_handle.isValid(); }
        AssetHandle handle() const { return m_handle; }

        template <typename T>
        T* get() const
        {
            SASSERT(m_handle.isValid() && m_registry);
            return static_cast<T*>(m_registry->getAssetData(m_handle));
        }

    private:
        AssetHandle m_handle;
        AssetRegistry* m_registry = nullptr;
    };
} // namespace spite
