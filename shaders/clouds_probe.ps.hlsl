// Volumetric clouds over a reflection-probe face: the face is drawn in place (the sky is already
// there, the geometry comes after), so this blends the march straight onto the target:
// out = dst * transmittance + in-scatter (blend ONE / SRC_ALPHA). The LDR path tonemaps the
// in-scatter like world.ps does (an approximation: the reflection, not the frame).
#include "clouds.hlsli"

Texture2D g_Clouds; SamplerState g_Clouds_sampler;   // the face's march (linear)

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float3 ToLDR(float3 c)
{
    float W = max(g_ClMisc.w, 1e-3);
    c = c * (1.0 + c / (W * W)) / (1.0 + c);
    return pow(max(c, 0.0), 1.0 / 2.2);
}

float4 main(in PSIn i) : SV_Target
{
    float4 cl = g_Clouds.Sample(g_Clouds_sampler, i.uv);
    float3 c  = (g_ClMisc.z > 0.5) ? ToLDR(cl.rgb) : cl.rgb;
    return float4(c, cl.a);
}
