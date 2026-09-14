// Physical atmosphere, the pure part (no cbuffers / textures): ray-sphere, the LUT
// parametrisations (Bruneton's transmittance mapping, Hillaire's sky-view mapping). Included by
// atmosphere.hlsli (the full medium + LUT access) and by world.ps (which samples the sky-view and
// transmittance LUTs with its own bindings). Units: km, planet-centred.
#ifndef ATMO_MAP_HLSLI
#define ATMO_MAP_HLSLI

static const float AT_PI = 3.14159265;

// Nearest positive ray/sphere hit distance (sphere at the origin), -1 if none.
float AtmoRaySphere(float3 o, float3 d, float r)
{
    float b = dot(o, d), c = dot(o, o) - r * r;
    float h = b * b - c;
    if (h < 0.0) return -1.0;
    h = sqrt(h);
    float t0 = -b - h, t1 = -b + h;
    if (t0 < 0.0 && t1 < 0.0) return -1.0;
    return (t0 < 0.0) ? t1 : t0;
}
float AtmoRaySphereFar(float3 o, float3 d, float r)   // the far hit (exit), -1 if none
{
    float b = dot(o, d), c = dot(o, o) - r * r;
    float h = b * b - c;
    if (h < 0.0) return -1.0;
    return -b + sqrt(h);
}

float2 AtmoSubUvToUnit(float2 uv, float2 res) { return (uv - 0.5 / res) * (res / (res - 1.0)); }
float2 AtmoUnitToSubUv(float2 uv, float2 res) { return (uv + 0.5 / res) * (res / (res + 1.0)); }

// Transmittance LUT: (viewHeight r, viewZenithCos mu) <-> uv.
void AtmoUvToTransParams(float Rg, float Rt, float2 uv, out float r, out float mu)
{
    float H = sqrt(max(Rt * Rt - Rg * Rg, 1e-6));
    float rho = H * uv.y;
    r = sqrt(rho * rho + Rg * Rg);
    float dMin = Rt - r, dMax = rho + H;
    float d = dMin + uv.x * (dMax - dMin);
    mu = (d <= 1e-6) ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = clamp(mu, -1.0, 1.0);
}
float2 AtmoTransParamsToUv(float Rg, float Rt, float r, float mu)
{
    r = clamp(r, Rg, Rt);
    float H = sqrt(max(Rt * Rt - Rg * Rg, 1e-6));
    float rho = sqrt(max(r * r - Rg * Rg, 0.0));
    float disc = r * r * (mu * mu - 1.0) + Rt * Rt;
    float d = max(0.0, -r * mu + sqrt(max(disc, 0.0)));
    float dMin = Rt - r, dMax = rho + H;
    return float2((d - dMin) / max(dMax - dMin, 1e-6), rho / H);
}

// Sky-view LUT: non-linear elevation around the horizon (half the texels above, half below),
// azimuth relative to the sun (the sky is symmetric about the sun's plane).
void AtmoUvToSkyViewParams(float Rg, float2 res, float2 uv, float viewHeight, out float viewZenithCos, out float lightViewCos)
{
    uv = AtmoSubUvToUnit(uv, res);
    float vh = sqrt(max(viewHeight * viewHeight - Rg * Rg, 0.0));
    float beta = acos(clamp(vh / max(viewHeight, 1e-4), -1.0, 1.0));
    float zenithHorizon = AT_PI - beta;
    if (uv.y < 0.5)
    {
        float c = 2.0 * uv.y; c = 1.0 - c; c *= c; c = 1.0 - c;
        viewZenithCos = cos(zenithHorizon * c);
    }
    else
    {
        float c = uv.y * 2.0 - 1.0; c *= c;
        viewZenithCos = cos(zenithHorizon + beta * c);
    }
    float c = uv.x; c *= c;
    lightViewCos = -(c * 2.0 - 1.0);
}
float2 AtmoSkyViewParamsToUv(float Rg, float2 res, bool intersectGround, float viewZenithCos, float lightViewCos, float viewHeight)
{
    float vh = sqrt(max(viewHeight * viewHeight - Rg * Rg, 0.0));
    float beta = acos(clamp(vh / max(viewHeight, 1e-4), -1.0, 1.0));
    float zenithHorizon = AT_PI - beta;
    float2 uv;
    if (!intersectGround)
    {
        float c = acos(clamp(viewZenithCos, -1.0, 1.0)) / max(zenithHorizon, 1e-4);
        c = 1.0 - c; c = sqrt(saturate(c)); c = 1.0 - c;
        uv.y = c * 0.5;
    }
    else
    {
        float c = (acos(clamp(viewZenithCos, -1.0, 1.0)) - zenithHorizon) / max(beta, 1e-4);
        c = sqrt(saturate(c));
        uv.y = c * 0.5 + 0.5;
    }
    { float c = -lightViewCos * 0.5 + 0.5; c = sqrt(saturate(c)); uv.x = c; }
    return AtmoUnitToSubUv(uv, res);
}
// The sky-view uv for a world direction from a camera at camKm with the sun direction `sun`.
float2 AtmoSkyViewUvFor(float Rg, float2 res, float3 camKm, float3 sun, float3 dir)
{
    float viewHeight = max(length(camKm), Rg + 0.001);
    float3 up = camKm / viewHeight;
    float3 s = sun - up * dot(sun, up);
    float sl = length(s);
    float3 sunT = (sl > 1e-4) ? s / sl : (abs(up.y) < 0.9 ? normalize(cross(up, float3(0, 1, 0))) : float3(1, 0, 0));
    float viewZenithCos = dot(dir, up);
    float3 dt = dir - up * viewZenithCos;
    float dl = length(dt);
    float lightViewCos = (dl > 1e-4) ? dot(dt / dl, sunT) : 1.0;
    bool ground = AtmoRaySphere(camKm, dir, Rg) >= 0.0;
    return AtmoSkyViewParamsToUv(Rg, res, ground, viewZenithCos, lightViewCos, viewHeight);
}
// The eclipsing moon along a world direction: 1 inside its disc (a hair wider than the sun's).
// x = the moon's offset from the sun's centre in sun radii (World maps the Eclipse slider to a
// full transit: far left -> centred -> far right); |x| >= 1000 = no moon.
float EclipseCover(float3 dir, float3 sunD, float size, float x)
{
    if (abs(x) >= 1000.0) return 0.0;
    float3 right  = normalize(cross(abs(sunD.y) < 0.95 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0), sunD));
    float3 moonC  = normalize(sunD + right * (x * size));
    float  mang   = acos(clamp(dot(dir, moonC), -1.0, 1.0));
    return 1.0 - smoothstep(size * 1.02, size * 1.08, mang);
}
#endif
