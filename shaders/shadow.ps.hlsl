// Shadow depth pass — pixel shader. Depth-only; for transparent casters it alpha-dithers (stochastic)
// so semi-transparent surfaces cast proportionally lighter shadows ("multiply by alpha").
cbuffer ShadowPSCB { float4 g_Alpha; };   // g_Alpha.x = material alpha, y = hasBaseTex
Texture2D    g_Tex;
SamplerState g_Tex_sampler;
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
void main(in PSIn i)
{
    float a = g_Alpha.x;
    if (g_Alpha.y > 0.5) a *= g_Tex.Sample(g_Tex_sampler, i.uv).a;
    if (a < 0.999)
    {
        // Interleaved-gradient dither: keep ~a fraction of fragments -> shadow coverage ~ alpha.
        float d = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))));
        if (a < d) discard;
    }
}
