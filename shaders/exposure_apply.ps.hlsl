// Auto-exposure pass 3 (full res): scale the chain colour by the adapted exposure. The key value
// (0.18, middle grey) lands on the adapted average luminance; the compensation shifts it in EV.
// Manual On replaces the adapted EV by the manual one. An LDR chain (sRGB) is scaled in linear.
Texture2D        g_Source; SamplerState g_Source_sampler;
Texture2D<float> g_Adapted;   // 1x1 adapted EV (log2 of the scene's average luminance)
cbuffer ExpCB
{
    float4 g_Exp0;   // min EV, max EV, speed up, speed down
    float4 g_Exp1;   // compensation EV, manual EV, manual on, dt
    float4 g_Exp2;   // 1/w, 1/h, LDR source, prev valid
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(in PSIn i) : SV_Target
{
    float4 c = g_Source.Sample(g_Source_sampler, i.uv);
    float ev = g_Adapted.Load(int3(0, 0, 0));
    float mul = (g_Exp1.z > 0.5) ? exp2(g_Exp1.y) : (0.18 / max(exp2(ev), 1e-5)) * exp2(g_Exp1.x);
    if (g_Exp2.z > 0.5)
    {   // LDR: decode, scale, re-encode - the picture's brightness adapts, its curve stays
        float3 lin = pow(max(c.rgb, 0.0), 2.2) * mul;
        return float4(pow(saturate(lin), 1.0 / 2.2), c.a);
    }
    return float4(c.rgb * mul, c.a);
}
