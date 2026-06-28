// World (3D) pass — vertex shader. Position + normal + uv; outputs world-space pos & normal for PBR.
cbuffer CB { float4x4 g_WVP; float4x4 g_World; };
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2; };
struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2; };
void main(in VSIn i, out PSIn o)
{
    o.wpos = mul(g_World, float4(i.pos, 1.0)).xyz;
    o.pos  = mul(g_WVP,   float4(i.pos, 1.0));
    o.nrm  = mul((float3x3)g_World, i.nrm);
    o.uv   = i.uv;
}
