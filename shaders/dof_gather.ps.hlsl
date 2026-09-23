// DOF pass 2 (half res): a bokeh disc gather over the colour+CoC texture. g_Dof2.z selects the
// field: 0 = far (a sample counts when its own CoC is at least as large as the tap radius, so a
// sharp foreground never bleeds into the far blur), 1 = near (a sample counts by its OWN near CoC,
// so near objects bleed over what is behind them; alpha carries the near coverage for the composite).
Texture2D g_Source; SamplerState g_Source_sampler;   // half-res colour + signed CoC (dof_coc)
cbuffer DofCB
{
    float4 g_Dof0;   // near, far, focus distance, focus range
    float4 g_Dof1;   // max CoC (px), near blur strength, full w, full h
    float4 g_Dof2;   // 1/half w, 1/half h, field (0 far / 1 near), 0
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

static const int kTaps = 24;

float4 main(in PSIn i) : SV_Target
{
    float4 c0 = g_Source.Sample(g_Source_sampler, i.uv);
    const bool nearField = g_Dof2.z > 0.5;
    float coc0 = nearField ? max(-c0.a, 0.0) : max(c0.a, 0.0);
    // The near field gathers over the largest near CoC around (a dilation): the blur of a near
    // object must reach past its own silhouette.
    float radius = coc0;
    if (nearField)
    {
        // Two rings (a quarter and a half of the max CoC, in half-res texels): a near object's
        // blur must reach as far as its own circle, and the mask must not break into blocks.
        [unroll] for (int k = 0; k < 16; ++k)
        {
            float a = (float)(k & 7) * 0.785398 + ((k >= 8) ? 0.392699 : 0.0);
            float rr = (k >= 8) ? 0.5 : 0.25;
            float2 o = float2(cos(a), sin(a)) * g_Dof1.x * rr * g_Dof2.xy;
            float sc = max(-g_Source.Sample(g_Source_sampler, i.uv + o).a, 0.0);
            // A neighbour's circle reaches this texel only if it is larger than the distance to it.
            if (sc * 0.5 >= g_Dof1.x * rr) radius = max(radius, sc);
        }
    }
    float rHalf = radius * 0.5;   // full-res px -> half-res px
    if (rHalf < 0.5) return float4(c0.rgb, nearField ? 0.0 : coc0);

    float3 sum = c0.rgb; float wsum = 1.0, cover = nearField ? saturate(coc0 / max(g_Dof1.x * 0.5, 1e-3)) : 0.0;
    // Golden-angle spiral: even disc coverage with few taps (a bokeh circle, not a Gaussian).
    [loop] for (int t = 1; t <= kTaps; ++t)
    {
        float r = sqrt((float)t / (float)kTaps) * rHalf;
        float a = (float)t * 2.399963;
        float2 o = float2(cos(a), sin(a)) * r * g_Dof2.xy;
        float4 s = g_Source.Sample(g_Source_sampler, i.uv + o);
        float sc = nearField ? max(-s.a, 0.0) : max(s.a, 0.0);
        // A tap contributes when its own circle reaches back to this pixel.
        float w = saturate((sc * 0.5 - r) / max(rHalf * 0.25, 0.5) + 1.0);
        sum += s.rgb * w; wsum += w;
        if (nearField) cover += saturate(sc / max(g_Dof1.x, 1e-3)) * w;
    }
    float3 col = sum / max(wsum, 1e-4);
    return float4(col, nearField ? saturate(cover / max(wsum, 1e-4)) : coc0);   // the near halo fades as fewer circles reach
}
