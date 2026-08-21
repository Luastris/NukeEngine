// Boot world shader (pixel): base color x albedo map with a fixed hemispheric light — enough
// to read the scene while the real pipelines compile. MatCB layout = the first 32 bytes of
// the world material CB (color, then params with x = has-albedo).
Texture2D    g_Tex;
SamplerState g_Tex_sampler;
cbuffer MatCB { float4 g_Color; float4 g_Params; };
struct PSIn { float4 pos : SV_POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
float4 main(in PSIn i) : SV_Target
{
    float4 c = g_Color;
    if (g_Params.x > 0.5) c *= g_Tex.Sample(g_Tex_sampler, i.uv);
    float3 n = normalize(i.nrm);
    float  l = 0.35 + 0.65 * saturate(dot(n, normalize(float3(0.4, 0.8, 0.3))));
    return float4(c.rgb * l, 1.0);
}
