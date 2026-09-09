// Froxel volumetrics, pass 3: composite. Every pixel takes the integrated column at its own
// depth (trilinear: the depth picks the slice, xy blends the four froxel columns) and mixes
// scene * transmittance + in-scatter. Runs before the post chain (after the reflection
// effects); on the LDR path the scene is un-tonemapped around the mix.
#include "vol.hlsli"

Texture2D          g_Source;   SamplerState g_Source_sampler;   // scene colour
Texture2D          g_Depth;    SamplerState g_Depth_sampler;    // prepass device depth (point)
Texture3D<float4>  g_Volume;   SamplerState g_Volume_sampler;   // integrated grid (linear, clamp)

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float3 FromLDR(float3 c)   // world.ps tonemap (extended Reinhard + sRGB) undone
{
    c = pow(max(c, 0.0), 2.2);
    float W = max(g_VolMisc.w, 1e-3);
    float3 y = min(c, 0.999);
    return max(0.5 * W * W * ((y - 1.0) + sqrt((1.0 - y) * (1.0 - y) + 4.0 * y / (W * W))), 0.0);
}
float3 ToLDR(float3 c)
{
    float W = max(g_VolMisc.w, 1e-3);
    c = c * (1.0 + c / (W * W)) / (1.0 + c);
    return pow(max(c, 0.0), 1.0 / 2.2);
}

float4 main(in PSIn i) : SV_Target
{
    float4 src = g_Source.Sample(g_Source_sampler, i.uv);
    float  d   = g_Depth.Sample(g_Depth_sampler, i.uv).r;
    float  z   = (d >= 0.99999) ? g_VolRange.y : VolLinearZ(d);   // sky: the whole column
    // Slice k's texel holds the column up to SliceZ(k + 1): continuous index s -> texel (s - 0.5)
    float  w   = saturate((VolZSlice(z) - 0.5) / g_VolGrid.z);
    float4 vol = g_Volume.SampleLevel(g_Volume_sampler, float3(i.uv, w), 0);
    float3 c   = src.rgb;
    if (g_VolMisc.z > 0.5) c = ToLDR(FromLDR(c) * vol.a + vol.rgb);
    else                   c = c * vol.a + vol.rgb;
    return float4(c, src.a);
}
