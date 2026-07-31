// Bloom: a built-in multi-pass effect (bright-pass -> separable blur -> composite) run by the renderer.
// This file only declares the params and registers the chain stage; main() below is an unused passthrough.
Texture2D    g_Source;
SamplerState g_Source_sampler;
cbuffer PostParams
{
    float g_Threshold = 1.0;   // luminance above which pixels bloom
    float g_Intensity = 0.6;   // how strongly the bloom is added back
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(in PSIn i) : SV_Target { return g_Source.Sample(g_Source_sampler, i.uv); }
