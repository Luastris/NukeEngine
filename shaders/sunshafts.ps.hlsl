// Screen-space sun shafts (crepuscular rays): the sky around the sun, masked by the scene's
// depth, blurred radially toward the sun's screen position - the rays between the occluders
// that clear air shows on a sunny day. Renderer-core, after the fog composite. Modes:
// 0 = mask (half res: the sun's disc + halo on sky pixels), 1 = radial blur (32 taps, the start
// dithered per pixel; run twice - the full reach, then one tap's worth - no ghost copies of the
// occluder edges), 2 = composite.
Texture2D g_Source; SamplerState g_Source_sampler;   // scene colour (composite)
Texture2D g_Depth;  SamplerState g_Depth_sampler;    // prepass device depth (point)
Texture2D g_Mask;   SamplerState g_Mask_sampler;     // mask / blurred shafts (linear)
cbuffer SunShaftCB
{
    float4   g_SSSun;   // xy = sun uv, z = on-screen weight, w = intensity
    float4   g_SSPrm;   // x = blur reach (uv), y = tap decay, z = mode, w = scene is LDR (1)
    float4   g_SSCol;   // rgb = shaft radiance at the sun, w = white point (LDR path)
    float4   g_SSDir;   // xyz = direction TO the sun (world)
    float4x4 g_SSInvViewProj;
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float3 FromLDR(float3 c)   // world.ps tonemap (extended Reinhard + sRGB) undone
{
    c = pow(max(c, 0.0), 2.2);
    float W = max(g_SSCol.w, 1e-3);
    float3 y = min(c, 0.999);
    return max(0.5 * W * W * ((y - 1.0) + sqrt((1.0 - y) * (1.0 - y) + 4.0 * y / (W * W))), 0.0);
}
float3 ToLDR(float3 c)
{
    float W = max(g_SSCol.w, 1e-3);
    c = c * (1.0 + c / (W * W)) / (1.0 + c);
    return pow(max(c, 0.0), 1.0 / 2.2);
}

float4 main(in PSIn i) : SV_Target
{
    const int mode = (int)g_SSPrm.z;
    if (mode == 0)
    {
        if (g_Depth.Sample(g_Depth_sampler, i.uv).r < 0.99999) return float4(0.0, 0.0, 0.0, 1.0);   // geometry: an occluder
        float2 ndc = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
        float4 wf = mul(g_SSInvViewProj, float4(ndc, 1.0, 1.0));
        float4 wn = mul(g_SSInvViewProj, float4(ndc, 0.0, 1.0));
        float3 dir = normalize(wf.xyz / wf.w - wn.xyz / wn.w);
        float  c = dot(dir, g_SSDir.xyz);
        // The source is the sun itself: a ~3 degree disc with a halo to ~8 degrees. A wide source
        // made a bright cloud instead of rays.
        float  disc = smoothstep(0.9975, 0.9990, c);                 // cos 4 .. cos 2.5 degrees
        float  halo = smoothstep(0.990, 0.9986, c) * 0.35;           // cos 8 .. cos 3 degrees
        return float4(g_SSCol.rgb * (disc + halo), 1.0);
    }
    if (mode == 1)
    {
        float2 step = (g_SSSun.xy - i.uv) * g_SSPrm.x / 32.0;
        float  jit  = frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))));   // interleaved gradient noise
        float3 sum = 0.0; float w = 1.0, wsum = 0.0;
        float2 uv = i.uv + step * jit;
        [unroll] for (int k = 0; k < 32; ++k)
        {
            sum += g_Mask.SampleLevel(g_Mask_sampler, uv, 0).rgb * w; wsum += w;
            uv += step; w *= g_SSPrm.y;
        }
        return float4(sum / wsum, 1.0);
    }
    float3 src = g_Source.Sample(g_Source_sampler, i.uv).rgb;
    float3 sh  = g_Mask.Sample(g_Mask_sampler, i.uv).rgb * (g_SSSun.w * g_SSSun.z);
    float3 outC = (g_SSPrm.w > 0.5) ? ToLDR(FromLDR(src) + sh) : src + sh;
    return float4(outC, 1.0);
}
