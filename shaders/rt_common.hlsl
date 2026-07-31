// Shared RT reflection code: payload, bindless geometry/material fetch, environment and the PBR model.
// The renderer CONCATENATES this ahead of rt_rgen / rt_rmiss / rt_rchit (it is not #included).

struct RTPayload { float3 color; uint depth; };   // color = reflected radiance; depth = current recursion depth

// TLAS instance-mask bit for "visible in reflections". Default instance Mask = 0xFF;
// excluded = 0xFF & ~RT_REFLECT_BIT. Both color and shadow rays inside a reflection use this mask.
#define RT_REFLECT_BIT  0x01
#define RT_REFLECT_MASK 0x01

RaytracingAccelerationStructure g_TLAS;
RWTexture2D<float4>             g_Output;          // ray-gen writes the final composited reflection here
Texture2D                      g_GBuffer;         // (octN.xy world normal, roughness, metalness)
Texture2D                      g_Depth;           // device depth (gbuffer prepass)
Texture2D                      g_Source;          // current HDR chain colour (base, for compositing)
TextureCube                    g_Probe;           SamplerState g_Probe_sampler;
ByteAddressBuffer              g_AllNrm;          // concatenated mesh normals (float3/vertex)
ByteAddressBuffer              g_AllUV;           // concatenated mesh uvs (float2/vertex)
ByteAddressBuffer              g_AllPos;          // concatenated mesh positions (float3/vertex), for the analytic TBN
Texture2D                      g_MatTex[256];     SamplerState g_MatTex_sampler;   // bindless material maps (all types)
// Mirrors the C++ Impl::RTInstanceData byte-for-byte (16-byte aligned rows).
// Bindless map indices use 0xFFFFFFFF for "no map".
struct RTInstanceData
{
    uint  nrmOffset, uvOffset, posOffset, matByteOffset;   // byte offsets into g_AllNrm/g_AllUV/g_AllPos
    uint  texIndex, nrmTexIndex, mrTexIndex, aoTexIndex;    // bindless slots: albedo, normal, metal-rough, occlusion
    uint  emTexIndex, specTexIndex; float specularFactor; uint nrmFlipG;   // nrmFlipG: 1 = flip green (OpenGL)
    float4 albedoMetal;     // rgb albedo factor, a = metallic factor
    float4 emissiveRough;   // rgb emissive (pre-multiplied), a = roughness factor
    // colOffset = byte offset into g_DynCol (0xFFFFFFFF = none); shadowShape = 0 quad / 1 disc / 2 strip.
    uint  colOffset; uint shadowShape; float shadowAlpha; uint pad0;
};
StructuredBuffer<RTInstanceData> g_Instances;
ByteAddressBuffer g_MatBytes;   // per-instance MatCB block, same packing as the raster MatCB
ByteAddressBuffer g_DynCol;     // per-frame particle colors (float4/vertex)

// Surface contract: a shader's Surface(IN, O) fills O; the generated closest-hit lights and recurses it,
// or outputs O.emissive when O.unlit. The raster path calls the same Surface() from its own harness.
struct SurfaceIn  { float3 worldPos; float3 worldNormal; float2 uv; float3 viewDir; };
struct SurfaceOut { float3 albedo; float metallic; float roughness; float3 emissive; float alpha; bool unlit; };

cbuffer RTRefCB { float4x4 g_InvProj; float4x4 g_InvView; float4 g_RTCam; float4 g_RTParams; float4 g_RTWater; float4 g_RTWaterCol; float4 g_RTWaterAbs; };  // g_RTParams = (intensity, maxDist, maxDepth, roughCut); g_RTWater = (level, on, fade, _); g_RTWaterAbs = (absorb.rgb, 1/opacityDepth)

#define MAX_LIGHTS 256   // must match world.ps and the renderer's FrameCB
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
// --- Water along a ray. The TLAS carries no water surface, so it is approximated by the flat rest plane.
// Submerged length of the segment [a -> b].
float RTWaterUnder(float3 a, float3 b)
{
    if (g_RTWater.y < 0.5) return 0.0;
    float da = g_RTWater.x - a.y, db = g_RTWater.x - b.y;   // > 0 = below the surface
    float len = length(b - a);
    if (da > 0.0 && db > 0.0)      return len;
    if (da > 0.0 || db > 0.0)      return len * max(da, db) / (abs(da) + abs(db) + 1e-6);
    return 0.0;
}
// Scalar transmittance, used as the reflection weight in ray-gen.
float RTWaterTrans(float3 a, float3 b)
{
    float under = RTWaterUnder(a, b);
    return (under <= 0.0) ? 1.0 : exp(-g_RTWater.z * under);
}
// Per-channel Beer-Lambert transmittance for radiance carried along a ray (matches the raster surface).
float3 RTWaterTrans3(float3 a, float3 b)
{
    float under = RTWaterUnder(a, b);
    if (under <= 0.0) return float3(1.0, 1.0, 1.0);
    return exp(-g_RTWaterAbs.rgb * (under * 6.0 * g_RTWaterAbs.w));
}
// Same, for a miss ray: the submerged run before it exits upward, else the whole span.
float3 RTWaterTransRay(float3 o, float3 d, float tMax)
{
    if (g_RTWater.y < 0.5) return float3(1.0, 1.0, 1.0);
    float below = g_RTWater.x - o.y;
    if (below <= 0.0) return float3(1.0, 1.0, 1.0);  // starts above: an escaping ray stays above
    float t = (d.y > 1e-4) ? min((g_RTWater.x - o.y) / d.y, tMax) : tMax;
    return exp(-g_RTWaterAbs.rgb * (max(t, 0.0) * 6.0 * g_RTWaterAbs.w));
}

float3 EnvSample(float3 dir, float rough)   // probe (parallax-free), analytic sky, or flat ambient when sky is off
{
    if (g_ProbePos.w > 0.5) return g_Probe.SampleLevel(g_Probe_sampler, dir, saturate(rough) * g_ProbeParams.y).rgb * g_ProbeParams.x;
    if (g_SkyParams.y > 0.5) return SkyColor(dir);
    return g_Ambient.rgb;
}

// What a ray sees of the water surface it crossed: sky mirrored about the horizontal plane,
// Fresnel-mixed with the body's scatter colour.
float3 RTWaterLook(float3 d)
{
    float3 sky = EnvSample(float3(d.x, -d.y, d.z), 0.15);
    float  f   = 0.02 + 0.98 * pow(1.0 - saturate(abs(d.y)), 5.0);   // grazing = mirror, steep = body colour
    float3 amb = (g_SkyParams.y > 0.5)
               ? (g_SkyTop.rgb + 2.0 * g_SkyHorizon.rgb + g_SkyGround.rgb) * 0.25 * g_SkyParams.x * g_Ambient.w
               : g_Ambient.rgb * g_Ambient.w;
    // In-scatter is lit by ambient AND the brightest directional, matching water.ps.
    float3 sunC = float3(0.0, 0.0, 0.0);
    float best = -1.0;
    [loop] for (int li = 0; li < (int)g_LightCount.x; ++li)
        if (g_Lights[li].posType.w < 0.5)
        {
            float3 c = g_Lights[li].colorIntensity.rgb * g_Lights[li].colorIntensity.w;
            float lum = dot(c, float3(0.299, 0.587, 0.114));
            if (lum > best) { best = lum; sunC = c; }
        }
    float3 volLight = min(amb * 0.8 + sunC * 0.35, 1.8);
    return lerp(g_RTWaterCol.rgb * volLight * 0.8, sky, f);
}

// --- Bindless geometry/material fetch at a triangle hit (instance + primitive + barycentrics)
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
// Per-vertex particle color for dynamic sprite meshes; white for everything else.
float4 FetchDynColor(RTInstanceData inst, uint prim, float2 bc)
{
    if (inst.colOffset == 0xFFFFFFFFu) return float4(1.0, 1.0, 1.0, 1.0);
    float w0 = 1.0 - bc.x - bc.y; uint cb = inst.colOffset + prim * 48u;   // 3 verts * 16 bytes (float4)
    return asfloat(g_DynCol.Load4(cb)) * w0 + asfloat(g_DynCol.Load4(cb + 16u)) * bc.x + asfloat(g_DynCol.Load4(cb + 32u)) * bc.y;
}
float3 SampleAlbedo(RTInstanceData inst, float2 uv)
{
    float3 a = inst.albedoMetal.rgb;
    if (inst.texIndex != 0xFFFFFFFFu) a *= g_MatTex[NonUniformResourceIndex(inst.texIndex)].SampleLevel(g_MatTex_sampler, uv, 0).rgb;
    return a;
}
// Generic bindless map fetch; returns `dflt` when the instance has no such map. LOD 0 (no ray cones).
float4 SampleMap(uint idx, float2 uv, float4 dflt)
{
    if (idx == 0xFFFFFFFFu) return dflt;
    return g_MatTex[NonUniformResourceIndex(idx)].SampleLevel(g_MatTex_sampler, uv, 0);
}
// glTF metallic-roughness map (G = roughness, B = metalness); overrides the scalar factors when present.
void SampleMR(RTInstanceData inst, float2 uv, inout float metal, inout float rough)
{
    if (inst.mrTexIndex == 0xFFFFFFFFu) return;
    float3 m = g_MatTex[NonUniformResourceIndex(inst.mrTexIndex)].SampleLevel(g_MatTex_sampler, uv, 0).rgb;
    rough = m.g; metal = m.b;
}
float  SampleAO(RTInstanceData inst, float2 uv)        { return SampleMap(inst.aoTexIndex,   uv, float4(1,1,1,1)).r; }
float3 SampleEmissiveMap(RTInstanceData inst, float2 uv){ return SampleMap(inst.emTexIndex,   uv, float4(1,1,1,1)).rgb; }
// KHR_materials_specular multiplier that scales dielectric F0.
float3 SampleSpec(RTInstanceData inst, float2 uv)      { return inst.specularFactor * SampleMap(inst.specTexIndex, uv, float4(1,1,1,1)).rgb; }

// Tangent-space to world normal for a normal-mapped hit; builds the TBN from the hit triangle's
// object-space positions + UVs, so no per-vertex tangents are needed. `geomN` = interpolated world normal.
float3 ApplyNormalMap(RTInstanceData inst, uint prim, float2 uv, float3 geomN, float3x4 o2w)
{
    if (inst.nrmTexIndex == 0xFFFFFFFFu) return geomN;
    uint pb = inst.posOffset + prim * 36u;                 // 3 verts * 12 bytes
    float3 p0 = asfloat(g_AllPos.Load3(pb)), p1 = asfloat(g_AllPos.Load3(pb + 12u)), p2 = asfloat(g_AllPos.Load3(pb + 24u));
    uint ub = inst.uvOffset + prim * 24u;                  // 3 verts * 8 bytes
    float2 u0 = asfloat(g_AllUV.Load2(ub)), u1 = asfloat(g_AllUV.Load2(ub + 8u)), u2 = asfloat(g_AllUV.Load2(ub + 16u));
    float3 e1 = p1 - p0, e2 = p2 - p0; float2 d1 = u1 - u0, d2 = u2 - u0;
    float det = d1.x * d2.y - d2.x * d1.y;
    if (abs(det) < 1e-12) return geomN;                    // degenerate UVs -> geometric normal
    float3 Tobj = (d2.y * e1 - d1.y * e2) / det;
    float3 T = normalize(mul((float3x3)o2w, Tobj));
    float3 N = normalize(geomN);
    T = normalize(T - N * dot(N, T));                      // Gram-Schmidt orthonormalize
    float3 B = cross(N, T);
    float2 nxy = g_MatTex[NonUniformResourceIndex(inst.nrmTexIndex)].SampleLevel(g_MatTex_sampler, uv, 0).rg * 2.0 - 1.0;
    if (inst.nrmFlipG != 0u) nxy.y = -nxy.y;   // OpenGL green convention; RG + reconstructed Z works for BC5
    float nz = sqrt(saturate(1.0 - dot(nxy, nxy)));
    return normalize(nxy.x * T + nxy.y * B + nz * N);
}

// Shadow ray inside a reflection: 1 = lit, 0 = occluded.
float RTShadow(float3 origin, float3 L, float maxD)
{
    RayDesc r; r.Origin = origin; r.Direction = L; r.TMin = 0.02; r.TMax = maxD;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // Reflect mask, not the 0x02 caster bit world.ps uses: mask bits are OR-tested, so
    // "reflect-visible AND casts" cannot be expressed in one trace.
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, RT_REFLECT_MASK, r);
    // Non-opaque candidates (particle quads) get an albedo-alpha test.
    while (q.Proceed())
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            RTInstanceData inst = g_Instances[q.CandidateInstanceID()];
            uint   prim = q.CandidatePrimitiveIndex();
            float2 bc   = q.CandidateTriangleBarycentrics();
            float  a    = FetchDynColor(inst, prim, bc).a;
            if (inst.texIndex != 0xFFFFFFFFu)
                a *= g_MatTex[NonUniformResourceIndex(inst.texIndex)].SampleLevel(g_MatTex_sampler,
                         FetchUV(inst.uvOffset, prim, bc), 0).a;
            if (a >= 0.35) q.CommitNonOpaqueTriangleHit();
        }
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}

// Metallic-roughness PBR, same math as the raster world.ps. `albedo` must be LINEAR (caller does sRGB->linear).
// Output is linear HDR: no in-shader tonemap.
static const float RTPI = 3.14159265359;
float3 RT_Fresnel(float cosT, float3 F0) { return F0 + (1.0 - F0) * pow(saturate(1.0 - cosT), 5.0); }
float RT_GGX(float3 N, float3 H, float rough)
{ float a = rough * rough, a2 = a * a; float ndh = max(dot(N, H), 0.0); float dd = ndh * ndh * (a2 - 1.0) + 1.0; return a2 / max(RTPI * dd * dd, 1e-5); }
float RT_GSch(float ndv, float k) { return ndv / (ndv * (1.0 - k) + k); }
float RT_GSm(float3 N, float3 V, float3 L, float rough)
{ float k = (rough + 1.0); k = k * k / 8.0; return RT_GSch(max(dot(N, V), 0.0), k) * RT_GSch(max(dot(N, L), 0.0), k); }

float3 ShadeSurface(float3 pos, float3 N, float3 V, float3 albedo, float metal, float rough, float3 emissive, float ao, float3 spec)
{
    metal = saturate(metal); rough = clamp(rough, 0.04, 1.0);
    float3 F0 = lerp(0.04 * spec, albedo, metal);
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
        // Only lights with a shadow slot (>= 0) trace a ray; per-particle VFX lights are shadowless.
        float sh = 1.0;
        bool casts = (type > 0.5 && type < 1.5) ? ((int)lt.spot.w >= 0) : ((int)lt.spot.z >= 0);
        if (casts) sh = RTShadow(pos + L * 0.05 + N * 0.02, L, lmax);
        float3 radiance = lt.colorIntensity.rgb * lt.colorIntensity.w * (atten * sh);
        float  D = RT_GGX(N, H, rough); float G = RT_GSm(N, V, L, rough); float3 F = RT_Fresnel(max(dot(H, V), 0.0), F0);
        float3 spec = (D * G) * F / max(4.0 * max(dot(N, V), 0.0) * ndl, 1e-4);
        float3 kd = (1.0 - F) * (1.0 - metal);
        Lo += (kd * albedo / RTPI + spec) * radiance * ndl;
    }
    // Diffuse image-based ambient only: the specular term is added by the caller (chit) from a traced ray.
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

    return ambient * ao + Lo + emissive;                          // occlusion attenuates ambient only, as in world.ps
}

// Fresnel-roughness reflectance used to weight a reflection (no ambient scaling).
float3 SpecFr(float3 N, float3 V, float rough, float3 albedo, float metal, float3 spec)
{
    float3 F0 = lerp(0.04 * spec, albedo, metal);
    float  ndv = max(dot(N, V), 0.0);
    return F0 + (max(float3(1.0 - rough, 1.0 - rough, 1.0 - rough), F0) - F0) * pow(1.0 - ndv, 5.0);
}
// Analytic reflection environment blurred by roughness: probe, procedural sky, or flat ambient.
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
