// "rtreflect" — built-in ray-traced reflections. The actual effect is a real RT pipeline (rt_rgen/rt_rmiss +
// auto-generated per-shader closest-hits) inside the renderer; this file exists only to register "rtreflect" as a
// pickable post effect. The PS below is never compiled — the renderer recognises the effect by name and runs
// TraceRays. There are NO per-effect params: this effect is just the ON/OFF switch for a camera. The quality
// knobs (intensity / distance / bounces / roughness cutoff) are GLOBAL — Project Settings -> Ray Tracing (RTX).
cbuffer PostParams { };
Texture2D    g_Source;
SamplerState g_Source_sampler;
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(in PSIn i) : SV_Target { return g_Source.Sample(g_Source_sampler, i.uv); }   // unused (RT pipeline replaces it)
