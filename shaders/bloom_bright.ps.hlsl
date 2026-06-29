// Bloom bright-pass: keep only the part of each pixel above the threshold (soft knee). Internal — paired
// with post.vs by the renderer.
Texture2D    g_Source;
SamplerState g_Source_sampler;
cbuffer BloomCB { float4 g_BloomParams; float4 g_BloomDir; };   // x=threshold, y=intensity
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(in PSIn i) : SV_Target
{
    float3 c = g_Source.Sample(g_Source_sampler, i.uv).rgb;
    float  b = max(max(c.r, c.g), c.b);
    float  k = max(b - g_BloomParams.x, 0.0) / max(b, 1e-4);   // soft threshold
    return float4(c * k, 1.0);
}
