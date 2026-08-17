// Screen-space decal PS: for each box pixel, read the scene depth, reconstruct the world surface,
// transform it into the decal box's local space, and if inside the box, project the decal texture.
cbuffer DecalCB
{
    float4x4 g_WVP;
    float4x4 g_InvWorld;
    float4x4 g_InvViewProj;
    float4   g_Tint;
    float4   g_Params;     // x = intensity, y = angleFade, z = appear 0..1, w = appear mode (0 fade, 1 spread)
    float4   g_ProjAxis;   // xyz = world projection axis (box +Z)
    float4   g_Res;        // xy = resolution
};
Texture2D    g_DecalTex;  SamplerState g_DecalTex_sampler;
Texture2D    g_Depth;     SamplerState g_Depth_sampler;

float4 main(float4 svpos : SV_POSITION) : SV_TARGET
{
    float2 uv = svpos.xy / g_Res.xy;
    float  d  = g_Depth.SampleLevel(g_Depth_sampler, uv, 0).r;

    // World position of the scene surface under this pixel (D3D clip: y up, z in [0,1]).
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    float4 wp = mul(g_InvViewProj, clip);
    wp /= wp.w;

    float3 lp = mul(g_InvWorld, float4(wp.xyz, 1.0)).xyz;
    if (abs(lp.x) > 0.5 || abs(lp.y) > 0.5 || abs(lp.z) > 0.5) discard;

    float2 duv = float2(lp.x + 0.5, 0.5 - lp.y);          // box local -> UV (y down)
    float4 tex = g_DecalTex.Sample(g_DecalTex_sampler, duv) * g_Tint;

    // Angle fade: surface normal from depth derivatives, against the projection axis.
    float3 n   = normalize(cross(ddx(wp.xyz), ddy(wp.xyz)));
    float  ndl = abs(dot(n, g_ProjAxis.xyz));
    float  fade = (g_Params.y > 0.001) ? smoothstep(0.0, g_Params.y, ndl) : 1.0;

    // PREMULTIPLIED output for the modulate blend (dest * lerp(1, tex.rgb, a)): the decal
    // tints the ALREADY-LIT pixel, so lighting and shadows stay intact underneath it.
    float a = saturate(tex.a * fade * g_Params.x);

    // Appear envelope: Spread reveals by the texture's density SHAPED from the decal center —
    // the dense core lands first and edges creep outward (blood); a flat-alpha texture still
    // creeps radially instead of popping. Fade is a plain alpha ramp.
    const float t = saturate(g_Params.z);
    if (g_Params.w > 0.5)
    {
        const float f   = 0.18;
        const float key = tex.a * (1.0 - saturate(length(lp.xy) * 1.6));
        const float th  = (1.0 - t) * (1.0 + f) - f;   // t=0 -> nothing shown, t=1 -> everything
        a *= saturate((key - th) / f);
    }
    else
        a *= t;
    return float4(tex.rgb * a, a);
}
