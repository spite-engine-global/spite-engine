#pragma once
#include "base/Platform.hpp"
#include "rendering/GraphicsTypes.hpp"
#include "rendering/RenderResourceHandles.hpp"

namespace spite
{
    constexpr sizet WHOLE_SIZE = ~0ULL;

    struct MemoryBarrier2
    {
        PipelineStage srcStageMask = PipelineStage::NONE;
        AccessFlags srcAccessMask = AccessFlags::NONE;
        PipelineStage dstStageMask = PipelineStage::NONE;
        AccessFlags dstAccessMask = AccessFlags::NONE;
    };

    struct BufferBarrier2
    {
        BufferHandle buffer;
        PipelineStage srcStageMask = PipelineStage::NONE;
        AccessFlags srcAccessMask = AccessFlags::NONE;
        PipelineStage dstStageMask = PipelineStage::NONE;
        AccessFlags dstAccessMask = AccessFlags::NONE;
        u32 srcQueueFamily = std::numeric_limits<u32>::max(); // VK_QUEUE_FAMILY_IGNORED
        u32 dstQueueFamily = std::numeric_limits<u32>::max(); // VK_QUEUE_FAMILY_IGNORED
        sizet offset = 0;
        sizet size = WHOLE_SIZE;
    };

    struct TextureBarrier2
    {
        TextureHandle texture;
        PipelineStage srcStageMask = PipelineStage::NONE;
        AccessFlags srcAccessMask = AccessFlags::NONE;
        PipelineStage dstStageMask = PipelineStage::NONE;
        AccessFlags dstAccessMask = AccessFlags::NONE;
        ImageLayout oldLayout = ImageLayout::UNDEFINED;
        ImageLayout newLayout = ImageLayout::UNDEFINED;
        u32 srcQueueFamily = std::numeric_limits<u32>::max(); // VK_QUEUE_FAMILY_IGNORED
        u32 dstQueueFamily = std::numeric_limits<u32>::max(); // VK_QUEUE_FAMILY_IGNORED
        u32 baseMipLevel = 0;
        u32 levelCount = 1;
        u32 baseArrayLayer = 0;
        u32 layerCount = 1;
    };

} // namespace spite
