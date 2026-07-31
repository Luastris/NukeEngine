// Selection outline vertex shader, used by both the stencil-mark and draw passes.
// Thickness comes from a uniform scale-up applied CPU-side, not from normal extrusion.
cbuffer CB { float4x4 g_WVP; float4x4 g_World; };
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2; };
struct PSIn { float4 pos : SV_POSITION; };
void main(in VSIn i, out PSIn o)
{
    o.pos = mul(g_WVP, float4(i.pos, 1.0));
}
