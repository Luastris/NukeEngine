// Bloom separable Gaussian blur (one axis per draw; g_BloomDir.xy = texel step along the axis). Internal.
Texture2D    g_Source;
SamplerState g_Source_sampler;
cbuffer BloomCB { float4 g_BloomParams; float4 g_BloomDir; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
static const float W[5] = { 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 };
float4 main(in PSIn i) : SV_Target
{
    float2 d = g_BloomDir.xy;
    float3 c = g_Source.Sample(g_Source_sampler, i.uv).rgb * W[0];
    [unroll] for (int k = 1; k < 5; ++k)
    {
        c += g_Source.Sample(g_Source_sampler, i.uv + d * (float)k).rgb * W[k];
        c += g_Source.Sample(g_Source_sampler, i.uv - d * (float)k).rgb * W[k];
    }
    return float4(c, 1.0);
}
