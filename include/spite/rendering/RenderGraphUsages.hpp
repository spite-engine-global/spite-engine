#pragma once
#include "rendering/GraphicsTypes.hpp"

namespace spite
{
    struct RGResourceHandle
    {
        u32 id = -1;

        bool isValid() const { return id != std::numeric_limits<u32>::max(); }
    };

    enum class RGResourceType : u8
    {
        Buffer,
        Texture
    };

    // Defines how a resource is used within a pass, which determines
    // the required layout, access flags, and pipeline stages for synchronization.
    struct RGResourceUsage
    {
        AccessFlags access;
        PipelineStage stage;
        // For textures only. For buffers, this is ignored.
        ImageLayout layout = ImageLayout::UNDEFINED;
    };

    // A record of how a resource is used in a pass.
    struct RGResourceAccess
    {
        RGResourceHandle handle;
        RGResourceUsage usage;
    };

    // Pre-defined common usages for convenience
    namespace RGUsage
    {
        // --- Texture Usages ---
        inline constexpr RGResourceUsage ColorAttachmentWrite = {.access = AccessFlags::COLOR_ATTACHMENT_WRITE,
                                                                 .stage = PipelineStage::COLOR_ATTACHMENT_OUTPUT,
                                                                 .layout = ImageLayout::COLOR_ATTACHMENT_OPTIMAL};

        inline RGResourceUsage DepthStencilAttachmentWrite = {
            .access = AccessFlags::DEPTH_STENCIL_ATTACHMENT_READ | AccessFlags::DEPTH_STENCIL_ATTACHMENT_WRITE,
            .stage = PipelineStage::EARLY_FRAGMENT_TESTS | PipelineStage::LATE_FRAGMENT_TESTS,
            .layout = ImageLayout::DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        inline RGResourceUsage DepthStencilAttachmentRead = {.access = AccessFlags::DEPTH_STENCIL_ATTACHMENT_READ,
                                                             .stage = PipelineStage::EARLY_FRAGMENT_TESTS |
                                                                 PipelineStage::LATE_FRAGMENT_TESTS,
                                                             .layout = ImageLayout::DEPTH_STENCIL_READ_ONLY_OPTIMAL};

        inline constexpr RGResourceUsage FragmentShaderReadSampled = {.access = AccessFlags::SHADER_READ,
                                                                      .stage = PipelineStage::FRAGMENT_SHADER,
                                                                      .layout = ImageLayout::SHADER_READ_ONLY_OPTIMAL};

        inline constexpr RGResourceUsage ComputeShaderReadSampled = {.access = AccessFlags::SHADER_READ,
                                                                     .stage = PipelineStage::COMPUTE_SHADER,
                                                                     .layout = ImageLayout::SHADER_READ_ONLY_OPTIMAL};

        inline constexpr RGResourceUsage ComputeShaderWriteStorage = {.access = AccessFlags::SHADER_WRITE,
                                                                      .stage = PipelineStage::COMPUTE_SHADER,
                                                                      .layout = ImageLayout::GENERAL};
        // a storage image read where its writer left it, with no layout change
        inline constexpr RGResourceUsage ComputeShaderReadStorage = {
            .access = AccessFlags::SHADER_READ, .stage = PipelineStage::COMPUTE_SHADER, .layout = ImageLayout::GENERAL};
        inline constexpr RGResourceUsage TaskShaderRead = {
            .access = AccessFlags::SHADER_READ,
            .stage = PipelineStage::TASK_SHADER,
        };
        inline constexpr RGResourceUsage TransferSrc = {.access = AccessFlags::TRANSFER_READ,
                                                        .stage = PipelineStage::TRANSFER,
                                                        .layout = ImageLayout::TRANSFER_SRC_OPTIMAL};

        inline constexpr RGResourceUsage TransferDst = {.access = AccessFlags::TRANSFER_WRITE,
                                                        .stage = PipelineStage::TRANSFER,
                                                        .layout = ImageLayout::TRANSFER_DST_OPTIMAL};

        inline constexpr RGResourceUsage Present = {
            .access = AccessFlags::NONE, // No access flags needed for present
            .stage = PipelineStage::BOTTOM_OF_PIPE, // Presentation happens at the end
            .layout = ImageLayout::PRESENT_SRC};

        // --- Buffer Usages ---
        inline constexpr RGResourceUsage VertexBufferRead = {
            .access = AccessFlags::VERTEX_ATTRIBUTE_READ,
            .stage = PipelineStage::VERTEX_INPUT,
        };

        inline constexpr RGResourceUsage IndexBufferRead = {
            .access = AccessFlags::INDEX_READ,
            .stage = PipelineStage::VERTEX_INPUT,
        };

        inline constexpr RGResourceUsage IndirectBufferRead = {
            .access = AccessFlags::INDIRECT_COMMAND_READ,
            .stage = PipelineStage::DRAW_INDIRECT,
        };
        inline RGResourceUsage StorageBufferWrite = {
            .access = AccessFlags::SHADER_WRITE,
            .stage = PipelineStage::ALL_GRAPHICS | PipelineStage::COMPUTE_SHADER,
        };
        // read-modify-write: atomic appends, persistent state rewritten in place
        inline RGResourceUsage StorageBufferReadWrite = {
            .access = AccessFlags::SHADER_READ | AccessFlags::SHADER_WRITE,
            .stage = PipelineStage::ALL_GRAPHICS | PipelineStage::COMPUTE_SHADER,
        };
        inline RGResourceUsage UniformBufferRead = {
            .access = AccessFlags::UNIFORM_READ,
            .stage = PipelineStage::ALL_GRAPHICS | PipelineStage::COMPUTE_SHADER,
        };
        inline RGResourceUsage StorageBufferRead = {
            .access = AccessFlags::SHADER_READ,
            .stage = PipelineStage::ALL_GRAPHICS | PipelineStage::COMPUTE_SHADER,
        };
    } // namespace RGUsage
} // namespace spite
