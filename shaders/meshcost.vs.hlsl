// Mesh-cost debug view VS: position-only flat draw; NUKE_INSTANCED fades the wire alpha by
// projected triangle density (the plain path gets it precomputed in g_Color.a).
// g_Params: x = NDC z bias (wire), y = projection px scale, z = tris per instance, w = mesh radius.
cbuffer CostCB
{
    float4x4 g_WVP;      // plain: world*view*proj; instanced: view*proj
    float4   g_Color;    // rgb = cost color, a = wire alpha
    float4   g_Params;
};
struct VSIn
{
    float3 pos  : ATTRIB0;
#if NUKE_INSTANCED
    float4 row0 : ATTRIB1;
    float4 row1 : ATTRIB2;
    float4 row2 : ATTRIB3;
#endif
};
struct PSIn { float4 pos : SV_POSITION; float alpha : TEXCOORD0; };
void main(in VSIn i, out PSIn o)
{
#if NUKE_INSTANCED
    float4 lp = float4(i.pos, 1.0);
    float3 wp = float3(dot(i.row0, lp), dot(i.row1, lp), dot(i.row2, lp));
    o.pos = mul(g_WVP, float4(wp, 1.0));
    float s  = length(float3(i.row0.x, i.row1.x, i.row2.x));            // instance scale
    float px = g_Params.w * s / max(o.pos.w, 1e-3) * g_Params.y;        // projected radius, px
    float pxPerTri = 3.1416 * px * px / max(g_Params.z, 1.0);
    o.alpha = g_Color.a * saturate(pxPerTri * 0.1);
#else
    o.pos   = mul(g_WVP, float4(i.pos, 1.0));
    o.alpha = g_Color.a;
#endif
    o.pos.z -= g_Params.x * o.pos.w;
}
