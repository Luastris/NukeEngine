// Temporal anti-aliasing — built-in post effect (renderer special-cases it: isTAA). Accumulates sub-pixel-jittered
// frames over time. The renderer jitters ONLY the colour projection each frame; the depth prepass + these matrices
// are UNJITTERED, so reprojection stays clean. Reprojects the previous resolved frame by depth + previous view/proj,
// clamps it to the local 3x3 colour neighbourhood (anti-ghosting), and blends. Camera + static motion handled;
// fast dynamic objects may ghost (no velocity buffer yet). Mirrors SSR's mul(Matrix, vector) convention.
Texture2D    g_Source;   SamplerState g_Source_sampler;    // current (jittered) HDR colour
Texture2D    g_Depth;    SamplerState g_Depth_sampler;     // current (UNjittered) prepass depth
Texture2D    g_History;  SamplerState g_History_sampler;   // previous resolved frame
Texture2D    g_Velocity; SamplerState g_Velocity_sampler;  // per-pixel screen-space motion (uv space); geometry only

cbuffer PostParams
{
    float g_Blend = 0.9;   // history weight (0 = no TAA, 0.9 = strong accumulation)
};
cbuffer TAACB
{
    float4x4 g_InvProj;    // current proj^-1
    float4x4 g_InvView;    // current view^-1
    float4x4 g_PrevView;   // previous frame view
    float4x4 g_PrevProj;   // previous frame proj
    float4   g_TAARes;     // (w, h, 1/w, 1/h)
    float4   g_TAAFlags;   // x = hasHistory (0 on the first frame / after resize)
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(in PSIn i) : SV_Target
{
    float3 cur = g_Source.Sample(g_Source_sampler, i.uv).rgb;
    if (g_TAAFlags.x < 0.5 || g_Blend <= 0.0) return float4(cur, 1.0);   // no history yet -> passthrough

    float depth = g_Depth.Sample(g_Depth_sampler, i.uv).r;
    float2 puv;
    if (depth < 0.99999)   // geometry: use the motion vector (handles camera + static + DYNAMIC objects)
    {
        puv = i.uv - g_Velocity.Sample(g_Velocity_sampler, i.uv).rg;
    }
    else                   // sky / background: no velocity -> reproject the far plane by the previous camera
    {
        float4 clip = float4(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0, depth, 1.0);
        float4 vp = mul(g_InvProj, clip); float3 vpos = vp.xyz / vp.w;
        float3 wpos = mul(g_InvView, float4(vpos, 1.0)).xyz;
        float3 pv   = mul(g_PrevView, float4(wpos, 1.0)).xyz;
        float4 pc   = mul(g_PrevProj, float4(pv, 1.0));
        if (pc.w <= 1e-4) return float4(cur, 1.0);
        float2 pndc = pc.xy / pc.w;
        puv = float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5);
    }
    if (puv.x < 0.0 || puv.x > 1.0 || puv.y < 0.0 || puv.y > 1.0) return float4(cur, 1.0);   // off-screen -> no history

    float3 hist = g_History.Sample(g_History_sampler, puv).rgb;

    // Neighbourhood clamp (anti-ghosting): constrain history to the local colour AABB.
    float2 rcp = g_TAARes.zw;
    float3 mn = cur, mx = cur;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float3 c = g_Source.Sample(g_Source_sampler, i.uv + float2(x, y) * rcp).rgb;
            mn = min(mn, c); mx = max(mx, c);
        }
    hist = clamp(hist, mn, mx);

    return float4(lerp(cur, hist, g_Blend), 1.0);
}
