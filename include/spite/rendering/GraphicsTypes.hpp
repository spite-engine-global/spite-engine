#pragma once
#include <type_traits>

#include "base/Platform.hpp"

// Defines a set of bitwise operators for a strongly-typed enum class, allowing it to be used as a bitmask.
#define SPITE_ENABLE_BITMASK_OPERATORS(E)                                                                              \
    inline E operator|(E lhs, E rhs)                                                                                   \
    {                                                                                                                  \
        using T = std::underlying_type_t<E>;                                                                           \
        return static_cast<E>(static_cast<T>(lhs) | static_cast<T>(rhs));                                              \
    }                                                                                                                  \
    inline E operator&(E lhs, E rhs)                                                                                   \
    {                                                                                                                  \
        using T = std::underlying_type_t<E>;                                                                           \
        return static_cast<E>(static_cast<T>(lhs) & static_cast<T>(rhs));                                              \
    }                                                                                                                  \
    inline E& operator|=(E& lhs, E rhs)                                                                                \
    {                                                                                                                  \
        lhs = lhs | rhs;                                                                                               \
        return lhs;                                                                                                    \
    }                                                                                                                  \
    inline E& operator&=(E& lhs, E rhs)                                                                                \
    {                                                                                                                  \
        lhs = lhs & rhs;                                                                                               \
        return lhs;                                                                                                    \
    }                                                                                                                  \
    inline bool has_flag(E lhs, E rhs) { return (lhs & rhs) == rhs; }

namespace spite
{
    // A comprehensive, API-agnostic set of enums for rendering.

    enum class PipelineBindPoint : u8
    {
        GRAPHICS,
        COMPUTE
    };

    enum class Format : u8
    {
        UNDEFINED,
        // 8-bit formats
        R8_UNORM,
        R8G8_UNORM,
        R8G8B8A8_UNORM,
        R8G8B8A8_SRGB,
        B8G8R8A8_UNORM,
        B8G8R8A8_SRGB,
        // 16-bit formats
        R16_SFLOAT,
        R16G16_SFLOAT,
        R16G16B16A16_SFLOAT,
        // 32-bit formats
        R32_UINT,
        R32_SFLOAT,
        R32G32_SFLOAT,
        R32G32B32_SFLOAT,
        R32G32B32A32_SFLOAT,
        R32G32B32A32_UINT,
        // Depth/stencil formats
        D32_SFLOAT,
        D24_UNORM_S8_UINT,
        D32_SFLOAT_S8_UINT,
        D16_UNORM,
        // appended after the depth formats: a baked texture header stores the numeric value
        R16G16_UNORM,
        B10G11R11_UFLOAT, // packed, unsigned, no alpha: HDR colour in 4 bytes
        E5B9G9R9_UFLOAT, // shared 5-bit exponent, three 9-bit mantissas: sampled HDR colour in 4 bytes, never stored to
        // block-compressed (4x4 texel blocks of 8 bytes, BC5 and BC7 of 16): sampled only, never a storage or
        // attachment image; a texture upload names whole blocks. The baked texture header stores the number.
        BC1_RGB_UNORM,
        BC1_RGB_SRGB,
        BC4_UNORM, // one channel (r)
        BC5_UNORM, // two channels (r, g)
        BC7_UNORM,
        BC7_SRGB,
    };

    // One past the last Format enumerator: tables indexed by Format size themselves from it. Move it when appending.
    // Appending a Format also grows RenderDeviceCapabilities (it embeds an array of this size): an ABI change.
    inline constexpr u32 FORMAT_COUNT = static_cast<u32>(Format::BC7_SRGB) + 1;

    enum class BufferUsage : u32
    {
        NONE = 0,
        TRANSFER_SRC = 1 << 0,
        TRANSFER_DST = 1 << 1,
        UNIFORM_BUFFER = 1 << 2,
        STORAGE_BUFFER = 1 << 3,
        INDEX_BUFFER = 1 << 4,
        VERTEX_BUFFER = 1 << 5,
        INDIRECT_BUFFER = 1 << 6,
        SHADER_DEVICE_ADDRESS = 1 << 9
    };

    SPITE_ENABLE_BITMASK_OPERATORS(BufferUsage);

    enum class TextureUsage : u32
    {
        NONE = 0,
        TRANSFER_SRC = 1 << 0,
        TRANSFER_DST = 1 << 1,
        SAMPLED = 1 << 2,
        STORAGE = 1 << 3,
        COLOR_ATTACHMENT = 1 << 4,
        DEPTH_STENCIL_ATTACHMENT = 1 << 5,
        INPUT_ATTACHMENT = 1 << 6,
    };

    SPITE_ENABLE_BITMASK_OPERATORS(TextureUsage);

    enum class MemoryUsage : u8
    {
        UNKNOWN,
        GPU_ONLY,
        CPU_ONLY,
        CPU_TO_GPU, // Mapped, coherent
        GPU_TO_CPU, // Mapped, cached
    };

    inline const char* to_string(MemoryUsage e)
    {
        switch (e)
        {
        case MemoryUsage::UNKNOWN:
            return "UNKNOWN";
        case MemoryUsage::GPU_ONLY:
            return "GPU_ONLY";
        case MemoryUsage::CPU_ONLY:
            return "CPU_ONLY";
        case MemoryUsage::CPU_TO_GPU:
            return "CPU_TO_GPU";
        case MemoryUsage::GPU_TO_CPU:
            return "GPU_TO_CPU";
        default:
            return "unknown";
        }
    }

    enum class DescriptorType
    {
        UNIFORM_BUFFER,
        STORAGE_BUFFER,
        SAMPLED_IMAGE,
        SAMPLER,
        COMBINED_TEXTURE_SAMPLER,
        SAMPLED_TEXTURE,
        STORAGE_TEXTURE
    };

    inline const char* to_string(DescriptorType e)
    {
        switch (e)
        {
        case DescriptorType::UNIFORM_BUFFER:
            return "UNIFORM_BUFFER";
        case DescriptorType::STORAGE_BUFFER:
            return "STORAGE_BUFFER";
        case DescriptorType::SAMPLED_IMAGE:
            return "SAMPLED_IMAGE";
        case DescriptorType::SAMPLED_TEXTURE:
            return "SAMPLED_TEXTURE";
        case DescriptorType::STORAGE_TEXTURE:
            return "STORAGE_TEXTURE";
        case DescriptorType::SAMPLER:
            return "SAMPLER";
        case DescriptorType::COMBINED_TEXTURE_SAMPLER:
            return "COMBINED_TEXTURE_SAMPLER";
        default:
            return "unknown";
        }
    }

    enum class CullMode : u8
    {
        NONE,
        FRONT,
        BACK,
        FRONT_AND_BACK
    };

    enum class CompareOp : u8
    {
        NEVER,
        LESS,
        EQUAL,
        LESS_OR_EQUAL,
        GREATER,
        NOT_EQUAL,
        GREATER_OR_EQUAL,
        ALWAYS
    };

    enum class AttachmentLoadOp : u8
    {
        LOAD,
        CLEAR,
        DONT_CARE
    };

    enum class AttachmentStoreOp : u8
    {
        STORE,
        DONT_CARE,
        NONE
    };

    enum class ImageLayout : u8
    {
        UNDEFINED,
        GENERAL,
        COLOR_ATTACHMENT_OPTIMAL,
        DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        SHADER_READ_ONLY_OPTIMAL,
        TRANSFER_SRC_OPTIMAL,
        TRANSFER_DST_OPTIMAL,
        PRESENT_SRC
    };

    enum class PrimitiveTopology : u8
    {
        POINT_LIST,
        LINE_LIST,
        LINE_STRIP,
        TRIANGLE_LIST,
        TRIANGLE_STRIP,
        TRIANGLE_FAN
    };

    enum class PolygonMode : u8
    {
        FILL,
        LINE,
        POINT
    };

    enum class BlendFactor : u8
    {
        ZERO,
        ONE,
        SRC_COLOR,
        ONE_MINUS_SRC_COLOR,
        DST_COLOR,
        ONE_MINUS_DST_COLOR,
        SRC_ALPHA,
        ONE_MINUS_SRC_ALPHA,
        DST_ALPHA,
        ONE_MINUS_DST_ALPHA
    };

    enum class BlendOp : u8
    {
        ADD,
        SUBTRACT,
        REVERSE_SUBTRACT,
        MIN,
        MAX
    };

    enum class FrontFace : u8
    {
        COUNTER_CLOCKWISE,
        CLOCKWISE
    };

    enum class Filter : u8
    {
        NEAREST,
        LINEAR
    };

    enum class AddressMode : u8
    {
        REPEAT,
        MIRRORED_REPEAT,
        CLAMP_TO_EDGE,
        CLAMP_TO_BORDER
    };

    enum class BorderColor : u8
    {
        FLOAT_TRANSPARENT_BLACK,
        INT_TRANSPARENT_BLACK,
        FLOAT_OPAQUE_BLACK,
        INT_OPAQUE_BLACK,
        FLOAT_OPAQUE_WHITE,
        INT_OPAQUE_WHITE
    };

    enum class ShaderStage : u32
    {
        NONE = 0,
        VERTEX = 1 << 0,
        FRAGMENT = 1 << 1,
        COMPUTE = 1 << 2,
        GEOMETRY = 1 << 3,
        TESSELLATION_CONTROL = 1 << 4,
        TESSELLATION_EVALUATION = 1 << 5,
        TASK = 1 << 6,
        MESH = 1 << 7,
        ALL_GRAPHICS = VERTEX | FRAGMENT | GEOMETRY | TESSELLATION_CONTROL | TESSELLATION_EVALUATION | TASK | MESH,
        ALL = ALL_GRAPHICS | COMPUTE
    };

    SPITE_ENABLE_BITMASK_OPERATORS(ShaderStage);

    enum class PipelineStage : u32
    {
        NONE = 0,
        TOP_OF_PIPE = 1 << 0,
        DRAW_INDIRECT = 1 << 1,
        VERTEX_INPUT = 1 << 2,
        VERTEX_SHADER = 1 << 3,
        TESSELLATION_CONTROL_SHADER = 1 << 4,
        TESSELLATION_EVALUATION_SHADER = 1 << 5,
        GEOMETRY_SHADER = 1 << 6,
        FRAGMENT_SHADER = 1 << 7,
        EARLY_FRAGMENT_TESTS = 1 << 8,
        LATE_FRAGMENT_TESTS = 1 << 9,
        COLOR_ATTACHMENT_OUTPUT = 1 << 10,
        COMPUTE_SHADER = 1 << 11,
        TRANSFER = 1 << 12,
        BOTTOM_OF_PIPE = 1 << 13,
        HOST = 1 << 14,
        ALL_GRAPHICS = 1 << 15,
        ALL_COMMANDS = 1 << 16,
        TASK_SHADER = 1 << 17,
        MESH_SHADER = 1 << 18,
        CLEAR = 1 << 19, // vkCmdClear*/vkCmdFillBuffer under sync2
    };

    SPITE_ENABLE_BITMASK_OPERATORS(PipelineStage);

    enum class AccessFlags : u32
    {
        NONE = 0,
        INDIRECT_COMMAND_READ = 1 << 0,
        INDEX_READ = 1 << 1,
        VERTEX_ATTRIBUTE_READ = 1 << 2,
        UNIFORM_READ = 1 << 3,
        INPUT_ATTACHMENT_READ = 1 << 4,
        SHADER_READ = 1 << 5,
        SHADER_WRITE = 1 << 6,
        COLOR_ATTACHMENT_READ = 1 << 7,
        COLOR_ATTACHMENT_WRITE = 1 << 8,
        DEPTH_STENCIL_ATTACHMENT_READ = 1 << 9,
        DEPTH_STENCIL_ATTACHMENT_WRITE = 1 << 10,
        TRANSFER_READ = 1 << 11,
        TRANSFER_WRITE = 1 << 12,
        HOST_READ = 1 << 13,
        HOST_WRITE = 1 << 14,
        MEMORY_READ = 1 << 15,
        MEMORY_WRITE = 1 << 16,
        SHADER_STORAGE_READ = 1 << 17,
        SHADER_STORAGE_WRITE = 1 << 18
    };

    SPITE_ENABLE_BITMASK_OPERATORS(AccessFlags);

    enum class DescriptorBindingFlags : u32
    {
        NONE = 0,
        UPDATE_AFTER_BIND = 1 << 0,
        UPDATE_UNUSED_WHILE_PENDING = 1 << 1,
        PARTIALLY_BOUND = 1 << 2,
        VARIABLE_DESCRIPTOR_COUNT = 1 << 3
    };

    SPITE_ENABLE_BITMASK_OPERATORS(DescriptorBindingFlags);

    enum class SharingMode : u32
    {
        CONCURRENT,
        EXCLUSIVE
    };

} // namespace spite
