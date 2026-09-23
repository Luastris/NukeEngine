// Motion blur: a built-in multi-pass effect (velocity tile max -> neighbour max -> reconstruction
// filter) run by the renderer. This file only declares the params and registers the chain stage;
// main() below is an unused passthrough. Needs the depth prepass (its velocity target).
Texture2D    g_Source;
SamplerState g_Source_sampler;
cbuffer PostParams
{
    float g_Shutter = 0.5;    // exposure time as a fraction of the frame (0 = off, 1 = a full frame's motion)
    float g_MaxBlur = 32.0;   // longest blur, pixels
    float g_Samples = 12.0;   // taps along the blur (4..32)
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(in PSIn i) : SV_Target { return g_Source.Sample(g_Source_sampler, i.uv); }
