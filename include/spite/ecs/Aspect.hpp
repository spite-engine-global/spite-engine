#pragma once


#include <algorithm>
#include <span>

#include "EcsExport.hpp"
#include "base/containers/SboVector.hpp"
#include "ComponentMetadata.hpp"

namespace spite
{
    namespace detail
    {
        // canonical id ordering for aspects and query buckets: sorted, deduplicated;
        // works for any sized, indexable range with pop_back (sbo_vector, std::vector)
        template <typename Range>
            requires requires(Range& r) {
                std::ranges::sort(r);
                r.size();
                r[0];
                r.pop_back();
            }
        inline void sort_unique_ids(Range& ids)
        {
            std::ranges::sort(ids);
            sizet write = 0;
            for (sizet read = 0; read < ids.size(); ++read)
            {
                if (write == 0 || ids[write - 1] != ids[read])
                {
                    ids[write++] = ids[read];
                }
            }
            while (ids.size() > write)
            {
                ids.pop_back();
            }
        }
    } // namespace detail

    class SPITE_ECS_API Aspect
    {
    private:
        sbo_vector<ComponentID> m_componentIds;

    public:
        Aspect();

        Aspect(std::initializer_list<ComponentID> ids);

        template <typename Iterator>
        Aspect(Iterator begin, Iterator end)
            requires std::is_base_of_v<std::forward_iterator_tag,
                                       typename std::iterator_traits<Iterator>::iterator_category>;

        explicit Aspect(ComponentID id);

        explicit Aspect(std::span<const ComponentID> ids);

        Aspect(const Aspect& other);
        Aspect(Aspect&& other) noexcept;
        Aspect& operator=(const Aspect& other);
        Aspect& operator=(Aspect&& other) noexcept;

        Aspect operator+(const Aspect& other) const;
        Aspect operator-(const Aspect& other) const;

        bool operator==(const Aspect& other) const;

        bool operator!=(const Aspect& other) const;

        const sbo_vector<ComponentID>& getComponentIds() const;

        // Check if this aspect contains all types from another aspect (subset check)
        bool contains(const Aspect& other) const;

        bool contains(ComponentID id) const;

        bool intersects(const Aspect& other) const;

        int getComponentIndex(ComponentID id) const;

        std::vector<ComponentID> getIntersection(const Aspect& other) const;

        sizet size() const;

        bool empty() const;

        struct SPITE_ECS_API hash
        {
            sizet operator()(const Aspect& aspect) const;
        };

        ~Aspect();
    };

    template <typename Iterator>
    Aspect::Aspect(Iterator begin, Iterator end)
        requires std::is_base_of_v<std::forward_iterator_tag,
                                   typename std::iterator_traits<Iterator>::iterator_category>
    {
        sizet dist = std::distance(begin, end);
        if (dist <= 0)
            return;

        m_componentIds.reserve(dist);
        for (auto it = begin; it != end; ++it)
        {
            m_componentIds.emplace_back(*it);
        }
        detail::sort_unique_ids(m_componentIds);
    }
} // namespace spite
