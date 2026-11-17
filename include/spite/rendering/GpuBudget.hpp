#pragma once
#include <algorithm>
#include <vector>

#include "base/Platform.hpp"

namespace spite
{
    // Report-only per-memory-heap probe (VK_EXT_memory_budget via VMA):
    // the input to any budget admission policy.
    struct GpuHeapMemoryInfo
    {
        u64 budgetBytes = 0;
        u64 usedBytes = 0;
        bool deviceLocal = false;
    };

    // The soft budget streaming growth is admitted against.
    struct GpuBudget
    {
        u64 budgetBytes = 0;
        u64 usedBytes = 0;
    };

    // min(user knob, device-local heap budget) minus a safety margin for
    // driver/swapchain allocations VMA does not account for. Host-visible
    // heaps are not streaming targets and are excluded.
    inline GpuBudget resolveGpuBudget(const std::vector<GpuHeapMemoryInfo>& heaps, u64 userBudgetBytes)
    {
        constexpr u64 SAFETY_MARGIN_BYTES = 256ull * 1024 * 1024;
        GpuBudget budget{};
        u64 deviceLocalBudget = 0;
        for (const GpuHeapMemoryInfo& heap : heaps)
        {
            if (!heap.deviceLocal)
            {
                continue;
            }
            deviceLocalBudget += heap.budgetBytes;
            budget.usedBytes += heap.usedBytes;
        }
        const u64 autoBudget = deviceLocalBudget > SAFETY_MARGIN_BYTES ? deviceLocalBudget - SAFETY_MARGIN_BYTES : 0;
        budget.budgetBytes = userBudgetBytes > 0 ? std::min(userBudgetBytes, autoBudget) : autoBudget;
        return budget;
    }
} // namespace spite
