// World (3D) vertex shader: outputs world-space position and normal for the PBR pixel shader.
// NUKE_INSTANCED opt-in: the per-instance world transform arrives as three HLSL-ready rows in
// ATTRIB3..5 (dot(row_i, float4(pos,1)) = world coord, translation in row_i.w), tint in ATTRIB6,
// custom float4 in ATTRIB7; g_WVP then holds VIEW*PROJ and g_World is identity.
cbuffer CB { float4x4 g_WVP; float4x4 g_World; };
#if NUKE_INSTANCED
// Foliage bend, opt-in via iCustom (z = wind coefficient, w = interaction coefficient).
// KEEP IN SYNC with gbuffer.vs.hlsl and shadow.vs.hlsl: depth, velocity and shadows must bend identically.
#include "nukebend.hlsl"
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2;
              float4 iRow0 : ATTRIB3; float4 iRow1 : ATTRIB4; float4 iRow2 : ATTRIB5;
              float4 iColor : ATTRIB6; float4 iCustom : ATTRIB7; };
struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2;
              float4 icol : TEXCOORD3; float4 icustom : TEXCOORD4; };
void main(in VSIn i, out PSIn o)
{
    float4 p4 = float4(i.pos, 1.0);
    o.wpos    = float3(dot(i.iRow0, p4), dot(i.iRow1, p4), dot(i.iRow2, p4));
    o.wpos    = NukeBend(o.wpos, i.pos, i.iCustom, float3(i.iRow0.w, i.iRow1.w, i.iRow2.w));
    o.pos     = mul(g_WVP, float4(o.wpos, 1.0));
    o.nrm     = float3(dot(i.iRow0.xyz, i.nrm), dot(i.iRow1.xyz, i.nrm), dot(i.iRow2.xyz, i.nrm));
    o.uv      = i.uv;
    o.icol    = i.iColor;
    o.icustom = i.iCustom;
}
#else
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2; };
struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2; };
void main(in VSIn i, out PSIn o)
{
    o.wpos = mul(g_World, float4(i.pos, 1.0)).xyz;
    o.pos  = mul(g_WVP,   float4(i.pos, 1.0));
    o.nrm  = mul((float3x3)g_World, i.nrm);
    o.uv   = i.uv;
}
#endif
