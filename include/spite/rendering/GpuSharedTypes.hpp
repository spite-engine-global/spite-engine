// Generated from the shader interface; do not edit.
// clang-format off
#pragma once

#include <cstddef>

#include "base/MathTypes.hpp"

namespace spite
{
    constexpr u32 MATERIAL_ALPHA_MODE_MASK = 3;
    constexpr u32 MATERIAL_FLAG_DOUBLE_SIDED = 4;
    constexpr u32 MATERIAL_FLAG_UNLIT = 8;
    constexpr u32 MATERIAL_FLAG_HAS_EXTENSION = 16;
    constexpr u32 MATERIAL_FLAG_UV_TRANSFORM = 32;
    constexpr u32 MATERIAL_SAMPLER_SHIFT = 7;
    constexpr u32 MATERIAL_SAMPLER_BITS = 5;
    constexpr u32 MATERIAL_SAMPLER_STATES = 27;
    constexpr u32 MATERIAL_SAMPLER_FIRST_SLOT = 3;
    constexpr u32 MATERIAL_ALPHA_BLEND = 2;
    constexpr u32 MATERIAL_ALPHA_EXTENDED_BLEND = 3;
    constexpr u32 MATERIAL_FLAG_ADDITIVE = 64;
    constexpr u32 MATERIAL_FLAG_CASTS_SHADOW = 64;
    constexpr u32 MATERIAL_CLASS_OPAQUE = 0;
    constexpr u32 MATERIAL_CLASS_MASK = 1;
    constexpr u32 MATERIAL_CLASS_BLEND = 2;
    constexpr u32 SCENE_UV_ATLAS_MARKER = 2080374784;
    constexpr u32 SCENE_UV_BILLBOARD = 2147483648;
    constexpr u32 SCENE_OBJECT_UPDATE_WRITE = 0;
    constexpr u32 SCENE_OBJECT_UPDATE_SPAWN = 1;
    constexpr u32 SCENE_OBJECT_UPDATE_CLEAR = 2;
    constexpr u32 AREA_LIGHT_TWO_SIDED = 1;
    constexpr u32 ENVIRONMENT_MODE_NONE = 0;
    constexpr u32 ENVIRONMENT_MODE_COLOR = 1;
    constexpr u32 ENVIRONMENT_MODE_GRADIENT = 2;
    constexpr u32 ENVIRONMENT_MODE_PROCEDURAL = 3;
    constexpr u32 ENVIRONMENT_MODE_HDRI = 4;
    constexpr u32 PROBE_VOLUME_READY = 1;
    constexpr u32 PROBE_SH_TEXELS = 7;
    constexpr u32 PROBE_MOMENT_EXTENT = 8;
    constexpr u32 PROBE_ATLAS_COLUMNS = 64;
    constexpr u32 MAX_PROBES_PER_VOLUME = 65536;
    constexpr u32 NO_SHADOWED_DIRECTIONAL = 4294967295;
    constexpr u32 MAX_SHADOW_CASCADES = 4;
    constexpr u32 MAX_LIGHT_SHADOW_MAPS = 144;
    constexpr u32 NO_LIGHT_SHADOW = 4294967295;
    constexpr u32 GLOBAL_ADDRESS_SLOT_COUNT = 64;
    constexpr u32 GLOBAL_ADDRESS_SLOT_OBJECT_BUFFER = 0;
    constexpr u32 GLOBAL_ADDRESS_SLOT_LIGHT_DATA = 1;
    constexpr u32 GLOBAL_ADDRESS_SLOT_CAMERA_UBO = 2;
    constexpr u32 GLOBAL_ADDRESS_SLOT_MATERIAL_BUFFER = 3;
    constexpr u32 GLOBAL_ADDRESS_SLOT_GLOBAL_VERTEX_BUFFER = 4;
    constexpr u32 GLOBAL_ADDRESS_SLOT_MESHLET_VERTEX_INDEX_BUFFER = 5;
    constexpr u32 GLOBAL_ADDRESS_SLOT_MESHLET_TRIANGLE_INDEX_BUFFER = 6;
    constexpr u32 GLOBAL_ADDRESS_SLOT_MESHLET_BUFFER = 7;
    constexpr u32 GLOBAL_ADDRESS_SLOT_PREVIOUS_TRANSFORM_BUFFER = 8;
    constexpr u32 GLOBAL_ADDRESS_SLOT_TEMPORAL = 9;
    constexpr u32 GLOBAL_ADDRESS_SLOT_ENVIRONMENT = 10;
    constexpr u32 GLOBAL_ADDRESS_SLOT_PROBE_VOLUME = 11;
    constexpr u32 GLOBAL_ADDRESS_SLOT_SKIN_WEIGHT_BUFFER = 12;
    constexpr u32 GLOBAL_ADDRESS_SLOT_JOINT_PALETTE_BUFFER = 13;
    constexpr u32 GLOBAL_ADDRESS_BUILT_IN_SLOT_COUNT = 14;
    constexpr u32 COMPARISON_SAMPLER_SLOT = 1;
    constexpr u32 LINEAR_SAMPLER_SLOT = 2;
    constexpr u32 ENVIRONMENT_SH_COEFFICIENTS = 9;
    constexpr u32 ENVIRONMENT_SPECULAR_EXTENT = 128;
    constexpr u32 ENVIRONMENT_SPECULAR_MIPS = 8;
    constexpr u32 ENVIRONMENT_LUT_EXTENT = 64;
    constexpr u32 BLOOM_PREFILTER = 0;
    constexpr u32 BLOOM_DOWNSAMPLE = 1;
    constexpr u32 BLOOM_UPSAMPLE = 2;
    constexpr u32 CULL_PHASE_EARLY = 0;
    constexpr u32 CULL_PHASE_LATE = 1;
    constexpr u32 OBJECT_CULL_LOD_EMIT_LEVEL0 = 4;
    constexpr u32 PIXEL_INFO_LIGHTS_NONE = 0;
    constexpr u32 PIXEL_INFO_LIGHTS_CLUSTER = 1;
    constexpr u32 PIXEL_INFO_LIGHTS_TABLE = 2;
    constexpr u32 PIXEL_INFO_MAX_LIGHTS = 16;
    constexpr u32 PIXEL_INFO_SPOT_LIGHT = 2147483648;
    constexpr u32 PIXEL_INFO_AREA_LIGHT = 1073741824;
    constexpr u32 PIXEL_INFO_NO_CLUSTER = 4294967295;

    struct GpuMaterial
    {
        Vec4 albedoFactor;
        Vec3 emissiveRadiance;
        float uvRotation;
        float metallicFactor;
        float roughnessFactor;
        float occlusionStrength;
        float normalScale;
        float alphaCutoff;
        u32 flags;
        u32 albedoTextureIndex;
        u32 normalTextureIndex;
        u32 metallicRoughnessTextureIndex;
        u32 emissiveTextureIndex;
        u32 occlusionTextureIndex;
        u32 extensionIndex;
        Vec2 uvOffset;
        Vec2 uvScale;
        float thinLeafTransmission;
        float _materialPad0;
        float _materialPad1;
        float _materialPad2;
    };
    static_assert(sizeof(GpuMaterial) == 112);
    static_assert(offsetof(GpuMaterial, albedoFactor) == 0);
    static_assert(offsetof(GpuMaterial, emissiveRadiance) == 16);
    static_assert(offsetof(GpuMaterial, uvRotation) == 28);
    static_assert(offsetof(GpuMaterial, metallicFactor) == 32);
    static_assert(offsetof(GpuMaterial, roughnessFactor) == 36);
    static_assert(offsetof(GpuMaterial, occlusionStrength) == 40);
    static_assert(offsetof(GpuMaterial, normalScale) == 44);
    static_assert(offsetof(GpuMaterial, alphaCutoff) == 48);
    static_assert(offsetof(GpuMaterial, flags) == 52);
    static_assert(offsetof(GpuMaterial, albedoTextureIndex) == 56);
    static_assert(offsetof(GpuMaterial, normalTextureIndex) == 60);
    static_assert(offsetof(GpuMaterial, metallicRoughnessTextureIndex) == 64);
    static_assert(offsetof(GpuMaterial, emissiveTextureIndex) == 68);
    static_assert(offsetof(GpuMaterial, occlusionTextureIndex) == 72);
    static_assert(offsetof(GpuMaterial, extensionIndex) == 76);
    static_assert(offsetof(GpuMaterial, uvOffset) == 80);
    static_assert(offsetof(GpuMaterial, uvScale) == 88);
    static_assert(offsetof(GpuMaterial, thinLeafTransmission) == 96);
    static_assert(offsetof(GpuMaterial, _materialPad0) == 100);
    static_assert(offsetof(GpuMaterial, _materialPad1) == 104);
    static_assert(offsetof(GpuMaterial, _materialPad2) == 108);

    struct GpuTerrainProfile
    {
        u32 mask;
        u32 grassAlbedo;
        u32 grassNormal;
        u32 grassRoughness;
        u32 soilAlbedo;
        u32 soilNormal;
        u32 soilRoughness;
        u32 rockAlbedo;
        u32 rockNormal;
        u32 rockRoughness;
        float detailMetres;
        float grassDetailMetres;
        float soilDetailMetres;
        float rockDetailMetres;
        u32 pad3;
        u32 pad4;
        u32 pad5;
        u32 pad6;
        u32 pad7;
        u32 pad8;
        u32 pad9;
        u32 pad10;
        u32 pad11;
        u32 pad12;
        u32 pad13;
        u32 pad14;
        u32 pad15;
        u32 pad16;
    };
    static_assert(sizeof(GpuTerrainProfile) == 112);
    static_assert(offsetof(GpuTerrainProfile, mask) == 0);
    static_assert(offsetof(GpuTerrainProfile, grassAlbedo) == 4);
    static_assert(offsetof(GpuTerrainProfile, grassNormal) == 8);
    static_assert(offsetof(GpuTerrainProfile, grassRoughness) == 12);
    static_assert(offsetof(GpuTerrainProfile, soilAlbedo) == 16);
    static_assert(offsetof(GpuTerrainProfile, soilNormal) == 20);
    static_assert(offsetof(GpuTerrainProfile, soilRoughness) == 24);
    static_assert(offsetof(GpuTerrainProfile, rockAlbedo) == 28);
    static_assert(offsetof(GpuTerrainProfile, rockNormal) == 32);
    static_assert(offsetof(GpuTerrainProfile, rockRoughness) == 36);
    static_assert(offsetof(GpuTerrainProfile, detailMetres) == 40);
    static_assert(offsetof(GpuTerrainProfile, grassDetailMetres) == 44);
    static_assert(offsetof(GpuTerrainProfile, soilDetailMetres) == 48);
    static_assert(offsetof(GpuTerrainProfile, rockDetailMetres) == 52);
    static_assert(offsetof(GpuTerrainProfile, pad3) == 56);
    static_assert(offsetof(GpuTerrainProfile, pad4) == 60);
    static_assert(offsetof(GpuTerrainProfile, pad5) == 64);
    static_assert(offsetof(GpuTerrainProfile, pad6) == 68);
    static_assert(offsetof(GpuTerrainProfile, pad7) == 72);
    static_assert(offsetof(GpuTerrainProfile, pad8) == 76);
    static_assert(offsetof(GpuTerrainProfile, pad9) == 80);
    static_assert(offsetof(GpuTerrainProfile, pad10) == 84);
    static_assert(offsetof(GpuTerrainProfile, pad11) == 88);
    static_assert(offsetof(GpuTerrainProfile, pad12) == 92);
    static_assert(offsetof(GpuTerrainProfile, pad13) == 96);
    static_assert(offsetof(GpuTerrainProfile, pad14) == 100);
    static_assert(offsetof(GpuTerrainProfile, pad15) == 104);
    static_assert(offsetof(GpuTerrainProfile, pad16) == 108);

    struct SceneObjectBounds
    {
        Vec3 min;
        u32 paletteBase;
        Vec3 max;
        u32 skinWeightDelta;
    };
    static_assert(sizeof(SceneObjectBounds) == 32);
    static_assert(offsetof(SceneObjectBounds, min) == 0);
    static_assert(offsetof(SceneObjectBounds, paletteBase) == 12);
    static_assert(offsetof(SceneObjectBounds, max) == 16);
    static_assert(offsetof(SceneObjectBounds, skinWeightDelta) == 28);

    struct SceneObject
    {
        Mat4 modelMatrix;
        Mat4 normalMatrix;
        SceneObjectBounds worldBounds;
        u32 firstMeshlet;
        u32 meshletCount;
        u32 firstTriangle;
        u32 scene;
        u32 tint;
        u32 lodMeshletCount;
        u32 packedUvOffset;
        u32 packedUvExtent;
        float fade;
        u32 motionFrame;
        float lodThresholdScale;
        u32 _fadePad2;
        Vec4 waterProfile;
        Vec4 waterTiming;
        Vec4 waterAppearance;
    };
    static_assert(sizeof(SceneObject) == 256);
    static_assert(offsetof(SceneObject, modelMatrix) == 0);
    static_assert(offsetof(SceneObject, normalMatrix) == 64);
    static_assert(offsetof(SceneObject, worldBounds) == 128);
    static_assert(offsetof(SceneObject, firstMeshlet) == 160);
    static_assert(offsetof(SceneObject, meshletCount) == 164);
    static_assert(offsetof(SceneObject, firstTriangle) == 168);
    static_assert(offsetof(SceneObject, scene) == 172);
    static_assert(offsetof(SceneObject, tint) == 176);
    static_assert(offsetof(SceneObject, lodMeshletCount) == 180);
    static_assert(offsetof(SceneObject, packedUvOffset) == 184);
    static_assert(offsetof(SceneObject, packedUvExtent) == 188);
    static_assert(offsetof(SceneObject, fade) == 192);
    static_assert(offsetof(SceneObject, motionFrame) == 196);
    static_assert(offsetof(SceneObject, lodThresholdScale) == 200);
    static_assert(offsetof(SceneObject, _fadePad2) == 204);
    static_assert(offsetof(SceneObject, waterProfile) == 208);
    static_assert(offsetof(SceneObject, waterTiming) == 224);
    static_assert(offsetof(SceneObject, waterAppearance) == 240);

    struct SceneObjectUpdate
    {
        Mat4 modelMatrix;
        SceneObjectBounds worldBounds;
        u32 slot;
        u32 kind;
        u32 firstMeshlet;
        u32 meshletCount;
        u32 firstTriangle;
        u32 scene;
        u32 tint;
        u32 lodMeshletCount;
        u32 packedUvOffset;
        u32 packedUvExtent;
        u32 poseVersion;
        float fade;
        Vec4 waterProfile;
        Vec4 waterTiming;
        Vec4 waterAppearance;
        float lodThresholdScale;
        u32 _lodPad0;
        u32 _lodPad1;
        u32 _lodPad2;
    };
    static_assert(sizeof(SceneObjectUpdate) == 208);
    static_assert(offsetof(SceneObjectUpdate, modelMatrix) == 0);
    static_assert(offsetof(SceneObjectUpdate, worldBounds) == 64);
    static_assert(offsetof(SceneObjectUpdate, slot) == 96);
    static_assert(offsetof(SceneObjectUpdate, kind) == 100);
    static_assert(offsetof(SceneObjectUpdate, firstMeshlet) == 104);
    static_assert(offsetof(SceneObjectUpdate, meshletCount) == 108);
    static_assert(offsetof(SceneObjectUpdate, firstTriangle) == 112);
    static_assert(offsetof(SceneObjectUpdate, scene) == 116);
    static_assert(offsetof(SceneObjectUpdate, tint) == 120);
    static_assert(offsetof(SceneObjectUpdate, lodMeshletCount) == 124);
    static_assert(offsetof(SceneObjectUpdate, packedUvOffset) == 128);
    static_assert(offsetof(SceneObjectUpdate, packedUvExtent) == 132);
    static_assert(offsetof(SceneObjectUpdate, poseVersion) == 136);
    static_assert(offsetof(SceneObjectUpdate, fade) == 140);
    static_assert(offsetof(SceneObjectUpdate, waterProfile) == 144);
    static_assert(offsetof(SceneObjectUpdate, waterTiming) == 160);
    static_assert(offsetof(SceneObjectUpdate, waterAppearance) == 176);
    static_assert(offsetof(SceneObjectUpdate, lodThresholdScale) == 192);
    static_assert(offsetof(SceneObjectUpdate, _lodPad0) == 196);
    static_assert(offsetof(SceneObjectUpdate, _lodPad1) == 200);
    static_assert(offsetof(SceneObjectUpdate, _lodPad2) == 204);

    struct SceneObjectMove
    {
        Vec4 model0;
        Vec4 model1;
        Vec4 model2;
        Vec3 boundsMin;
        u32 slot;
        Vec3 boundsMax;
        u32 _pad0;
    };
    static_assert(sizeof(SceneObjectMove) == 80);
    static_assert(offsetof(SceneObjectMove, model0) == 0);
    static_assert(offsetof(SceneObjectMove, model1) == 16);
    static_assert(offsetof(SceneObjectMove, model2) == 32);
    static_assert(offsetof(SceneObjectMove, boundsMin) == 48);
    static_assert(offsetof(SceneObjectMove, slot) == 60);
    static_assert(offsetof(SceneObjectMove, boundsMax) == 64);
    static_assert(offsetof(SceneObjectMove, _pad0) == 76);

    struct Vertex
    {
        float positionX;
        float positionY;
        float positionZ;
        u32 packedNormal;
        u32 packedUv;
        u32 packedTangent;
    };
    static_assert(sizeof(Vertex) == 24);
    static_assert(offsetof(Vertex, positionX) == 0);
    static_assert(offsetof(Vertex, positionY) == 4);
    static_assert(offsetof(Vertex, positionZ) == 8);
    static_assert(offsetof(Vertex, packedNormal) == 12);
    static_assert(offsetof(Vertex, packedUv) == 16);
    static_assert(offsetof(Vertex, packedTangent) == 20);

    struct SkinVertexWeights
    {
        u32 joints;
        u32 weights;
    };
    static_assert(sizeof(SkinVertexWeights) == 8);
    static_assert(offsetof(SkinVertexWeights, joints) == 0);
    static_assert(offsetof(SkinVertexWeights, weights) == 4);

    struct GpuPaletteJoint
    {
        Vec4 row0;
        Vec4 row1;
        Vec4 row2;
    };
    static_assert(sizeof(GpuPaletteJoint) == 48);
    static_assert(offsetof(GpuPaletteJoint, row0) == 0);
    static_assert(offsetof(GpuPaletteJoint, row1) == 16);
    static_assert(offsetof(GpuPaletteJoint, row2) == 32);

    struct SkinPaletteUpdate
    {
        u32 joint;
        u32 _pad0;
        u32 _pad1;
        u32 _pad2;
        GpuPaletteJoint matrix;
    };
    static_assert(sizeof(SkinPaletteUpdate) == 64);
    static_assert(offsetof(SkinPaletteUpdate, joint) == 0);
    static_assert(offsetof(SkinPaletteUpdate, _pad0) == 4);
    static_assert(offsetof(SkinPaletteUpdate, _pad1) == 8);
    static_assert(offsetof(SkinPaletteUpdate, _pad2) == 12);
    static_assert(offsetof(SkinPaletteUpdate, matrix) == 16);

    struct DispatchIndirectCommand
    {
        u32 x;
        u32 y;
        u32 z;
    };
    static_assert(sizeof(DispatchIndirectCommand) == 12);
    static_assert(offsetof(DispatchIndirectCommand, x) == 0);
    static_assert(offsetof(DispatchIndirectCommand, y) == 4);
    static_assert(offsetof(DispatchIndirectCommand, z) == 8);

    struct PointLight
    {
        Vec4 position;
        Vec4 color;
    };
    static_assert(sizeof(PointLight) == 32);
    static_assert(offsetof(PointLight, position) == 0);
    static_assert(offsetof(PointLight, color) == 16);

    struct DirectionalLight
    {
        Vec4 direction;
        Vec4 color;
    };
    static_assert(sizeof(DirectionalLight) == 32);
    static_assert(offsetof(DirectionalLight, direction) == 0);
    static_assert(offsetof(DirectionalLight, color) == 16);

    struct SpotLight
    {
        Vec4 position;
        Vec4 direction;
        Vec4 color;
        Vec4 cutOffs;
    };
    static_assert(sizeof(SpotLight) == 64);
    static_assert(offsetof(SpotLight, position) == 0);
    static_assert(offsetof(SpotLight, direction) == 16);
    static_assert(offsetof(SpotLight, color) == 32);
    static_assert(offsetof(SpotLight, cutOffs) == 48);

    struct AreaLight
    {
        Vec4 position;
        Vec4 color;
        Vec4 halfU;
        Vec4 halfV;
    };
    static_assert(sizeof(AreaLight) == 64);
    static_assert(offsetof(AreaLight, position) == 0);
    static_assert(offsetof(AreaLight, color) == 16);
    static_assert(offsetof(AreaLight, halfU) == 32);
    static_assert(offsetof(AreaLight, halfV) == 48);

    struct LightData
    {
        u32 pointLightCount;
        u32 directionalLightCount;
        u32 spotLightCount;
        u32 shadowedDirectional;
        u64 pointLightsAddress;
        u64 directionalLightsAddress;
        u64 spotLightsAddress;
        u64 _padding2;
        u32 areaLightCount;
        u32 _padding3;
        u64 areaLightsAddress;
    };
    static_assert(sizeof(LightData) == 64);
    static_assert(offsetof(LightData, pointLightCount) == 0);
    static_assert(offsetof(LightData, directionalLightCount) == 4);
    static_assert(offsetof(LightData, spotLightCount) == 8);
    static_assert(offsetof(LightData, shadowedDirectional) == 12);
    static_assert(offsetof(LightData, pointLightsAddress) == 16);
    static_assert(offsetof(LightData, directionalLightsAddress) == 24);
    static_assert(offsetof(LightData, spotLightsAddress) == 32);
    static_assert(offsetof(LightData, _padding2) == 40);
    static_assert(offsetof(LightData, areaLightCount) == 48);
    static_assert(offsetof(LightData, _padding3) == 52);
    static_assert(offsetof(LightData, areaLightsAddress) == 56);

    struct CameraData
    {
        Mat4 view;
        Mat4 projection;
        Mat4 viewProjection;
        Mat4 inverseView;
        Mat4 inverseProjection;
        Mat4 inverseViewProjection;
        Vec4 frustumPlanes[6];
        Vec4 position;
        Vec4 forward;
        float nearPlane;
        float farPlane;
        Vec2 screenSize;
        u32 scene;
        float exposure;
        u32 temporal;
        u32 _pad2;
    };
    static_assert(sizeof(CameraData) == 544);
    static_assert(offsetof(CameraData, view) == 0);
    static_assert(offsetof(CameraData, projection) == 64);
    static_assert(offsetof(CameraData, viewProjection) == 128);
    static_assert(offsetof(CameraData, inverseView) == 192);
    static_assert(offsetof(CameraData, inverseProjection) == 256);
    static_assert(offsetof(CameraData, inverseViewProjection) == 320);
    static_assert(offsetof(CameraData, frustumPlanes) == 384);
    static_assert(offsetof(CameraData, position) == 480);
    static_assert(offsetof(CameraData, forward) == 496);
    static_assert(offsetof(CameraData, nearPlane) == 512);
    static_assert(offsetof(CameraData, farPlane) == 516);
    static_assert(offsetof(CameraData, screenSize) == 520);
    static_assert(offsetof(CameraData, scene) == 528);
    static_assert(offsetof(CameraData, exposure) == 532);
    static_assert(offsetof(CameraData, temporal) == 536);
    static_assert(offsetof(CameraData, _pad2) == 540);

    struct TemporalData
    {
        Mat4 viewProjectionUnjittered;
        Mat4 previousViewProjectionUnjittered;
        Mat4 inverseProjectionUnjittered;
        Vec2 jitterPixels;
        Vec2 jitterClip;
        Vec2 renderSize;
        Vec2 displaySize;
        u32 frameIndex;
        u32 phaseCount;
        u32 cutGeneration;
        float mipBias;
    };
    static_assert(sizeof(TemporalData) == 240);
    static_assert(offsetof(TemporalData, viewProjectionUnjittered) == 0);
    static_assert(offsetof(TemporalData, previousViewProjectionUnjittered) == 64);
    static_assert(offsetof(TemporalData, inverseProjectionUnjittered) == 128);
    static_assert(offsetof(TemporalData, jitterPixels) == 192);
    static_assert(offsetof(TemporalData, jitterClip) == 200);
    static_assert(offsetof(TemporalData, renderSize) == 208);
    static_assert(offsetof(TemporalData, displaySize) == 216);
    static_assert(offsetof(TemporalData, frameIndex) == 224);
    static_assert(offsetof(TemporalData, phaseCount) == 228);
    static_assert(offsetof(TemporalData, cutGeneration) == 232);
    static_assert(offsetof(TemporalData, mipBias) == 236);

    struct EnvironmentData
    {
        Vec4 tintIntensity;
        Vec4 zenith;
        Vec4 horizon;
        Vec4 ground;
        Vec4 parameters;
        u32 mode;
        float sunIrradiance;
        u32 _pad1;
        u32 _pad2;
        Vec4 sun;
    };
    static_assert(sizeof(EnvironmentData) == 112);
    static_assert(offsetof(EnvironmentData, tintIntensity) == 0);
    static_assert(offsetof(EnvironmentData, zenith) == 16);
    static_assert(offsetof(EnvironmentData, horizon) == 32);
    static_assert(offsetof(EnvironmentData, ground) == 48);
    static_assert(offsetof(EnvironmentData, parameters) == 64);
    static_assert(offsetof(EnvironmentData, mode) == 80);
    static_assert(offsetof(EnvironmentData, sunIrradiance) == 84);
    static_assert(offsetof(EnvironmentData, _pad1) == 88);
    static_assert(offsetof(EnvironmentData, _pad2) == 92);
    static_assert(offsetof(EnvironmentData, sun) == 96);

    struct ProbeVolumeData
    {
        Vec4 originSpacing;
        Vec4 parameters;
        u32 dimsX;
        u32 dimsY;
        u32 dimsZ;
        u32 columns;
        u32 shIndex;
        u32 momentsIndex;
        u32 flags;
        u32 _pad0;
    };
    static_assert(sizeof(ProbeVolumeData) == 64);
    static_assert(offsetof(ProbeVolumeData, originSpacing) == 0);
    static_assert(offsetof(ProbeVolumeData, parameters) == 16);
    static_assert(offsetof(ProbeVolumeData, dimsX) == 32);
    static_assert(offsetof(ProbeVolumeData, dimsY) == 36);
    static_assert(offsetof(ProbeVolumeData, dimsZ) == 40);
    static_assert(offsetof(ProbeVolumeData, columns) == 44);
    static_assert(offsetof(ProbeVolumeData, shIndex) == 48);
    static_assert(offsetof(ProbeVolumeData, momentsIndex) == 52);
    static_assert(offsetof(ProbeVolumeData, flags) == 56);
    static_assert(offsetof(ProbeVolumeData, _pad0) == 60);

    struct ShadowCascade
    {
        CameraData camera;
        float splitNear;
        float splitFar;
        float texelWorldSize;
        float radius;
    };
    static_assert(sizeof(ShadowCascade) == 560);
    static_assert(offsetof(ShadowCascade, camera) == 0);
    static_assert(offsetof(ShadowCascade, splitNear) == 544);
    static_assert(offsetof(ShadowCascade, splitFar) == 548);
    static_assert(offsetof(ShadowCascade, texelWorldSize) == 552);
    static_assert(offsetof(ShadowCascade, radius) == 556);

    struct LightShadowMap
    {
        CameraData camera;
        float texelWorldSize;
        float pcfRadius;
        u32 _pad0;
        u32 _pad1;
    };
    static_assert(sizeof(LightShadowMap) == 560);
    static_assert(offsetof(LightShadowMap, camera) == 0);
    static_assert(offsetof(LightShadowMap, texelWorldSize) == 544);
    static_assert(offsetof(LightShadowMap, pcfRadius) == 548);
    static_assert(offsetof(LightShadowMap, _pad0) == 552);
    static_assert(offsetof(LightShadowMap, _pad1) == 556);

    struct GlobalAddresses
    {
        u64 addresses[64];
    };
    static_assert(sizeof(GlobalAddresses) == 512);
    static_assert(offsetof(GlobalAddresses, addresses) == 0);

    struct VisibleMeshletData
    {
        u32 objectIndex;
        u32 meshletIndex;
    };
    static_assert(sizeof(VisibleMeshletData) == 8);
    static_assert(offsetof(VisibleMeshletData, objectIndex) == 0);
    static_assert(offsetof(VisibleMeshletData, meshletIndex) == 4);

    struct MaterialClassRoute
    {
        u64 list;
        u32 count;
        u32 base[2];
        u32 overflow;
        u32 _pad;
    };
    static_assert(sizeof(MaterialClassRoute) == 32);
    static_assert(offsetof(MaterialClassRoute, list) == 0);
    static_assert(offsetof(MaterialClassRoute, count) == 8);
    static_assert(offsetof(MaterialClassRoute, base) == 12);
    static_assert(offsetof(MaterialClassRoute, overflow) == 20);
    static_assert(offsetof(MaterialClassRoute, _pad) == 24);

    struct Meshlet
    {
        u32 vertexOffset;
        u32 triangleOffset;
        u32 vertexCount;
        u32 triangleCount;
        u32 globalTriangleBase;
        u32 materialIndex;
        u32 firstIndex;
        u32 packedCone;
        Vec4 boundingSphere;
        float lodSelfError;
        float lodParentError;
        u32 lodParent;
        u32 lodLevel;
    };
    static_assert(sizeof(Meshlet) == 64);
    static_assert(offsetof(Meshlet, vertexOffset) == 0);
    static_assert(offsetof(Meshlet, triangleOffset) == 4);
    static_assert(offsetof(Meshlet, vertexCount) == 8);
    static_assert(offsetof(Meshlet, triangleCount) == 12);
    static_assert(offsetof(Meshlet, globalTriangleBase) == 16);
    static_assert(offsetof(Meshlet, materialIndex) == 20);
    static_assert(offsetof(Meshlet, firstIndex) == 24);
    static_assert(offsetof(Meshlet, packedCone) == 28);
    static_assert(offsetof(Meshlet, boundingSphere) == 32);
    static_assert(offsetof(Meshlet, lodSelfError) == 48);
    static_assert(offsetof(Meshlet, lodParentError) == 52);
    static_assert(offsetof(Meshlet, lodParent) == 56);
    static_assert(offsetof(Meshlet, lodLevel) == 60);

    struct PixelInfoRecord
    {
        u32 hit;
        u32 triangle;
        u32 object;
        u32 meshlet;
        u32 localTriangle;
        u32 meshletInObject;
        u32 material;
        u32 hasShadow;
        u32 width;
        u32 height;
        u32 _pad0;
        u32 _pad1;
        Vec4 position;
        Vec4 normal;
        Vec4 albedo;
        Vec4 surface;
        Vec4 color;
        Vec4 display;
        u32 lightSource;
        u32 cluster;
        u32 clusterLights;
        u32 lightCount;
        u32 lights[16];
        u32 blendHit;
        u32 blendTriangle;
        u32 blendObject;
        u32 blendMeshlet;
        u32 blendLocalTriangle;
        u32 blendMeshletInObject;
        u32 blendMaterial;
        u32 _pad2;
        Vec4 blendPosition;
    };
    static_assert(sizeof(PixelInfoRecord) == 272);
    static_assert(offsetof(PixelInfoRecord, hit) == 0);
    static_assert(offsetof(PixelInfoRecord, triangle) == 4);
    static_assert(offsetof(PixelInfoRecord, object) == 8);
    static_assert(offsetof(PixelInfoRecord, meshlet) == 12);
    static_assert(offsetof(PixelInfoRecord, localTriangle) == 16);
    static_assert(offsetof(PixelInfoRecord, meshletInObject) == 20);
    static_assert(offsetof(PixelInfoRecord, material) == 24);
    static_assert(offsetof(PixelInfoRecord, hasShadow) == 28);
    static_assert(offsetof(PixelInfoRecord, width) == 32);
    static_assert(offsetof(PixelInfoRecord, height) == 36);
    static_assert(offsetof(PixelInfoRecord, _pad0) == 40);
    static_assert(offsetof(PixelInfoRecord, _pad1) == 44);
    static_assert(offsetof(PixelInfoRecord, position) == 48);
    static_assert(offsetof(PixelInfoRecord, normal) == 64);
    static_assert(offsetof(PixelInfoRecord, albedo) == 80);
    static_assert(offsetof(PixelInfoRecord, surface) == 96);
    static_assert(offsetof(PixelInfoRecord, color) == 112);
    static_assert(offsetof(PixelInfoRecord, display) == 128);
    static_assert(offsetof(PixelInfoRecord, lightSource) == 144);
    static_assert(offsetof(PixelInfoRecord, cluster) == 148);
    static_assert(offsetof(PixelInfoRecord, clusterLights) == 152);
    static_assert(offsetof(PixelInfoRecord, lightCount) == 156);
    static_assert(offsetof(PixelInfoRecord, lights) == 160);
    static_assert(offsetof(PixelInfoRecord, blendHit) == 224);
    static_assert(offsetof(PixelInfoRecord, blendTriangle) == 228);
    static_assert(offsetof(PixelInfoRecord, blendObject) == 232);
    static_assert(offsetof(PixelInfoRecord, blendMeshlet) == 236);
    static_assert(offsetof(PixelInfoRecord, blendLocalTriangle) == 240);
    static_assert(offsetof(PixelInfoRecord, blendMeshletInObject) == 244);
    static_assert(offsetof(PixelInfoRecord, blendMaterial) == 248);
    static_assert(offsetof(PixelInfoRecord, _pad2) == 252);
    static_assert(offsetof(PixelInfoRecord, blendPosition) == 256);

    struct GpuUiSeries
    {
        u64 samples;
        u32 capacity;
        u32 start;
        u32 count;
        float minValue;
        float maxValue;
        u32 anchor;
    };
    static_assert(sizeof(GpuUiSeries) == 32);
    static_assert(offsetof(GpuUiSeries, samples) == 0);
    static_assert(offsetof(GpuUiSeries, capacity) == 8);
    static_assert(offsetof(GpuUiSeries, start) == 12);
    static_assert(offsetof(GpuUiSeries, count) == 16);
    static_assert(offsetof(GpuUiSeries, minValue) == 20);
    static_assert(offsetof(GpuUiSeries, maxValue) == 24);
    static_assert(offsetof(GpuUiSeries, anchor) == 28);

    struct GpuUiInstance
    {
        Vec2 position;
        Vec2 size;
        Vec2 uvMin;
        Vec2 uvMax;
        Vec2 clipMin;
        Vec2 clipMax;
        Vec4 clipRadii;
        u32 color;
        u32 textureIndex;
        u32 shape;
        float param;
    };
    static_assert(sizeof(GpuUiInstance) == 80);
    static_assert(offsetof(GpuUiInstance, position) == 0);
    static_assert(offsetof(GpuUiInstance, size) == 8);
    static_assert(offsetof(GpuUiInstance, uvMin) == 16);
    static_assert(offsetof(GpuUiInstance, uvMax) == 24);
    static_assert(offsetof(GpuUiInstance, clipMin) == 32);
    static_assert(offsetof(GpuUiInstance, clipMax) == 40);
    static_assert(offsetof(GpuUiInstance, clipRadii) == 48);
    static_assert(offsetof(GpuUiInstance, color) == 64);
    static_assert(offsetof(GpuUiInstance, textureIndex) == 68);
    static_assert(offsetof(GpuUiInstance, shape) == 72);
    static_assert(offsetof(GpuUiInstance, param) == 76);
} // namespace spite
