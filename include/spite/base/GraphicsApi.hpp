#pragma once
#include <string_view>

namespace spite
{
    enum class GraphicsApi
    {
        Undefined,
        Vulkan
    };

    inline const char* to_string(GraphicsApi e)
    {
        switch (e)
        {
        case GraphicsApi::Vulkan:
            return "Vulkan";
        default:
            return "unknown";
        }
    }

    inline GraphicsApi graphics_api_from_string(std::string_view api)
    {
        if (api == "Vulkan")
            return GraphicsApi::Vulkan;
        return GraphicsApi::Undefined;
    }
} // namespace spite
