// Shadow depth pass — vertex shader. Transforms by the light's world-view-proj; passes uv for alpha.
// NUKE_INSTANCED (7.1): per-instance world rows (ATTRIB3..5, HLSL-ready: row·(pos,1) = world);
// the CB then carries the LIGHT's view*proj only. Tint/custom (ATTRIB6..7) ride along unused —
// the instance buffer layout is shared with the world pass.
cbuffer ShadowVSCB { float4x4 g_LightWVP; };
#if NUKE_INSTANCED
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2;
              float4 iRow0 : ATTRIB3; float4 iRow1 : ATTRIB4; float4 iRow2 : ATTRIB5;
              float4 iColor : ATTRIB6; float4 iCustom : ATTRIB7; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
void main(in VSIn i, out PSIn o)
{
    float4 p4 = float4(i.pos, 1.0);
    float3 wp = float3(dot(i.iRow0, p4), dot(i.iRow1, p4), dot(i.iRow2, p4));
    o.pos = mul(g_LightWVP, float4(wp, 1.0));
    o.uv  = i.uv;
}
#else
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
void main(in VSIn i, out PSIn o)
{
    o.pos = mul(g_LightWVP, float4(i.pos, 1.0));
    o.uv  = i.uv;
}
#endif
