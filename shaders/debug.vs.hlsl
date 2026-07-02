// Debug/gizmo lines: world-space position+color vertices, one VP transform.
cbuffer DebugCB { float4x4 g_VP; };
struct VSIn { float3 pos : ATTRIB0; float4 col : ATTRIB1; };
struct PSIn { float4 pos : SV_POSITION; float4 col : COLOR0; };
void main(in VSIn i, out PSIn o)
{
    o.pos = mul(g_VP, float4(i.pos, 1.0));
    o.col = i.col;
}
