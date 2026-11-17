#pragma once

#include <array>
#include <memory>
#include <span>

#include "base/containers/SboVector.hpp"
#include "ComponentRegistry.hpp"
#include "EcsExport.hpp"
#include "EcsTypes.hpp"

namespace spite
{
    class Query;
    class QueryRegistry;
    struct SystemDependencies;

    // Bare components count as reads; Read/Write/Exclude/Enabled/Modified classify as-is.
    template <typename T>
    struct query_arg
    {
        static_assert(t_component<T>,
                      "query arguments must be component types or Read/Write/Exclude/Enabled/Modified wrappers.");
        using type = Read<T>;
    };
    template <typename T>
    struct query_arg<Read<T>>
    {
        using type = Read<T>;
    };
    template <typename T>
    struct query_arg<Write<T>>
    {
        using type = Write<T>;
    };
    template <typename T>
    struct query_arg<Exclude<T>>
    {
        using type = Exclude<T>;
    };
    template <typename T>
    struct query_arg<Enabled<T>>
    {
        using type = Enabled<T>;
    };
    template <typename T>
    struct query_arg<Modified<T>>
    {
        using type = Modified<T>;
    };
    template <>
    struct query_arg<IncludeInactive>
    {
        using type = IncludeInactive;
    };
    template <typename T>
    using query_arg_t = typename query_arg<T>::type;

    class SPITE_ECS_API QueryBuilder
    {
    private:
        // inline storage: per-frame builders in systems do not touch the heap
        using IdList = sbo_vector<ComponentID, 8>;

        QueryRegistry* m_queryRegistry;
        // when set, buildQuery asserts that every read/write is declared in it (systems)
        const SystemDependencies* m_accessDependencies = nullptr;
        IdList m_readTypes;
        IdList m_writeTypes;
        IdList m_excludeTypes;
        IdList m_enabledTypes;
        IdList m_modifiedTypes;
        bool m_includeInactive = false;

        // QueryRegistry and SystemDependencies are internal; the touching parts
        // live in the .cpp. The single build exit for both API paths: asserts
        // declared access, excludes InactiveComponent unless a clause names it or
        // includeInactive is set, canonicalizes the buckets, compiles through the registry.
        std::shared_ptr<Query> buildQueryInternal(std::span<const ComponentID> read, std::span<const ComponentID> write,
                                                  std::span<const ComponentID> exclude,
                                                  std::span<const ComponentID> enabled,
                                                  std::span<const ComponentID> modified, bool includeInactive);
        static void assertReadDeclared(const SystemDependencies* dependencies, ComponentID id);
        static void assertWriteDeclared(const SystemDependencies* dependencies, ComponentID id);

    public:
        QueryBuilder(QueryRegistry* queryRegistry, const SystemDependencies* accessDependencies = nullptr);

        template <typename... T>
        QueryBuilder& with()
        {
            (
                [&]<typename U>(std::type_identity<U>)
                {
                    static_assert(!std::is_same_v<U, IncludeInactive>, "use includeInactive()");
                    using Arg = query_arg_t<U>;
                    using ComponentType = get_component_type<Arg>;
                    if constexpr (is_write_wrapper_v<Arg>)
                    {
                        m_writeTypes.push_back(getComponentId<ComponentType>());
                    }
                    else
                    {
                        m_readTypes.push_back(getComponentId<ComponentType>());
                    }
                }(std::type_identity<T>{}),
                ...);
            return *this;
        }

        template <t_component... T>
        QueryBuilder& with_read()
        {
            (m_readTypes.push_back(getComponentId<T>()), ...);
            return *this;
        }

        QueryBuilder& with_read(std::span<const ComponentID> components);

        template <t_component... T>
        QueryBuilder& with_write()
        {
            (m_writeTypes.push_back(getComponentId<T>()), ...);
            return *this;
        }

        QueryBuilder& with_write(std::span<const ComponentID> components);

        template <t_component... T>
        QueryBuilder& without()
        {
            (m_excludeTypes.push_back(getComponentId<T>()), ...);
            return *this;
        }

        QueryBuilder& without(std::span<const ComponentID> components);

        template <t_component... T>
        QueryBuilder& enabled()
        {
            (m_enabledTypes.push_back(getComponentId<T>()), ...);
            return *this;
        }

        QueryBuilder& enabled(std::span<const ComponentID> components);

        template <t_component... T>
        QueryBuilder& modified()
        {
            (m_modifiedTypes.push_back(getComponentId<T>()), ...);
            return *this;
        }

        QueryBuilder& modified(std::span<const ComponentID> components);

        // the query also visits inactive entities: bookkeeping that must cover every entity
        QueryBuilder& includeInactive()
        {
            m_includeInactive = true;
            return *this;
        }

        std::shared_ptr<Query> buildQuery();

        [[nodiscard]] std::span<const ComponentID> readTypes() const { return m_readTypes; }
        [[nodiscard]] std::span<const ComponentID> writeTypes() const { return m_writeTypes; }

        template <typename... T>
        std::shared_ptr<Query> buildQuery()
        {
            static_assert(
                ((is_read_wrapper_v<T> || is_write_wrapper_v<T> || is_exclude_wrapper_v<T> || is_enabled_wrapper_v<T> ||
                  is_modified_wrapper_v<T> || t_component<T> || std::is_same_v<T, IncludeInactive>) &&
                 ...),
                "buildQuery<>() accepts components (treated as Read<T>), Read<T>, Write<T>, Exclude<T>, "
                "Enabled<T>, Modified<T>, or IncludeInactive.");

            constexpr sizet count = sizeof...(T);
            std::array<ComponentID, count> read{};
            std::array<ComponentID, count> write{};
            std::array<ComponentID, count> exclude{};
            std::array<ComponentID, count> enabled{};
            std::array<ComponentID, count> modified{};
            sizet readCount = 0;
            sizet writeCount = 0;
            sizet excludeCount = 0;
            sizet enabledCount = 0;
            sizet modifiedCount = 0;
            bool includeInactive = m_includeInactive;

            const auto add = [&]<typename U>(std::type_identity<U>)
            {
                if constexpr (std::is_same_v<U, IncludeInactive>)
                {
                    includeInactive = true;
                }
                else
                {
                    using Component = get_component_type<U>;
                    const ComponentID id = getComponentId<Component>();
                    if constexpr (is_read_wrapper_v<U>)
                        read[readCount++] = id;
                    else if constexpr (is_write_wrapper_v<U>)
                        write[writeCount++] = id;
                    else if constexpr (is_exclude_wrapper_v<U>)
                        exclude[excludeCount++] = id;
                    else if constexpr (is_enabled_wrapper_v<U>)
                        enabled[enabledCount++] = id;
                    else
                        modified[modifiedCount++] = id;
                }
            };
            (add(std::type_identity<query_arg_t<T>>{}), ...);

            return buildQueryInternal({read.data(), readCount}, {write.data(), writeCount},
                                      {exclude.data(), excludeCount}, {enabled.data(), enabledCount},
                                      {modified.data(), modifiedCount}, includeInactive);
        }
    };
} // namespace spite
