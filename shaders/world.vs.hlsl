// World (3D) pass — vertex shader. Position + normal + uv; outputs world-space pos & normal for PBR.
//
// NUKE_INSTANCED (7.1): the instanced variant reads the per-instance world transform as three
// HLSL-READY rows (row_i · (pos,1) = world-space coord; translation sits in row_i.w) plus a tint
// and a free custom float4 from PER-INSTANCE vertex attributes (ATTRIB3..7). The CB then carries
// VIEW*PROJ in g_WVP and identity in g_World. Normals use the row 3x3 directly — exact for
// rotation + uniform scale (the standard instancing tradeoff; non-uniform scale skews normals).
// A CUSTOM shader opts into instancing by handling this same define; sources that ignore it
// simply never get an instanced pipeline (the renderer falls back to the default world shader).
cbuffer CB { float4x4 g_WVP; float4x4 g_World; };
#if NUKE_INSTANCED
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2;
              float4 iRow0 : ATTRIB3; float4 iRow1 : ATTRIB4; float4 iRow2 : ATTRIB5;
              float4 iColor : ATTRIB6; float4 iCustom : ATTRIB7; };
struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2;
              float4 icol : TEXCOORD3; float4 icustom : TEXCOORD4; };
void main(in VSIn i, out PSIn o)
{
    float4 p4 = float4(i.pos, 1.0);
    o.wpos    = float3(dot(i.iRow0, p4), dot(i.iRow1, p4), dot(i.iRow2, p4));
    o.pos     = mul(g_WVP, float4(o.wpos, 1.0));
    o.nrm     = float3(dot(i.iRow0.xyz, i.nrm), dot(i.iRow1.xyz, i.nrm), dot(i.iRow2.xyz, i.nrm));
    o.uv      = i.uv;
    o.icol    = i.iColor;
    o.icustom = i.iCustom;
}
#else
struct VSIn { float3 pos : ATTRIB0; float3 nrm : ATTRIB1; float2 uv : ATTRIB2; };
struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2; };
void main(in VSIn i, out PSIn o)
{
    o.wpos = mul(g_World, float4(i.pos, 1.0)).xyz;
    o.pos  = mul(g_WVP,   float4(i.pos, 1.0));
    o.nrm  = mul((float3x3)g_World, i.nrm);
    o.uv   = i.uv;
}
#endif
