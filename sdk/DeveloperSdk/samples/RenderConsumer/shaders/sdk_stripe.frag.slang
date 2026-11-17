[[shader("fragment")]]
float4 main(float2 uv : TEXCOORD0, nointerpolation uint witness : TEXCOORD1) : SV_Target0
{
    if (uv.y > 0.12 || uv.x < 0.18 || uv.x > 0.82 ||
        (witness == 1 && uv.x >= 0.5) || (witness == 2 && uv.x < 0.5))
        discard;
    return witness == 0 ? float4(1.0, 0.0, 1.0, 1.0) : float4(0.0, 1.0, 1.0, 1.0);
}
