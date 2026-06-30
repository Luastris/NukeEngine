// Shared RT reflection code — concatenated (NOT #included) ahead of rt_rgen / rt_rmiss / rt_rchit by the renderer.
// Payload, bindless geometry/material, environment, and the standard material+lighting model reused by the
// closest-hit shader. Phase 1: one hit group reproduces the engine material models (lit PBR + unlit + props).

struct RTPayload { float3 color; uint depth; };   // color = reflected radiance; depth = current recursion depth

RaytracingAccelerationStructure g_TLAS;
RWTexture2D<float4>             g_Output;          // ray-gen writes the final composited reflection here
Texture2D                      g_GBuffer;         // (octN.xy world normal, roughness, metalness)
Texture2D                      g_Depth;           // device depth (gbuffer prepass)
Texture2D                      g_Source;          // current HDR chain colour (base, for compositing)
TextureCube                    g_Probe;           SamplerState g_Probe_sampler;
ByteAddressBuffer              g_AllNrm;          // concatenated mesh normals (float3/vertex)
ByteAddressBuffer              g_AllUV;           // concatenated mesh uvs (float2/vertex)
Texture2D                      g_MatTex[64];      SamplerState g_MatTex_sampler;   // bindless albedo
struct RTInstanceData { uint nrmOffset; uint uvOffset; uint texIndex; uint matByteOffset; float4 albedoMetal; float4 emissiveRough; };
StructuredBuffer<RTInstanceData> g_Instances;
ByteAddressBuffer g_MatBytes;   // per-instance MatCB block (same packing as the raster MatCB); auto-gen chits load from here

// Surface contract — a shader's Surface(IN, O) fills O; the generated closest-hit lights/recurses it (or, if
// O.unlit, outputs O.emissive). The raster path uses the same Surface() through its own harness.
struct SurfaceIn  { float3 worldPos; float3 worldNormal; float2 uv; float3 viewDir; };
struct SurfaceOut { float3 albedo; float metallic; float roughness; float3 emissive; float alpha; bool unlit; };

cbuffer RTRefCB { float4x4 g_InvProj; float4x4 g_InvView; float4 g_RTCam; float4 g_RTParams; };  // camPos.xyz; (intensity, maxDist, maxDepth, _)

#define MAX_LIGHTS 16
#define MAX_SHADOWS 4
struct Light { float4 posType; float4 dirRange; float4 colorIntensity; float4 spot; };
cbuffer FrameCB   // identical layout to world.ps / worldFrameCB
{
    float4 g_CamPos; float4 g_Ambient; float4 g_LightCount; Light g_Lights[MAX_LIGHTS];
    float4x4 g_ShadowVP[MAX_SHADOWS]; float4 g_ShadowParams;
    float4 g_SkyTop; float4 g_SkyHorizon; float4 g_SkyGround; float4 g_SkyParams;
    float4 g_ProbePos; float4 g_ProbeParams; float4 g_ProbeBox;
};

float3 OctDecode(float2 e)
{
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
float3 SkyColor(float3 d)
{
    float up = d.y;
    float3 c = (up >= 0.0) ? lerp(g_SkyHorizon.rgb, g_SkyTop.rgb, pow(saturate(up), 0.5))
                           : lerp(g_SkyHorizon.rgb, g_SkyGround.rgb, saturate(-up));
    return c * g_SkyParams.x;
}
float3 EnvSample(float3 dir, float rough)   // probe (parallax-free, infinite), analytic sky, or flat (sky off)
{
    if (g_ProbePos.w > 0.5) return g_Probe.SampleLevel(g_Probe_sampler, dir, saturate(rough) * g_ProbeParams.y).rgb * g_ProbeParams.x;
    if (g_SkyParams.y > 0.5) return SkyColor(dir);   // procedural sky ON
    return g_Ambient.rgb;                             // sky OFF -> flat environment (no gradient; matches the direct ambient)
}

// --- Bindless geometry/material fetch at a triangle hit (instance + primitive + barycentrics) ----------------
float3 FetchWorldNormal(uint nrmOffset, uint prim, float2 bc, float3x4 o2w)
{
    float w0 = 1.0 - bc.x - bc.y; uint nb = nrmOffset + prim * 36u;   // 3 verts * 12 bytes (float3)
    float3 oN = asfloat(g_AllNrm.Load3(nb)) * w0 + asfloat(g_AllNrm.Load3(nb + 12u)) * bc.x + asfloat(g_AllNrm.Load3(nb + 24u)) * bc.y;
    return normalize(mul((float3x3)o2w, oN));
}
float2 FetchUV(uint uvOffset, uint prim, float2 bc)
{
    float w0 = 1.0 - bc.x - bc.y; uint ub = uvOffset + prim * 24u;    // 3 verts * 8 bytes (float2)
    return asfloat(g_AllUV.Load2(ub)) * w0 + asfloat(g_AllUV.Load2(ub + 8u)) * bc.x + asfloat(g_AllUV.Load2(ub + 16u)) * bc.y;
}
float3 SampleAlbedo(RTInstanceData inst, float2 uv)
{
    float3 a = inst.albedoMetal.rgb;
    if (inst.texIndex != 0xFFFFFFFFu) a *= g_MatTex[NonUniformResourceIndex(inst.texIndex)].SampleLevel(g_MatTex_sampler, uv, 0).rgb;
    return a;
}

// Shadow ray: 1 = lit, 0 = occluded (inline query, cheap; no separate hit group needed for visibility).
float RTShadow(float3 origin, float3 L, float maxD)
{
    RayDesc r; r.Origin = origin; r.Direction = L; r.TMin = 0.02; r.TMax = maxD;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}

// Honest metallic-roughness PBR — byte-for-byte the same math as the raster world.ps (Cook-Torrance direct + RT
// shadows + image-based ambient INCLUDING the specular env). `albedo` must be LINEAR (caller does sRGB->linear,
// like world.ps). The analytic env*Fr IS the specular reflection (exactly as the raster pass), so a surface seen
// in a reflection looks the same as its direct view. No in-shader tonemap (chit output is linear HDR).
static const float RTPI = 3.14159265359;
float3 RT_Fresnel(float cosT, float3 F0) { return F0 + (1.0 - F0) * pow(saturate(1.0 - cosT), 5.0); }
float RT_GGX(float3 N, float3 H, float rough)
{ float a = rough * rough, a2 = a * a; float ndh = max(dot(N, H), 0.0); float dd = ndh * ndh * (a2 - 1.0) + 1.0; return a2 / max(RTPI * dd * dd, 1e-5); }
float RT_GSch(float ndv, float k) { return ndv / (ndv * (1.0 - k) + k); }
float RT_GSm(float3 N, float3 V, float3 L, float rough)
{ float k = (rough + 1.0); k = k * k / 8.0; return RT_GSch(max(dot(N, V), 0.0), k) * RT_GSch(max(dot(N, L), 0.0), k); }

float3 ShadeSurface(float3 pos, float3 N, float3 V, float3 albedo, float metal, float rough, float3 emissive)
{
    metal = saturate(metal); rough = clamp(rough, 0.04, 1.0);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    float3 Lo = 0.0;
    int cnt = (int)g_LightCount.x;
    [loop] for (int li = 0; li < cnt; ++li)
    {
        Light lt = g_Lights[li]; float type = lt.posType.w;
        float3 L; float atten = 1.0; float lmax = 1e4;
        if (type < 0.5) L = normalize(-lt.dirRange.xyz);
        else
        {
            float3 d = lt.posType.xyz - pos; float dist = length(d); L = d / max(dist, 1e-4); lmax = dist;
            float rng = max(lt.dirRange.w, 1e-4); float win = saturate(1.0 - pow(dist / rng, 4.0));
            atten = (win * win) / (dist * dist + 1.0);
            if (type > 1.5) { float cd = dot(normalize(-lt.dirRange.xyz), -L); float s = saturate((cd - lt.spot.y) / max(lt.spot.x - lt.spot.y, 1e-4)); atten *= s * s; }
        }
        float ndl = max(dot(N, L), 0.0);
        if (ndl <= 0.0 || atten <= 0.0) continue;
        float3 H = normalize(V + L);
        float3 radiance = lt.colorIntensity.rgb * lt.colorIntensity.w * (atten * RTShadow(pos + L * 0.05 + N * 0.02, L, lmax));
        float  D = RT_GGX(N, H, rough); float G = RT_GSm(N, V, L, rough); float3 F = RT_Fresnel(max(dot(H, V), 0.0), F0);
        float3 spec = (D * G) * F / max(4.0 * max(dot(N, V), 0.0) * ndl, 1e-4);
        float3 kd = (1.0 - F) * (1.0 - metal);
        Lo += (kd * albedo / RTPI + spec) * radiance * ndl;
    }
    // DIFFUSE image-based ambient only. The SPECULAR reflection is added by the caller (chit) via a traced ray
    // (recursion) for sharp reflections / blurred env for rough — because this is a REFLECTION, it must show the
    // actual reflected scene, not just the analytic IBL the raster pass uses.
    float3 ambient;
    if (g_SkyParams.y > 0.5)
    {
        float  ndv = max(dot(N, V), 0.0);
        float3 irr = (g_ProbePos.w > 0.5) ? g_Probe.SampleLevel(g_Probe_sampler, N, g_ProbeParams.y).rgb * g_ProbeParams.x : SkyColor(N);
        float3 Fr  = F0 + (max(float3(1.0 - rough, 1.0 - rough, 1.0 - rough), F0) - F0) * pow(1.0 - ndv, 5.0);
        float3 kd  = (1.0 - Fr) * (1.0 - metal);
        ambient = kd * irr * albedo * g_Ambient.w;
    }
    else ambient = g_Ambient.rgb * g_Ambient.w * albedo;          // flat ambient (sky off)

    return ambient + Lo + emissive;
}

// Fresnel-roughness reflectance for weighting a reflection (NO ambient scaling — a reflection is real radiance).
float3 SpecFr(float3 N, float3 V, float rough, float3 albedo, float metal)
{
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
    float  ndv = max(dot(N, V), 0.0);
    return F0 + (max(float3(1.0 - rough, 1.0 - rough, 1.0 - rough), F0) - F0) * pow(1.0 - ndv, 5.0);
}
// Analytic reflection environment (blurred by roughness): probe, procedural sky, or flat (sky off).
float3 ReflEnv(float3 R, float rough)
{
    if (g_ProbePos.w > 0.5) return g_Probe.SampleLevel(g_Probe_sampler, R, saturate(rough) * g_ProbeParams.y).rgb * g_ProbeParams.x;
    if (g_SkyParams.y > 0.5)
    {
        float3 avg = (g_SkyTop.rgb + 2.0 * g_SkyHorizon.rgb + g_SkyGround.rgb) * 0.25 * g_SkyParams.x;
        return lerp(SkyColor(R), avg, rough);
    }
    return g_Ambient.rgb;
}
