// World (3D) lit pixel shader: metallic-roughness PBR, directional/point/spot lights, base-color + normal maps.
// MatCB packing: g_Params = (hasBaseTex, hasNormalTex, metallic, roughness); g_Params2 = (hasMR, hasAO, hasEmissive, specularFactor); g_Emissive2 = (rgb, intensity).
cbuffer MatCB { float4 g_Color; float4 g_Params; float4 g_Params2; float4 g_Emissive2; };

#define MAX_LIGHTS 256   // must match the renderer's FrameCB light array
struct Light { float4 posType; float4 dirRange; float4 colorIntensity; float4 spot; };
#define MAX_SHADOWS 4
// Per-camera lighting. g_Ambient = (rgb, intensity); g_ShadowParams = (slotCount, normalOffset, texelSize, depthBias).
// A light's shadow slot (or -1) lives in Light.spot.z (2D) / spot.w (cube).
cbuffer FrameCB
{
    float4 g_CamPos; float4 g_Ambient; float4 g_LightCount; Light g_Lights[MAX_LIGHTS];
    float4x4 g_ShadowVP[MAX_SHADOWS]; float4 g_ShadowParams;
    // g_SkyParams = (skyIntensity, hasSky, tonemapInShader, tonemapWhite).
    float4 g_SkyTop; float4 g_SkyHorizon; float4 g_SkyGround; float4 g_SkyParams;
    // g_ProbePos = (pos.xyz, active); g_ProbeParams = (intensity, maxMip, _, _); g_ProbeBox = (boxHalf.xyz, parallaxValid).
    float4 g_ProbePos; float4 g_ProbeParams; float4 g_ProbeBox;
    // g_Wind = (dir.xyz, strength m/s); g_Wind2 = (turbulence, 1/turbScale, time, gustFreq); read by vertex-bend shaders.
    float4 g_Wind; float4 g_Wind2;
};
TextureCube  g_Probe;          // scene-captured reflection cubemap (when g_ProbePos.w > 0.5)
SamplerState g_Probe_sampler;

// g_DrawFlags.x = receiveShadows (0 -> surface ignores all shadowing).
cbuffer DrawFlagsCB { float4 g_DrawFlags; };

#ifdef RT_ENABLED   // D3D12 + DXR: inline ray-traced shadows (RayQuery, SM6.5) instead of shadow maps
RaytracingAccelerationStructure g_TLAS;
// BYTE-MIRROR of the renderer's RTInstanceData (96 bytes); only shadowShape/shadowAlpha are read here,
// the rest is typed loosely to keep the stride. Change with NukeDiligentImpl.h + rt_common.hlsl.
struct RTInstInfo
{
    uint4  offs;          // nrmOffset, uvOffset, posOffset, matByteOffset
    uint4  texA;          // texIndex, nrmTexIndex, mrTexIndex, aoTexIndex
    uint4  texB;          // emTexIndex, specTexIndex, specularFactor(asfloat), nrmFlipG
    float4 albedoMetal;
    float4 emissiveRough;
    uint   colOffset; uint shadowShape; float shadowAlpha; uint pad0;
};
StructuredBuffer<RTInstInfo> g_RTInst;
// Ray-traced shadow toward a light: 1 = lit, 0 = occluded.
float RTShadow(float3 origin, float3 L, float maxDist)
{
    RayDesc ray; ray.Origin = origin; ray.Direction = L; ray.TMin = 0.02; ray.TMax = maxDist;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // 0x02 = shadow-caster bit of the TLAS instance mask (0x01 = visible in reflections).
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0x02, ray);
    // Non-opaque candidates are particle quads: shadowShape selects the footprint (0 quad / 1 disc / 2 strip
    // across u). UV is derived analytically from primitive parity + barycentrics; bindless maps are RT-only.
    while (q.Proceed())
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            RTInstInfo inst = g_RTInst[q.CandidateInstanceID()];
            float2 bc = q.CandidateTriangleBarycentrics();
            float  w0 = 1.0 - bc.x - bc.y;
            // Quad builder's vertex order: even tri = (0,1)(1,1)(1,0), odd tri = (0,1)(1,0)(0,0)
            float2 uv = (q.CandidatePrimitiveIndex() & 1)
                      ? float2(0.0, 1.0) * w0 + float2(1.0, 0.0) * bc.x
                      : float2(0.0, 1.0) * w0 + float2(1.0, 1.0) * bc.x + float2(1.0, 0.0) * bc.y;
            bool inside = (inst.shadowShape == 1u) ? (length(uv - 0.5) < 0.45)
                        : (inst.shadowShape == 2u) ? (abs(uv.x - 0.5) < 0.45)
                        : true;
            if (inside && inst.shadowAlpha >= 0.35) q.CommitNonOpaqueTriangleHit();
        }
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}
#endif

// Procedural sky colour for a world direction, used for image-based lighting. Must match sky.ps.
float3 SkyColor(float3 dir)
{
    float up = dir.y;
    float3 c = (up >= 0.0) ? lerp(g_SkyHorizon.rgb, g_SkyTop.rgb, pow(saturate(up), 0.5))
                           : lerp(g_SkyHorizon.rgb, g_SkyGround.rgb, saturate(-up));
    return c * g_SkyParams.x;
}
Texture2DArray          g_Shadow;
SamplerComparisonState  g_Shadow_sampler;

// Shadow factor (1 = lit, 0 = shadowed) for a light's 2D shadow-map slot, 3x3 PCF.
// ndl = saturate(N.L) of the receiver, used to slope-scale the depth bias.
float SampleShadow(float3 wpos, int slot, float ndl)
{
    if (slot < 0) return 1.0;
    float4 lp = mul(g_ShadowVP[slot], float4(wpos, 1.0));
    lp.xyz /= lp.w;
    float2 uv = lp.xy * float2(0.5, -0.5) + 0.5;   // NDC -> UV (Diligent: flip Y)
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || lp.z > 1.0) return 1.0;
    float depth = lp.z - g_ShadowParams.w * (1.0 + 1.5 * (1.0 - saturate(ndl)));
    float t = g_ShadowParams.z, s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        s += g_Shadow.SampleCmpLevelZero(g_Shadow_sampler, float3(uv + float2(x, y) * t, (float)slot), depth);
    return s / 9.0;
}

TextureCubeArray        g_ShadowCube;
SamplerComparisonState  g_ShadowCube_sampler;

// Point-light shadow: sample the cube by world->light direction, reconstructing the perspective
// zero-to-one depth the cube face stored from the major-axis distance.
float SamplePointShadow(float3 wpos, float3 lpos, int cube, float farZ)
{
    if (cube < 0) return 1.0;
    float3 dir = wpos - lpos;
    float3 ad  = abs(dir);
    float  z   = max(ad.x, max(ad.y, ad.z));
    float  n   = 0.1;
    float  ndc = (farZ / (farZ - n)) * (1.0 - n / max(z, 1e-4));
    ndc -= g_ShadowParams.w;
    return g_ShadowCube.SampleCmpLevelZero(g_ShadowCube_sampler, float4(dir, (float)cube), ndc);
}

// UseCombinedTextureSamplers is ON: each texture MUST be sampled through its own "<name>_sampler",
// one SamplerState per texture and one immutable sampler each in the PSO.
Texture2D    g_Tex;          SamplerState g_Tex_sampler;         // base color
Texture2D    g_Normal;       SamplerState g_Normal_sampler;      // tangent-space normal map
Texture2D    g_MetalRough;   SamplerState g_MetalRough_sampler;  // G = roughness, B = metallic (glTF)
Texture2D    g_Occlusion;    SamplerState g_Occlusion_sampler;   // R = ambient occlusion
Texture2D    g_Emissive;     SamplerState g_Emissive_sampler;    // emissive color
Texture2D    g_Spec;         SamplerState g_Spec_sampler;        // specular reflectance (KHR); white = 0.04 F0

// NUKE_INSTANCED opt-in: per-instance tint + custom float4 arrive as extra interpolants.
// This struct must mirror world.vs's instanced PSIn exactly.
#if NUKE_INSTANCED
struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2;
              float4 icol : TEXCOORD3; float4 icustom : TEXCOORD4; };
#else
struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2; };
#endif

static const float PI = 3.14159265359;

float3 FresnelSchlick(float cosT, float3 F0) { return F0 + (1.0 - F0) * pow(saturate(1.0 - cosT), 5.0); }
float DistributionGGX(float3 N, float3 H, float rough)
{
    float a = rough * rough; float a2 = a * a;
    float ndh = max(dot(N, H), 0.0);
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-5);
}
float GeometrySchlick(float ndv, float k) { return ndv / (ndv * (1.0 - k) + k); }
float GeometrySmith(float3 N, float3 V, float3 L, float rough)
{
    float k = (rough + 1.0); k = k * k / 8.0;
    return GeometrySchlick(max(dot(N, V), 0.0), k) * GeometrySchlick(max(dot(N, L), 0.0), k);
}
// Tangent-space normal mapping without mesh tangents: cotangent frame from screen-space derivatives.
// dp1/dp2 = ddx/ddy of WORLD POSITION, du1/du2 = ddx/ddy of uv. They must be taken by the caller where the
// interpolated input is in scope: ddx/ddy on a passed-in parameter miscompiles to zero on DXC.
float3 PerturbNormal(float3 N, float3 n, float3 dp1, float3 dp2, float2 du1, float2 du2)
{
    float3 dp2p = cross(dp2, N), dp1p = cross(N, dp1);
    float3 T = dp2p * du1.x + dp1p * du2.x;
    float3 B = dp2p * du1.y + dp1p * du2.y;
    float inv = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));   // floor is a div-by-zero guard only; 1e-8 flattens fine tangent frames
    return normalize(T * (inv * n.x) + B * (inv * n.y) + N * n.z);
}

float4 main(in PSIn i) : SV_Target
{
    float4 base = g_Color;
#if NUKE_INSTANCED
    base *= i.icol;   // per-instance tint
#endif
    if (g_Params.x > 0.5) base *= g_Tex.Sample(g_Tex_sampler, i.uv);
    float3 albedo = pow(max(base.rgb, 0.0), 2.2);   // sRGB -> linear

    float metallic = saturate(g_Params.z);
    float rough    = clamp(g_Params.w, 0.04, 1.0);
    if (g_Params2.x > 0.5)                                  // metallic-roughness map wins (glTF G/B)
    {
        float3 m = g_MetalRough.Sample(g_MetalRough_sampler, i.uv).rgb;
        rough = clamp(m.g, 0.04, 1.0); metallic = saturate(m.b);
    }

    float3 specF = g_Params2.w * g_Spec.Sample(g_Spec_sampler, i.uv).rgb;   // KHR specular: factor x spec map

    float3 V = normalize(g_CamPos.xyz - i.wpos);
    float3 N = normalize(i.nrm);
    // g_Params.y: 0 = no normal map; >0 = OpenGL green (+Y, flip); <0 = DirectX green (no flip).
    // RG only + reconstructed Z, so BC5 (which stores no Z) works.
    if (abs(g_Params.y) > 0.5)
    {
        float2 nxy = g_Normal.Sample(g_Normal_sampler, i.uv).rg * 2.0 - 1.0;
        if (g_Params.y > 0.0) nxy.y = -nxy.y;
        float3 nTS = float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
        N = PerturbNormal(N, nTS, ddx(i.wpos), ddy(i.wpos), ddx(i.uv), ddy(i.uv));
    }
    float3 swpos = i.wpos + N * g_ShadowParams.y;   // normal-offset bias: sample shadows slightly off the surface

    float3 F0 = lerp(0.04 * specF, albedo, metallic);   // dielectric F0 scaled by KHR specular; conductor uses albedo
    float3 Lo = 0.0;

    int cnt = (int)g_LightCount.x;
    [loop] for (int li = 0; li < cnt; ++li)
    {
        Light lt = g_Lights[li];
        float  type = lt.posType.w;
        float3 L; float atten = 1.0;
        if (type < 0.5)                       // directional
        {
            L = normalize(-lt.dirRange.xyz);
        }
        else                                  // point / spot
        {
            float3 d = lt.posType.xyz - i.wpos;
            float  dist = length(d);
            L = d / max(dist, 1e-4);
            float rng = max(lt.dirRange.w, 1e-4);
            float win = saturate(1.0 - pow(dist / rng, 4.0));
            atten = (win * win) / (dist * dist + 1.0);
            if (type > 1.5)                   // spot cone
            {
                float cd = dot(normalize(-lt.dirRange.xyz), -L);
                float s  = saturate((cd - lt.spot.y) / max(lt.spot.x - lt.spot.y, 1e-4));
                atten *= s * s;
            }
        }
        float ndl = max(dot(N, L), 0.0);
        if (ndl <= 0.0 || atten <= 1e-6) continue;
        float3 H = normalize(V + L);
        float3 radiance = lt.colorIntensity.rgb * lt.colorIntensity.w * atten;
        if (g_DrawFlags.x > 0.5)   // receiveShadows
        {
#ifdef RT_ENABLED
            bool casts = (type > 0.5 && type < 1.5) ? ((int)lt.spot.w >= 0) : ((int)lt.spot.z >= 0);
            if (casts)
            {
                float maxD = (type < 0.5) ? 1e4 : length(lt.posType.xyz - i.wpos);
                radiance *= RTShadow(swpos, L, maxD);
            }
#else
            if (type > 0.5 && type < 1.5) radiance *= SamplePointShadow(swpos, lt.posType.xyz, (int)lt.spot.w, lt.dirRange.w);
            else                          radiance *= SampleShadow(swpos, (int)lt.spot.z, ndl);   // dir/spot 2D slot
#endif
        }

        float  D = DistributionGGX(N, H, rough);
        float  G = GeometrySmith(N, V, L, rough);
        float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
        float3 spec = (D * G) * F / max(4.0 * max(dot(N, V), 0.0) * ndl, 1e-4);
        float3 kd = (1.0 - F) * (1.0 - metallic);
        Lo += (kd * albedo / PI + spec) * radiance * ndl;
    }

    float ao = (g_Params2.y > 0.5) ? g_Occlusion.Sample(g_Occlusion_sampler, i.uv).r : 1.0;
    float3 ambient;
    if (g_SkyParams.y > 0.5)   // image-based lighting from the procedural sky
    {
        float  ndv = max(dot(N, V), 0.0);
        float3 R   = reflect(-V, N);
        float3 avg = (g_SkyTop.rgb + 2.0 * g_SkyHorizon.rgb + g_SkyGround.rgb) * 0.25 * g_SkyParams.x;
        float3 irr, env;
        if (g_ProbePos.w > 0.5)   // reflection probe captured the actual scene -> sample it
        {
            float mm = g_ProbeParams.y;
            // Box-parallax correction: intersect R with the probe box and sample by probe-centre -> hit direction.
            float3 Rp = R;
            if (g_ProbeBox.w > 0.5)
            {
                float3 c = g_ProbePos.xyz;
                float3 invR = 1.0 / R;                       // inf on axis-aligned rays is fine: min/max picks the finite plane
                float3 t1 = (c + g_ProbeBox.xyz - i.wpos) * invR;
                float3 t2 = (c - g_ProbeBox.xyz - i.wpos) * invR;
                float3 tmax = max(t1, t2);
                float  t = min(min(tmax.x, tmax.y), tmax.z);
                Rp = (i.wpos + R * t) - c;
            }
            env = g_Probe.SampleLevel(g_Probe_sampler, Rp, saturate(rough) * mm).rgb * g_ProbeParams.x;
            irr = g_Probe.SampleLevel(g_Probe_sampler, N, mm).rgb * g_ProbeParams.x;
        }
        else
        {
            irr = SkyColor(N);
            env = lerp(SkyColor(R), avg, rough);
        }
        float3 Fr  = F0 + (max(float3(1.0 - rough, 1.0 - rough, 1.0 - rough), F0) - F0) * pow(1.0 - ndv, 5.0);
        float3 kd  = (1.0 - Fr) * (1.0 - metallic);
        ambient = (kd * irr * albedo + env * Fr) * g_Ambient.w * ao;
    }
    else
        ambient = g_Ambient.rgb * g_Ambient.w * albedo * ao;                   // flat ambient (no sky)
    float3 emissive = g_Emissive2.rgb * g_Emissive2.w;
    if (g_Params2.z > 0.5) emissive *= g_Emissive.Sample(g_Emissive_sampler, i.uv).rgb;
    float3 color = ambient + Lo + emissive;

    // g_SkyParams.z == 0: emit linear HDR and let the post pass tonemap; == 1: tonemap here.
    if (g_SkyParams.z > 0.5)
    {
        float W = (g_SkyParams.w > 1e-3) ? g_SkyParams.w : 1.0;   // tonemap white point
        color = color * (1.0 + color / (W * W)) / (1.0 + color);  // extended Reinhard
        color = pow(max(color, 0.0), 1.0 / 2.2);                  // linear -> sRGB
    }
    return float4(color, base.a);
}
