#pragma once

#include "base/Platform.hpp"

namespace vk
{
    class Instance;
    class SurfaceKHR;
} // namespace vk

namespace spite
{
    class IVulkanWindowSurface
    {
    public:
        virtual ~IVulkanWindowSurface() = default;
        [[nodiscard]] virtual const char* const* getRequiredInstanceExtensions(u32& extensionCount) const = 0;
        [[nodiscard]] virtual vk::SurfaceKHR createSurface(vk::Instance instance) const = 0;
    };
} // namespace spite
