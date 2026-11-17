#pragma once
#include <limits>
#include <span>
#include <vector>

#include "asset/AssetExport.hpp"
#include "asset/TextureMips.hpp"
#include "base/Expected.hpp"
#include "base/InternedString.hpp"
#include "base/Platform.hpp"

namespace spite
{
    // Numeric texture-format ids stored in a .spiteenv: the values of the Format enum in
    // rendering/GraphicsTypes.hpp (GpuAssetFactory static_asserts them).
    constexpr u8 BAKED_ENVIRONMENT_FORMAT_RGBA16F = 9; // R16G16B16A16_SFLOAT
    constexpr u8 BAKED_ENVIRONMENT_FORMAT_R11G11B10 = 21; // B10G11R11_UFLOAT
    constexpr u8 BAKED_ENVIRONMENT_FORMAT_RGB9E5 = 22; // E5B9G9R9_UFLOAT

    // Bumped whenever the .spiteenv layout changes; loaders reject other versions.
    constexpr u32 SPITE_ENVIRONMENT_VERSION = 1;

    // The .spiteenv layout: this header, then `mipLevels` tightly packed levels of an equirectangular image (the
    // convention of EnvironmentImport.hpp), largest first, down to the extent mipExtent gives each. Every byte is
    // written: the reserved bytes are zero, so equal sources bake to equal files.
    struct SpiteEnvironmentHeader
    {
        char magic[4] = {'S', 'P', 'E', 'V'};
        u32 version = SPITE_ENVIRONMENT_VERSION;
        u8 format = BAKED_ENVIRONMENT_FORMAT_RGBA16F;
        u8 reserved[3] = {};
        u32 width = 0;
        u32 height = 0;
        u32 mipLevels = 0;
    };
    static_assert(sizeof(SpiteEnvironmentHeader) == 4 + 4 + 4 + 3 * 4, "SpiteEnvironmentHeader must have no padding");

    // The name the asset commands show for a baked format and accept for importEnvironment's format argument.
    constexpr const char* bakedEnvironmentFormatName(const u8 format)
    {
        return format == BAKED_ENVIRONMENT_FORMAT_RGBA16F  ? "rgba16f"
            : format == BAKED_ENVIRONMENT_FORMAT_RGB9E5    ? "rgb9e5"
            : format == BAKED_ENVIRONMENT_FORMAT_R11G11B10 ? "r11g11b10"
                                                           : "unknown";
    }

    // Bytes per texel of a baked format, 0 for one the file cannot hold.
    constexpr u32 bakedEnvironmentTexelBytes(const u8 format)
    {
        return format == BAKED_ENVIRONMENT_FORMAT_RGBA16F                                               ? 8
            : format == BAKED_ENVIRONMENT_FORMAT_R11G11B10 || format == BAKED_ENVIRONMENT_FORMAT_RGB9E5 ? 4
                                                                                                        : 0;
    }

    // The largest width or height an environment has: what a baked file may declare and what an import produces (the
    // largest 2D texture the supported GPUs create).
    constexpr u32 MAX_ENVIRONMENT_EXTENT = 16384;

    // Bytes of the first `levels` levels of a width x height chain in the format; the largest u64 when the chain does
    // not fit in 64 bits (never a wrapped, smaller number: a header of an untrusted file may name any extent).
    inline u64 bakedEnvironmentChainBytes(const u32 width, const u32 height, const u32 levels, const u8 format)
    {
        const u64 texelBytes = bakedEnvironmentTexelBytes(format);
        u64 bytes = 0;
        for (u32 level = 0; level < levels; ++level)
        {
            const u64 texels = u64{mipExtent(width, level)} * mipExtent(height, level); // below 2^64: both are u32
            if (texelBytes != 0 && texels > std::numeric_limits<u64>::max() / texelBytes)
            {
                return std::numeric_limits<u64>::max();
            }
            const u64 levelBytes = texels * texelBytes;
            if (levelBytes > std::numeric_limits<u64>::max() - bytes)
            {
                return std::numeric_limits<u64>::max();
            }
            bytes += levelBytes;
        }
        return bytes;
    }

    // An environment as loaded: the equirect chain in its baked format, ready to upload as one region per level.
    class SPITE_ASSET_API EnvironmentAsset
    {
    public:
        EnvironmentAsset(InternedString name, u8 format, u32 width, u32 height, u32 mipLevels,
                         std::vector<std::byte>&& texels) :
            m_name(name), m_format(format), m_width(width), m_height(height), m_mipLevels(mipLevels),
            m_texels(std::move(texels))
        {
        }

        InternedString getName() const { return m_name; }
        u8 getFormat() const { return m_format; } // a BAKED_ENVIRONMENT_FORMAT_* id, also the Format enum's value
        u32 getWidth() const { return m_width; }
        u32 getHeight() const { return m_height; }
        u32 getMipLevels() const { return m_mipLevels; }
        const std::byte* getTexels() const { return m_texels.data(); }
        sizet getSizeInBytes() const { return m_texels.size(); }

    private:
        InternedString m_name;
        u8 m_format;
        u32 m_width;
        u32 m_height;
        u32 m_mipLevels;
        std::vector<std::byte> m_texels;
    };

    // The EnvironmentAsset a .spiteenv file holds; an unknown magic, version or format, a zero extent or one over
    // MAX_ENVIRONMENT_EXTENT, more levels than the extent has, or a size other than the chain's (computed without
    // overflow) is an error. Readers size every buffer from the header checked here, never from the raw fields of an
    // untrusted file. Nonzero reserved bytes are ignored and any extent within the limit is accepted, not only 2:1.
    SPITE_ASSET_API Expected<EnvironmentAsset> readEnvironmentFile(InternedString name,
                                                                   std::span<const std::byte> file);
} // namespace spite
