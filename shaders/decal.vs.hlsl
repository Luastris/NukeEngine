// Screen-space decal: rasterize the decal's BOX volume; the PS reconstructs the scene surface from
// the depth prepass and projects the decal texture onto it. VS just transforms the unit cube.
cbuffer DecalCB
{
    float4x4 g_WVP;          // box world * view * proj
    float4x4 g_InvWorld;     // world -> decal-local (box is [-0.5,0.5]^3)
    float4x4 g_InvViewProj;  // clip -> world (depth reconstruction)
    float4   g_Tint;
    float4   g_Params;       // x = intensity, y = angleFade
    float4   g_ProjAxis;     // xyz = world projection axis (the box's local +Z, normalized)
    float4   g_Res;          // xy = target resolution (pixels)
};
struct VSIn { float3 pos : ATTRIB0; };
struct PSIn { float4 pos : SV_POSITION; };
void main(in VSIn i, out PSIn o) { o.pos = mul(g_WVP, float4(i.pos, 1.0)); }
