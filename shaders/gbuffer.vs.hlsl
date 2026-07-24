// G-buffer prepass vertex shader. Like world.vs but ALSO outputs the current + previous clip position so the pixel
// shader can write a screen-space motion vector (velocity) for TAA. g_PrevWVP = prevWorld * prevView * prevProj
// (per-object previous transform + previous camera), both UNjittered — TAA reprojection must be jitter-free.
// NUKE_INSTANCED (7.1): per-instance world rows (ATTRIB3..5); g_WVP/g_PrevWVP then carry the CURRENT and
// PREVIOUS camera view*proj only — instance motion is camera-only (per-instance previous transforms are
// not stored; static instances get exact velocity, moving ones fall back to camera reprojection).
cbuffer CB { float4x4 g_WVP; float4x4 g_World; float4x4 g_PrevWVP; };
#if NUKE_INSTANCED
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
    o.pos      = mul(g_WVP, float4(o.wpos, 1.0));
    o.nrm      = float3(dot(i.iRow0.xyz, i.nrm), dot(i.iRow1.xyz, i.nrm), dot(i.iRow2.xyz, i.nrm));
    o.uv       = i.uv;
    o.curClip  = o.pos;
    o.prevClip = mul(g_PrevWVP, float4(o.wpos, 1.0));
    // Per-INSTANCE id: hash of the instance translation (rows' .w) — same contract as per-object.
    float3 pivot = float3(i.iRow0.w, i.iRow1.w, i.iRow2.w);
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
    // Generic per-OBJECT id: a stable hash of the object's pivot, flat across the draw.
    // Consumers derive whatever they need from it (musicvis: a pitch class; outlines /
    // per-object masks later) — the G-buffer itself carries NO effect semantics.
    float3 pivot = mul(g_World, float4(0.0, 0.0, 0.0, 1.0)).xyz;
    o.objId = frac(sin(dot(pivot, float3(127.1, 311.7, 74.7))) * 43758.5453);
}
#endif
