// "rtreflect" — built-in ray-traced reflections. The actual effect is a real RT pipeline (rt_rgen/rt_rmiss +
// auto-generated per-shader closest-hits) inside the renderer; this file exists only to (1) register "rtreflect"
// as a pickable post effect and (2) declare its tweakables via PostParams. The PS below is never compiled —
// the renderer recognises the effect by name and runs TraceRays instead. Params flow through PostParams order:
//   params[0]=Intensity, [1]=MaxDist, [2]=Bounces, [3]=RoughCutoff   (offset/4 = index)
cbuffer PostParams
{
    float g_Intensity   = 1.0;    // reflection strength (0 = off)
    float g_MaxDist     = 100.0;  // max ray distance (world units)
    float g_Bounces     = 3.0;    // recursion depth: mirror-in-mirror (1 = single reflection)
    float g_RoughCutoff = 0.6;    // reflections fade out as roughness approaches this (sharp RT = smooth surfaces only)
};
Texture2D    g_Source;
SamplerState g_Source_sampler;
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(in PSIn i) : SV_Target { return g_Source.Sample(g_Source_sampler, i.uv); }   // unused (RT pipeline replaces it)
