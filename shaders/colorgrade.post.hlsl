// Stock post-process effect: exposure + colour grade. A custom post shader — sample g_Source, read params
// from PostParams (auto-shown in the inspector), output linear HDR. Drop it from the chain or replace it.
Texture2D    g_Source;
SamplerState g_Source_sampler;
cbuffer PostParams
{
    float g_Exposure    = 1.0;   // linear multiplier
    float g_Contrast    = 1.0;
    float g_Saturation  = 1.0;
    float g_Temperature = 0.0;   // warm (+) / cool (-)
    float g_Tint        = 0.0;   // magenta (+) / green (-)
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(in PSIn i) : SV_Target
{
    float3 c = g_Source.Sample(g_Source_sampler, i.uv).rgb;
    c *= max(g_Exposure, 0.0);
    c *= float3(1.0 + g_Temperature * 0.2, 1.0 + g_Tint * 0.1, 1.0 - g_Temperature * 0.2);
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(float3(luma, luma, luma), c, g_Saturation);
    c = (c - 0.5) * g_Contrast + 0.5;
    return float4(max(c, 0.0), 1.0);
}
