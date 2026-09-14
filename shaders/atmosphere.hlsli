// Physical atmosphere, shared (Hillaire 2020 "A Scalable and Production Ready Sky and
// Atmosphere Rendering Technique" on the Bruneton medium): a planet of radius Rg with an
// atmosphere to Rt, Rayleigh + Mie scattering, Mie + ozone absorption, exponential / tent
// density profiles. Four LUTs: transmittance (r, mu) to the top of the atmosphere, multiple
// scattering Psi(r, mu_sun), the camera's sky-view (in-scatter + transmittance per direction) and
// the aerial-perspective froxel volume. Units: kilometres, planet-centred; world metres convert
// through AtmoToKm. Included by atmo_lut.cs, atmo_apply.ps, sky.ps and clouds.hlsli.
#ifndef ATMOSPHERE_HLSLI
#define ATMOSPHERE_HLSLI
#include "atmo_map.hlsli"

cbuffer AtmoCB
{
    float4   g_AtRadii;     // x = planet radius Rg (km), y = atmosphere top Rt (km), z = density scale, w = mode (2 = physical)
    float4   g_AtRayleigh;  // rgb = scattering /km at sea level, w = scale height (km)
    float4   g_AtMie;       // x = scattering /km, y = absorption /km, z = scale height (km), w = anisotropy g
    float4   g_AtOzone;     // rgb = absorption /km at the layer centre, w = layer centre (km)
    float4   g_AtOzone2;    // x = layer half-width (km), y = aerial perspective range (km), z = aerial perspective strength, w = sky-view LUT height
    float4   g_AtGround;    // rgb = ground albedo, w = sky-view LUT width
    float4   g_AtSun;       // xyz = direction toward the sun, w = sun intensity
    float4   g_AtSunCol;    // rgb = sun colour, w = sky intensity
    float4   g_AtCam;       // xyz = camera position (world, m), w = planet centre y (world, m) = -Rg * 1000
    float4   g_AtScreen;    // x, y = pass target size, z = LUT pass mode, w = frame
    float4x4 g_AtInvVP;     // clip -> world (unjittered) for the AP volume / apply
    float4   g_AtMisc;      // camera near, far, scene is LDR (1), white point
};

Texture2D<float4> g_AtTrans;   SamplerState g_AtTrans_sampler;     // 256x64: transmittance to the top (rgb)
Texture2D<float4> g_AtMulti;   SamplerState g_AtMulti_sampler;     // 32x32: multiple scattering Psi (rgb)
Texture2D<float4> g_AtSkyView; SamplerState g_AtSkyView_sampler;   // 192x108: in-scatter (rgb, sun applied), transmittance (a)
Texture3D<float4> g_AtAP;      SamplerState g_AtAP_sampler;        // 32^3: aerial perspective in-scatter (rgb), transmittance (a)

static const float AT_TRANS_W = 256.0, AT_TRANS_H = 64.0, AT_MULTI_N = 32.0;

float  AtmoRg() { return g_AtRadii.x; }
float  AtmoRt() { return g_AtRadii.y; }
bool   AtmoOn() { return g_AtRadii.w > 1.5; }
float2 AtmoSkyViewRes() { return float2(g_AtGround.w, g_AtOzone2.w); }
// The planet sits under the camera (a flat world rides the top of the sphere): centre xz = camera xz.
float3 AtmoCentreWorld() { return float3(g_AtCam.x, g_AtCam.w, g_AtCam.z); }
float3 AtmoToKm(float3 wpos) { return (wpos - AtmoCentreWorld()) * 0.001; }   // world metres -> planet-centred km

// ---- the medium ------------------------------------------------------------------------------
struct AtmoMedium { float3 scatR; float3 scatM; float3 ext; };
AtmoMedium AtmoSample(float3 p)   // p in planet-centred km
{
    float h = max(length(p) - AtmoRg(), 0.0);
    float dR = exp(-h / max(g_AtRayleigh.w, 0.01));
    float dM = exp(-h / max(g_AtMie.z, 0.01));
    float dO = saturate(1.0 - abs(h - g_AtOzone.w) / max(g_AtOzone2.x, 0.01));
    float s = g_AtRadii.z;
    AtmoMedium m;
    m.scatR = g_AtRayleigh.rgb * dR * s;
    m.scatM = g_AtMie.x * dM * s;
    m.ext   = m.scatR + (g_AtMie.x + g_AtMie.y) * dM * s + g_AtOzone.rgb * dO * s;
    return m;
}
float AtmoPhaseR(float mu) { return 3.0 / (16.0 * AT_PI) * (1.0 + mu * mu); }
float AtmoPhaseM(float mu)
{
    float g = g_AtMie.w, g2 = g * g;
    return (1.0 - g2) / (4.0 * AT_PI * pow(max(1.0 + g2 - 2.0 * g * mu, 1e-4), 1.5));
}

// ---- LUT access ----------------------------------------------------------------------------
// Transmittance from a point at radius r along a direction with zenith cosine mu to the top.
float3 AtmoTransmittance(float r, float mu)
{
    // below the horizon the LUT's mapping clamps to the grazing ray (a red glow through the
    // planet): the planet occludes instead, softened over the sun's own width
    float rr = max(r, AtmoRg());
    float muH = -sqrt(max(1.0 - (AtmoRg() * AtmoRg()) / (rr * rr), 0.0));
    float occl = smoothstep(muH - 0.004, muH + 0.004, mu);
    return g_AtTrans.SampleLevel(g_AtTrans_sampler, AtmoTransParamsToUv(AtmoRg(), AtmoRt(), r, mu), 0).rgb * occl;
}
// Multiple scattering Psi for (sun zenith cosine, altitude).
float3 AtmoMulti(float r, float muSun)
{
    float2 uv = float2(muSun * 0.5 + 0.5, saturate((r - AtmoRg()) / max(AtmoRt() - AtmoRg(), 1e-6)));
    uv = AtmoUnitToSubUv(uv, float2(AT_MULTI_N, AT_MULTI_N));
    return g_AtMulti.SampleLevel(g_AtMulti_sampler, uv, 0).rgb;
}
// The camera's frame on the sphere: up = away from the planet centre; the sun's azimuth in the tangent plane.
void AtmoCameraFrame(out float3 camKm, out float viewHeight, out float3 up, out float3 sunTangent)
{
    camKm = AtmoToKm(g_AtCam.xyz);
    viewHeight = max(length(camKm), AtmoRg() + 0.001);
    up = camKm / viewHeight;
    float3 s = g_AtSun.xyz - up * dot(g_AtSun.xyz, up);
    float sl = length(s);
    sunTangent = (sl > 1e-4) ? s / sl : (abs(up.y) < 0.9 ? normalize(cross(up, float3(0, 1, 0))) : float3(1, 0, 0));
}
// The sky along a world direction from the camera: rgb = in-scatter (sun + sky intensity
// applied), a = transmittance to the end of the ray (the ground or the top of the atmosphere).
float4 AtmoSkyView(float3 dir)
{
    float2 uv = AtmoSkyViewUvFor(AtmoRg(), AtmoSkyViewRes(), AtmoToKm(g_AtCam.xyz), g_AtSun.xyz, dir);
    return g_AtSkyView.SampleLevel(g_AtSkyView_sampler, uv, 0);
}
// Aerial perspective for a world point seen from the camera: rgb = in-scatter, a = transmittance.
float4 AtmoAerial(float2 uv, float distM)
{
    float w = sqrt(saturate(distM * 0.001 / max(g_AtOzone2.y, 0.1)));
    float4 ap = g_AtAP.SampleLevel(g_AtAP_sampler, float3(uv, w), 0);
    float s = g_AtOzone2.z;
    return float4(ap.rgb * s, lerp(1.0, ap.a, s));
}

// ---- integration --------------------------------------------------------------------------
// In-scattered luminance and transmittance along a ray from a planet-centred point (km); the
// ray is clipped to the atmosphere (entered from outside when needed), to the ground and to
// tMax (km, <0 = none). `steps` samples, `msOnly` = uniform phase, no Psi (the multi-scatter LUT pass).
struct AtmoResult { float3 L; float3 T; float3 fms; float tEnd; bool ground; };
AtmoResult AtmoIntegrate(float3 o, float3 d, float3 sun, float tMax, int steps, bool msOnly)
{
    AtmoResult res;
    res.L = 0.0; res.T = 1.0; res.fms = 0.0; res.tEnd = 0.0; res.ground = false;
    float Rg = AtmoRg(), Rt = AtmoRt();
    float t0 = 0.0;
    if (length(o) > Rt)
    {   // outside: enter the atmosphere first
        float tIn = AtmoRaySphere(o, d, Rt);
        if (tIn < 0.0) return res;
        t0 = tIn;
        if (tMax >= 0.0 && tMax <= t0) return res;
    }
    float tTop = AtmoRaySphereFar(o, d, Rt);
    float tGround = AtmoRaySphere(o, d, Rg);
    float t1 = tTop;
    if (tGround >= 0.0 && tGround < t1) { t1 = tGround; res.ground = true; }
    if (tMax >= 0.0 && tMax < t1) { t1 = tMax; res.ground = false; }
    if (t1 <= t0) return res;
    res.tEnd = t1;
    float mu = dot(d, sun);
    float phR = msOnly ? 1.0 / (4.0 * AT_PI) : AtmoPhaseR(mu);
    float phM = msOnly ? 1.0 / (4.0 * AT_PI) : AtmoPhaseM(mu);
    float3 T = 1.0, L = 0.0, fms = 0.0;
    float dt = (t1 - t0) / (float)steps;
    float t = t0 + dt * 0.3;
    [loop] for (int i = 0; i < steps; ++i)
    {
        float3 p = o + d * t;
        float  r = length(p);
        float3 upP = p / max(r, 1e-4);
        float  muS = dot(upP, sun);
        AtmoMedium m = AtmoSample(p);
        float3 sunT = AtmoTransmittance(r, muS);
        float  shadow = (AtmoRaySphere(p, sun, Rg) >= 0.0) ? 0.0 : 1.0;   // the planet's own shadow
        float3 psi = msOnly ? 0.0 : AtmoMulti(r, muS);
        float3 scat = m.scatR + m.scatM;
        float3 S = sunT * shadow * (m.scatR * phR + m.scatM * phM) + psi * scat;
        float3 Te = exp(-m.ext * dt);
        float3 ext = max(m.ext, 1e-6);
        L += T * (S - S * Te) / ext;
        if (msOnly) fms += T * (scat - scat * Te) / ext;
        T *= Te;
        t += dt;
    }
    res.L = L; res.T = T; res.fms = fms;
    return res;
}

float3 AtmoSunRadiance() { return g_AtSunCol.rgb * g_AtSun.w; }
// Transmittance along a direction from a point (km) to space: the LUT inside the atmosphere;
// from outside, the LUT at the entry point (a ray that misses the atmosphere is clear).
float3 AtmoViewTransmittance(float3 pKm, float3 dir)
{
    float r = length(pKm);
    if (r <= AtmoRt()) return AtmoTransmittance(max(r, AtmoRg()), dot(dir, pKm / max(r, 1e-4)));
    float tIn = AtmoRaySphere(pKm, dir, AtmoRt());
    if (tIn < 0.0) return 1.0;
    float3 e = pKm + dir * tIn;
    return AtmoTransmittance(AtmoRt(), dot(dir, e / AtmoRt()));
}
// The ground's radiance where a ray hits the planet: the albedo lit by the sun through the
// atmosphere plus the diffuse sky, times the view transmittance.
float3 AtmoGroundRadiance(float3 hitKm, float viewT)
{
    float r = length(hitKm);
    float3 up = hitKm / max(r, 1e-4);
    float  muS = dot(up, g_AtSun.xyz);
    float3 sunT = AtmoTransmittance(r, muS) * saturate(muS);
    float3 skyL = AtmoMulti(r, muS) * 4.0 * AT_PI;   // Psi = radiance per unit sun; the hemisphere integrates it
    return g_AtGround.rgb / AT_PI * AtmoSunRadiance() * (sunT + skyL) * g_AtSunCol.w * viewT;
}
#endif
