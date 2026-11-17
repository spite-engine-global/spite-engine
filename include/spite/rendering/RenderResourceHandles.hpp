#pragma once
#include <compare>

#include "base/HashCombine.hpp"
#include "base/Platform.hpp"

namespace spite
{
    constexpr u32 INVALID_RESOURCE_ID = -1;

    // Generic handle structure, providing a common base for all resource handles.
    struct GpuResourceHandle
    {
        u32 id = INVALID_RESOURCE_ID;
        u32 generation = 0;

        [[nodiscard]] bool isValid() const { return id != INVALID_RESOURCE_ID; }

        auto operator<=>(const GpuResourceHandle& other) const = default;
        struct Hash
        {
            sizet operator()(const GpuResourceHandle& res) const
            {
                sizet seed = 0;
                hashCombine(seed, res.generation, res.id);
                return seed;
            }
        };
    };


    struct BufferHandle : GpuResourceHandle
    {
    };

    struct TextureHandle : GpuResourceHandle
    {
    };

    struct SamplerHandle : GpuResourceHandle
    {
    };

    struct PipelineHandle : GpuResourceHandle
    {
    };

    struct ImageViewHandle : GpuResourceHandle
    {
    };

    struct ResourceSetLayoutHandle : GpuResourceHandle
    {
    };

    struct RenderPassHandle : GpuResourceHandle
    {
    };

    struct FramebufferHandle : GpuResourceHandle
    {
    };

    struct ShaderModuleHandle : GpuResourceHandle
    {
    };

    struct PipelineLayoutHandle : GpuResourceHandle
    {
    };
} // namespace spite
