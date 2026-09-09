// Froxel volumetrics, pass 1b: the temporal blend, between inject and integrate. The raw grid
// (this frame's medium + in-scatter) is blended with last frame's grid reprojected through the
// previous view-projection - but the history is first CLAMPED to the min/max of the raw 3x3x3
// neighbourhood (TAA neighbourhood clamping): a moving medium (fluid fog, wind-drifting clumps)
// otherwise dragged a ten-frame smear behind every clump - parallel streaks along the wind -
// and a turning camera dragged the sun's glow. Static air still averages the jittered samples.
#include "vol.hlsli"

Texture3D<float4>   g_ScatRaw;                                      // this frame, unblended
Texture3D<float4>   g_ScatPrev;  SamplerState g_ScatPrev_sampler;   // last frame's blended grid (linear, clamp)
RWTexture3D<float4> g_Scat;                                         // blended: integrate reads it, next frame's history

[numthreads(8, 8, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int3 grid = (int3)g_VolGrid.xyz;
    if (any((int3)id >= grid)) return;
    float4 cur = g_ScatRaw[id];
    if (g_VolMisc.y <= 0.0) { g_Scat[id] = cur; return; }
    float3 f = (float3(id) + 0.5) / float3(grid);
    float4 clip = float4(f.x * 2.0 - 1.0, 1.0 - f.y * 2.0, VolDeviceDepth(VolSliceZ(f.z * g_VolGrid.z)), 1.0);
    float4 wp4  = mul(g_VolInvViewProj, clip);
    float3 P    = wp4.xyz / wp4.w;
    float4 pc   = mul(g_VolPrevViewProj, float4(P, 1.0));
    if (pc.w <= 1e-4) { g_Scat[id] = cur; return; }
    float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
    float  pw  = VolZSlice(pc.w) / g_VolGrid.z;
    if (any(puv < 0.0) || any(puv > 1.0) || pw < 0.0 || pw > 1.0) { g_Scat[id] = cur; return; }
    float4 prev = g_ScatPrev.SampleLevel(g_ScatPrev_sampler, float3(puv, pw), 0);
    // the raw neighbourhood's bounds, widened a little so a lone jittered sample still averages
    float4 mn = cur, mx = cur;
    [unroll] for (int z = -1; z <= 1; ++z)
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        float4 s = g_ScatRaw[clamp((int3)id + int3(x, y, z), int3(0, 0, 0), grid - 1)];
        mn = min(mn, s); mx = max(mx, s);
    }
    float4 pad = (mx - mn) * 0.25 + 1e-4;
    prev = clamp(prev, mn - pad, mx + pad);
    g_Scat[id] = lerp(cur, prev, g_VolMisc.y);
}
