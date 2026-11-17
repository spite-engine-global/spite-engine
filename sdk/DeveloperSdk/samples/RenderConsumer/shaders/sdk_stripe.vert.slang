struct FullscreenVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint witness : TEXCOORD1;
};

[[shader("vertex")]]
FullscreenVertex main([[vk::location(0)]] float2 position : POSITION, uint instance : SV_StartInstanceLocation)
{
    FullscreenVertex result;
    result.position = float4(position, 0.5, 1.0);
    const float2 uv = (position + 1.0) * 0.5;
    result.uv = float2(uv.x, 1.0 - uv.y);
    result.witness = instance;
    return result;
}
