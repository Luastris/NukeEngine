// G-buffer prepass vertex shader. Like world.vs but ALSO outputs the current + previous clip position so the pixel
// shader can write a screen-space motion vector (velocity) for TAA. g_PrevWVP = prevWorld * prevView * prevProj
// (per-object previous transform + previous camera), both UNjittered — TAA reprojection must be jitter-free.
cbuffer CB { float4x4 g_WVP; float4x4 g_World; float4x4 g_PrevWVP; };
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
