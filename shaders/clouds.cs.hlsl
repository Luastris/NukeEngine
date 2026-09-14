// Volumetric clouds, the march (at the march resolution, g_ClScreen.xy). One ray per texel
// through the layer's segment (clipped by the scene depth and the max distance), a jittered
// two-level march (coarse steps through empty air, fine steps with the detail erosion inside a
// cloud), each step lit by the sun (Beer-Lambert + multiple-scattering octaves + phase) and the
// sky, integrated with the energy-conserving per-step form: L += T (S - S T_step) / sigma.
// Out: g_CloudOut = (in-scattered radiance, transmittance); g_CloudDist = (entry distance,
// transmittance-weighted mean distance) for the composite and the temporal reprojection.
#include "clouds.hlsli"

Texture2D<float>       g_Depth;   // scene device depth, full resolution (point Load)
RWTexture2D<float4>    g_CloudOut;
RWTexture2D<float2>    g_CloudDist;

float LinearZ(float d) { float n = g_ClMisc.x, f = g_ClMisc.y; return n * f / max(f - d * (f - n), 1e-6); }
// Interleaved gradient noise: the per-texel, per-frame start jitter (the temporal pass averages it)
float IGN(float2 px, float frame)
{
    px += frame * float2(5.588238, 5.588238);
    return frac(52.9829189 * frac(0.06711056 * px.x + 0.00583715 * px.y));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int2 res = (int2)g_ClScreen.xy;
    if (any((int2)id.xy >= res)) return;
    const float frame = g_ClCam.w;
    // the texel's ray, its centre offset by the frame's sub-texel jitter so that frames sample
    // different points and the temporal blend resolves the full resolution
    float2 jit = float2(frac(frame * 0.5 + 0.25), frac(frame * 0.25 + 0.5)) - 0.5;
    float2 uv  = (float2(id.xy) + 0.5 + jit * 0.9) / g_ClScreen.xy;
    float4 wp  = mul(g_ClInvVP, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0));
    float3 o   = g_ClCam.xyz;
    float3 d   = normalize(wp.xyz / wp.w);   // the direction matrix has no translation (precision far from the origin): no camera subtraction
    // the scene in front: the ray ends at the surface (full-res depth, the texel's centre)
    int2   fpx = (int2)(uv * g_ClScreen.zw);
    float  dev = g_Depth.Load(int3(clamp(fpx, int2(0, 0), (int2)g_ClScreen.zw - 1), 0));
    float4 fc  = mul(g_ClInvVP, float4(0.0, 0.0, 1.0, 1.0));
    float3 fwd = normalize(fc.xyz / fc.w);   // the camera's forward: view depth -> distance along this ray
    float  sceneDist = (dev >= 0.99999) ? 1e9 : LinearZ(dev) / max(dot(d, fwd), 1e-4);
    float  tA, tB;
    float4 outC = float4(0.0, 0.0, 0.0, 1.0);
    float2 outD = float2(1e9, 1e9);
    if (CloudSegment(o, d, sceneDist, tA, tB))
    {
        tB = min(tB, tA + g_ClLayer.z);   // the max distance is the path INSIDE the layer (from space the entry alone is hundreds of km)
        const int   N = (int)g_ClLayer.w;
        const float thick = g_ClLayer.y - g_ClLayer.x;
        const float3 L = g_ClSunDir.xyz;
        const float mu = dot(d, L);
        const float3 M = g_ClMoon.xyz;
        const float muM = dot(d, M);
        const bool  moon = g_ClMoon.w > 0.0;   // the moon as a second light (night clouds are not black holes)
        const float sigma0 = CL_SIGMA * g_ClCover.z;
        const bool  atmo = AtmoOn();
        float segLen = tB - tA;
        float stepF  = segLen / (float)N;              // the fine step
        float t = tA + stepF * IGN(float2(id.xy), frame);
        float T = 1.0;
        float3 Lacc = 0.0;
        float  dAcc = 0.0, wAcc = 0.0;
        int    emptyRun = 0;
        [loop] for (int i = 0; i < N * 2 && t < tB; ++i)
        {
            float3 p = o + d * t;
            float  hf;
            // coarse look first: no detail; empty air is crossed at double steps
            float  dens = CloudDensity(p, false, hf);
            if (dens <= 0.0)
            {
                emptyRun++;
                t += (emptyRun > 2) ? stepF * 2.0 : stepF;
                continue;
            }
            if (emptyRun > 2) { t -= stepF; emptyRun = 0; continue; }   // stepped into a cloud at double pace: back up, go fine
            emptyRun = 0;
            dens = CloudDensity(p, true, hf);
            if (dens > 0.0)
            {
                float  sigma = dens * sigma0;
                float  tau   = CloudSunOpticalDepth(p, L, thick);
                float3 sunT  = 1.0;
                if (atmo) { float3 pk = AtmoToKm(p); float rk = length(pk); sunT = AtmoTransmittance(rk, dot(pk / rk, L)); }
                float3 S     = CloudSunLight(tau, mu, sunT) + CloudAmbient(hf);   // single-scatter albedo ~1
                if (moon)
                {
                    float  tauM  = CloudSunOpticalDepth(p, M, thick);
                    float3 moonT = 1.0;
                    if (atmo) { float3 pk = AtmoToKm(p); float rk = length(pk); moonT = AtmoTransmittance(rk, dot(pk / rk, M)); }
                    S += g_ClMoonCol.rgb * g_ClMoon.w * CloudLightScatter(tauM, muM) * moonT;
                }
                S *= sigma;
                float  Ts    = exp(-sigma * stepF);
                Lacc += T * (S - S * Ts) / sigma;
                float  w = T * (1.0 - Ts);
                dAcc += w * t; wAcc += w;
                T *= Ts;
                if (T < 0.005) break;
            }
            t += stepF;
        }
        if (atmo && T < 0.999)
        {   // aerial perspective up to the cloud: its light through the air, the air's light in front of it
            float3 camKm = AtmoToKm(o);
            AtmoResult ap = AtmoIntegrate(camKm, d, L, ((wAcc > 1e-5) ? dAcc / wAcc : tA) * 0.001, 12, false);
            Lacc = Lacc * ap.T + ap.L * AtmoSunRadiance() * g_AtSunCol.w * (1.0 - T);
        }
        outC = float4(Lacc, T);
        outD = float2(tA, (wAcc > 1e-5) ? dAcc / wAcc : tB);
    }
    g_CloudOut[id.xy]  = outC;
    g_CloudDist[id.xy] = outD;
}
