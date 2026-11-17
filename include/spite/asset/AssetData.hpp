#pragma once
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include "asset/AnimationData.hpp"
#include "asset/AssetExport.hpp"
#include "asset/MaterialTypes.hpp"
#include "asset/TextureFormat.hpp"
#include "asset/UvRange.hpp"
#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "rendering/GpuSharedTypes.hpp"

struct aiScene;

namespace spite
{
    struct FragmentData
    {
        Vec4 color{0.0f, 1.0f, 0.0f, 1.0f};
    };

    enum class AssetType : u8
    {
        UNKNOWN = 0,
        MESH,
        TEXTURE,
        MATERIAL,
        ENVIRONMENT, // an equirect HDR image with mips, the source of a scene sky and image-based lighting
        SKELETON, // joints, rest poses and inverse binds (a model's rig); CPU data, baked as .spiteskel
        ANIMATION, // one clip on a skeleton, baked as .spiteanim
        OCEAN_PROFILE, // CPU-resident signed displacement, normals and foam
    };

    inline const char* to_string(AssetType e)
    {
        switch (e)
        {
        case AssetType::UNKNOWN:
            return "UNKNOWN";
        case AssetType::MESH:
            return "MESH";
        case AssetType::TEXTURE:
            return "TEXTURE";
        case AssetType::MATERIAL:
            return "MATERIAL";
        case AssetType::ENVIRONMENT:
            return "ENVIRONMENT";
        case AssetType::SKELETON:
            return "SKELETON";
        case AssetType::ANIMATION:
            return "ANIMATION";
        case AssetType::OCEAN_PROFILE:
            return "OCEAN_PROFILE";
        default:
            return "unknown";
        }
    }

    class SPITE_ASSET_API TextureAsset
    {
    public:
        using Deleter = std::function<void(u8*)>;

        // Constructor takes ownership of the pixel data via a unique_ptr: level 0, then the rest of the chain
        // (mipLevels levels, tightly packed, largest first) when the file carried one. `format` is how the bytes are
        // stored: RGBA8, or the blocks of a BC format (bakedTextureLevelBytes per level).
        TextureAsset(std::unique_ptr<u8[], Deleter>&& pixelData, int width, int height, int channels,
                     InternedString name, u32 mipLevels = 1, u8 format = BAKED_TEXTURE_FORMAT_R8G8B8A8_SRGB);

        ~TextureAsset() = default;

        TextureAsset(const TextureAsset&) = delete;
        TextureAsset& operator=(const TextureAsset&) = delete;

        TextureAsset(TextureAsset&& other) noexcept;
        TextureAsset& operator=(TextureAsset&& other) noexcept;

        const InternedString getName() const;
        const u8* getPixels() const;
        int getWidth() const;
        int getHeight() const;
        int getChannels() const;
        u32 getMipLevels() const;
        u8 getFormat() const; // a BAKED_TEXTURE_FORMAT_* id
        sizet getSizeInBytes() const; // level 0, in the texture's format
        sizet getTotalSizeInBytes() const; // every level of the chain, in the texture's format

    private:
        InternedString m_name{};
        std::unique_ptr<u8[], Deleter> m_pixels;
        int m_width{};
        int m_height{};
        int m_channels{};
        u32 m_mipLevels = 1;
        u8 m_format = BAKED_TEXTURE_FORMAT_R8G8B8A8_SRGB;
    };


    // A material as imported or loaded: the factors and textures of the baked descriptor (BakedMaterialWriteData).
    struct ParsedMaterialData
    {
        InternedString name;
        Vec4 albedoFactor = {1.0f, 1.0f, 1.0f, 1.0f};
        f32 metallicFactor = 0.0f;
        f32 roughnessFactor = 1.0f;
        Vec3 emissiveFactor = {0.0f, 0.0f, 0.0f};
        f32 emissiveStrength = 1.0f;
        f32 occlusionStrength = 1.0f;
        f32 normalScale = 1.0f;
        f32 thinLeafTransmission = 0.0f;
        MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
        f32 alphaCutoff = 0.5f;
        bool doubleSided = false;
        bool castsShadow = false; // BakedMaterialWriteData::castsShadow
        bool unlit = false; // BakedMaterialWriteData::unlit
        // KHR_texture_transform of the material's textures, in the texture's own space (origin at the image's top-left,
        // v down): the rotation turns the uv counter-clockwise, the scale applies first, the offset last
        Vec2 uvOffset = {0.0f, 0.0f};
        Vec2 uvScale = {1.0f, 1.0f};
        f32 uvRotation = 0.0f;

        std::shared_ptr<TextureAsset> albedoTexture;
        std::shared_ptr<TextureAsset> normalTexture;
        std::shared_ptr<TextureAsset> metallicRoughnessTexture;
        std::shared_ptr<TextureAsset> occlusionTexture;
        std::shared_ptr<TextureAsset> emissiveTexture;
        // Empty mask means this is an ordinary material. Detail maps repeat in world metres.
        std::array<std::shared_ptr<TextureAsset>, TERRAIN_TEXTURE_SLOT_COUNT> terrainTextures{};
        f32 terrainDetailMetres = 32.0f;
        f32 terrainGrassDetailMetres = 0.0f;
        f32 terrainSoilDetailMetres = 0.0f;
        f32 terrainRockDetailMetres = 0.0f;
        // the colour space each slot's texture is sampled in, indexed by MaterialTextureSlot
        std::array<TextureColorSpace, MATERIAL_TEXTURE_SLOT_COUNT> textureSpaces{
            defaultTextureSpace(MaterialTextureSlot::Albedo), defaultTextureSpace(MaterialTextureSlot::Normal),
            defaultTextureSpace(MaterialTextureSlot::MetallicRoughness),
            defaultTextureSpace(MaterialTextureSlot::Occlusion), defaultTextureSpace(MaterialTextureSlot::Emissive)};

        std::shared_ptr<TextureAsset>& texture(const MaterialTextureSlot slot)
        {
            switch (slot)
            {
            case MaterialTextureSlot::Normal:
                return normalTexture;
            case MaterialTextureSlot::MetallicRoughness:
                return metallicRoughnessTexture;
            case MaterialTextureSlot::Occlusion:
                return occlusionTexture;
            case MaterialTextureSlot::Emissive:
                return emissiveTexture;
            default:
                return albedoTexture;
            }
        }
        const std::shared_ptr<TextureAsset>& texture(const MaterialTextureSlot slot) const
        {
            return const_cast<ParsedMaterialData&>(*this).texture(slot);
        }
        TextureColorSpace& space(const MaterialTextureSlot slot) { return textureSpaces[static_cast<sizet>(slot)]; }
        TextureColorSpace space(const MaterialTextureSlot slot) const
        {
            return textureSpaces[static_cast<sizet>(slot)];
        }
        // How the textures are sampled: the material's filter and each slot's wrap, indexed by MaterialTextureSlot
        TextureFilter textureFilter = TextureFilter::Smooth;
        std::array<TextureWrap, MATERIAL_TEXTURE_SLOT_COUNT> wrapU{}; // all Repeat
        std::array<TextureWrap, MATERIAL_TEXTURE_SLOT_COUNT> wrapV{};
        MaterialSamplerState samplerState(const MaterialTextureSlot slot) const
        {
            return {wrapU[static_cast<sizet>(slot)], wrapV[static_cast<sizet>(slot)], textureFilter};
        }
    };

    static constexpr u32 DEFAULT_MATERIAL_INDEX = std::numeric_limits<u32>::max();


    struct ProcessedMeshlet
    {
        u32 firstVertex;
        u32 vertexCount;
        u32 firstTriangleByte; // Offset into global meshlet triangle index buffer (u8)
        u32 triangleCount;
        // object-space bounding sphere and normal cone for the GPU meshlet cull
        Vec3 boundsCenter = {0.0f, 0.0f, 0.0f};
        f32 boundsRadius = 0.0f;
        // cone axis x, y, z and cutoff as s8 snorm (value / 127), low byte first;
        // cutoff 127 disables the cone test
        u32 packedCone = 0x7F000000u;
        // Cluster LOD: the error a cluster's simplification adds over the level below it (0 at level
        // 0) and the error of the group that simplified it further (FLT_MAX: no coarser parent). The sphere above is
        // the shared group sphere of an upper-level cluster; lodParent is the submesh-relative index of one cluster
        // of the parent group, whose sphere is the parent's (NO_LOD_PARENT: none). lodLevel is the DAG level.
        f32 lodSelfError = 0.0f;
        f32 lodParentError = std::numeric_limits<f32>::max();
        u32 lodParent = 0xFFFFFFFFu;
        u32 lodLevel = 0;
    };
    static_assert(sizeof(ProcessedMeshlet) == 52, "ProcessedMeshlet is baked as raw bytes: no padding");
    constexpr u32 NO_LOD_PARENT = 0xFFFFFFFFu;

    // The version 2 baked record: no cluster LOD, every meshlet is level 0.
    struct ProcessedMeshletV2
    {
        u32 firstVertex;
        u32 vertexCount;
        u32 firstTriangleByte;
        u32 triangleCount;
        Vec3 boundsCenter;
        f32 boundsRadius;
        u32 packedCone;
    };
    static_assert(sizeof(ProcessedMeshletV2) == 36);

    // The vertex of baked version 3 and below: no tangent frame.
    struct VertexV3
    {
        f32 positionX;
        f32 positionY;
        f32 positionZ;
        u32 packedNormal;
        u32 packedUv;
    };
    static_assert(sizeof(VertexV3) == 20);

    // A struct to hold the final, GPU-ready geometry for ONE submesh.
    struct ProcessedSubmesh
    {
        ProcessedSubmesh() = default;

        InternedString name{};

        std::vector<Vertex> vertices;
        // set for a submesh with bones: the skin of each of `vertices`, in the same order
        std::optional<SkinStream> skin;
        // the span Vertex::packedUv's unorm fields cover; the identity range for uvs inside [0, 1]
        UvRange uvRange;
        std::vector<u32> meshletVertexIndices;
        std::vector<u8> meshletTriangleIndices; // Packed u8s
        // level 0 first, then the coarser clusters of the DAG (lodMeshletCount of them)
        std::vector<ProcessedMeshlet> meshlets;
        BoundingBox localBounds;
    };

    // The coarser clusters that trail the level-0 meshlets of a meshlet list.
    inline u32 lodMeshletCount(const std::vector<ProcessedMeshlet>& meshlets)
    {
        return static_cast<u32>(std::count_if(meshlets.begin(), meshlets.end(),
                                              [](const ProcessedMeshlet& meshlet) { return meshlet.lodLevel != 0; }));
    }

    // identical to TransformComponent
    struct TransformData
    {
        Vec3 position;
        Quat rotation;
        Vec3 scale;
    };

    // Engine-owned source properties, independent of ECS and of any importer library.
    struct ImportedNodeProperties
    {
        u32 camera = 0; // 0 absent, 1 perspective
        u32 light = 0; // 0 absent, 1 directional, 2 point, 3 spot
        f32 fov = 0.0f;
        f32 nearPlane = 0.0f;
        f32 farPlane = 0.0f;
        f32 color[3] = {};
        f32 intensity = 0.0f;
        f32 range = 0.0f;
        f32 innerCone = 0.0f;
        f32 outerCone = 0.0f;
    };
    static_assert(sizeof(ImportedNodeProperties) == 12 * sizeof(u32));

    // A serializable representation of a single node in the model's hierarchy.
    struct ProcessedNode
    {
        TransformData localTransform;
        InternedString name;
        ImportedNodeProperties imported;
        InternedString sourceObject;
        bool sourceObjectStable = false;
        // Indices into ProcessedModelData.submeshes
        std::vector<u32> meshIndices;
        // Indices into the ProcessedModelData's top-level 'nodes' std::vector.
        std::vector<u32> childrenIndices;
    };

    struct ParsedMeshData
    {
        InternedString path;
        std::vector<ProcessedSubmesh> submeshes;
        std::vector<ProcessedNode> nodes;
        u32 rootNodeIndex = -1;
    };

    struct GltfExtensionDiagnostic
    {
        std::string extension;
        bool required = false;
        std::string code = "unsupported_extension";
        std::string policy;
    };

    // A struct to hold the result of a fully parsed 3D model.
    struct ParsedModelData
    {
        InternedString name;

        ParsedMeshData mesh;
        std::vector<ParsedMaterialData> materials;
        std::vector<u32> submeshMaterialIndices;
        // a model with at least one skinned mesh: its skeleton and the clips on it (no joints: no skin)
        RawSkeleton skeleton;
        std::vector<RawClip> clips;
    };

    // Decodes the packed octahedral vertex normal. Mirrors decodeOctahedral in
    // shared/bindless_vbuffer.glsl; the leading unpackSnorm2x16 is folded in
    // (the shader's *0.5+0.5 then *2-1 roundtrip is exact). Lives next to the
    // Vertex type: the packing is part of the vertex format.
    inline Vec3 decodeOctahedralNormal(u32 packed)
    {
        const auto unpackSnorm16 = [](u16 raw) -> f32
        {
            const auto s = static_cast<i16>(raw);
            return std::max(static_cast<f32>(s) / 32767.f, -1.f);
        };
        const f32 fx = unpackSnorm16(static_cast<u16>(packed & 0xFFFF));
        const f32 fy = unpackSnorm16(static_cast<u16>(packed >> 16));
        f32 nx = fx, ny = fy;
        f32 nz = 1.f - std::abs(nx) - std::abs(ny);
        const f32 t = std::max(-nz, 0.f);
        nx += nx >= 0.f ? -t : t;
        ny += ny >= 0.f ? -t : t;
        const f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
        return Vec3(nx / len, ny / len, nz / len);
    }

} // namespace spite
