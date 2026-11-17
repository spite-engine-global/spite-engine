#pragma once
#include <vector>
#include "asset/AssetExport.hpp"
#include "asset/TextureFormat.hpp"
#include "asset/TextureMips.hpp"
#include "base/MathTypes.hpp"

namespace spite
{
    // Part of the content hash of every compressed bake: raise it when an encoder, its settings or the edge padding
    // changes the bytes a bake writes, so the cache never serves a bake an older encoder wrote.
    constexpr u32 TEXTURE_ENCODER_VERSION = 1;

    // The format a texture bakes to. `filter` is the class of slot that reads it (the chain's filter): colour is
    // sRGB-encoded BC7 (BC1 when Fast and `opaque`), a tangent-space normal map is BC5 (two channels: the shader
    // rebuilds z), other data is BC7 read as linear. Uncompressed stays RGBA8; SingleChannel is BC4 (red only).
    constexpr u8 bakedTextureFormatFor(const BakedTextureFilter filter, const TextureCompression compression,
                                       const bool opaque)
    {
        if (compression == TextureCompression::Uncompressed)
        {
            return BAKED_TEXTURE_FORMAT_R8G8B8A8_SRGB;
        }
        if (compression == TextureCompression::SingleChannel)
        {
            return BAKED_TEXTURE_FORMAT_BC4_UNORM;
        }
        if (filter == BakedTextureFilter::NORMAL)
        {
            return BAKED_TEXTURE_FORMAT_BC5_UNORM;
        }
        if (isSrgbFilter(filter))
        {
            return compression == TextureCompression::Fast && opaque ? BAKED_TEXTURE_FORMAT_BC1_RGB_SRGB
                                                                     : BAKED_TEXTURE_FORMAT_BC7_SRGB;
        }
        return BAKED_TEXTURE_FORMAT_BC7_UNORM;
    }

    // Encodes the first `levels` levels of a tightly packed RGBA8 chain (buildMipChain's layout) into `format`'s
    // blocks, level after level (bakedTextureChainBytes). A level whose extent is not a multiple of 4 is padded to
    // whole blocks with its edge texels. The bytes depend only on the chain and the format: never on `threads`
    // (0: one per hardware thread), which only splits the block rows between workers. BC4 reads the red channel,
    // BC5 red and green, BC1 and BC7 colour (BC7 alpha too). `format` must be a compressed baked format.
    SPITE_ASSET_API std::vector<u8> compressMipChain(const u8* rgbaChain, u32 width, u32 height, u32 levels, u8 format,
                                                     u32 threads = 0);

    // Decodes one level (width x height texels, whole blocks) to tight RGBA8 as a GPU samples the format: BC1 gets
    // alpha 255, BC5 reads b = 0 and a = 255, BC4 reads g = b = 0 and a = 255.
    SPITE_ASSET_API void decodeTextureLevel(u8 format, const u8* blocks, u32 width, u32 height, u8* rgba);
} // namespace spite
