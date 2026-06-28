// Shadow depth pass — vertex shader. Transforms by the light's world-view-proj; passes uv for alpha.
cbuffer ShadowVSCB { float4x4 g_LightWVP; };
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
void main(in VSIn i, out PSIn o)
{
    o.pos = mul(g_LightWVP, float4(i.pos, 1.0));
    o.uv  = i.uv;
}
