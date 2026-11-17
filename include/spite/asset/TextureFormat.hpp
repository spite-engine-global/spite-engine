#pragma once
#include "asset/MaterialTypes.hpp"
#include "base/MathTypes.hpp"

namespace spite
{
    // Numeric texture-format ids stored in the bake file contract; the values are the Format enum's in
    // rendering/GraphicsTypes.hpp (GpuAssetFactory checks them). The asset module sits below rendering and never
    // includes it. The two RGBA8 ids are the same bytes: a texture's colour space picks one at upload.
    constexpr u8 BAKED_TEXTURE_FORMAT_R8G8B8A8_UNORM = 3;
    constexpr u8 BAKED_TEXTURE_FORMAT_R8G8B8A8_SRGB = 4;
    constexpr u8 BAKED_TEXTURE_FORMAT_BC1_RGB_UNORM = 23;
    constexpr u8 BAKED_TEXTURE_FORMAT_BC1_RGB_SRGB = 24;
    constexpr u8 BAKED_TEXTURE_FORMAT_BC4_UNORM = 25;
    constexpr u8 BAKED_TEXTURE_FORMAT_BC5_UNORM = 26;
    constexpr u8 BAKED_TEXTURE_FORMAT_BC7_UNORM = 27;
    constexpr u8 BAKED_TEXTURE_FORMAT_BC7_SRGB = 28;

    // How a texture is stored when it is baked (the importer's `textures` option). Auto: BC7 for colour (sRGB
    // encoded) and for data, BC5 for tangent-space normals (the shader rebuilds z); Fast: opaque colour as BC1, the
    // rest as Auto; Uncompressed: RGBA8; SingleChannel: BC4 of the red channel, for data nothing reads past r (a
    // caller's choice: the importer never picks it, an ORM image is read in g and b too).
    enum class TextureCompression : u8
    {
        Auto = 0,
        Fast = 1,
        Uncompressed = 2,
        SingleChannel = 3
    };

    constexpr bool bakedTextureFormatCompressed(const u8 format)
    {
        return format >= BAKED_TEXTURE_FORMAT_BC1_RGB_UNORM && format <= BAKED_TEXTURE_FORMAT_BC7_SRGB;
    }

    constexpr bool bakedTextureFormatKnown(const u8 format)
    {
        return format == BAKED_TEXTURE_FORMAT_R8G8B8A8_SRGB || bakedTextureFormatCompressed(format);
    }

    // The texel block of a format: 4x4 texels when compressed, else one.
    constexpr u32 bakedTextureBlockExtent(const u8 format) { return bakedTextureFormatCompressed(format) ? 4u : 1u; }

    // Bytes of one block: 8 for BC1 and BC4, 16 for BC5 and BC7, 4 for a texel of RGBA8.
    constexpr u32 bakedTextureBlockBytes(const u8 format)
    {
        switch (format)
        {
        case BAKED_TEXTURE_FORMAT_BC1_RGB_UNORM:
        case BAKED_TEXTURE_FORMAT_BC1_RGB_SRGB:
        case BAKED_TEXTURE_FORMAT_BC4_UNORM:
            return 8;
        case BAKED_TEXTURE_FORMAT_BC5_UNORM:
        case BAKED_TEXTURE_FORMAT_BC7_UNORM:
        case BAKED_TEXTURE_FORMAT_BC7_SRGB:
            return 16;
        default:
            return 4;
        }
    }

    // Bytes of one mip level: whole blocks, so a level below 4x4 still takes one block of a compressed format.
    constexpr sizet bakedTextureLevelBytes(const u8 format, const u32 width, const u32 height)
    {
        const u32 extent = bakedTextureBlockExtent(format);
        return sizet{(width + extent - 1) / extent} * ((height + extent - 1) / extent) * bakedTextureBlockBytes(format);
    }

    // The id the GPU texture of a baked format takes in a colour space: an RGBA8, BC1 or BC7 chain is read as sRGB
    // or as linear bytes; BC4 and BC5 hold data and are linear in either.
    constexpr u8 bakedTextureFormatInSpace(const u8 format, const TextureColorSpace space)
    {
        const bool srgb = space == TextureColorSpace::SRGB;
        switch (format)
        {
        case BAKED_TEXTURE_FORMAT_BC1_RGB_UNORM:
        case BAKED_TEXTURE_FORMAT_BC1_RGB_SRGB:
            return srgb ? BAKED_TEXTURE_FORMAT_BC1_RGB_SRGB : BAKED_TEXTURE_FORMAT_BC1_RGB_UNORM;
        case BAKED_TEXTURE_FORMAT_BC7_UNORM:
        case BAKED_TEXTURE_FORMAT_BC7_SRGB:
            return srgb ? BAKED_TEXTURE_FORMAT_BC7_SRGB : BAKED_TEXTURE_FORMAT_BC7_UNORM;
        case BAKED_TEXTURE_FORMAT_BC4_UNORM:
        case BAKED_TEXTURE_FORMAT_BC5_UNORM:
            return format;
        default:
            return srgb ? BAKED_TEXTURE_FORMAT_R8G8B8A8_SRGB : BAKED_TEXTURE_FORMAT_R8G8B8A8_UNORM;
        }
    }

    // The format as a texture row shows it: the family and, where the encoding has one, its colour space.
    constexpr const char* bakedTextureFormatName(const u8 format)
    {
        switch (format)
        {
        case BAKED_TEXTURE_FORMAT_R8G8B8A8_UNORM:
            return "RGBA8 UNORM";
        case BAKED_TEXTURE_FORMAT_R8G8B8A8_SRGB:
            return "RGBA8 sRGB";
        case BAKED_TEXTURE_FORMAT_BC1_RGB_UNORM:
            return "BC1 UNORM";
        case BAKED_TEXTURE_FORMAT_BC1_RGB_SRGB:
            return "BC1 sRGB";
        case BAKED_TEXTURE_FORMAT_BC4_UNORM:
            return "BC4";
        case BAKED_TEXTURE_FORMAT_BC5_UNORM:
            return "BC5";
        case BAKED_TEXTURE_FORMAT_BC7_UNORM:
            return "BC7 UNORM";
        case BAKED_TEXTURE_FORMAT_BC7_SRGB:
            return "BC7 sRGB";
        default:
            return "unknown";
        }
    }
} // namespace spite
