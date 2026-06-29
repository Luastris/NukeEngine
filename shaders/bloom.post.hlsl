// Bloom — a BUILT-IN multi-pass post effect (bright-pass -> separable blur -> composite). This file only
// declares the params (shown in the inspector) and marks the chain stage as "bloom"; the renderer runs the
// actual passes (its main() here is just a passthrough and is never used for the effect).
Texture2D    g_Source;
SamplerState g_Source_sampler;
cbuffer PostParams
{
    float g_Threshold = 1.0;   // luminance above which pixels bloom
    float g_Intensity = 0.6;   // how strongly the bloom is added back
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(in PSIn i) : SV_Target { return g_Source.Sample(g_Source_sampler, i.uv); }
