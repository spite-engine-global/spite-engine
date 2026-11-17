#pragma once

#include <algorithm>
#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "base/Assert.hpp"
#include "base/InternedString.hpp"
#include "base/TypeName.hpp"
#include "BaseExport.hpp"

namespace spite
{
    struct StructSchema;

    // Receives one row at a time; row points at an instance described by the
    // table's row schema and is valid only for the call.
    using TableRowSink = std::function<void(const void* row)>;

    // A service's tabular view: rows exist only while a query runs.
    struct ServiceTable
    {
        std::string_view name;
        const StructSchema* rowSchema = nullptr;
        std::function<void(const void* service, const TableRowSink& sink)> produce;
    };

    struct ServiceTableInfo
    {
        std::string_view name;
        const StructSchema* rowSchema = nullptr;
    };

    struct ServiceInfo
    {
        std::string_view name;
        u32 depth = 0; // 0: the queried locator, 1: its parent, ...
        std::vector<ServiceTableInfo> tables;
    };

    struct ServiceEntry
    {
        ServiceEntry(const std::string_view serviceName, std::shared_ptr<void> value,
                     const InternedString ownerModule) :
            name(serviceName), instance(std::move(value)), owner(ownerModule)
        {
        }

        std::string_view name;
        std::shared_ptr<void> instance;
        InternedString owner; // the registering module; undefined for the engine's own services
        std::vector<ServiceTable> tables; // guarded by the owning locator's mutex
        mutable std::shared_mutex mutex;
    };

    namespace detail
    {
        template <class T>
        concept hasServiceName = requires {
            { T::SPITE_SERVICE_NAME } -> std::convertible_to<const char*>;
        };

        // The stable name a service registers under: the explicit
        // SPITE_SERVICE_NAME member, otherwise derived from the registration type.
        template <class T>
        std::string_view serviceNameOf()
        {
            if constexpr (hasServiceName<T>)
            {
                return T::SPITE_SERVICE_NAME;
            }
            else
            {
                return shortTypeName<T>();
            }
        }
    } // namespace detail

    template <typename T>
    class ReadService
    {
    public:
        ReadService() = default;
        ReadService(const ReadService&) = delete;
        ReadService& operator=(const ReadService&) = delete;
        ReadService(ReadService&&) noexcept = default;
        ReadService& operator=(ReadService&&) noexcept = default;

        [[nodiscard]] const T& get() const { return *static_cast<const T*>(m_entry->instance.get()); }
        [[nodiscard]] const T* operator->() const { return &get(); }
        [[nodiscard]] const T& operator*() const { return get(); }

    private:
        friend class ServiceLocator;
        explicit ReadService(std::shared_ptr<ServiceEntry> entry) : m_entry(std::move(entry)), m_lock(m_entry->mutex) {}

        std::shared_ptr<ServiceEntry> m_entry;
        std::shared_lock<std::shared_mutex> m_lock;
    };

    template <typename T>
    class WriteService
    {
    public:
        WriteService() = default;
        WriteService(const WriteService&) = delete;
        WriteService& operator=(const WriteService&) = delete;
        WriteService(WriteService&&) noexcept = default;
        WriteService& operator=(WriteService&&) noexcept = default;

        [[nodiscard]] T& get() const { return *static_cast<T*>(m_entry->instance.get()); }
        [[nodiscard]] T* operator->() const { return &get(); }
        [[nodiscard]] T& operator*() const { return get(); }

    private:
        friend class ServiceLocator;
        explicit WriteService(std::shared_ptr<ServiceEntry> entry) : m_entry(std::move(entry)), m_lock(m_entry->mutex)
        {
        }

        std::shared_ptr<ServiceEntry> m_entry;
        std::unique_lock<std::shared_mutex> m_lock;
    };

    class SPITE_BASE_API ServiceLocator
    {
    public:
        // Out-of-line: whole-class dll-export does not emit defaulted-inline
        // special members, and dllimport consumers need the import thunks.
        ServiceLocator();
        explicit ServiceLocator(ServiceLocator* parent);
        ~ServiceLocator();
        ServiceLocator(const ServiceLocator&) = delete;
        ServiceLocator& operator=(const ServiceLocator&) = delete;

        void setParent(ServiceLocator* parent) { m_parent = parent; }

        [[nodiscard]] ServiceLocator& rootServices() { return m_parent ? m_parent->rootServices() : *this; }

        [[nodiscard]] const ServiceLocator& rootServices() const { return m_parent ? m_parent->rootServices() : *this; }

        // Registers under detail::serviceNameOf<T>(); names are unique per locator.
        // owner: the registering runtime module, dropped by unregisterOwner.
        template <typename T>
        void registerService(std::shared_ptr<T> instance, const InternedString owner = {})
        {
            SASSERTM(instance, "Cannot register a null service instance.");
            const std::type_index type = typeid(T);
            const std::string_view name = detail::serviceNameOf<T>();
            std::unique_lock lock(m_mutex);
            SASSERTM(!m_typedEntries.contains(type), "Service {} is already registered.", name);
            SASSERTM(!findOwnByName(name), "Service name {} is already taken in this locator.", name);
            m_typedEntries.emplace(type, std::make_shared<ServiceEntry>(name, std::move(instance), owner));
        }

        // non-owning registration for services owned elsewhere (engine subsystems registering themselves)
        template <typename T>
        void registerService(T* raw, const InternedString owner = {})
        {
            SASSERTM(raw, "Cannot register a null service instance.");
            registerService<T>(std::shared_ptr<T>(raw, [](T*) {}), owner);
        }

        // false when neither this locator nor its parents hold the service;
        // use before read()/write() when a service is optional
        [[nodiscard]] bool contains(std::type_index type) const
        {
            {
                std::shared_lock lock(m_mutex);
                if (m_typedEntries.contains(type))
                {
                    return true;
                }
            }
            return m_parent && m_parent->contains(type);
        }

        template <typename T>
        [[nodiscard]] bool contains() const
        {
            return contains(typeid(T));
        }

        template <typename T>
        [[nodiscard]] ReadService<T> read() const
        {
            return ReadService<T>(find(typeid(T)));
        }

        template <typename T>
        [[nodiscard]] WriteService<T> write()
        {
            return WriteService<T>(find(typeid(T)));
        }

        // Mutable access without the per-entry lock; only for services registered non-owning
        // (registerService(T*)) that are internally synchronized and live as long as the locator.
        template <typename T>
        [[nodiscard]] T& get()
        {
            std::shared_ptr<ServiceEntry> entry = find(typeid(T));
            return *static_cast<T*>(entry->instance.get());
        }

        template <typename T>
        bool unregisterService()
        {
            std::unique_lock lock(m_mutex);
            return m_typedEntries.erase(typeid(T)) != 0;
        }

        // Drops this locator's services (and their tables) the module registered;
        // returns how many.
        u32 unregisterOwner(InternedString owner);

        // Attaches a table to TService, which must be registered on this
        // locator; the table drops with the service. produce runs only inside
        // queryTable and emits rows described by rowSchema.
        template <typename TService>
        void registerTable(const std::string_view table, const StructSchema& rowSchema,
                           std::function<void(const TService& service, const TableRowSink& sink)> produce)
        {
            SASSERTM(produce, "Table {} needs a row producer.", table);
            std::unique_lock lock(m_mutex);
            const auto it = m_typedEntries.find(typeid(TService));
            SASSERTM(it != m_typedEntries.end(), "Table {} registered before its service {}.", table,
                     detail::serviceNameOf<TService>());
            std::vector<ServiceTable>& tables = it->second->tables;
            SASSERTM(std::ranges::none_of(tables, [table](const ServiceTable& t) { return t.name == table; }),
                     "Service {} already has a table {}.", it->second->name, table);
            tables.push_back({table, &rowSchema,
                              [produce = std::move(produce)](const void* service, const TableRowSink& sink)
                              { produce(*static_cast<const TService*>(service), sink); }});
        }

        // Every service visible from this locator, nearest first; a name that
        // appears at two depths is shadowed at the deeper one.
        [[nodiscard]] std::vector<ServiceInfo> listServices() const;

        // Streams the rows of service.table into sink on the calling thread,
        // holding the service's read lock; false when either name is unknown.
        // Rows are materialized only here: call at the control frame point,
        // never from a frame-critical path.
        bool queryTable(std::string_view service, std::string_view table, const TableRowSink& sink) const;

    private:
        // caller holds m_mutex
        [[nodiscard]] std::shared_ptr<ServiceEntry> findOwnByName(std::string_view name) const;

        [[nodiscard]] std::shared_ptr<ServiceEntry> find(std::type_index type) const
        {
            {
                std::shared_lock lock(m_mutex);
                const auto it = m_typedEntries.find(type);
                if (it != m_typedEntries.end())
                {
                    return it->second;
                }
            }
            SASSERTM(m_parent, "Service {} was accessed before registration.", type.name());
            return m_parent->find(type);
        }

        ServiceLocator* m_parent = nullptr;
        mutable std::shared_mutex m_mutex;
        std::unordered_map<std::type_index, std::shared_ptr<ServiceEntry>> m_typedEntries;
    };
} // namespace spite
