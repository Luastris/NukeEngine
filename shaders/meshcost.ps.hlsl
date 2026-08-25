// Mesh-cost debug view PS: the flat cost color (alpha from the VS density fade; the solid
// pass has blending off, so its alpha is ignored).
cbuffer CostCB
{
    float4x4 g_WVP;
    float4   g_Color;
    float4   g_Params;
};
struct PSIn { float4 pos : SV_POSITION; float alpha : TEXCOORD0; };
float4 main(in PSIn i) : SV_TARGET
{
    return float4(g_Color.rgb, i.alpha);
}
