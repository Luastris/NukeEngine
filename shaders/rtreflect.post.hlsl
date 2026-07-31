// Registers "rtreflect" as a pickable post effect; the renderer recognises the name and runs the RT
// pipeline instead. Deliberately parameterless: the quality knobs are global, in Project Settings.
cbuffer PostParams { };
Texture2D    g_Source;
SamplerState g_Source_sampler;
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(in PSIn i) : SV_Target { return g_Source.Sample(g_Source_sampler, i.uv); }   // unused
