// Depth of field: a built-in multi-pass effect (CoC from depth -> half-res near/far bokeh gather ->
// composite) run by the renderer. This file only declares the params and registers the chain
// stage; main() below is an unused passthrough. Needs the depth prepass.
Texture2D    g_Source;
SamplerState g_Source_sampler;
cbuffer PostParams
{
    float g_FocusDistance = 8.0;   // metres from the camera that are sharp
    float g_FocusRange    = 4.0;   // metres around the focus distance that stay sharp
    float g_MaxCoC        = 12.0;  // largest blur circle, pixels (0 = off)
    float g_NearBlur      = 1.0;   // near field strength (0 = only the far field blurs)
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(in PSIn i) : SV_Target { return g_Source.Sample(g_Source_sampler, i.uv); }
