#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "base/FunctionRef.hpp"
#include "Chunk.hpp"
#include "EcsExport.hpp"
#include "EcsTypes.hpp"

namespace enki
{
    class TaskScheduler;
}

namespace spite
{
    class Archetype;
    class ArchetypeManager;

    // the change bits a changed-chunk walk follows: last frame's writes (what Modified<> filters on), this
    // frame's so far, or both
    enum class ChangeWindow : u8
    {
        LastFrame,
        ThisFrame,
        SinceLastFrame
    };
    class QueryRegistry;

    // A compiled, cached query over a world's archetypes. Queries are world-owned:
    // identical shapes resolve to the same instance (see QueryRegistry). Every
    // iteration entry point first checks the world's archetype-set epoch and re-walks
    // the archetypes if a matching one was created, filled or destroyed since, so a
    // held query never iterates a destroyed archetype. getChunk(index) does not check:
    // index it after getChunkCount().
    class SPITE_ECS_API Query
    {
        friend class QueryRegistry;

    private:
        struct State;
        std::unique_ptr<State> m_state;

        bool refresh() const;
        bool refreshSlow() const;

    public:
        // the most Enabled and the most Modified components one query filters on
        static constexpr sizet MAX_FILTER_COMPONENTS = 8;

        Query(const ArchetypeManager* archetypeManager, const Aspect* includeAspect,
              const Aspect* excludeAspect = nullptr, const Aspect* mustBeEnabledAspect = nullptr,
              const Aspect* mustBeModifiedAspect = nullptr, enki::TaskScheduler* scheduler = nullptr);

        ~Query();
        Query(const Query&) = delete;
        Query& operator=(const Query&) = delete;
        Query(Query&&) = delete;
        Query& operator=(Query&&) = delete;

        // counts entities passing the per-entity enabled/modified filters
        sizet getEntityCount();
        bool hasAnyModified();

        // counts all entities in the matched chunks without filtering
        sizet getGeneralEntityCount();

        [[nodiscard]] sizet getChunkCount() const;
        [[nodiscard]] Chunk* getChunk(sizet index) const;

        // what this cached query was built from (debugging); the optional filters
        // are null when they were not part of the query shape
        [[nodiscard]] const Aspect& getIncludeAspect() const;
        [[nodiscard]] const Aspect* getExcludeAspect() const;
        [[nodiscard]] const Aspect* getEnabledAspect() const;
        [[nodiscard]] const Aspect* getModifiedAspect() const;

        // Chunk iteration visits every matched chunk: the enabled/modified filters
        // apply only to entity iteration, getEntityCount and hasAnyModified, so
        // chunk callers test the per-entity bits themselves.
        void forEachChunkConst(function_ref<void(const Chunk* chunk)> func) const;
        void forEachChunk(function_ref<void(Chunk* chunk)> func);

        // runs on the world's task scheduler; serial fallback on scheduler-less worlds
        void forEachChunkParallelConst(function_ref<void(const Chunk* chunk, u32 threadNum)> func,
                                       u32 minRange = 8) const;
        void forEachChunkParallel(function_ref<void(Chunk* chunk, u32 threadNum)> func, u32 minRange = 8);

        // Visit only the matched chunks whose archetype change summary says one of `components` may have
        // changed in `window`, read from the summary without touching untouched chunks: a superset of the
        // chunks with such a live row, so the callback still tests the per-chunk bits. Components an archetype
        // lacks are skipped. A quiescent world visits nothing and allocates nothing.
        void forEachChangedChunkConst(std::span<const ComponentID> components, ChangeWindow window,
                                      function_ref<void(const Chunk* chunk)> func) const;
        void forEachChangedChunkParallelConst(std::span<const ComponentID> components, ChangeWindow window,
                                              function_ref<void(const Chunk* chunk, u32 threadNum)> func,
                                              u32 minRange = 8) const;
        void forEachChangedChunkParallel(std::span<const ComponentID> components, ChangeWindow window,
                                         function_ref<void(Chunk* chunk, u32 threadNum)> func, u32 minRange = 8);

        // flat chunk snapshot for the typed view
        [[nodiscard]] std::vector<Chunk*> collectChunks() const;

        // Views take their snapshot vector from a per-thread pool and give it back, so a steady-state view
        // allocates nothing; the pool (not the Query) owns the vector, so nested views of one shared Query and
        // views on several threads never share a list.
    private:
        template <typename... TArgs>
        friend class View;

        static std::vector<Chunk*> acquireChunkList();
        static void releaseChunkList(std::vector<Chunk*>&& list);
        void collectChunksInto(std::vector<Chunk*>& out) const;

    public:
        // Debug: marks the archetypes the snapshot was taken from as iterated, so an immediate structural change
        // of one while a View is alive asserts (the snapshot would dangle). Empty in Release.
        class SPITE_ECS_API ViewGuard
        {
#if SPITE_DEBUG_BUILD
            std::vector<Archetype*> m_archetypes;

        public:
            explicit ViewGuard(const Query& query);
            ~ViewGuard();
            ViewGuard(ViewGuard&&) = delete;
            ViewGuard& operator=(ViewGuard&&) = delete;
#else
        public:
            explicit ViewGuard(const Query&) {}
#endif
        };

        template <typename... TArgs>
        class Iterator;

        template <typename... TArgs>
        class View;

        // Typed iteration over the query's entities. Arguments may be Entity, Read<T>
        // and Write<T>. Exclude/Enabled/Modified are build-time and per-entity filters,
        // not iteration arguments.
        template <typename... TArgs>
        View<TArgs...> view();
    };

    template <typename... TArgs>
    class Query::Iterator
    {
    private:
        Query* m_query;
        const std::vector<Chunk*>* m_chunks = nullptr;
        sizet m_chunkIndex = 0;
        sizet m_entityIndexInChunk = 0;
        Chunk* m_currentChunk = nullptr;

        const Aspect* m_enabledAspect = nullptr;
        const Aspect* m_modifiedAspect = nullptr;

        static_assert(((is_read_wrapper_v<TArgs> || is_write_wrapper_v<TArgs> || is_entity_v<TArgs>) && ...),
                      "All arguments to a query view must be Read<T>, Write<T>, or Entity.");

        static constexpr sizet entity_count = (is_entity_v<TArgs> + ...);
        static_assert(entity_count <= 1, "Cannot request Entity more than once in a query view.");

        static constexpr sizet component_count = ((is_read_wrapper_v<TArgs> || is_write_wrapper_v<TArgs>)+...);

        std::array<std::byte*, component_count> m_componentArrayPtrs{};
        std::array<sizet, component_count> m_componentSizes{};

        // Maps the index in TArgs... to the index in the component-only list. -1 if not a component.
        static constexpr std::array<int, sizeof...(TArgs)> arg_to_comp_idx_map = []
        {
            std::array<int, sizeof...(TArgs)> map{};
            int current_comp_idx = 0;
            int current_arg_idx = 0;
            auto set_map = [&]<typename T0>(std::type_identity<T0>)
            {
                if constexpr (is_read_wrapper_v<T0> || is_write_wrapper_v<T0>)
                {
                    map[current_arg_idx] = current_comp_idx++;
                }
                else
                {
                    map[current_arg_idx] = -1;
                }
                current_arg_idx++;
            };
            (set_map(std::type_identity<TArgs>{}), ...);
            return map;
        }();

        std::array<int, component_count> m_componentIndicesInChunk{};
        std::array<ComponentID, component_count> m_componentIds{};

        std::array<int, MAX_FILTER_COMPONENTS> m_enabledIndicesInChunk{};
        sizet m_enabledIndicesCount = 0;

        std::array<int, MAX_FILTER_COMPONENTS> m_modifiedIndicesInChunk{};
        sizet m_modifiedIndicesCount = 0;
        using first_arg_type = std::tuple_element_t<0, std::tuple<TArgs...>>;

        void updateChunkFilterCache()
        {
            m_enabledIndicesCount = 0;
            m_modifiedIndicesCount = 0;
            if (m_enabledAspect)
            {
                for (const ComponentID id : m_enabledAspect->getComponentIds())
                {
                    const int index = m_currentChunk->getComponentIndex(id);
                    if (index != -1)
                    {
                        m_enabledIndicesInChunk[m_enabledIndicesCount++] = index;
                    }
                }
            }
            if (m_modifiedAspect)
            {
                for (const ComponentID id : m_modifiedAspect->getComponentIds())
                {
                    const int index = m_currentChunk->getComponentIndex(id);
                    if (index != -1)
                    {
                        m_modifiedIndicesInChunk[m_modifiedIndicesCount++] = index;
                    }
                }
            }
        }

        void findNextValidEntity()
        {
            while (m_currentChunk)
            {
                while (m_entityIndexInChunk < m_currentChunk->size())
                {
                    bool passedFilters = true;
                    if (m_enabledIndicesCount > 0)
                    {
                        for (sizet i = 0; i < m_enabledIndicesCount; ++i)
                        {
                            if (!m_currentChunk->isComponentEnabledByIndex(m_enabledIndicesInChunk[i],
                                                                           m_entityIndexInChunk))
                            {
                                passedFilters = false;
                                break;
                            }
                        }
                    }

                    if (passedFilters && m_modifiedIndicesCount > 0)
                    {
                        bool anyComponentModified = false;
                        for (sizet i = 0; i < m_modifiedIndicesCount; ++i)
                        {
                            if (m_currentChunk->wasModifiedLastFrameByIndex(m_modifiedIndicesInChunk[i],
                                                                            m_entityIndexInChunk))
                            {
                                anyComponentModified = true;
                                break;
                            }
                        }
                        if (!anyComponentModified)
                        {
                            passedFilters = false;
                        }
                    }

                    if (passedFilters)
                    {
                        return;
                    }

                    m_entityIndexInChunk++;
                }

                ++m_chunkIndex;
                findNextValidChunk();
            }
        }

        void findNextValidChunk()
        {
            while (m_chunks && m_chunkIndex < m_chunks->size())
            {
                Chunk* chunk = (*m_chunks)[m_chunkIndex];
                if (!chunk->empty())
                {
                    m_currentChunk = chunk;
                    m_entityIndexInChunk = 0;
                    updateChunkFilterCache();
                    updateChunkCache();
                    return;
                }
                ++m_chunkIndex;
            }
            m_currentChunk = nullptr; // End of iteration
            m_entityIndexInChunk = 0; // must match the end iterator's state or operator== never holds
        }

        void updateChunkCache()
        {
            if constexpr (component_count > 0)
            {
                int current_comp_idx = 0;
                auto cache_component = [&]<typename T0>(std::type_identity<T0>)
                {
                    if constexpr (is_read_wrapper_v<T0> || is_write_wrapper_v<T0>)
                    {
                        const int index = m_currentChunk->getComponentIndex(m_componentIds[current_comp_idx]);
                        m_componentIndicesInChunk[current_comp_idx] = index;
                        m_componentArrayPtrs[current_comp_idx] =
                            static_cast<std::byte*>(m_currentChunk->getComponentArrayByIndex(index));
                        m_componentSizes[current_comp_idx] = sizeof(get_component_type<T0>);
                        current_comp_idx++;
                    }
                };
                (cache_component(std::type_identity<TArgs>{}), ...);
            }
        }

        template <typename T, typename = void>
        struct reference_type_for_helper;

        template <typename T>
        struct reference_type_for_helper<T, std::enable_if_t<is_read_wrapper_v<T> || is_write_wrapper_v<T>>>
        {
            using type =
                std::conditional_t<is_write_wrapper_v<T>, get_component_type<T>&, const get_component_type<T>&>;
        };

        template <typename T>
        struct reference_type_for_helper<T, std::enable_if_t<is_entity_v<T>>>
        {
            using type = Entity;
        };

        template <typename T>
        using reference_type_for = reference_type_for_helper<T>::type;

        template <typename T, typename = void>
        struct pointer_type_for_helper;

        template <typename T>
        struct pointer_type_for_helper<T, std::enable_if_t<is_read_wrapper_v<T> || is_write_wrapper_v<T>>>
        {
            using type =
                std::conditional_t<is_write_wrapper_v<T>, get_component_type<T>*, const get_component_type<T>*>;
        };

        template <typename T>
        struct pointer_type_for_helper<T, std::enable_if_t<is_entity_v<T>>>
        {
            using type = Entity*;
        };

        template <typename T>
        using pointer_type_for = pointer_type_for_helper<T>::type;

        template <typename ArgType, sizet ArgIdx>
        auto get_arg() -> reference_type_for<ArgType>
        {
            if constexpr (is_entity_v<ArgType>)
            {
                return getEntity();
            }
            else // is Read<T> or Write<T>
            {
                using ComponentType = get_component_type<ArgType>;
                constexpr int comp_idx = arg_to_comp_idx_map[ArgIdx];

                ComponentType* arr = reinterpret_cast<ComponentType*>(m_componentArrayPtrs[comp_idx]);

                if constexpr (is_write_wrapper_v<ArgType>)
                {
                    m_currentChunk->markModifiedByIndex(m_componentIndicesInChunk[comp_idx], m_entityIndexInChunk);
                }
                return arr[m_entityIndexInChunk];
            }
        }

        template <sizet... I>
        auto create_reference_tuple(std::index_sequence<I...>) -> std::tuple<reference_type_for<TArgs>...>
        {
            return std::forward_as_tuple(get_arg<std::tuple_element_t<I, std::tuple<TArgs...>>, I>()...);
        }

    public:
        using iterator_category = std::forward_iterator_tag;
        using difference_type = std::ptrdiff_t;

        using reference = std::conditional_t<sizeof...(TArgs) == 1, reference_type_for<first_arg_type>,
                                             std::tuple<reference_type_for<TArgs>...>>;
        using value_type = std::conditional_t<sizeof...(TArgs) == 1, std::decay_t<reference_type_for<first_arg_type>>,
                                              std::tuple<std::decay_t<reference_type_for<TArgs>>...>>;
        using pointer = std::conditional_t<sizeof...(TArgs) == 1, pointer_type_for<first_arg_type>,
                                           void // Returning a pointer to a temporary tuple is not feasible
                                           >;

        Iterator(Query* query, const std::vector<Chunk*>* chunks, const bool isEnd = false) :
            m_query(query), m_chunks(chunks)
        {
            m_enabledAspect = m_query->getEnabledAspect();
            if (m_enabledAspect && m_enabledAspect->empty())
            {
                m_enabledAspect = nullptr;
            }
            m_modifiedAspect = m_query->getModifiedAspect();
            if (m_modifiedAspect && m_modifiedAspect->empty())
            {
                m_modifiedAspect = nullptr;
            }

            if constexpr (component_count > 0)
            {
                int current_comp_idx = 0;
                auto cache_componentId = [&]<typename T0>(std::type_identity<T0>)
                {
                    if constexpr (is_read_wrapper_v<T0> || is_write_wrapper_v<T0>)
                    {
                        m_componentIds[current_comp_idx++] = getComponentId<get_component_type<T0>>();
                    }
                };
                (cache_componentId(std::type_identity<TArgs>{}), ...);
            }

            if (isEnd || !m_chunks || m_chunks->empty())
            {
                m_currentChunk = nullptr;
                m_chunkIndex = m_chunks ? m_chunks->size() : 0;
            }
            else
            {
                findNextValidChunk();
                findNextValidEntity();
            }
        }

        Iterator& operator++()
        {
            m_entityIndexInChunk++;
            findNextValidEntity();
            return *this;
        }

        Iterator operator++(int)
        {
            Iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        reference operator*()
        {
            if constexpr (sizeof...(TArgs) == 1)
            {
                return get_arg<first_arg_type, 0>();
            }
            else
            {
                return create_reference_tuple(std::index_sequence_for<TArgs...>{});
            }
        }

        Entity getEntity() const { return m_currentChunk->entity(m_entityIndexInChunk); }

        bool operator==(const Iterator& other) const
        {
            return m_currentChunk == other.m_currentChunk && m_entityIndexInChunk == other.m_entityIndexInChunk;
        }

        bool operator!=(const Iterator& other) const { return !(*this == other); }
    };

    template <typename... TArgs>
    class Query::View
    {
    private:
        Query* m_query;
        std::vector<Chunk*> m_chunks;
        ViewGuard m_guard; // after m_chunks: the snapshot refreshes the archetype list it holds

        // fills the list, then hands the query on: the guard marks the archetypes the fill refreshed
        static const Query& snapshot(Query& query, std::vector<Chunk*>& chunks)
        {
            query.collectChunksInto(chunks);
            return query;
        }

    public:
        explicit View(Query* query) : m_query(query), m_chunks(acquireChunkList()), m_guard(snapshot(*query, m_chunks))
        {
        }
        ~View() { releaseChunkList(std::move(m_chunks)); }
        View(const View&) = delete;
        View& operator=(const View&) = delete;

        auto begin() const { return Iterator<TArgs...>(m_query, &m_chunks); }
        auto end() const { return Iterator<TArgs...>(m_query, &m_chunks, true); }
    };

    template <typename... TArgs>
    Query::View<TArgs...> Query::view()
    {
        return View<TArgs...>(this);
    }
} // namespace spite
