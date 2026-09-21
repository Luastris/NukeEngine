// Screen-space sun shafts (crepuscular rays): the sky around the sun, masked by the scene's
// depth, blurred radially toward the sun's screen position - the rays between the occluders
// that clear air shows on a sunny day. Renderer-core, after the fog composite. Modes:
// 0 = mask (half res: the sun's disc + halo on sky pixels), 1 = radial blur (32 taps, the start
// dithered per pixel; run twice - the full reach, then one tap's worth - no ghost copies of the
// occluder edges), 3 = a 3x3 box over the blurred shafts (the per-pixel dither averaged out:
// without it the halo carries a fine cross-hatch), 2 = composite.
#include "atmosphere.hlsli"   // the physical atmosphere: the source is the sun THROUGH it (none below the horizon, none in space)
Texture2D g_Source; SamplerState g_Source_sampler;   // scene colour (composite)
Texture2D g_Depth;  SamplerState g_Depth_sampler;    // prepass device depth (point)
Texture2D g_Mask;   SamplerState g_Mask_sampler;     // mask / blurred shafts (linear)
Texture2D g_Clouds; SamplerState g_Clouds_sampler;   // resolved clouds (a = transmittance; white when off)
cbuffer SunShaftCB
{
    float4   g_SSSun;   // xy = sun uv, z = on-screen weight, w = intensity
    float4   g_SSPrm;   // x = blur reach (uv), y = tap decay, z = mode, w = scene is LDR (1)
    float4   g_SSCol;   // rgb = shaft radiance at the sun, w = white point (LDR path)
    float4   g_SSDir;   // xyz = direction TO the sun (world), w = the sky's sun disc radius (radians)
    float4x4 g_SSInvViewProj;
    float4   g_SSEcl;   // x = the eclipsing moon's offset from the sun (sun radii; >= 1000 = none)
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

// The source: the sky's sun disc (its authored size) with a short halo, on sky pixels only.
// Shared by the mask pass and the composite (which subtracts it so the rays never thicken the sun).
float3 Source(float2 uv)
{
    if (g_Depth.Sample(g_Depth_sampler, uv).r < 0.99999) return 0.0;   // geometry: an occluder
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 wf = mul(g_SSInvViewProj, float4(ndc, 1.0, 1.0));
    float3 dir = normalize(wf.xyz / wf.w);   // the direction matrix has no translation (precision far from the origin): no camera subtraction
    float  ang  = acos(clamp(dot(dir, g_SSDir.xyz), -1.0, 1.0));
    float  size = g_SSDir.w;
    float  disc = 1.0 - smoothstep(size * 0.9, size * 1.3, ang);
    float  halo = (1.0 - smoothstep(size * 1.3, size * 3.0, ang)) * 0.2;
    float  cloudT = g_Clouds.Sample(g_Clouds_sampler, uv).a;   // the clouds occlude the source
    float3 src = g_SSCol.rgb * (disc + halo) * cloudT;
    src *= 1.0 - EclipseCover(dir, g_SSDir.xyz, size, g_SSEcl.x);   // the moon over the source: the rays fade with the sun
    if (AtmoOn())
    {   // the sun seen through the air (red, none behind the planet), and rays need air around the camera
        float3 camKm = AtmoToKm(g_AtCam.xyz);
        float  air = exp(-max(length(camKm) - AtmoRg(), 0.0) / max(g_AtRayleigh.w, 0.01));
        src *= AtmoViewTransmittance(camKm, g_SSDir.xyz) * air;
    }
    return src;
}

float4 main(in PSIn i) : SV_Target
{
    const int mode = (int)g_SSPrm.z;
    if (mode == 0) return float4(Source(i.uv), 1.0);
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
    if (mode == 3)
    {
        uint mw, mh; g_Mask.GetDimensions(mw, mh);
        float2 px = 1.0 / float2(max(mw, 1u), max(mh, 1u));
        float3 acc = 0.0;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            acc += g_Mask.SampleLevel(g_Mask_sampler, i.uv + float2(x, y) * px, 0).rgb;
        return float4(acc / 9.0, 1.0);
    }
    float3 src = g_Source.Sample(g_Source_sampler, i.uv).rgb;
    // only the smear: the source itself is already in the sky (subtracted, so the sun keeps its size)
    float3 sh  = max(g_Mask.Sample(g_Mask_sampler, i.uv).rgb - Source(i.uv), 0.0) * (g_SSSun.w * g_SSSun.z);
    float3 outC = (g_SSPrm.w > 0.5) ? ToLDR(FromLDR(src) + sh) : src + sh;
    return float4(outC, 1.0);
}
