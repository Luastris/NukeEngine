// Auto-exposure: a built-in multi-pass effect (log-luminance histogram -> eye adaptation -> apply)
// run by the renderer. This file only declares the params and registers the chain stage; main()
// below is an unused passthrough. The stage scales the chain colour, so in an HDR project it sits
// before the tonemap; in an LDR project it adapts the finished picture's brightness.
Texture2D    g_Source;
SamplerState g_Source_sampler;
cbuffer PostParams
{
    float g_MinEV        = -4.0;   // darkest scene the eye adapts to (EV: log2 of the average luminance)
    float g_MaxEV        =  10.0;  // brightest scene the eye adapts to
    float g_SpeedUp      =  3.0;   // adaptation speed toward a brighter scene, 1/s
    float g_SpeedDown    =  1.0;   // adaptation speed toward a darker scene, 1/s
    float g_Compensation =  0.0;   // EV offset on the adapted result (+1 = twice as bright)
    float g_Manual       =  0.0;   // manual override, EV (0 = x1) - used when Manual On is set
    float g_ManualOn     =  0.0;   // 1 = fixed exposure (cinematics), 0 = adapt
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(in PSIn i) : SV_Target { return g_Source.Sample(g_Source_sampler, i.uv); }
