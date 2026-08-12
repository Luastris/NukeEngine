// Displacement tessellation hull: 3-control-point passthrough. Tess factors are UNIFORM from
// MatCB g_Disp.w — CPU-computed per draw with a distance fade, so 1.0 collapses to the plain
// (untessellated) look and the renderer swaps back to the normal PSO seamlessly.
// MatCB layout must byte-match world.ps.
cbuffer MatCB { float4 g_Color; float4 g_Params; float4 g_Params2; float4 g_Emissive2; float4 g_UVT; float4 g_UVT2; float4 g_Disp;
                float4 g_Ov0;  float4 g_Ov1;  float4 g_Ov2;  float4 g_Ov3;  float4 g_Ov4;  float4 g_Ov5;  float4 g_Ov6;  float4 g_Ov7;
                float4 g_OvT0; float4 g_OvT1; float4 g_OvT2; float4 g_OvT3; float4 g_OvT4; float4 g_OvT5; float4 g_OvT6; float4 g_OvT7;
                float4 g_OvP0; float4 g_OvP1; float4 g_OvP2; float4 g_OvP3; float4 g_OvP4; float4 g_OvP5; float4 g_OvP6; float4 g_OvP7;
                float4 g_OvM0; float4 g_OvM1; float4 g_OvM2; float4 g_OvMQ;
                float4 g_Det; float4 g_Var; };

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
