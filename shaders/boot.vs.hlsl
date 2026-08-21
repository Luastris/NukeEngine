// Boot world shader (vertex): the stand-in every world draw uses until its real pipeline
// lands from the background builder. Same input layout and CB contract as world.vs
// (NUKE_INSTANCED rows in ATTRIB3..5), nothing else.
cbuffer CB { float4x4 g_WVP; float4x4 g_World; };
#if NUKE_INSTANCED
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2;
              float4 iRow0 : ATTRIB3; float4 iRow1 : ATTRIB4; float4 iRow2 : ATTRIB5;
              float4 iColor : ATTRIB6; float4 iCustom : ATTRIB7; };
#else
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2; };
#endif
struct PSIn { float4 pos : SV_POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
void main(in VSIn i, out PSIn o)
{
    float4 p = float4(i.pos, 1.0);
    float3 n = i.nrm;
#if NUKE_INSTANCED
    float3 wp = float3(dot(i.iRow0, p), dot(i.iRow1, p), dot(i.iRow2, p));
    n = normalize(float3(dot(i.iRow0.xyz, n), dot(i.iRow1.xyz, n), dot(i.iRow2.xyz, n)));
    o.pos = mul(g_WVP, float4(wp, 1.0));
#else
    o.pos = mul(g_WVP, p);
    n = normalize(mul((float3x3)g_World, n));
#endif
    o.nrm = n;
    o.uv  = i.uv;
}
