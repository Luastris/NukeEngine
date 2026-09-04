// DDGI probe rays (ray-tracing devices): one thread per (probe, ray). Inline RayQuery against
// the scene TLAS; a hit is shaded diffuse-only — direct lights with ray-traced shadows, the
// previous frame's probe irradiance as the bounce term (infinite bounces over time), emissive —
// a miss returns the sky. Output g_RayData[probe * rays + ray] = (radiance, distance); a
// back-face hit stores a negative, shortened distance so the update pass darkens and shrinks
// the visibility of probes stuck inside geometry (the classic DDGI convention).
#include "rt_common.hlsl"
#include "ddgi.hlsli"

// GICB + g_GIIrr / g_GIVis are declared by rt_common.hlsl (shared with the reflection stages).
cbuffer GIPassCB
{
    int4   g_GIPass;   // x = volume index, y = first probe, z = rays per probe, w = probe count in this dispatch
    float4 g_GIRot;    // random rotation quaternion for this frame's ray set
    float4 g_GIMisc;   // x = max ray distance, y = hysteresis, z = frame, w = 0
};
RWStructuredBuffer<float4> g_RayData;

float3 RotateQ(float3 v, float4 q) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }

// Spherical Fibonacci direction i of n (well spread, cheap).
float3 FibDir(uint i, uint n)
{
    float phi = 2.399963 * (float)i;                 // golden angle
    float z   = 1.0 - (2.0 * i + 1.0) / (float)n;
    float r   = sqrt(max(0.0, 1.0 - z * z));
    return float3(r * cos(phi), r * sin(phi), z);
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint rays  = (uint)g_GIPass.z;
    const uint local = tid.x / rays;
    const uint ray   = tid.x % rays;
    if (local >= (uint)g_GIPass.w) return;
    const uint probe = (uint)g_GIPass.y + local;

    GIVolumeGPU v = g_GIVol[g_GIPass.x];
    float3 origin = DDGIProbePos(v, DDGIProbeCoord(v, (int)probe));
    float3 dir    = RotateQ(FibDir(ray, rays), g_GIRot);
    float  maxD   = g_GIMisc.x;

    RayDesc rd; rd.Origin = origin; rd.Direction = dir; rd.TMin = 0.0; rd.TMax = maxD;
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0x04, rd);   // 0x04: every instance (particles skipped below)
    // Geometry is non-opaque in the TLAS (the reflection path alpha-tests in any-hit): commit every
    // candidate triangle except particle quads (dynamic colour block = a particle instance).
    while (q.Proceed())
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && g_Instances[q.CandidateInstanceID()].colOffset == 0xFFFFFFFFu)
            q.CommitNonOpaqueTriangleHit();

    float3 radiance; float dist;
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        radiance = EnvSample(dir, 0.0);                  // sky / reflection probe / flat ambient
        dist     = maxD;
    }
    else
    {
        float  t    = q.CommittedRayT();
        RTInstanceData inst = g_Instances[q.CommittedInstanceID()];
        uint   prim = q.CommittedPrimitiveIndex();
        float2 bc   = q.CommittedTriangleBarycentrics();
        float3x4 o2w = q.CommittedObjectToWorld3x4();
        float3 geomN = FetchWorldNormal(inst.nrmOffset, prim, bc, o2w);
        if (dot(geomN, dir) > 0.0)
        {
            radiance = 0.0;                              // inside geometry: no light, shortened distance
            dist     = -t * 0.2;
        }
        else
        {
            float2 uv     = FetchUV(inst.uvOffset, prim, bc);
            float3 N      = ApplyNormalMap(inst, prim, uv, geomN, o2w);
            float3 pos    = origin + dir * t;
            float4 dc     = FetchDynColor(inst, prim, bc);
            float3 albedo = pow(max(SampleAlbedo(inst, uv), 0.0), 2.2) * dc.rgb;
            float  metal  = inst.albedoMetal.w, rough = inst.emissiveRough.w;
            SampleMR(inst, uv, metal, rough);
            float3 emiss  = inst.emissiveRough.rgb * SampleEmissiveMap(inst, uv) * dc.rgb * dc.a;
            float3 kd     = albedo * (1.0 - saturate(metal));

            // direct: every light, diffuse only, ray-traced shadow where the light casts
            float3 Lo = 0.0;
            int cnt = (int)g_LightCount.x;
            [loop] for (int li = 0; li < cnt; ++li)
            {
                Light lt = g_Lights[li]; float type = lt.posType.w;
                float3 L; float atten = 1.0; float lmax = 1e4;
                if (type < 0.5) L = normalize(-lt.dirRange.xyz);
                else
                {
                    float3 d = lt.posType.xyz - pos; float ld = length(d); L = d / max(ld, 1e-4); lmax = ld;
                    float rng = max(lt.dirRange.w, 1e-4); float win = saturate(1.0 - pow(ld / rng, 4.0));
                    atten = (win * win) / (ld * ld + 1.0);
                    if (type > 1.5) { float cd = dot(normalize(-lt.dirRange.xyz), -L); float s = saturate((cd - lt.spot.y) / max(lt.spot.x - lt.spot.y, 1e-4)); atten *= s * s; }
                }
                float ndl = max(dot(N, L), 0.0);
                if (ndl <= 0.0 || atten <= 0.0) continue;
                bool casts = (type > 0.5 && type < 1.5) ? ((int)lt.spot.w >= 0) : ((int)lt.spot.z >= 0);
                float sh = casts ? RTShadow(pos + L * 0.05 + N * 0.02, L, lmax) : 1.0;
                Lo += kd / RTPI * lt.colorIntensity.rgb * lt.colorIntensity.w * (atten * sh) * ndl;
            }
            // bounce: last frame's probes at the hit (the volume covering it), else the sky irradiance
            float3 irr;
            if (!DDGISample(g_GIIrr, g_GIVis, g_GIIrr_sampler, g_GIVol, g_GICount.x, g_GIAtlasInv.xy, g_GIAtlasInv.zw, pos, N, -dir, irr))
                irr = (g_SkyParams.y > 0.5) ? SkyColor(N) * g_Ambient.w : g_Ambient.rgb * g_Ambient.w;
            radiance = Lo + kd * irr + emiss;
            dist     = t;
        }
    }
    g_RayData[probe * rays + ray] = float4(radiance, dist);
}
