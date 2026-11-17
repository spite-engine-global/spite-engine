#pragma once

#include <deque>
#include <vector>

namespace spite
{
    // Defines a pool for a specific type of GPU resource.
    // resources/generations use deque storage: pool slots are handed out as
    // references (getAllocation), so element addresses must stay stable while
    // a concurrent allocate() grows the pool - vector growth reallocates and
    // invalidates every outstanding reference.
    template <typename T_Resource>
    struct ResourcePool
    {
        std::deque<T_Resource> resources;
        std::deque<u32> generations;
        std::vector<u32> freeIndices;

        void reset()
        {
            // Do not clear the slot storage. Its size must be preserved to keep slots available.
            // The objects within them will be overwritten by new allocations.
            freeIndices.clear();
            freeIndices.reserve(generations.size());
            for (u32 i = 0; i < generations.size(); ++i)
            {
                // Increment the generation to invalidate any handles from previous frames.
                generations[i]++;
                freeIndices.push_back(i);
            }
        }
    };
} // namespace spite
