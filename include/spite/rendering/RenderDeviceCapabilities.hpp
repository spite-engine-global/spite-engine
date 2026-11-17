#pragma once

#include <array>

#include "rendering/GraphicsTypes.hpp"

namespace spite
{
    enum class RenderDeviceFeature : u64
    {
        NONE = 0,
        MESH_SHADER = 1ULL << 0,
        CALIBRATED_TIMESTAMPS = 1ULL << 2,
        DRAW_INDIRECT_COUNT = 1ULL << 3,
        FILL_MODE_NON_SOLID = 1ULL << 4,
        SOFTWARE_ADAPTER = 1ULL << 5,
    };

    SPITE_ENABLE_BITMASK_OPERATORS(RenderDeviceFeature);

    struct RenderDeviceCapabilities
    {
        RenderDeviceFeature features = RenderDeviceFeature::NONE;
        u32 maxImageDimension2D = 0;

        // The texture usages each Format supports on this device, from the device's format features at creation
        // (indexed by Format). Transfer and input-attachment usage are always set: they are not checked.
        // createTexture refuses a usage a format lacks, so a caller with a choice of formats asks first.
        std::array<TextureUsage, FORMAT_COUNT> formatUsage{};

        bool hasFeature(RenderDeviceFeature feature) const { return has_flag(features, feature); }

        bool supportsTextureUsage(Format format, TextureUsage usage) const
        {
            return has_flag(formatUsage[static_cast<u32>(format)], usage);
        }

        void setFeature(RenderDeviceFeature feature, bool enabled = true)
        {
            if (enabled)
            {
                features = features | feature;
            }
        }
    };
} // namespace spite
