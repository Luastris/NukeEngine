// G-buffer prepass vertex shader: world.vs plus current/previous clip position for the TAA motion vector.
// g_PrevWVP = prevWorld * prevView * prevProj, UNjittered (TAA reprojection must be jitter-free).
// NUKE_INSTANCED opt-in: g_WVP/g_PrevWVP then carry only the current/previous camera view*proj.
cbuffer CB { float4x4 g_WVP; float4x4 g_World; float4x4 g_PrevWVP; };
#if NUKE_INSTANCED
// KEEP IN SYNC with world.vs.hlsl: the depth and velocity must match the bent lit surface.
// prevClip uses the SAME bent position, so sway is deliberately excluded from velocity.
#include "nukebend.hlsl"
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2;
              float4 iRow0 : ATTRIB3; float4 iRow1 : ATTRIB4; float4 iRow2 : ATTRIB5;
              float4 iColor : ATTRIB6; float4 iCustom : ATTRIB7; };
struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2;
              float4 curClip : TEXCOORD3; float4 prevClip : TEXCOORD4;
              nointerpolation float objId : TEXCOORD5; };
void main(in VSIn i, out PSIn o)
{
    float4 p4 = float4(i.pos, 1.0);
    o.wpos     = float3(dot(i.iRow0, p4), dot(i.iRow1, p4), dot(i.iRow2, p4));
    o.wpos     = NukeBend(o.wpos, i.pos, i.iCustom, float3(i.iRow0.w, i.iRow1.w, i.iRow2.w));
    o.pos      = mul(g_WVP, float4(o.wpos, 1.0));
    o.nrm      = float3(dot(i.iRow0.xyz, i.nrm), dot(i.iRow1.xyz, i.nrm), dot(i.iRow2.xyz, i.nrm));
    o.uv       = i.uv;
    o.curClip  = o.pos;
    o.prevClip = mul(g_PrevWVP, float4(o.wpos, 1.0));
    // Per-instance id: hash of the instance translation, same contract as the per-object id below.
    float3 pivot = float3(i.iRow0.w, i.iRow1.w, i.iRow2.w);
    o.objId = frac(sin(dot(pivot, float3(127.1, 311.7, 74.7))) * 43758.5453);
}
#elif NUKE_SKINNED
// GPU-skinned meshes: stream 3 carries the PREVIOUS frame's skinned position (written by
// skin.cs) — velocity is the true pose delta, not the rigid-transform approximation.
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2; float3 prevPos : ATTRIB3; };
struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2;
              float4 curClip : TEXCOORD3; float4 prevClip : TEXCOORD4;
              nointerpolation float objId : TEXCOORD5; };
void main(in VSIn i, out PSIn o)
{
    o.wpos     = mul(g_World, float4(i.pos, 1.0)).xyz;
    o.pos      = mul(g_WVP,     float4(i.pos, 1.0));
    o.nrm      = mul((float3x3)g_World, i.nrm);
    o.uv       = i.uv;
    o.curClip  = o.pos;
    o.prevClip = mul(g_PrevWVP, float4(i.prevPos, 1.0));
    float3 pivot = mul(g_World, float4(0.0, 0.0, 0.0, 1.0)).xyz;
    o.objId = frac(sin(dot(pivot, float3(127.1, 311.7, 74.7))) * 43758.5453);
}
#else
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2; };
struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2;
              float4 curClip : TEXCOORD3; float4 prevClip : TEXCOORD4;
              nointerpolation float objId : TEXCOORD5; };   // per-OBJECT id (generic G-buffer channel)
void main(in VSIn i, out PSIn o)
{
    o.wpos     = mul(g_World, float4(i.pos, 1.0)).xyz;
    o.pos      = mul(g_WVP,     float4(i.pos, 1.0));
    o.nrm      = mul((float3x3)g_World, i.nrm);
    o.uv       = i.uv;
    o.curClip  = o.pos;
    o.prevClip = mul(g_PrevWVP, float4(i.pos, 1.0));
    // Generic per-object id: a stable hash of the object's pivot, flat across the draw.
    float3 pivot = mul(g_World, float4(0.0, 0.0, 0.0, 1.0)).xyz;
    o.objId = frac(sin(dot(pivot, float3(127.1, 311.7, 74.7))) * 43758.5453);
}
#endif
