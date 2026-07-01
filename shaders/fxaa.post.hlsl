// Stock post-process effect: FXAA (fast approximate anti-aliasing) — Timothy Lottes' classic edge-blur, one pass.
// A cheap AA that complements/replaces MSAA (smooths shader/specular aliasing MSAA misses). Add it LAST in a
// camera's post chain (after tonemap-affecting effects; it works on whatever colour is in the chain). Custom post
// shader: sample g_Source, params in PostParams, resolution in PostFrame, output what it samples' space (linear HDR).
Texture2D    g_Source;
SamplerState g_Source_sampler;
cbuffer PostParams
{
    float g_Amount = 1.0;   // blend of the AA result (0 = off, 1 = full)
};
cbuffer PostFrame
{
    float4 g_Res;           // (width, height, 1/width, 1/height)
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// Perceptual-ish luma. The chain is linear HDR, so sqrt approximates gamma for edge detection (FXAA expects
// perceptual luma). Tone down huge HDR values so bright highlights don't dominate the edge search.
float FxLuma(float3 c)
{
    c = c / (1.0 + max(max(c.r, c.g), c.b));          // soft range compress (HDR-safe)
    return sqrt(dot(c, float3(0.299, 0.587, 0.114)));
}

float4 main(in PSIn i) : SV_Target
{
    const float EDGE_MIN = 1.0 / 128.0, EDGE_THRESHOLD = 1.0 / 8.0;
    const float REDUCE_MUL = 1.0 / 8.0, REDUCE_MIN = 1.0 / 128.0, SPAN_MAX = 8.0;
    float2 rcp = g_Res.zw;

    float3 rgbM = g_Source.Sample(g_Source_sampler, i.uv).rgb;
    float lM  = FxLuma(rgbM);
    float lNW = FxLuma(g_Source.Sample(g_Source_sampler, i.uv + float2(-rcp.x, -rcp.y)).rgb);
    float lNE = FxLuma(g_Source.Sample(g_Source_sampler, i.uv + float2( rcp.x, -rcp.y)).rgb);
    float lSW = FxLuma(g_Source.Sample(g_Source_sampler, i.uv + float2(-rcp.x,  rcp.y)).rgb);
    float lSE = FxLuma(g_Source.Sample(g_Source_sampler, i.uv + float2( rcp.x,  rcp.y)).rgb);

    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    float range = lMax - lMin;
    if (range < max(EDGE_MIN, lMax * EDGE_THRESHOLD)) return float4(rgbM, 1.0);   // not an edge

    float2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));
    float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * REDUCE_MUL, REDUCE_MIN);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -SPAN_MAX, SPAN_MAX) * rcp;

    float3 rgbA = 0.5 * (g_Source.Sample(g_Source_sampler, i.uv + dir * (1.0 / 3.0 - 0.5)).rgb
                       + g_Source.Sample(g_Source_sampler, i.uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    float3 rgbB = rgbA * 0.5 + 0.25 * (g_Source.Sample(g_Source_sampler, i.uv + dir * -0.5).rgb
                                     + g_Source.Sample(g_Source_sampler, i.uv + dir *  0.5).rgb);
    float lB = FxLuma(rgbB);
    float3 aa = (lB < lMin || lB > lMax) ? rgbA : rgbB;
    return float4(lerp(rgbM, aa, saturate(g_Amount)), 1.0);
}
