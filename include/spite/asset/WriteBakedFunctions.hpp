#pragma once

#include <cstddef>
#include <cmath>
#include <limits>
#include <string_view>
#include <tuple>
#include <span>
#include <vector>

#include "asset/AssetExport.hpp"
#include "asset/MaterialTypes.hpp"
#include "base/Expected.hpp"
#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "base/Schema.hpp"

namespace spite
{
    // A material's baked fields: what a .spitemat holds and the material control commands read and write. Texture
    // names are baked texture names; empty takes the slot's default. A factor multiplies its texture's sample (a slot
    // without a texture samples 1), so a bound metallic-roughness texture needs metallicFactor 1.
    // The file is one tag per schema field (MATERIAL_FILE_VERSION): a field added here is stored and read with no
    // codec change, and a file lacking a tag keeps the default below. A field's tag is the hash of its name, so a
    // rename is a format change.
    struct BakedMaterialWriteData
    {
        Vec4 albedoFactor{1.0f};
        f32 metallicFactor = 0.0f;
        f32 roughnessFactor = 1.0f;
        Vec3 emissiveFactor{0.0f};
        f32 emissiveStrength = 1.0f;
        f32 occlusionStrength = 1.0f;
        f32 normalScale = 1.0f;
        f32 thinLeafTransmission = 0.0f;
        MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
        f32 alphaCutoff = 0.5f;
        bool doubleSided = false;
        // A Blend material casts a shadow into the sun's cascades and the light maps, dithered by its alpha; any other
        // mode ignores it. The file leaves it out while false (omittedWhileFalse), so a material that never sets it
        // keeps its bytes and content hash.
        bool castsShadow = false;
        // KHR_materials_unlit: Light and Transparent output the base colour (plus the emission) with no lighting. Left
        // out of the file while false, like castsShadow.
        bool unlit = false;
        InternedString albedoTextureName;
        TextureColorSpace albedoTextureSpace = defaultTextureSpace(MaterialTextureSlot::Albedo);
        InternedString normalTextureName;
        TextureColorSpace normalTextureSpace = defaultTextureSpace(MaterialTextureSlot::Normal);
        InternedString metallicRoughnessTextureName;
        TextureColorSpace metallicRoughnessTextureSpace = defaultTextureSpace(MaterialTextureSlot::MetallicRoughness);
        InternedString occlusionTextureName;
        TextureColorSpace occlusionTextureSpace = defaultTextureSpace(MaterialTextureSlot::Occlusion);
        InternedString emissiveTextureName;
        TextureColorSpace emissiveTextureSpace = defaultTextureSpace(MaterialTextureSlot::Emissive);
        // KHR_texture_transform of every texture of the material (ParsedMaterialData::uvOffset)
        Vec2 uvOffset{0.0f};
        Vec2 uvScale{1.0f};
        f32 uvRotation = 0.0f;
        // How the textures are sampled: the filter of the whole material (a glTF file states one per texture; the
        // importer takes the base colour's), and each slot's wrap in u and v
        TextureFilter textureFilter = TextureFilter::Smooth;
        TextureWrap albedoWrapU = TextureWrap::Repeat;
        TextureWrap albedoWrapV = TextureWrap::Repeat;
        TextureWrap normalWrapU = TextureWrap::Repeat;
        TextureWrap normalWrapV = TextureWrap::Repeat;
        TextureWrap metallicRoughnessWrapU = TextureWrap::Repeat;
        TextureWrap metallicRoughnessWrapV = TextureWrap::Repeat;
        TextureWrap occlusionWrapU = TextureWrap::Repeat;
        TextureWrap occlusionWrapV = TextureWrap::Repeat;
        TextureWrap emissiveWrapU = TextureWrap::Repeat;
        TextureWrap emissiveWrapV = TextureWrap::Repeat;
        InternedString terrainMask;
        InternedString terrainGrassAlbedo;
        InternedString terrainGrassNormal;
        InternedString terrainGrassRoughness;
        InternedString terrainSoilAlbedo;
        InternedString terrainSoilNormal;
        InternedString terrainSoilRoughness;
        InternedString terrainRockAlbedo;
        InternedString terrainRockNormal;
        InternedString terrainRockRoughness;
        f32 terrainDetailMetres = 32.0f;
        f32 terrainGrassDetailMetres = 0.0f;
        f32 terrainSoilDetailMetres = 0.0f;
        f32 terrainRockDetailMetres = 0.0f;

        SPITE_SCHEMA(
            BakedMaterialWriteData,
            SPITE_FIELD(albedoFactor,
                        COLOR | fieldRange(0.0, std::numeric_limits<double>::infinity()) | fieldStep(0.01)),
            SPITE_FIELD(metallicFactor, fieldRange(0.0, 1.0) | fieldStep(0.01)),
            SPITE_FIELD(roughnessFactor, fieldRange(0.0, 1.0) | fieldStep(0.01)),
            SPITE_FIELD(emissiveFactor, COLOR | fieldRange(0.0, 1.0) | fieldStep(0.01)),
            SPITE_FIELD(emissiveStrength, fieldRange(0.0, std::numeric_limits<double>::infinity()) | fieldStep(0.1)),
            SPITE_FIELD(occlusionStrength, fieldRange(0.0, 1.0) | fieldStep(0.01)),
            SPITE_FIELD(normalScale, fieldStep(0.05)), alphaMode,
            SPITE_FIELD(alphaCutoff, fieldRange(0.0, 1.0) | fieldStep(0.01)), doubleSided, castsShadow, unlit,
            albedoTextureName, albedoTextureSpace, normalTextureName, normalTextureSpace, metallicRoughnessTextureName,
            metallicRoughnessTextureSpace, occlusionTextureName, occlusionTextureSpace, emissiveTextureName,
            emissiveTextureSpace, SPITE_FIELD(uvOffset, fieldStep(0.01)), SPITE_FIELD(uvScale, fieldStep(0.01)),
            SPITE_FIELD(uvRotation, fieldStep(0.01)), textureFilter, albedoWrapU, albedoWrapV, normalWrapU, normalWrapV,
            metallicRoughnessWrapU, metallicRoughnessWrapV, occlusionWrapU, occlusionWrapV, emissiveWrapU,
            emissiveWrapV, SPITE_FIELD(thinLeafTransmission, fieldRange(0.0, 1.0) | fieldStep(0.01)), terrainMask,
            terrainGrassAlbedo, terrainGrassNormal, terrainGrassRoughness, terrainSoilAlbedo, terrainSoilNormal,
            terrainSoilRoughness, terrainRockAlbedo, terrainRockNormal, terrainRockRoughness,
            SPITE_FIELD(terrainDetailMetres, fieldRange(0.01, 10000.0) | fieldStep(0.1)),
            SPITE_FIELD(terrainGrassDetailMetres, fieldRange(0.0, 10000.0) | fieldStep(0.1)),
            SPITE_FIELD(terrainSoilDetailMetres, fieldRange(0.0, 10000.0) | fieldStep(0.1)),
            SPITE_FIELD(terrainRockDetailMetres, fieldRange(0.0, 10000.0) | fieldStep(0.1)))

        [[nodiscard]] InternedString& terrainTextureName(const TerrainTextureSlot slot)
        {
            switch (slot)
            {
            case TerrainTextureSlot::GrassAlbedo:
                return terrainGrassAlbedo;
            case TerrainTextureSlot::GrassNormal:
                return terrainGrassNormal;
            case TerrainTextureSlot::GrassRoughness:
                return terrainGrassRoughness;
            case TerrainTextureSlot::SoilAlbedo:
                return terrainSoilAlbedo;
            case TerrainTextureSlot::SoilNormal:
                return terrainSoilNormal;
            case TerrainTextureSlot::SoilRoughness:
                return terrainSoilRoughness;
            case TerrainTextureSlot::RockAlbedo:
                return terrainRockAlbedo;
            case TerrainTextureSlot::RockNormal:
                return terrainRockNormal;
            case TerrainTextureSlot::RockRoughness:
                return terrainRockRoughness;
            default:
                return terrainMask;
            }
        }
        [[nodiscard]] const InternedString& terrainTextureName(const TerrainTextureSlot slot) const
        {
            return const_cast<BakedMaterialWriteData&>(*this).terrainTextureName(slot);
        }

        [[nodiscard]] bool validTerrain() const
        {
            u32 count = 0;
            for (const auto slot : TERRAIN_TEXTURE_SLOTS)
                count += terrainTextureName(slot).isValid() ? 1u : 0u;
            return validTerrainLayerScale(terrainGrassDetailMetres) &&
                validTerrainLayerScale(terrainSoilDetailMetres) && validTerrainLayerScale(terrainRockDetailMetres) &&
                (count == 0 ||
                 (count == TERRAIN_TEXTURE_SLOT_COUNT && albedoTextureName.isValid() && !normalTextureName.isValid() &&
                  alphaMode == MaterialAlphaMode::Opaque && validTerrainLayerScale(terrainDetailMetres, false)));
        }

        [[nodiscard]] InternedString& textureName(const MaterialTextureSlot slot)
        {
            switch (slot)
            {
            case MaterialTextureSlot::Normal:
                return normalTextureName;
            case MaterialTextureSlot::MetallicRoughness:
                return metallicRoughnessTextureName;
            case MaterialTextureSlot::Occlusion:
                return occlusionTextureName;
            case MaterialTextureSlot::Emissive:
                return emissiveTextureName;
            default:
                return albedoTextureName;
            }
        }
        [[nodiscard]] const InternedString& textureName(const MaterialTextureSlot slot) const
        {
            return const_cast<BakedMaterialWriteData&>(*this).textureName(slot);
        }
        [[nodiscard]] TextureColorSpace& textureSpace(const MaterialTextureSlot slot)
        {
            switch (slot)
            {
            case MaterialTextureSlot::Normal:
                return normalTextureSpace;
            case MaterialTextureSlot::MetallicRoughness:
                return metallicRoughnessTextureSpace;
            case MaterialTextureSlot::Occlusion:
                return occlusionTextureSpace;
            case MaterialTextureSlot::Emissive:
                return emissiveTextureSpace;
            default:
                return albedoTextureSpace;
            }
        }
        [[nodiscard]] TextureColorSpace textureSpace(const MaterialTextureSlot slot) const
        {
            return const_cast<BakedMaterialWriteData&>(*this).textureSpace(slot);
        }

        [[nodiscard]] MaterialSamplerState samplerState(const MaterialTextureSlot slot) const
        {
            switch (slot)
            {
            case MaterialTextureSlot::Normal:
                return {normalWrapU, normalWrapV, textureFilter};
            case MaterialTextureSlot::MetallicRoughness:
                return {metallicRoughnessWrapU, metallicRoughnessWrapV, textureFilter};
            case MaterialTextureSlot::Occlusion:
                return {occlusionWrapU, occlusionWrapV, textureFilter};
            case MaterialTextureSlot::Emissive:
                return {emissiveWrapU, emissiveWrapV, textureFilter};
            default:
                return {albedoWrapU, albedoWrapV, textureFilter};
            }
        }
        // the slot's wrap in u and v; the filter is the material's (textureFilter)
        void setWrap(const MaterialTextureSlot slot, const TextureWrap u, const TextureWrap v)
        {
            switch (slot)
            {
            case MaterialTextureSlot::Normal:
                std::tie(normalWrapU, normalWrapV) = std::tuple(u, v);
                break;
            case MaterialTextureSlot::MetallicRoughness:
                std::tie(metallicRoughnessWrapU, metallicRoughnessWrapV) = std::tuple(u, v);
                break;
            case MaterialTextureSlot::Occlusion:
                std::tie(occlusionWrapU, occlusionWrapV) = std::tuple(u, v);
                break;
            case MaterialTextureSlot::Emissive:
                std::tie(emissiveWrapU, emissiveWrapV) = std::tuple(u, v);
                break;
            default:
                std::tie(albedoWrapU, albedoWrapV) = std::tuple(u, v);
                break;
            }
        }
    };

    // The .spitemat file: a 16-byte header (magic "SPMT", version, tag count, zero), then per schema field a record
    // of u32 tag, u32 payload size, the payload (little endian; a name as its bytes) and zero padding to 4 bytes.
    // Version 1 is the legacy untagged layout (a Vec4 and three NUL-terminated names, no header): only the project
    // load's migrator reads it. A reader skips an unknown tag and keeps the default of a missing one; a newer
    // version is refused.
    constexpr u32 MATERIAL_FILE_VERSION = 2;

    // A hash of the names of the BakedMaterialWriteData fields in order: it changes when a field is added or renamed.
    SPITE_ASSET_API u32 bakedMaterialSchemaHash();

    SPITE_ASSET_API std::vector<std::byte> encodeBakedMaterial(const BakedMaterialWriteData& data);

    // Reads a version-2 file; a legacy or malformed one is an error (CorruptedData).
    SPITE_ASSET_API Expected<BakedMaterialWriteData> decodeBakedMaterial(std::span<const std::byte> bytes);

    // A file with no tagged header: the legacy layout (or garbage, which decodeLegacyMaterial refuses).
    SPITE_ASSET_API bool isLegacyMaterialFile(std::span<const std::byte> bytes);

    // The legacy file as the v2 fields: a metallic-roughness texture sets metallicFactor 1 (the old shader read the
    // texture alone), none leaves the plain material non-metallic, so the material renders as it did.
    SPITE_ASSET_API Expected<BakedMaterialWriteData> decodeLegacyMaterial(std::span<const std::byte> bytes);

    SPITE_ASSET_API Expected<void> writeBakedMaterial(const char* outputPath, const BakedMaterialWriteData& data);

    struct MaterialMigration
    {
        BakedMaterialWriteData fields; // what the file holds after the call
        bool rewritten = false; // it was a legacy file and is a tagged one now
    };

    // Brings the file at path to the tagged form: a legacy file is rewritten, a tagged one is only read.
    SPITE_ASSET_API Expected<MaterialMigration> migrateBakedMaterial(std::string_view path);
} // namespace spite
