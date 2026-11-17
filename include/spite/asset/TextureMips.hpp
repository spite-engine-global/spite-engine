#pragma once
#include <bit>
#include <vector>
#include "asset/AssetExport.hpp"
#include "asset/MaterialTypes.hpp"
#include "asset/TextureFormat.hpp"
#include "base/MathTypes.hpp"

namespace spite
{
    // How a baked chain's levels were averaged (stored in SpiteTextureHeader::mipFilter). SRGB decodes the colour
    // channels to linear before averaging and encodes the result; LINEAR averages the bytes as they are (data
    // textures); NORMAL averages tangent-space vectors and renormalises them (a normal map is linear data).
    // SRGB_ALPHA weights colour by alpha before averaging and stores straight colour; alpha is averaged linearly.
    enum class BakedTextureFilter : u8
    {
        SRGB = 0,
        LINEAR = 1,
        NORMAL = 2,
        SRGB_ALPHA = 3
    };

    constexpr BakedTextureFilter terrainFilter(const TerrainTextureSlot slot)
    {
        if (terrainTextureSpace(slot) == TextureColorSpace::SRGB)
            return BakedTextureFilter::SRGB;
        if (slot == TerrainTextureSlot::GrassNormal || slot == TerrainTextureSlot::SoilNormal ||
            slot == TerrainTextureSlot::RockNormal)
            return BakedTextureFilter::NORMAL;
        return BakedTextureFilter::LINEAR;
    }

    constexpr bool isSrgbFilter(const BakedTextureFilter filter)
    {
        return filter == BakedTextureFilter::SRGB || filter == BakedTextureFilter::SRGB_ALPHA;
    }

    // The filter a texture bound to a slot in the given space wants: a normal map in a linear slot renormalises.
    constexpr BakedTextureFilter bakedFilterFor(const MaterialTextureSlot slot, const TextureColorSpace space)
    {
        if (space == TextureColorSpace::SRGB)
        {
            return BakedTextureFilter::SRGB;
        }
        return slot == MaterialTextureSlot::Normal ? BakedTextureFilter::NORMAL : BakedTextureFilter::LINEAR;
    }

    // A chain filtered one way serves a slot space that wants the other only when both read the bytes as linear.
    constexpr bool bakedFilterServes(const BakedTextureFilter filter, const TextureColorSpace space)
    {
        return isSrgbFilter(filter) == (space == TextureColorSpace::SRGB);
    }

    constexpr u32 textureMipCount(const u32 width, const u32 height)
    {
        return static_cast<u32>(std::bit_width(width > height ? width : height));
    }

    constexpr u32 mipExtent(const u32 base, const u32 level) { return (base >> level) > 0 ? base >> level : 1u; }

    // bytes of a tightly packed RGBA8 chain of the first `levels` levels
    SPITE_ASSET_API sizet mipChainBytes(u32 width, u32 height, u32 levels);

    // bytes of a tightly packed chain of the first `levels` levels in a baked format: each level whole blocks
    constexpr sizet bakedTextureChainBytes(const u8 format, const u32 width, const u32 height, const u32 levels)
    {
        sizet total = 0;
        for (u32 level = 0; level < levels; ++level)
        {
            total += bakedTextureLevelBytes(format, mipExtent(width, level), mipExtent(height, level));
        }
        return total;
    }

    // The first `levels` levels (0: every level) of an RGBA8 image (level 0 copied first), tightly packed, down to 1x1.
    // Each texel of a level averages the area of the level above it that it covers (odd extents weigh the shared texels
    // by their overlap), in an order that depends only on the input: the same pixels give the same bytes.
    SPITE_ASSET_API std::vector<u8> buildMipChain(const u8* level0, u32 width, u32 height, BakedTextureFilter filter,
                                                  u32 levels = 0);
} // namespace spite
