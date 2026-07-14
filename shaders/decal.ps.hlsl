// Screen-space decal PS: for each box pixel, read the scene depth, reconstruct the world surface,
// transform it into the decal box's local space, and if inside the box, project the decal texture.
cbuffer DecalCB
{
    float4x4 g_WVP;
    float4x4 g_InvWorld;
    float4x4 g_InvViewProj;
    float4   g_Tint;
    float4   g_Params;     // x = intensity, y = angleFade
    float4   g_ProjAxis;   // xyz = world projection axis (box +Z)
    float4   g_Res;        // xy = resolution
};
Texture2D    g_DecalTex;  SamplerState g_DecalTex_sampler;
Texture2D    g_Depth;     SamplerState g_Depth_sampler;

float4 main(float4 svpos : SV_POSITION) : SV_TARGET
{
    float2 uv = svpos.xy / g_Res.xy;
    float  d  = g_Depth.SampleLevel(g_Depth_sampler, uv, 0).r;

    // reconstruct the world position of the scene surface under this pixel (D3D clip: y up, z in [0,1])
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    float4 wp = mul(g_InvViewProj, clip);
    wp /= wp.w;

    // into the decal box's local space; discard anything outside the unit box
    float3 lp = mul(g_InvWorld, float4(wp.xyz, 1.0)).xyz;
    if (abs(lp.x) > 0.5 || abs(lp.y) > 0.5 || abs(lp.z) > 0.5) discard;

    float2 duv = float2(lp.x + 0.5, 0.5 - lp.y);          // box local -> UV (y down)
    float4 tex = g_DecalTex.Sample(g_DecalTex_sampler, duv) * g_Tint;

    // angle fade: surface normal from depth derivatives, vs the projection axis (grazing surfaces fade)
    float3 n   = normalize(cross(ddx(wp.xyz), ddy(wp.xyz)));
    float  ndl = abs(dot(n, g_ProjAxis.xyz));
    float  fade = (g_Params.y > 0.001) ? smoothstep(0.0, g_Params.y, ndl) : 1.0;

    return float4(tex.rgb, tex.a * fade * g_Params.x);
}
