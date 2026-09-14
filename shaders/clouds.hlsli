// Volumetric clouds, shared: the constants, the cloud layer's spherical geometry, the density
// field (weather map x base shape x detail erosion, Nubis / Horizon-class) and the light model
// (Beer-Lambert with multiple-scattering octaves, dual-lobe Henyey-Greenstein, powder).
// Included by clouds.cs (march), clouds_shadow.cs (sun transmittance map), clouds_gen.cs
// (noise / weather generation) and clouds_apply.ps (composite).
#ifndef CLOUDS_HLSLI
#define CLOUDS_HLSLI

cbuffer CloudCB
{
    float4x4 g_ClInvVP;    // clip -> world (unjittered)
    float4x4 g_ClPrevVP;   // world -> last frame's clip (temporal reprojection)
    float4   g_ClCam;      // camera position xyz, w = frame index
    float4   g_ClSunDir;   // xyz = toward the sun, w = sun intensity
    float4   g_ClSunCol;   // rgb = sun colour, w = ambient intensity
    float4   g_ClSkyTop;   // rgb = sky zenith colour, w = sky intensity
    float4   g_ClSkyHor;   // rgb = sky horizon colour, w = planet radius (m)
    float4   g_ClLayer;    // bottom altitude, top altitude (m above sea level), max distance (m), primary steps
    float4   g_ClShape;    // shape scale (m), detail scale (m), weather scale (m), erosion 0..1
    float4   g_ClCover;    // coverage 0..1, cloud type 0..1, density (extinction scale), silver lining 0..1
    float4   g_ClPhase;    // forward g, back g, multi-scatter attenuation a, multi-scatter contribution b
    float4   g_ClWind;     // wind offset x, z (m), time (s), generation mode / temporal blend
    float4   g_ClScreen;   // march w, h, full w, full h
    float4   g_ClMisc;     // camera near, far, scene is LDR (1) / linear (0), white point
    float4   g_ClShadow;   // shadow map origin x, z, 1 / size, strength
    float4   g_ClMoon;     // xyz = toward the moon, w = moon light intensity (0 = none)
    float4   g_ClMoonCol;  // rgb = moon light colour
};

#include "atmosphere.hlsli"
static const float CL_PI = 3.14159265;
static const float CL_SIGMA = 0.05;   // extinction per metre of a full-density cloud (1/m), scaled by g_ClCover.z

// ---- geometry: the cloud layer is a shell around the planet ----------------------------
float3 CloudPlanetCentre() { return float3(g_ClCam.x, -g_ClSkyHor.w, g_ClCam.z); }   // sea level = y 0; the planet sits under the camera (as the atmosphere)
float  CloudRadiusBottom() { return g_ClSkyHor.w + g_ClLayer.x; }
float  CloudRadiusTop()    { return g_ClSkyHor.w + g_ClLayer.y; }

// Ray / sphere: the two hit distances (t0 <= t1), or false.
bool RaySphere(float3 o, float3 d, float3 c, float r, out float t0, out float t1)
{
    float3 oc = o - c;
    float  b  = dot(oc, d);
    float  cc = dot(oc, oc) - r * r;
    float  h  = b * b - cc;
    if (h < 0.0) { t0 = t1 = 0.0; return false; }
    h = sqrt(h);
    t0 = -b - h; t1 = -b + h;
    return true;
}
// The stretch of the ray inside the layer: [tA, tB], false if none (camera below, inside or
// above the layer; a ray from above that misses the bottom shell crosses the top twice).
bool CloudSegment(float3 o, float3 d, float maxDist, out float tA, out float tB)
{
    float3 c = CloudPlanetCentre();
    float  rb = CloudRadiusBottom(), rt = CloudRadiusTop();
    float  r  = length(o - c);
    float  b0, b1, u0, u1;
    bool hitB = RaySphere(o, d, c, rb, b0, b1);
    bool hitT = RaySphere(o, d, c, rt, u0, u1);
    tA = 0.0; tB = 0.0;
    if (r < rb)
    {   // below the layer: from the bottom shell to the top shell
        if (!hitT) return false;
        tA = max(b1, 0.0); tB = u1;
    }
    else if (r < rt)
    {   // inside the layer: from here to whichever shell comes first (the bottom only if the ray dips into it)
        tA = 0.0;
        tB = u1;
        if (hitB && b0 > 0.0) tB = min(tB, b0);
    }
    else
    {   // above the layer
        if (!hitT || u1 <= 0.0) return false;
        tA = max(u0, 0.0);
        tB = (hitB && b0 > 0.0) ? min(b0, u1) : u1;
    }
    // the planet itself ends the ray: a ray into the ground never reaches the far side's clouds
    float g0, g1;
    if (RaySphere(o, d, c, g_ClSkyHor.w, g0, g1) && g1 > 0.0)
    {
        float tG = (g0 > 0.0) ? g0 : g1;
        if (r >= g_ClSkyHor.w && g0 > 0.0 && tG <= tA) return false;   // outside the planet, the ground comes first
        if (r < g_ClSkyHor.w) return false;                            // below the surface (a mine, a basement): no sky at all
        tB = min(tB, tG);
    }
    tB = min(tB, maxDist);
    return tB > tA;
}

// ---- the density field ---------------------------------------------------------------
Texture3D<float4> g_CloudBase;    SamplerState g_CloudBase_sampler;      // 128^3: R = Perlin-Worley, GBA = Worley at 2/4/8 x
Texture3D<float4> g_CloudDetail;  SamplerState g_CloudDetail_sampler;    // 32^3: Worley at 1/2/4 x
Texture2D<float4> g_CloudWeather; SamplerState g_CloudWeather_sampler;   // 512^2: R = coverage field, G = type field, B = precipitation (reserved)

float Remap(float v, float a, float b, float c, float d) { return c + (v - a) / max(b - a, 1e-5) * (d - c); }

// How a cloud of a given type fills the layer's height: stratus hug the bottom, cumulus tower.
float CloudHeightGradient(float h, float type)
{
    const float4 stratus       = float4(0.00, 0.06, 0.09, 0.26);
    const float4 stratocumulus = float4(0.00, 0.10, 0.35, 0.62);
    const float4 cumulus       = float4(0.00, 0.06, 0.55, 0.98);
    float4 g = (type < 0.5) ? lerp(stratus, stratocumulus, type * 2.0) : lerp(stratocumulus, cumulus, type * 2.0 - 1.0);
    return smoothstep(g.x, g.y, h) * (1.0 - smoothstep(g.z, g.w, h));
}

// Density 0..1 of the medium at a world point; `detail` adds the erosion (the expensive part);
// hf = height fraction inside the layer (0 bottom, 1 top).
float CloudDensity(float3 p, bool detail, out float hf)
{
    float3 c  = CloudPlanetCentre();
    float  r  = length(p - c);
    float  rb = CloudRadiusBottom(), rt = CloudRadiusTop();
    hf = saturate((r - rb) / max(rt - rb, 1.0));
    if (r < rb || r > rt) return 0.0;
    float3 off = float3(g_ClWind.x, 0.0, g_ClWind.y);
    // weather: coverage and type over the map
    float2 wuv = (p.xz + off.xz) / g_ClShape.z;
    float4 wth = g_CloudWeather.SampleLevel(g_CloudWeather_sampler, wuv, 0);
    {   // a second, 5.8x larger, rotated octave of the same map: the repetition of one period shows from orbit
        float2 wuv2 = float2(wuv.x * 0.62 - wuv.y * 0.78, wuv.x * 0.78 + wuv.y * 0.62) * 0.173 + 0.37;
        wth = lerp(wth, g_CloudWeather.SampleLevel(g_CloudWeather_sampler, wuv2, 0), 0.45);
    }
    // the coverage setting thresholds the weather field; full cover 0.35 above the threshold (soft edges, solid cores)
    float  cov = saturate(Remap(wth.r, 1.0 - g_ClCover.x, 1.35 - g_ClCover.x, 0.0, 1.0)) * saturate(g_ClCover.x * 4.0);
    if (cov <= 0.001) return 0.0;
    float  type = saturate(g_ClCover.y + (wth.g - 0.5) * 0.6);
    // base shape: Perlin-Worley carved by the Worley octaves, then the height profile
    float3 sp = (p + off * 1.0 + float3(0.0, 0.0, 0.0)) / g_ClShape.x;
    sp.y += g_ClWind.z * 0.002;   // a slow lift: the shapes evolve
    float4 bs = g_CloudBase.SampleLevel(g_CloudBase_sampler, sp, 0);
    float  fbm = bs.g * 0.625 + bs.b * 0.25 + bs.a * 0.125;
    float  shape = saturate(Remap(bs.r, fbm - 1.0, 1.0, 0.0, 1.0));
    shape *= CloudHeightGradient(hf, type);
    // coverage: carve the shape by the local coverage and thin it toward the coverage edge
    shape = saturate(Remap(shape, 1.0 - cov, 1.0, 0.0, 1.0)) * cov;
    if (shape <= 0.0) return 0.0;
    if (detail)
    {   // erosion: wispy Worley at the bottom, billowy (inverted) higher up, eating the edges only
        float3 dp = (p + off * 1.3) / g_ClShape.y;
        dp.y += g_ClWind.z * 0.01;
        float4 dn = g_CloudDetail.SampleLevel(g_CloudDetail_sampler, dp, 0);
        float  dfbm = dn.r * 0.625 + dn.g * 0.25 + dn.b * 0.125;
        float  dmod = lerp(dfbm, 1.0 - dfbm, saturate(hf * 10.0));
        shape = saturate(Remap(shape, dmod * g_ClShape.w * (1.0 - shape) , 1.0, 0.0, 1.0));
    }
    return shape;
}

// ---- light -----------------------------------------------------------------------------
float PhaseHG(float mu, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * CL_PI * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));
}
// Dual-lobe phase: the forward lobe (the sun through the cloud), a back lobe, and a silver-lining
// weight that favours the forward scatter near the sun.
float CloudPhase(float mu, float gScale)
{
    float pf = PhaseHG(mu, g_ClPhase.x * gScale);
    float pb = PhaseHG(mu, g_ClPhase.y * gScale);
    return lerp(pb, pf, 0.5 + 0.5 * g_ClCover.w);
}
// Sun light reaching a point: optical depth along the sun direction (6 samples, growing steps
// through the layer), then Beer-Lambert with the multiple-scattering octaves (Wrenninge):
// sum_k a^k exp(-b^k tau) phase(g c^k), the first octave with the powder term of Nubis.
float CloudSunOpticalDepth(float3 p, float3 L, float layerThick)
{
    float tau = 0.0;
    float t = 0.0;
    float step = layerThick * 0.04;
    [unroll] for (int i = 0; i < 6; ++i)
    {
        t += step;
        float hf;
        float d = CloudDensity(p + L * t, i < 3, hf);   // the first three with detail, the rest coarse
        tau += d * step;
        step *= 1.6;
    }
    // a last, far sample for the deep shadow of a thick cloud
    { float hf; tau += CloudDensity(p + L * (t + layerThick * 0.5), false, hf) * layerThick * 0.4; }
    return tau * CL_SIGMA * g_ClCover.z;
}
// The scattered fraction of a directional light at a point: Beer-Lambert with the multi-scatter
// octaves and the powder term, times the dual-lobe phase. Callers scale by the light's radiance.
float CloudLightScatter(float tau, float mu)
{
    float a = g_ClPhase.z, b = g_ClPhase.w;
    float e = 0.0;
    float ak = 1.0, bk = 1.0, ck = 1.0;
    [unroll] for (int k = 0; k < 3; ++k)
    {
        float beer = exp(-bk * tau);
        float powder = (k == 0) ? (1.0 - 0.5 * exp(-2.0 * tau)) : 1.0;   // the darker edges of a lit cloud
        e += ak * beer * powder * CloudPhase(mu, ck);
        ak *= a; bk *= b; ck *= 0.5;
    }
    return e;
}
float3 CloudSunLight(float tau, float mu, float3 sunT)   // sunT = the atmosphere's transmittance to the point (1 = procedural sky)
{
    return g_ClSunCol.rgb * g_ClSunDir.w * CloudLightScatter(tau, mu) * sunT;
}
// Ambient: the sky's light, more of the zenith on top, more of the horizon (and less) at the bottom.
float3 CloudAmbient(float hf)
{
    float3 top = g_ClSkyTop.rgb * g_ClSkyTop.w, hor = g_ClSkyHor.rgb * g_ClSkyTop.w;
    return lerp(hor * 0.35, top, hf) * g_ClSunCol.w / CL_PI;
}

#endif
