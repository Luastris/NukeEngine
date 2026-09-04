// DDGI debug probes: one small sphere per probe, generated from the vertex id (no vertex
// buffer), instanced over the probes of one volume. The pixel shader lights it with the
// probe's own irradiance, so a volume can be read at a glance.
#include "ddgi.hlsli"

cbuffer GICB { GIVolumeGPU g_GIVol[DDGI_MAX_VOLUMES]; int4 g_GICount; float4 g_GIAtlasInv; };
cbuffer GIProbeCB { float4x4 g_ViewProj; float4 g_ProbeDraw; };   // ProbeDraw: x = volume index, y = sphere radius

struct VSOut { float4 pos : SV_POSITION; float3 nrm : TEXCOORD0; nointerpolation int probe : TEXCOORD1; };

#define SEG_U 12
#define SEG_V 8

VSOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    // 6 vertices per quad of a SEG_U x SEG_V lat-long grid
    uint quad = vid / 6, corner = vid % 6;
    uint qu = quad % SEG_U, qv = quad / SEG_U;
    uint cu = (corner == 1 || corner == 2 || corner == 4) ? 1 : 0;
    uint cv = (corner == 2 || corner == 4 || corner == 5) ? 1 : 0;
    float u = (float)(qu + cu) / SEG_U * 6.2831853;
    float v = (float)(qv + cv) / SEG_V * 3.1415927;
    float3 n = float3(sin(v) * cos(u), cos(v), sin(v) * sin(u));

    GIVolumeGPU vol = g_GIVol[(int)g_ProbeDraw.x];
    float3 center = DDGIProbePos(vol, DDGIProbeCoord(vol, (int)iid));
    VSOut o;
    o.pos   = mul(g_ViewProj, float4(center + n * g_ProbeDraw.y, 1.0));
    o.nrm   = n;
    o.probe = (int)iid;
    return o;
}
