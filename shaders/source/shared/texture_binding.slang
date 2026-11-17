#ifndef TEXTURE_BINDING_SLANG
#define TEXTURE_BINDING_SLANG

[[vk::binding(0, 1)]] Texture2D globalTextureImages[];
[[vk::binding(1, 1)]] StructuredBuffer<uint> globalTextureSamplerIndices;
[[vk::binding(0, 2)]] SamplerState globalTextureSamplers[];

// Implicit-LOD sample (GLSL texture()); fragment shaders only.
float4 SAMPLE_TEXTURE(uint index, float2 uv)
{
    Texture2D image = globalTextureImages[NonUniformResourceIndex(index)];
    uint samplerIndex = globalTextureSamplerIndices[NonUniformResourceIndex(index)];
    SamplerState samplerState = globalTextureSamplers[NonUniformResourceIndex(samplerIndex)];
    return image.Sample(samplerState, uv);
}

// Explicit base-level sample; valid in compute stages.
float4 SAMPLE_TEXTURE_LOD0(uint index, float2 uv)
{
    Texture2D image = globalTextureImages[NonUniformResourceIndex(index)];
    uint samplerIndex = globalTextureSamplerIndices[NonUniformResourceIndex(index)];
    SamplerState samplerState = globalTextureSamplers[NonUniformResourceIndex(samplerIndex)];
    return image.SampleLevel(samplerState, uv, 0);
}

#endif
