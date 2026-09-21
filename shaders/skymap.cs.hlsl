// The sky map (skymap.hlsli), one texel per direction. Mode 0 = the CLOUD LAYER: one row in
// g_SmSize.z marched per frame (the layer converges over a few frames; a per-frame jitter blended
// into the previous value smooths the march), (in-scatter, transmittance) per texel. Mode 2 =
// the map: the sky (physical: the camera's sky-view LUT + the planet where the ray hits it;
// procedural: the gradient) under the cloud layer, every texel every frame (cheap). Mode 1 = the
// summary of the finished map: zenith / horizon ring / ground averages into g_OutSummary - the
// FrameCB sky colours the ambient and the roughest lobes use, so an overcast sky lights the
// world bright and grey, not clear-sky blue.
#include "clouds.hlsli"
#include "skymap.hlsli"

cbuffer SkyMapCB
{
    float4 g_SmTop;    // procedural zenith rgb, w = sky intensity
    float4 g_SmHor;    // procedural horizon rgb, w = a boundless ocean is live
    float4 g_SmGnd;    // procedural ground rgb, w = the ocean's level
    float4 g_SmP;      // mode (0 clouds, 1 summary, 2 map), physical sky, clouds on, frame phase
    float4 g_SmCam;    // camera xyz, w = the cloud layer holds a previous frame (blend into it)
    float4 g_SmSize;   // map w, h, row stride of the cloud march, summary mip
};
RWTexture2D<float4> g_SkyOut;       // mode 0: the cloud layer; mode 2: the map
Texture2D<float4> g_CloudIn;        // mode 2: the cloud layer (Load)
Texture2D g_SkyIn; SamplerState g_SkyIn_sampler;   // the finished map (summary mode)
RWStructuredBuffer<float4> g_OutSummary;

float SkyIGN(float2 px, float frame)
{
    px += frame * float2(5.588238, 5.588238);
    return frac(52.9829189 * frac(0.06711056 * px.x + 0.00583715 * px.y));
}

// The clouds along a sky direction: (in-scatter, transmittance). clouds.cs's march, no jitter.
float4 SkyMapClouds(float3 o, float3 d, float jit)
{
    float tA, tB;
    if (!CloudSegment(o, d, 1e9, tA, tB)) return float4(0.0, 0.0, 0.0, 1.0);
    tB = min(tB, tA + g_ClLayer.z);
    const int   N = max((int)g_ClLayer.w, 8);
    const float thick = g_ClLayer.y - g_ClLayer.x;
    const float3 L = g_ClSunDir.xyz;
    const float mu = dot(d, L);
    const float3 M = g_ClMoon.xyz;
    const float muM = dot(d, M);
    const bool  moon = g_ClMoon.w > 0.0;
    const float sigma0 = CL_SIGMA * g_ClCover.z;
    const bool  atmo = AtmoOn();
    float stepF = (tB - tA) / (float)N;
    float t = tA + stepF * jit;
    float T = 1.0;
    float3 Lacc = 0.0;
    float  dAcc = 0.0, wAcc = 0.0;
    int    emptyRun = 0;
    [loop] for (int i = 0; i < N * 2 && t < tB; ++i)
    {
        float3 p = o + d * t;
        float  hf;
        float  dens = CloudDensity(p, false, hf);
        if (dens <= 0.0)
        {
            emptyRun++;
            t += (emptyRun > 2) ? stepF * 2.0 : stepF;
            continue;
        }
        if (emptyRun > 2) { t -= stepF; emptyRun = 0; continue; }
        emptyRun = 0;
        dens = CloudDensity(p, true, hf);
        if (dens > 0.0)
        {
            float  sigma = dens * sigma0;
            float  tau   = CloudSunOpticalDepth(p, L, thick);
            float3 sunT  = 1.0;
            if (atmo) { float3 pk = AtmoToKm(p); float rk = length(pk); sunT = AtmoTransmittance(rk, dot(pk / rk, L)); }
            float3 S     = CloudSunLight(tau, mu, sunT) + CloudAmbient(hf);
            if (moon)
            {
                float  tauM  = CloudSunOpticalDepth(p, M, thick);
                float3 moonT = 1.0;
                if (atmo) { float3 pk = AtmoToKm(p); float rk = length(pk); moonT = AtmoTransmittance(rk, dot(pk / rk, M)); }
                S += g_ClMoonCol.rgb * g_ClMoon.w * CloudLightScatter(tauM, muM) * moonT;
            }
            S *= sigma;
            float  Ts = exp(-sigma * stepF);
            Lacc += T * (S - S * Ts) / sigma;
            float  w = T * (1.0 - Ts);
            dAcc += w * t; wAcc += w;
            T *= Ts;
            if (T < 0.005) break;
        }
        t += stepF;
    }
    if (atmo && T < 0.999)
    {
        float3 camKm = AtmoToKm(o);
        AtmoResult ap = AtmoIntegrate(camKm, d, L, ((wAcc > 1e-5) ? dAcc / wAcc : tA) * 0.001, 12, false);
        Lacc = Lacc * ap.T + ap.L * AtmoSunRadiance() * g_AtSunCol.w * (1.0 - T);
    }
    return float4(Lacc, T);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (g_SmP.x > 0.5 && g_SmP.x < 1.5)
    {   // the summary off the 64 x 32 mip: the zenith cap, the ring just above the horizon, the ground
        if (any(id.xy != 0)) return;
        const float mip = g_SmSize.w;
        float3 zen = 0.0, hor = 0.0, gnd = 0.0; float zw = 0.0, gw = 0.0;
        [loop] for (int x = 0; x < 64; ++x)
        {
            float u = ((float)x + 0.5) / 64.0;
            [loop] for (int y = 0; y < 8; ++y)
            {   // the cap: 0..45 degrees from the zenith, weighted by the ring's solid angle
                float v = ((float)y + 0.5) / 32.0;
                float w = sin(v * SKYMAP_PI);
                zen += g_SkyIn.SampleLevel(g_SkyIn_sampler, float2(u, v), mip).rgb * w; zw += w;
            }
            hor += g_SkyIn.SampleLevel(g_SkyIn_sampler, float2(u, 0.5 - 0.02), mip).rgb;
            [loop] for (int y2 = 20; y2 < 32; ++y2)
            {
                float v = ((float)y2 + 0.5) / 32.0;
                float w = sin(v * SKYMAP_PI);
                gnd += g_SkyIn.SampleLevel(g_SkyIn_sampler, float2(u, v), mip).rgb * w; gw += w;
            }
        }
        g_OutSummary[0] = float4(zen / max(zw, 1e-4), 1.0);
        g_OutSummary[1] = float4(hor / 64.0, 1.0);
        g_OutSummary[2] = float4(gnd / max(gw, 1e-4), 1.0);
        return;
    }
    const int2 res = (int2)g_SmSize.xy;
    if (g_SmP.x < 0.5)
    {   // the cloud layer: this frame's rows
        const int stride = max((int)g_SmSize.z, 1);
        int2 px = int2((int)id.x, (int)id.y * stride + (int)g_SmP.w);
        if (any(px >= res)) return;
        float2 uv = (float2(px) + 0.5) / g_SmSize.xy;
        float3 d = SkyMapDir(uv);
        float4 cl = float4(0.0, 0.0, 0.0, 1.0);
        if (g_SmP.z > 0.5 && d.y > -0.05)
        {
            bool ground = false;
            if (g_SmP.y > 0.5) ground = AtmoRaySphere(AtmoToKm(g_SmCam.xyz), d, AtmoRg()) >= 0.0;
            if (!ground) cl = SkyMapClouds(g_SmCam.xyz, d, SkyIGN(float2(px), g_SmP.w));
        }
        if (g_SmCam.w > 0.5) cl = lerp(g_SkyOut[px], cl, 0.35);   // converge over frames (the jitter averages out)
        g_SkyOut[px] = cl;
        return;
    }
    if (any((int2)id.xy >= res)) return;
    float2 uv = (float2(id.xy) + 0.5) / g_SmSize.xy;
    float3 d = SkyMapDir(uv);
    float3 sky;
    bool ground = false;
    if (g_SmP.y > 0.5)
    {
        float4 sv = AtmoSkyView(d);
        sky = sv.rgb;
        float3 camKm = AtmoToKm(g_SmCam.xyz);
        float  tG = AtmoRaySphere(camKm, d, AtmoRg());
        ground = tG >= 0.0;
        if (ground)
        {
            if (g_SmHor.w > 0.5) sky += AtmoSkyView(float3(d.x, -d.y, d.z)).rgb * sv.a;   // the sea: the sky mirrored (sky.ps)
            else sky += AtmoGroundRadiance(camKm + d * tG, sv.a);
        }
    }
    else
    {
        sky = (d.y >= 0.0) ? lerp(g_SmHor.rgb, g_SmTop.rgb, pow(saturate(d.y), 0.5))
                           : lerp(g_SmHor.rgb, g_SmGnd.rgb, saturate(-d.y));
        sky *= g_SmTop.w;
    }
    if (g_SmP.z > 0.5 && !ground)
    {
        float4 cl = g_CloudIn.Load(int3((int2)id.xy, 0));
        sky = sky * cl.a + cl.rgb;
    }
    g_SkyOut[id.xy] = float4(sky, 1.0);
}
