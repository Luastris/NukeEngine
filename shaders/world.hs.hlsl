// Displacement tessellation hull: 3-control-point passthrough. Tess factors are UNIFORM from
// MatCB g_Disp.w — CPU-computed per draw with a distance fade, so 1.0 collapses to the plain
// (untessellated) look and the renderer swaps back to the normal PSO seamlessly.
// MatCB layout must byte-match world.ps.
cbuffer MatCB {
#include "matcb_std.hlsli"
};

struct HSIn  { float3 pos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2; };
struct HSOut { float3 pos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2; };
struct PatchTess { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };

PatchTess PatchFn(InputPatch<HSIn, 3> p)
{
    PatchTess t;
    float f = max(g_Disp.w, 1.0);
    t.edge[0] = f; t.edge[1] = f; t.edge[2] = f;
    t.inside = f;
    return t;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchFn")]
HSOut main(InputPatch<HSIn, 3> p, uint i : SV_OutputControlPointID)
{
    HSOut o;
    o.pos = p[i].pos; o.nrm = p[i].nrm; o.uv = p[i].uv;
    return o;
}
