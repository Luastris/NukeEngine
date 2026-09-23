// Auto-exposure pass 1: the chain colour reduced to a 64x64 grid of log2 luminance (each texel
// averages a 2x2 box of the source at its centre; the histogram pass reads the grid).
Texture2D g_Source; SamplerState g_Source_sampler;
cbuffer ExpCB
{
    float4 g_Exp0;   // min EV, max EV, speed up, speed down
    float4 g_Exp1;   // compensation EV, manual EV, manual on, dt (s)
    float4 g_Exp2;   // 1/full w, 1/full h, LDR source (1 = decode sRGB), 0
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float main(in PSIn i) : SV_Target
{
    float2 o = g_Exp2.xy * 0.5;
    float3 c = g_Source.Sample(g_Source_sampler, i.uv + float2(-o.x, -o.y)).rgb
             + g_Source.Sample(g_Source_sampler, i.uv + float2( o.x, -o.y)).rgb
             + g_Source.Sample(g_Source_sampler, i.uv + float2(-o.x,  o.y)).rgb
             + g_Source.Sample(g_Source_sampler, i.uv + float2( o.x,  o.y)).rgb;
    c *= 0.25;
    if (g_Exp2.z > 0.5) c = pow(max(c, 0.0), 2.2);   // an LDR chain is sRGB-encoded: measure linear light
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    return log2(max(lum, 1e-5));
}
