// Displacement tessellation domain: interpolate the patch, displace along the normal by the
// height map (g_Disp = POM depth, displacement in world units, mid level, tess factor) and
// project. Outputs the exact PSIn world.ps expects, so the SAME pixel shader lights it.
// The displacement fades with the tess factor: at factor 1 it is zero, matching the plain PSO.
cbuffer CB { float4x4 g_WVP; float4x4 g_World; };
cbuffer MatCB {
#include "matcb_std.hlsli"
};
#define NUKE_MAT_NO_TEX
#include "nuke_material.hlsli"
Texture2D    g_Height;
SamplerState g_Height_sampler;

struct HSOut { float3 pos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2; };
struct PatchTess { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2; };

[domain("tri")]
PSIn main(PatchTess pt, float3 b : SV_DomainLocation, const OutputPatch<HSOut, 3> p)
{
    float3 pos = p[0].pos * b.x + p[1].pos * b.y + p[2].pos * b.z;
    float3 nrm = normalize(p[0].nrm * b.x + p[1].nrm * b.y + p[2].nrm * b.z);
    float2 uv  = p[0].uv  * b.x + p[1].uv  * b.y + p[2].uv  * b.z;

    // Sample the height where the material maps land: the same UV transform as the passes.
    float2 tl = (abs(g_UVT.x) + abs(g_UVT.y) < 1e-6) ? float2(1.0, 1.0) : g_UVT.xy;
    float2 suv = uv * tl + g_UVT.zw;
    if (abs(g_UVT2.x) > 1e-6)
    {
        float sr, cr; sincos(g_UVT2.x, sr, cr);
        suv = float2(suv.x * cr - suv.y * sr, suv.x * sr + suv.y * cr);
    }
    float h = g_Height.SampleLevel(g_Height_sampler, suv, 0).r;

    // Analytic uv->world frame from the patch corners (no derivatives in a DS): masks below
    // measure PHYSICAL meters, matching the pixel passes exactly (rings stay rings).
    {
        float2 t0 = p[0].uv * tl + g_UVT.zw, t1 = p[1].uv * tl + g_UVT.zw, t2 = p[2].uv * tl + g_UVT.zw;
        if (abs(g_UVT2.x) > 1e-6)
        {
            float sr2, cr2; sincos(g_UVT2.x, sr2, cr2);
            t0 = float2(t0.x * cr2 - t0.y * sr2, t0.x * sr2 + t0.y * cr2);
            t1 = float2(t1.x * cr2 - t1.y * sr2, t1.x * sr2 + t1.y * cr2);
            t2 = float2(t2.x * cr2 - t2.y * sr2, t2.x * sr2 + t2.y * cr2);
        }
        const float3 w0 = mul(g_World, float4(p[0].pos, 1.0)).xyz;
        const float3 w1 = mul(g_World, float4(p[1].pos, 1.0)).xyz;
        const float3 w2 = mul(g_World, float4(p[2].pos, 1.0)).xyz;
        const float2 dU1 = t1 - t0, dU2 = t2 - t0;
        const float  jd  = dU1.x * dU2.y - dU1.y * dU2.x;
        if (abs(jd) > 1e-9)
        {
            g_NukeMaskT = ( (w1 - w0) * dU2.y - (w2 - w0) * dU1.y) / jd;
            g_NukeMaskB = (-(w1 - w0) * dU2.x + (w2 - w0) * dU1.x) / jd;
        }
    }

    // Displacement scales WITH the object (like the texture does): dScale is in the mesh's
    // LOCAL units, so a scaled-up atom keeps the same RELATIVE relief the preview shows.
    // Full height across the whole tessellated range; only the last factor step down to 1
    // (the plain-PSO handover) ramps to zero so the swap stays seam-free.
    float fade = saturate(g_Disp.w - 1.0);
    float dScale = g_Disp.y, dMid = g_Disp.z;
    [branch] if (g_DispT.w > 0.5)   // masked displacement tween: the surface moves only inside the mask
    {
        float mtw = NukeMaskWNoTex((int)(g_DispT.w - 0.5), suv, mul(g_World, float4(pos, 1.0)).xyz);
        dScale = lerp(dScale, g_DispT.y, mtw);
        dMid   = lerp(dMid,   g_DispT.z, mtw);
    }
    pos += nrm * ((h - dMid) * dScale) * fade;

    PSIn o;
    o.pos  = mul(g_WVP,   float4(pos, 1.0));
    o.wpos = mul(g_World, float4(pos, 1.0)).xyz;
    o.nrm  = mul((float3x3)g_World, nrm);
    o.uv   = uv;
    return o;
}
