// Target-filtered decal PS: the surface position arrives interpolated from the mesh re-draw
// (no depth reconstruction), the box test / projection / appear envelope match decal.ps.
cbuffer DecalCB
{
    float4x4 g_WVP;
    float4x4 g_InvWorld;
    float4x4 g_MeshWorld;
    float4   g_Tint;
    float4   g_Params;     // x = intensity, y = angleFade, z = appear 0..1, w = appear mode
    float4   g_ProjAxis;   // xyz = world projection axis (box +Z)
    float4   g_Res;
};
Texture2D    g_DecalTex;  SamplerState g_DecalTex_sampler;

struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; };

float4 main(in PSIn i) : SV_TARGET
{
    float3 lp = mul(g_InvWorld, float4(i.wpos, 1.0)).xyz;
    if (abs(lp.x) > 0.5 || abs(lp.y) > 0.5 || abs(lp.z) > 0.5) discard;

    float2 duv = float2(lp.x + 0.5, 0.5 - lp.y);
    float4 tex = g_DecalTex.Sample(g_DecalTex_sampler, duv) * g_Tint;

    float3 n   = normalize(cross(ddx(i.wpos), ddy(i.wpos)));
    float  ndl = abs(dot(n, g_ProjAxis.xyz));
    float  fade = (g_Params.y > 0.001) ? smoothstep(0.0, g_Params.y, ndl) : 1.0;

    float a = saturate(tex.a * fade * g_Params.x);

    const float t = saturate(g_Params.z);
    if (g_Params.w > 0.5)
    {
        const float f   = 0.18;
        const float key = tex.a * (1.0 - saturate(length(lp.xy) * 1.6));
        const float th  = (1.0 - t) * (1.0 + f) - f;
        a *= saturate((key - th) / f);
    }
    else
        a *= t;
    return float4(tex.rgb * a, a);
}
