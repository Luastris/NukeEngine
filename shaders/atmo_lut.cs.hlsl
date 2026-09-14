// Physical atmosphere, the LUT passes (g_AtScreen.z): 0 = transmittance 256x64 (when the medium
// changes), 1 = multiple scattering 32x32 (same), 2 = the camera's sky-view 192x108 (every pass),
// 3 = the aerial-perspective froxel volume 32^3 (every pass), 4 = the sky summary (zenith /
// horizon / ground colours for the FrameCB fallbacks: water, GI, RT misses read those).
#include "atmosphere.hlsli"

RWTexture2D<float4>        g_Out2D;
RWTexture3D<float4>        g_Out3D;
RWStructuredBuffer<float4> g_OutSummary;

// A Fibonacci sphere direction.
float3 FibDir(int i, int n)
{
    float k = (float)i + 0.5;
    float z = 1.0 - 2.0 * k / (float)n;
    float r = sqrt(saturate(1.0 - z * z));
    float a = k * 2.399963;
    return float3(r * cos(a), z, r * sin(a));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int mode = (int)g_AtScreen.z;
    const float Rg = AtmoRg(), Rt = AtmoRt();
    if (mode == 0)
    {   // transmittance to the top: exp(-integral of extinction), 40 steps (through the planet = ~0)
        if (id.x >= (uint)AT_TRANS_W || id.y >= (uint)AT_TRANS_H) return;
        float2 uv = (float2(id.xy) + 0.5) / float2(AT_TRANS_W, AT_TRANS_H);
        float r, mu; AtmoUvToTransParams(Rg, Rt, uv, r, mu);
        float3 o = float3(0.0, r, 0.0), d = float3(sqrt(saturate(1.0 - mu * mu)), mu, 0.0);
        float tEnd = AtmoRaySphereFar(o, d, Rt);
        float3 tau = 0.0;
        const int N = 40;
        float dt = tEnd / (float)N, t = dt * 0.5;
        [loop] for (int i = 0; i < N; ++i) { tau += AtmoSample(o + d * t).ext * dt; t += dt; }
        g_Out2D[id.xy] = float4(exp(-tau), 1.0);
    }
    else if (mode == 1)
    {   // multiple scattering Psi(mu_sun, altitude): 64 directions, uniform phase, ground bounce
        if (id.x >= (uint)AT_MULTI_N || id.y >= (uint)AT_MULTI_N) return;
        float2 uv = AtmoSubUvToUnit((float2(id.xy) + 0.5) / AT_MULTI_N, float2(AT_MULTI_N, AT_MULTI_N));
        float muS = uv.x * 2.0 - 1.0;
        float r   = Rg + uv.y * (Rt - Rg);
        float3 o  = float3(0.0, r, 0.0);
        float3 sun = float3(sqrt(saturate(1.0 - muS * muS)), muS, 0.0);
        float3 L2 = 0.0, fms = 0.0;
        const int N = 64;
        [loop] for (int i = 0; i < N; ++i)
        {
            float3 d = FibDir(i, N);
            AtmoResult res = AtmoIntegrate(o, d, sun, -1.0, 20, true);
            float3 L = res.L;
            if (res.ground)
            {   // the sunlit ground bounces back into the atmosphere
                float3 hit = o + d * res.tEnd;
                float  rh = length(hit);
                float  nl = saturate(dot(hit / rh, sun));
                L += res.T * g_AtGround.rgb / AT_PI * AtmoTransmittance(rh, dot(hit / rh, sun)) * nl;
            }
            L2 += L; fms += res.fms;
        }
        L2 /= (float)N; fms /= (float)N;
        g_Out2D[id.xy] = float4(L2 / max(1.0 - fms, 1e-3), 1.0);
    }
    else if (mode == 2)
    {   // the camera's sky-view: in-scatter (sun + sky intensity applied) and transmittance per direction
        const int2 res = int2((int)g_AtGround.w, (int)g_AtOzone2.w);
        if (any((int2)id.xy >= res)) return;
        float3 camKm, up, sunT; float viewHeight;
        AtmoCameraFrame(camKm, viewHeight, up, sunT);
        float2 uv = (float2(id.xy) + 0.5) / float2(res);
        float vz, lv; AtmoUvToSkyViewParams(Rg, float2(res), uv, viewHeight, vz, lv);
        float3 bit = normalize(cross(up, sunT));
        float  sz = sqrt(saturate(1.0 - vz * vz));
        float3 d  = up * vz + (sunT * lv + bit * sqrt(saturate(1.0 - lv * lv))) * sz;
        AtmoResult r = AtmoIntegrate(camKm, d, g_AtSun.xyz, -1.0, 32, false);
        g_Out2D[id.xy] = float4(r.L * AtmoSunRadiance() * g_AtSunCol.w, dot(r.T, 1.0 / 3.0));
    }
    else if (mode == 3)
    {   // aerial perspective: one thread per froxel column, 32 slices at squared depth over the range
        const int N = 32;
        if (id.x >= (uint)N || id.y >= (uint)N) return;
        float2 uv = (float2(id.xy) + 0.5) / (float)N;
        float4 wp = mul(g_AtInvVP, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0));
        float3 d  = normalize(wp.xyz / wp.w);   // the direction matrix has no translation (precision far from the origin): no camera subtraction
        float3 o  = AtmoToKm(g_AtCam.xyz);
        float3 sun = g_AtSun.xyz;
        float  range = max(g_AtOzone2.y, 0.1);
        // the stretch of the ray inside the medium: enter from space if needed, stop at the ground / the top
        float t0 = 0.0;
        bool  inAir = true;
        if (length(o) > Rt) { float tIn = AtmoRaySphere(o, d, Rt); if (tIn < 0.0) inAir = false; else t0 = tIn; }
        float tEnd = AtmoRaySphereFar(o, d, Rt);
        float tG = AtmoRaySphere(o, d, Rg); if (tG >= 0.0) tEnd = min(tEnd, tG);
        float3 T = 1.0, L = 0.0;
        float  mu = dot(d, sun), phR = AtmoPhaseR(mu), phM = AtmoPhaseM(mu);
        float  tPrev = 0.0;
        [loop] for (int i = 0; i < N; ++i)
        {
            float f = ((float)i + 1.0) / (float)N;
            float tSlice = f * f * range;
            if (inAir)
            {
                float a = max(tPrev, t0), b = min(tSlice, tEnd);
                if (b > a)
                {
                    const int S = 3;
                    float dt = (b - a) / (float)S, t = a + dt * 0.5;
                    [unroll] for (int k = 0; k < S; ++k)
                    {
                        float3 p = o + d * t;
                        float  r = length(p);
                        float3 upP = p / max(r, 1e-4);
                        float  muS = dot(upP, sun);
                        AtmoMedium m = AtmoSample(p);
                        float3 sunTr = AtmoTransmittance(r, muS) * ((AtmoRaySphere(p, sun, Rg) >= 0.0) ? 0.0 : 1.0);
                        float3 S3 = sunTr * (m.scatR * phR + m.scatM * phM) + AtmoMulti(r, muS) * (m.scatR + m.scatM);
                        float3 Te = exp(-m.ext * dt);
                        L += T * (S3 - S3 * Te) / max(m.ext, 1e-6);
                        T *= Te;
                        t += dt;
                    }
                }
            }
            g_Out3D[int3(id.xy, i)] = float4(L * AtmoSunRadiance() * g_AtSunCol.w, dot(T, 1.0 / 3.0));
            tPrev = tSlice;
        }
    }
    else
    {   // the summary: zenith, a horizon ring, the ground (for the analytic-sky fallbacks)
        if (any(id.xy != 0)) return;
        float3 camKm, up, sunT; float viewHeight;
        AtmoCameraFrame(camKm, viewHeight, up, sunT);
        float3 bit = normalize(cross(up, sunT));
        float3 zen = AtmoSkyView(up).rgb;
        float3 hor = 0.0, gnd = 0.0;
        const int N = 16;
        [loop] for (int i = 0; i < N; ++i)
        {
            float a = (float)i / (float)N * 2.0 * AT_PI;
            float3 tg = sunT * cos(a) + bit * sin(a);
            hor += AtmoSkyView(normalize(tg * cos(0.05) + up * sin(0.05))).rgb;
            // the ground's own radiance (world.ps adds the in-scatter and the view transmittance itself)
            float3 dd = normalize(tg * cos(0.2) - up * sin(0.2));
            float  tg2 = AtmoRaySphere(camKm, dd, Rg);
            gnd += (tg2 >= 0.0) ? AtmoGroundRadiance(camKm + dd * tg2, 1.0) : 0.0;
        }
        g_OutSummary[0] = float4(zen, 1.0);
        g_OutSummary[1] = float4(hor / (float)N, 1.0);
        g_OutSummary[2] = float4(gnd / (float)N, 1.0);
    }
}
