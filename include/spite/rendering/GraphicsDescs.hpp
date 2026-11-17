#pragma once
#include <algorithm>
#include <bit>
#include <variant>
#include <vector>

#include "rendering/Extent2D.hpp"
#include "rendering/GpuAllocationManager.hpp"
#include "rendering/GraphicsTypes.hpp"
#include "rendering/RenderResourceHandles.hpp"
#include "base/InternedString.hpp"

namespace spite
{
    // A single descriptor-slot declaration inside a resource set layout.
    struct ResourceBinding
    {
        u32 binding = 0;
        DescriptorType type;
        u32 descriptorCount = 1; // For arrays of resources
        ShaderStage shaderStages = ShaderStage::NONE;
        InternedString name; // For UBOs, this is the block name from the shader
        sizet size = 0;
        DescriptorBindingFlags flags = DescriptorBindingFlags::NONE;

        bool operator==(const ResourceBinding& other) const = default;
    };

    struct ResourceSetLayoutDescription
    {
        std::vector<ResourceBinding> bindings;

        bool operator==(const ResourceSetLayoutDescription& other) const;
        struct Hash
        {
            sizet operator()(const ResourceSetLayoutDescription& desc) const;
        };
    };

    struct Offset2D
    {
        i32 x = 0;
        i32 y = 0;
    };

    // the swapchain view index when every render view draws offscreen (editor panels)
    constexpr u32 NO_SWAPCHAIN_VIEW = ~0u;

    struct Rect2D
    {
        Offset2D offset;
        Extent2D extent;
    };

    struct ClearColorValue
    {
        union
        {
            struct
            {
                float r, g, b, a;
            };

            float float32[4];
            i32 int32[4];
            u32 uint32[4];
        };

        ClearColorValue() : float32{0.0f, 0.0f, 0.0f, 1.0f} {}

        ClearColorValue(float r, float g, float b, float a = 1.0f) : float32{r, g, b, a} {}

        static ClearColorValue fromUint(u32 r, u32 g, u32 b, u32 a)
        {
            ClearColorValue v;
            v.uint32[0] = r;
            v.uint32[1] = g;
            v.uint32[2] = b;
            v.uint32[3] = a;
            return v;
        }

        static ClearColorValue fromInt(i32 r, i32 g, i32 b, i32 a)
        {
            ClearColorValue v;
            v.int32[0] = r;
            v.int32[1] = g;
            v.int32[2] = b;
            v.int32[3] = a;
            return v;
        }
    };

    struct ClearDepthStencilValue
    {
        float depth = 1.0f;
        u32 stencil = 0;
    };

    using ClearValue = std::variant<ClearColorValue, ClearDepthStencilValue>;

    struct BufferDesc
    {
        sizet size = 0;
        BufferUsage usage = BufferUsage::NONE;
        MemoryUsage memoryUsage = MemoryUsage::UNKNOWN;
        SharingMode sharingMode = SharingMode::EXCLUSIVE;
    };

    // How a render-graph texture is sized: FIXED uses width/height; the others derive them at graph compile, times
    // extentScale. VIEWPORT: the view's render extent (its display extent times the view's render scale; the same
    // while the scale is 1). DISPLAY: the view's output extent, whatever its scale (view targets, what draws onto
    // them). SWAPCHAIN: the swapchain's for the swapchain view, else the view's output extent.
    enum class TextureExtent : u8
    {
        FIXED,
        VIEWPORT,
        SWAPCHAIN,
        DISPLAY,
    };

    struct TextureDesc
    {
        u32 width = 1;
        u32 height = 1;
        u32 depth = 1;
        u32 mipLevels = 1; // 0 in a render-graph texture: the full chain of its compiled extent
        u32 arrayLayers = 1; // a cube texture: six faces per cube (+X, -X, +Y, -Y, +Z, -Z), so a multiple of 6
        Format format = Format::UNDEFINED;
        TextureUsage usage = TextureUsage::NONE;
        MemoryUsage memoryUsage = MemoryUsage::GPU_ONLY;
        TextureExtent extent = TextureExtent::FIXED;
        f32 extentScale = 1.0f;
        bool cube = false; // cube-compatible image: square, arrayLayers a multiple of 6; views may be CUBE
    };

    inline u32 fullMipCount(u32 width, u32 height) { return std::bit_width(std::max(width, height)); }

    struct SamplerDesc
    {
        Filter magFilter = Filter::LINEAR;
        Filter minFilter = Filter::LINEAR;
        AddressMode addressModeU = AddressMode::REPEAT;
        AddressMode addressModeV = AddressMode::REPEAT;
        AddressMode addressModeW = AddressMode::REPEAT;
        bool anisotropyEnable = true;
        float maxAnisotropy = 16.0f;
        BorderColor borderColor = BorderColor::INT_OPAQUE_BLACK;
        CompareOp compareOp = CompareOp::ALWAYS;
        bool compareEnable = false;
        f32 maxLod = 0.0f; // the highest mip level the sampler reaches: 0 samples the base level only
    };

    enum class VertexInputRate : u8
    {
        VERTEX,
        INSTANCE
    };

    struct VertexInputBindingDesc
    {
        u32 binding = 0;
        u32 stride = 0;
        VertexInputRate inputRate = VertexInputRate::VERTEX;

        bool operator==(const VertexInputBindingDesc& other) const = default;
    };

    struct VertexInputAttributeDesc
    {
        u32 location = 0;
        u32 binding = 0;
        Format format = Format::UNDEFINED;
        u32 offset = 0;
        InternedString name; // The name of the attribute from the shader source

        bool operator==(const VertexInputAttributeDesc& other) const = default;
    };

    struct ImageViewDesc
    {
        TextureHandle texture;
        Format format = Format::UNDEFINED; // Can be different from texture format for compatibility
        u32 baseMipLevel = 0;
        u32 levelCount = 1;
        u32 baseArrayLayer = 0;
        u32 layerCount = 1;
        bool array = false; // a 2D_ARRAY view (Texture2DArray in shaders), even over one layer
        bool cube = false; // a CUBE view (TextureCube in shaders): layerCount 6 over a cube texture; not with array
    };

    struct AttachmentDesc
    {
        Format format = Format::UNDEFINED;
        AttachmentLoadOp loadOp = AttachmentLoadOp::DONT_CARE;
        AttachmentStoreOp storeOp = AttachmentStoreOp::DONT_CARE;
        ImageLayout initialLayout = ImageLayout::UNDEFINED;
        ImageLayout finalLayout = ImageLayout::UNDEFINED;

        bool operator==(const AttachmentDesc& other) const = default;
    };

    struct AttachmentReference
    {
        u32 attachmentIndex = -1; // Index into the RenderPass's attachment array
        ImageLayout layout = ImageLayout::UNDEFINED;

        bool operator==(const AttachmentReference& other) const = default;
    };

    struct SubpassDependencyDesc
    {
        u32 srcSubpass = -1; // VK_SUBPASS_EXTERNAL
        u32 dstSubpass = -1;
        PipelineStage srcStageMask = PipelineStage::TOP_OF_PIPE;
        PipelineStage dstStageMask = PipelineStage::BOTTOM_OF_PIPE;
        AccessFlags srcAccessMask = AccessFlags::NONE;
        AccessFlags dstAccessMask = AccessFlags::NONE;

        bool operator==(const SubpassDependencyDesc& other) const = default;
    };

    struct BlendStateDesc
    {
        bool blendEnable = false;
        BlendFactor srcColorBlendFactor = BlendFactor::ONE;
        BlendFactor dstColorBlendFactor = BlendFactor::ZERO;
        BlendOp colorBlendOp = BlendOp::ADD;
        BlendFactor srcAlphaBlendFactor = BlendFactor::ONE;
        BlendFactor dstAlphaBlendFactor = BlendFactor::ZERO;
        BlendOp alphaBlendOp = BlendOp::ADD;

        bool operator==(const BlendStateDesc& other) const = default;
    };

    struct PushConstantRange
    {
        ShaderStage shaderStages = ShaderStage::NONE;
        u32 offset = 0;
        u32 size = 0;

        bool operator==(const PushConstantRange& other) const = default;
    };

    // Mirrors VkDrawIndexedIndirectCommand
    struct DrawIndexedIndirectCommand
    {
        u32 indexCount;
        u32 instanceCount;
        u32 firstIndex;
        i32 vertexOffset;
        u32 firstInstance;
    };

    // Mirrors VkDrawIndirectCommand
    struct DrawIndirectCommand
    {
        u32 vertexCount;
        u32 instanceCount;
        u32 firstVertex;
        u32 firstInstance;
    };

    // Mirrors VkDrawMeshTasksIndirectCommandEXT
    struct DrawMeshTasksIndirectCommand
    {
        u32 groupCountX;
        u32 groupCountY;
        u32 groupCountZ;
    };
} // namespace spite
