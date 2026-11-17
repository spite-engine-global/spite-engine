struct HdrPushConstants
{
    float4* color;
};

[[vk::push_constant]] ConstantBuffer<HdrPushConstants> pc;

[[shader("fragment")]]
float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    if (uv.x < 0.1 || uv.x > 0.2 || uv.y < 0.4 || uv.y > 0.5)
        discard;
    return *pc.color;
}
