#pragma once

#include <functional>
#include "rendering/GpuBudget.hpp"

namespace spite
{
    // Host-owned callbacks remain valid until module unload. A GPU-less host reports empty heaps and budget.
    struct GpuMemoryService
    {
        std::function<std::vector<GpuHeapMemoryInfo>()> heaps;
        std::function<GpuBudget()> budget;
    };
} // namespace spite
