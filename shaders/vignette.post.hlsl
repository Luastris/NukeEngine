// Stock post-process effect: vignette (darkens toward the corners). Custom post shader — sample g_Source,
// params from PostParams, output linear HDR.
Texture2D    g_Source;
SamplerState g_Source_sampler;
cbuffer PostParams
{
    float g_Strength = 0.4;   // 0 = off
    float g_Radius   = 0.7;   // where the darkening starts (0 centre .. 1 corner)
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(in PSIn i) : SV_Target
{
    float3 c = g_Source.Sample(g_Source_sampler, i.uv).rgb;
    float  r = length(i.uv - 0.5) * 1.41421356;
    float  v = 1.0 - smoothstep(g_Radius, 1.0, r) * g_Strength;
    return float4(c * v, 1.0);
}
