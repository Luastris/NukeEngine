// Bloom composite: scene + blurred bloom * intensity. Internal (g_Source = scene, g_Bloom = blurred).
Texture2D    g_Source;
SamplerState g_Source_sampler;
Texture2D    g_Bloom;
SamplerState g_Bloom_sampler;
cbuffer BloomCB { float4 g_BloomParams; float4 g_BloomDir; };   // y = intensity
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(in PSIn i) : SV_Target
{
    float3 s = g_Source.Sample(g_Source_sampler, i.uv).rgb;
    float3 b = g_Bloom.Sample(g_Bloom_sampler, i.uv).rgb;
    return float4(s + b * g_BloomParams.y, 1.0);
}
