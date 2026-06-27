// Selection outline — vertex shader. Plain transform; the outline thickness comes from a uniform
// scale-up applied on the CPU side (works for flat planes too, unlike normal extrusion). Used by
// both the stencil-mark pass (normal transform) and the draw pass (scaled-up transform).
cbuffer CB { float4x4 g_WVP; float4x4 g_World; };
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2; };
struct PSIn { float4 pos : SV_POSITION; };
void main(in VSIn i, out PSIn o)
{
    o.pos = mul(g_WVP, float4(i.pos, 1.0));
}
