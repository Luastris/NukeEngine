// Volumetric clouds, temporal resolve (full resolution). The march ran at a lower resolution
// with a per-frame sub-texel jitter; this pass upsamples it and blends it with last frame's
// result reprojected through the previous view-projection at the clouds' mean distance
// (clamped to the neighbourhood of the fresh samples: no ghosting behind a moving camera).
// Over a few frames the blend resolves the full resolution and averages the march jitter.
#include "clouds.hlsli"

Texture2D<float4> g_CloudIn;    SamplerState g_CloudIn_sampler;     // the march (linear, clamp)
Texture2D<float2> g_CloudDistIn;                                    // entry / mean distance (Load)
Texture2D<float4> g_CloudHist;  SamplerState g_CloudHist_sampler;   // last frame's resolve
RWTexture2D<float4> g_CloudResolved;
RWTexture2D<float2> g_CloudDistOut;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int2 res = (int2)g_ClScreen.zw;
    if (any((int2)id.xy >= res)) return;
    float2 uv = (float2(id.xy) + 0.5) / g_ClScreen.zw;
    float4 cur = g_CloudIn.SampleLevel(g_CloudIn_sampler, uv, 0);
    int2   mp  = clamp((int2)(uv * g_ClScreen.xy), int2(0, 0), (int2)g_ClScreen.xy - 1);
    float2 dist = g_CloudDistIn.Load(int3(mp, 0));
    g_CloudDistOut[id.xy] = dist;
    float4 outC = cur;
    const float blend = g_ClWind.w;   // history weight (0 = no history)
    if (blend > 0.0)
    {
        // the clouds' point for this texel, reprojected into last frame
        float4 wp = mul(g_ClInvVP, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0));
        float3 d  = normalize(wp.xyz / wp.w - g_ClCam.xyz);
        float  md = min(dist.y, g_ClLayer.z);
        float3 P  = g_ClCam.xyz + d * md;
        float4 pc = mul(g_ClPrevVP, float4(P, 1.0));
        if (pc.w > 1e-4)
        {
            float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
            if (all(puv >= 0.0) && all(puv <= 1.0))
            {
                float4 hist = g_CloudHist.SampleLevel(g_CloudHist_sampler, puv, 0);
                // the fresh neighbourhood bounds the history (a little slack so the jitter still averages)
                float4 mn = cur, mx = cur;
                [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x)
                {
                    float4 s = g_CloudIn.Load(int3(clamp(mp + int2(x, y), int2(0, 0), (int2)g_ClScreen.xy - 1), 0));
                    mn = min(mn, s); mx = max(mx, s);
                }
                float4 pad = (mx - mn) * 0.5 + 0.002;
                hist = clamp(hist, mn - pad, mx + pad);
                outC = lerp(cur, hist, blend);
            }
        }
    }
    g_CloudResolved[id.xy] = outC;
}
