#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <type_traits>
#include <cmath>

#include "base/Schema.hpp"

namespace spite
{
    template <typename T>
    [[nodiscard]] inline bool validTerrainLayerScale(T scale, bool allowZero = true)
    {
        // Compare IEEE representations so fast floating point cannot flush a subnormal to zero.
        using Bits = std::conditional_t<sizeof(T) == sizeof(std::uint32_t), std::uint32_t, std::uint64_t>;
        const Bits bits = std::bit_cast<Bits>(scale);
        const Bits sign = Bits{1} << (sizeof(T) * 8 - 1);
        const Bits magnitude = bits & ~sign;
        return (allowZero && magnitude == 0) ||
            (!(bits & sign) && magnitude >= std::bit_cast<Bits>(static_cast<T>(0.01)) &&
             magnitude <= std::bit_cast<Bits>(T{10000}));
    }

    // A texture's colour space belongs to the slot that samples it, not to the file: it is the TEXTURE handle's
    // subAssetIndex, so one image sampled in two spaces is two registry entries and in one space is shared.
    enum class TextureColorSpace : u32
    {
        SRGB = 0, // base colour, emissive (and the default of a texture loaded without a slot)
        LINEAR = 1 // data: normal, metallic-roughness, occlusion
    };
    SPITE_ENUM_NAMES(TextureColorSpace, SRGB, LINEAR)

    // glTF alphaMode: Opaque ignores alpha, Mask discards below the cutoff, Blend composites. Premultiplied and
    // Additive are blend modes beyond glTF: the Transparent pass draws all three through one blend
    // equation, src + dst * (1 - src.a): Blend multiplies the lit colour by alpha, Premultiplied takes the lit colour
    // as it is (the albedo carries the alpha, so specular stays whole: glass), Additive multiplies by alpha and adds
    // (the surface darkens nothing behind it). They share the blend class, so the cull routes them with Blend. The mode
    // keeps the values glTF's three have, so a baked material of one of those reads as before.
    enum class MaterialAlphaMode : u32
    {
        Opaque = 0,
        Mask = 1,
        Blend = 2,
        Premultiplied = 3,
        Additive = 4
    };
    SPITE_ENUM_NAMES(MaterialAlphaMode, Opaque, Mask, Blend, Premultiplied, Additive)

    // glTF sampler wrapS and wrapT. The values are the wrap numbers of the material sampler state key
    // (shaders/source/shared/bindless.slang).
    enum class TextureWrap : u32
    {
        Repeat = 0,
        MirroredRepeat = 1,
        ClampToEdge = 2
    };
    SPITE_ENUM_NAMES(TextureWrap, Repeat, MirroredRepeat, ClampToEdge)

    // Smooth is the engine's own: trilinear over the mip chain with anisotropy. Linear and Nearest read the base level
    // only, as a glTF sampler without mip filtering (or with a nearest magnification) does.
    enum class TextureFilter : u32
    {
        Smooth = 0,
        Linear = 1,
        Nearest = 2
    };
    SPITE_ENUM_NAMES(TextureFilter, Smooth, Linear, Nearest)

    // How a material slot's texture is sampled. A default state is the engine's one sampler.
    struct MaterialSamplerState
    {
        TextureWrap wrapU = TextureWrap::Repeat;
        TextureWrap wrapV = TextureWrap::Repeat;
        TextureFilter filter = TextureFilter::Smooth;

        bool operator==(const MaterialSamplerState&) const = default;
    };

    // The key the GPU material holds and the bindless sampler table is indexed by
    constexpr u32 materialSamplerKey(const MaterialSamplerState& state)
    {
        return static_cast<u32>(state.filter) + 3 * (static_cast<u32>(state.wrapV) + 3 * static_cast<u32>(state.wrapU));
    }

    // The texture inputs of a metallic-roughness material, in the order the baked file and the tools list them.
    enum class MaterialTextureSlot : u8
    {
        Albedo, // base colour
        Normal,
        MetallicRoughness, // glTF packing: green roughness, blue metallic
        Occlusion,
        Emissive,
    };

    constexpr u32 MATERIAL_TEXTURE_SLOT_COUNT = 5;

    // Fixed terrain profile: one distribution map and three matched PBR surfaces.
    enum class TerrainTextureSlot : u8
    {
        Mask,
        GrassAlbedo,
        GrassNormal,
        GrassRoughness,
        SoilAlbedo,
        SoilNormal,
        SoilRoughness,
        RockAlbedo,
        RockNormal,
        RockRoughness
    };
    constexpr u32 TERRAIN_TEXTURE_SLOT_COUNT = 10;
    constexpr std::array<TerrainTextureSlot, TERRAIN_TEXTURE_SLOT_COUNT> TERRAIN_TEXTURE_SLOTS{
        TerrainTextureSlot::Mask,           TerrainTextureSlot::GrassAlbedo, TerrainTextureSlot::GrassNormal,
        TerrainTextureSlot::GrassRoughness, TerrainTextureSlot::SoilAlbedo,  TerrainTextureSlot::SoilNormal,
        TerrainTextureSlot::SoilRoughness,  TerrainTextureSlot::RockAlbedo,  TerrainTextureSlot::RockNormal,
        TerrainTextureSlot::RockRoughness};

    constexpr TextureColorSpace terrainTextureSpace(const TerrainTextureSlot slot)
    {
        return slot == TerrainTextureSlot::GrassAlbedo || slot == TerrainTextureSlot::SoilAlbedo ||
                slot == TerrainTextureSlot::RockAlbedo
            ? TextureColorSpace::SRGB
            : TextureColorSpace::LINEAR;
    }

    constexpr std::array<MaterialTextureSlot, MATERIAL_TEXTURE_SLOT_COUNT> MATERIAL_TEXTURE_SLOTS{
        MaterialTextureSlot::Albedo, MaterialTextureSlot::Normal, MaterialTextureSlot::MetallicRoughness,
        MaterialTextureSlot::Occlusion, MaterialTextureSlot::Emissive};

    constexpr TextureColorSpace defaultTextureSpace(const MaterialTextureSlot slot)
    {
        return slot == MaterialTextureSlot::Albedo || slot == MaterialTextureSlot::Emissive ? TextureColorSpace::SRGB
                                                                                            : TextureColorSpace::LINEAR;
    }
} // namespace spite
