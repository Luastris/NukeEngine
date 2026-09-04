// DDGI debug probes: the sphere shows the probe's irradiance in the direction of its normal.
#include "ddgi.hlsli"

cbuffer GICB { GIVolumeGPU g_GIVol[DDGI_MAX_VOLUMES]; int4 g_GICount; float4 g_GIAtlasInv; };
cbuffer GIProbeCB { float4x4 g_ViewProj; float4 g_ProbeDraw; };
Texture2D g_GIIrr;  SamplerState g_GIIrr_sampler;

struct PSIn { float4 pos : SV_POSITION; float3 nrm : TEXCOORD0; nointerpolation int probe : TEXCOORD1; };

float4 main(in PSIn i) : SV_Target
{
    GIVolumeGPU vol = g_GIVol[(int)g_ProbeDraw.x];
    float3 n = normalize(i.nrm);
    int sidx = DDGIStorageIndex(vol, DDGIProbeCoord(vol, i.probe));
    float3 irr = g_GIIrr.SampleLevel(g_GIIrr_sampler, DDGIIrrUV(vol, sidx, n, g_GIAtlasInv.xy), 0).rgb * vol.origin.w;
    return float4(irr, 1.0);
}
