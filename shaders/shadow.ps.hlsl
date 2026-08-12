// Shadow depth pass pixel shader: depth-only, with stochastic alpha dithering so semi-transparent
// casters produce proportionally lighter shadows. LiveMaterial: the same UV transform, cutout
// clip and luma-wipe clip as the color pass, so their holes punch shadows too.
// g_Alpha = (material alpha, hasBaseTex, alphaCutoff 0=off, wipeThreshold 0=off);
// g_SUVT = (uvTiling.xy; 0,0 = identity, uvOffset.xy + tween scroll).
cbuffer ShadowPSCB { float4 g_Alpha; float4 g_SUVT; };
Texture2D    g_Tex;
SamplerState g_Tex_sampler;
Texture2D    g_WipeMask;
SamplerState g_WipeMask_sampler;
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
void main(in PSIn i)
{
    float2 tl = (abs(g_SUVT.x) + abs(g_SUVT.y) < 1e-6) ? float2(1.0, 1.0) : g_SUVT.xy;
    float2 uv = i.uv * tl + g_SUVT.zw;
    if (g_Alpha.w > 0.0)
        clip(g_WipeMask.Sample(g_WipeMask_sampler, uv).r - g_Alpha.w);   // luma wipe
    float a = g_Alpha.x;
    if (g_Alpha.y > 0.5) a *= g_Tex.Sample(g_Tex_sampler, uv).a;
    if (g_Alpha.z > 0.0) clip(a - g_Alpha.z);                            // cutout blend
    if (a < 0.999)
    {
        // Interleaved-gradient dither: keep ~a fraction of fragments -> shadow coverage ~ alpha.
        float d = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))));
        if (a < d) discard;
    }
}
