// World (3D) lit pixel shader: metallic-roughness PBR, directional/point/spot lights, base-color + normal maps.
// MatCB packing: g_Params = (hasBaseTex, hasNormalTex, metallic, roughness); g_Params2 = (hasMR, hasAO, hasEmissive, specularFactor); g_Emissive2 = (rgb, intensity).
// LiveMaterial: g_UVT = (uvTiling.xy, uvOffset.xy + tween scroll; 0,0 tiling = identity);
// g_UVT2 = (uvRotation rad, alphaCutoff 0=off, wipeThreshold 0=off, wipeFeather);
// g_Disp = (POM depth uv-space 0=off, tess displacement m, mid level, reserved).
// Overlay slots (states + static layers): g_Ov = (value, threshold, feather, topOnly);
// g_OvT = tint rgba; g_OvP = (metallic target -1=keep, roughness target -1=keep, mask3D channel
// -1=none, flags 1=albedo 2=normal 4=MR 8=mask2D 16=flipG); g_OvM0..2 = world->mask uvw rows;
// g_OvMQ = (mask resolution, hasMask3D, 0, 0).
cbuffer MatCB {
#include "matcb_std.hlsli"
};
#include "nuke_material.hlsli"
// BRDF pack: g_Brdf1 = (clearCoat, coatRoughness, anisotropy, sheen);
// g_Brdf2 = (translucency, ior, iridescence, iridescenceThickness);
// g_Brdf3 = (sheen tint rgb, hasFlowMap); g_Brdf4 = (translucency tint rgb, refraction on/off).
// g_Det = (detail tiling, strength 0=off, flags 1=albedo 2=normal 4=flipG, 0);
// g_Var = (anti-tiling amount, cell scale in repeats, hue variation, flags 1=triplanar 2=vcolTint 4=vcolMask).

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
    float4 g_Misc;   // x = DDGI probe capture in progress (alpha = distance / y), y = max ray distance
};
TextureCube  g_Probe;          // scene-captured reflection cubemap (when g_ProbePos.w > 0.5)
SamplerState g_Probe_sampler;

// g_DrawFlags.x = receiveShadows (0 -> surface ignores all shadowing).
cbuffer DrawFlagsCB { float4 g_DrawFlags; };
#include "ddgi.hlsli"
// Dynamic GI probe volumes (World::Render pushes them; count 0 = none).
cbuffer GICB { GIVolumeGPU g_GIVol[DDGI_MAX_VOLUMES]; int4 g_GICount; float4 g_GIAtlasInv; };   // AtlasInv: xy irradiance 1/size, zw visibility 1/size
Texture2D    g_GIIrr;        SamplerState g_GIIrr_sampler;   // irradiance atlas (linear, clamp)
Texture2D    g_GIVis;                                        // visibility atlas (distance, distance^2), same sampler
Texture2D    g_ScreenGI;     // screen-space bounce (E / pi), full-res, Load by pixel (black-ish white when off: guarded like g_ScreenAO)

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
Texture2D    g_WipeMask;     SamplerState g_WipeMask_sampler;    // luma-wipe mask (white = last to dissolve)
Texture2D    g_Height;       SamplerState g_Height_sampler;      // R = height (POM + displacement)

// Overlay slot maps (8 slots x albedo/normal/MR/mask2D). ONE shared sampler (g_Ov0Alb_sampler)
// serves the whole block — D3D11 caps samplers at 16 per stage; the 3D-mask coordinates are
// clamped manually so wrap never engages.
#define OV_DECL(N) Texture2D g_Ov##N##Alb; Texture2D g_Ov##N##Nrm; Texture2D g_Ov##N##MR; Texture2D g_Ov##N##Mask;
Texture2D    g_Ov0Alb;       SamplerState g_Ov0Alb_sampler;
Texture2D    g_Ov0Nrm; Texture2D g_Ov0MR; Texture2D g_Ov0Mask;
OV_DECL(1) OV_DECL(2) OV_DECL(3) OV_DECL(4) OV_DECL(5) OV_DECL(6) OV_DECL(7)
Texture2D    g_Mask3D;       // painted SurfaceMask flipbook: width = res*res (Z slabs), height = res
Texture2D    g_Detail;       // high-frequency detail albedo (gray = neutral) - shares g_Ov0Alb_sampler
Texture2D    g_DetailNrm;    // high-frequency detail normal
Texture2D    g_Flow;         // RG = anisotropy tangent direction (0.5,0.5 = neutral)
Texture2D    g_ScreenAO;     // screen-space AO visibility, full-res, Load by pixel (1 = open; white when off)
Texture2D    g_SceneRefr;    // pre-transparent scene snapshot (1x1 white when absent)

// Anisotropic GGX: the highlight stretches along the tangent (at) vs the bitangent (ab).
float D_Aniso(float3 N, float3 H, float3 T, float3 B, float rough, float aniso)
{
    float at = max(rough * (1.0 + aniso), 0.02);
    float ab = max(rough * (1.0 - aniso), 0.02);
    float th = dot(T, H), bh = dot(B, H), nh = max(dot(N, H), 0.0);
    float d = th * th / (at * at) + bh * bh / (ab * ab) + nh * nh;
    return 1.0 / max(3.14159265 * at * ab * d * d, 1e-5);
}

// The material UV transform, shared by every projection plane (0,0 tiling = identity).
float2 ApplyUVT(float2 uv)
{
    float2 tl = (abs(g_UVT.x) + abs(g_UVT.y) < 1e-6) ? float2(1.0, 1.0) : g_UVT.xy;
    uv = uv * tl + g_UVT.zw;
    if (abs(g_UVT2.x) > 1e-6)
    {
        float sr, cr; sincos(g_UVT2.x, sr, cr);
        uv = float2(uv.x * cr - uv.y * sr, uv.x * sr + uv.y * cr);
    }
    return uv;
}

// Cheap 2D hash for the anti-tiling cells.
float3 Hash3(float2 c)
{
    float3 p3 = frac(float3(c.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.xxy + p3.yzz) * p3.zyx);
}

// Anti-tiling albedo sample: two half-cell-offset grids, each cell reads the texture at a
// hashed offset (scaled by amount); blended by distance to the owning cell's centre.
float4 SampleAntiTile(Texture2D t, SamplerState smp, float2 uv)
{
    float amount = g_Var.x;
    if (amount <= 0.0) return t.Sample(smp, uv);
    float scale = max(g_Var.y, 1e-3);
    float2 cf = uv / scale;
    float2 cA = floor(cf), cB = floor(cf + 0.5);
    float2 offA = (Hash3(cA).xy - 0.5) * amount;
    float2 offB = (Hash3(cB * 1.37 + 17.0).xy - 0.5) * amount;
    float2 fA = abs(frac(cf) - 0.5), fB = abs(frac(cf + 0.5) - 0.5);
    float wA = saturate(1.0 - 2.0 * max(fA.x, fA.y));
    float wB = saturate(1.0 - 2.0 * max(fB.x, fB.y));
    float w = wA / max(wA + wB, 1e-4);
    return lerp(t.Sample(smp, uv + offB), t.Sample(smp, uv + offA), w);
}

// Painted-mask channel at a world position; 0 outside the mask box. Manual trilinear Z: the
// flipbook stores slab z at x-offset z*res, matching the CPU grid layout.
float OvMask3D(float3 wpos, float chan)
{
    float4 hp = float4(wpos, 1.0);
    float3 c = float3(dot(g_OvM0, hp), dot(g_OvM1, hp), dot(g_OvM2, hp));
    if (any(c < 0.0) || any(c > 1.0)) return 0.0;
    float res  = g_OvMQ.x;
    float3 cell = c * res;
    float sx = clamp(cell.x, 0.5, res - 0.5);
    float sv = clamp(cell.y, 0.5, res - 0.5) / res;
    float fz = clamp(cell.z - 0.5, 0.0, res - 1.001);
    float z0 = floor(fz), tz = fz - z0;
    float u0 = (z0 * res + sx) / (res * res);
    float u1 = (min(z0 + 1.0, res - 1.0) * res + sx) / (res * res);
    float4 a = g_Mask3D.SampleLevel(g_Ov0Alb_sampler, float2(u0, sv), 0);
    float4 b = g_Mask3D.SampleLevel(g_Ov0Alb_sampler, float2(u1, sv), 0);
    float4 s = lerp(a, b, tz);
    return (chan < 0.5) ? s.r : (chan < 1.5) ? s.g : (chan < 2.5) ? s.b : s.a;
}

// Blend weight of one overlay slot: uniform value lifted by the painted mask, modulated by the
// slot's 2D mask map, thresholded with a feathered edge, optionally settled on up-facing only.
float OvWeight(float4 ov, float4 ovp, uint flags, float mask2d, float3 wpos, float3 ng)
{
    float v = ov.x;
    if (g_OvMQ.y > 0.5 && ovp.z >= 0.0) v = max(v, OvMask3D(wpos, ovp.z));
    if (flags & 8u) v *= mask2d;
    float w = smoothstep(ov.y, ov.y + max(ov.z, 1e-3), v);
    if (ov.w > 0.0) { float up = saturate(ng.y); w *= lerp(1.0, up * up, ov.w); }
    return w;
}

// NUKE_INSTANCED opt-in: per-instance tint + custom float4 arrive as extra interpolants.
// This struct must mirror world.vs's instanced PSIn exactly.
#if NUKE_INSTANCED
struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2;
              float4 icol : TEXCOORD3; float4 icustom : TEXCOORD4; };
#elif NUKE_VCTINT
struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2;
              float4 vcol : TEXCOORD3; };
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

float4 main(in PSIn i, bool isFront : SV_IsFrontFace) : SV_Target
{
    // Triplanar projection (world position, UV Tiling = repeats per meter): the base albedo
    // blends across the three planes; everything else follows the DOMINANT plane's uv so all
    // downstream sampling (POM, wipe, overlays, detail) stays consistent.
    const uint varF = (uint)(g_Var.w + 0.5);
    float3 triW = float3(0.0, 0.0, 0.0);
    float2 triUVx = float2(0.0, 0.0), triUVy = float2(0.0, 0.0), triUVz = float2(0.0, 0.0);
    [branch] if (varF & 1u)
    {
        float3 an = abs(normalize(i.nrm));
        triW = pow(an, 8.0); triW /= (triW.x + triW.y + triW.z);
        triUVx = ApplyUVT(i.wpos.zy); triUVy = ApplyUVT(i.wpos.xz); triUVz = ApplyUVT(i.wpos.xy);
        i.uv = (an.x >= an.y && an.x >= an.z) ? triUVx : (an.y >= an.z ? triUVy : triUVz);
    }
    else
        // LiveMaterial UV transform: applied once, every map below samples the transformed uv.
        i.uv = ApplyUVT(i.uv);

    // Parallax occlusion mapping: march the height field along the tangent-space view ray and
    // shift the uv to the intersection. Tangent frame from screen-space derivatives (the same
    // cotangent trick normal mapping uses), so no mesh tangents are needed.
    float pomD = g_Disp.x;
    [branch] if (g_DispT.w > 0.5)   // masked POM depth tween
        pomD = lerp(pomD, g_DispT.x, NukeMaskW((int)(g_DispT.w - 0.5), i.uv, i.wpos, g_MskStamp, g_Ov0Alb_sampler));
    if (pomD > 0.0)
    {
        float3 Ng = normalize(i.nrm);
        float3 dp1 = ddx(i.wpos), dp2 = ddy(i.wpos);
        float2 du1 = ddx(i.uv),  du2 = ddy(i.uv);
        float3 dp2p = cross(dp2, Ng), dp1p = cross(Ng, dp1);
        float3 T = dp2p * du1.x + dp1p * du2.x;
        float3 B = dp2p * du1.y + dp1p * du2.y;
        float inv = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
        T *= inv; B *= inv;
        float3 Vw = normalize(g_CamPos.xyz - i.wpos);
        float3 Vt = float3(dot(Vw, T), dot(Vw, B), dot(Vw, Ng));
        const int kSteps = 16;
        float2 duv  = (Vt.xy / max(Vt.z, 0.2)) * (pomD / kSteps);
        float  stp  = 1.0 / kSteps;
        float2 uvp  = i.uv;
        float  cur  = 0.0;
        float  h    = 1.0 - g_Height.SampleLevel(g_Height_sampler, uvp, 0).r;   // depth below the surface
        [loop] for (int s = 0; s < kSteps; ++s)
        {
            if (cur >= h) break;
            uvp -= duv; cur += stp;
            h = 1.0 - g_Height.SampleLevel(g_Height_sampler, uvp, 0).r;
        }
        // One secant refinement between the last two samples smooths the stair-stepping.
        {
            float2 uvPrev = uvp + duv;
            float  hPrev  = 1.0 - g_Height.SampleLevel(g_Height_sampler, uvPrev, 0).r;
            float  aPrev  = (cur - stp) - hPrev, aCur = cur - h;
            float  w = saturate(aCur / max(aCur - aPrev, 1e-5));
            uvp = lerp(uvp, uvPrev, w);
        }
        i.uv = uvp;
    }

    // C5 eye: the iris sinks under the cornea by view parallax (one tap at iris depth) —
    // the eye reads as a sphere with interior depth instead of a painted ball.
    [branch] if (g_Sss.y > 0.0)
    {
        float3 Ng = normalize(i.nrm);
        float3 dp1 = ddx(i.wpos), dp2 = ddy(i.wpos);
        float2 du1 = ddx(i.uv),  du2 = ddy(i.uv);
        float3 dp2p = cross(dp2, Ng), dp1p = cross(Ng, dp1);
        float3 T = dp2p * du1.x + dp1p * du2.x;
        float3 B = dp2p * du1.y + dp1p * du2.y;
        float inv = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
        float3 Vw = normalize(g_CamPos.xyz - i.wpos);
        float3 Vt = float3(dot(Vw, T * inv), dot(Vw, B * inv), dot(Vw, Ng));
        i.uv -= Vt.xy / max(Vt.z, 0.35) * g_Sss.y;
    }

    // Overlay slot weights (uv after POM so the overlays sit on the parallaxed surface; the
    // geometric normal drives topOnly — snow settles by geometry, not by the normal map).
    float3 ovNg = normalize(i.nrm);
    uint  ovF[8];
    float ovW[8];
#define OV_WEIGHT(N) \
    ovF[N] = (uint)(g_OvP##N.w + 0.5); ovW[N] = 0.0; \
    [branch] if (g_Ov##N.x > 0.0 || g_OvP##N.z >= 0.0) \
        ovW[N] = OvWeight(g_Ov##N, g_OvP##N, ovF[N], (ovF[N] & 8u) ? g_Ov##N##Mask.Sample(g_Ov0Alb_sampler, i.uv).r : 1.0, i.wpos, ovNg);
    OV_WEIGHT(0) OV_WEIGHT(1) OV_WEIGHT(2) OV_WEIGHT(3)
    OV_WEIGHT(4) OV_WEIGHT(5) OV_WEIGHT(6) OV_WEIGHT(7)
    // Slots COMBINE: oversubscribed weights share the pixel proportionally instead of the
    // last slot erasing the earlier ones (wet 1 + snow 1 -> a 50/50 mix, not pure snow).
    {
        float ovTot = ovW[0] + ovW[1] + ovW[2] + ovW[3] + ovW[4] + ovW[5] + ovW[6] + ovW[7];
        if (ovTot > 1.0)
        {
            float ovK = 1.0 / ovTot;
            [unroll] for (int on = 0; on < 8; ++on) ovW[on] *= ovK;
        }
    }
#if NUKE_VCTINT
    // Vertex Color = Overlay Mask: R/G/B/A painted in the DCC drive overlay slots 0-3.
    if ((uint)(g_Var.w + 0.5) & 4u)
    { ovW[0] *= i.vcol.r; ovW[1] *= i.vcol.g; ovW[2] *= i.vcol.b; ovW[3] *= i.vcol.a; }
#endif

    // Luma wipe: pixels whose mask luma falls below the animated threshold dissolve; the band
    // just above the edge glows (burn) within the feather width.
    // Wipe threshold, per-pixel maskable (g_ParamsT.z twin): a dissolve can spread from a point.
    float wipeTh = g_UVT2.z;
    [branch] if (g_ParamsT.w > 0.5)
        wipeTh = lerp(wipeTh, g_ParamsT.z, NukeMaskW((int)(g_ParamsT.w - 0.5), i.uv, i.wpos, g_MskStamp, g_Ov0Alb_sampler));
    float wipeL = 1.0;
    if (wipeTh > 0.0)
    {
        wipeL = g_WipeMask.Sample(g_WipeMask_sampler, i.uv).r;
        clip(wipeL - wipeTh);
    }

    float4 base = g_Color;
    [branch] if (g_ColorT.w > 0.5)   // masked base-color tween: blend by the mask, per pixel
        base.rgb = lerp(base.rgb, g_ColorT.rgb, NukeMaskW((int)(g_ColorT.w - 0.5), i.uv, i.wpos, g_MskStamp, g_Ov0Alb_sampler));
#if NUKE_INSTANCED
    base *= i.icol;   // per-instance tint
#endif
    if (g_Params.x > 0.5)
    {
        [branch] if (varF & 1u)
            base *= g_Tex.Sample(g_Tex_sampler, triUVx) * triW.x
                  + g_Tex.Sample(g_Tex_sampler, triUVy) * triW.y
                  + g_Tex.Sample(g_Tex_sampler, triUVz) * triW.z;
        else
            base *= SampleAntiTile(g_Tex, g_Tex_sampler, i.uv);
    }
#if NUKE_VCTINT
    if (varF & 2u) base *= i.vcol;   // Vertex Color = Tint
#endif
    // Per-cell hue/brightness variation breaks the remaining repetition.
    if (g_Var.z > 0.0)
        base.rgb *= 1.0 + (Hash3(floor(i.uv / max(g_Var.y, 1e-3))) - 0.5) * g_Var.z * 0.6;
    if (g_UVT2.y > 0.0)
    {
        // Hashed alpha (hair cards): stochastic coverage instead of the hard threshold —
        // semi-transparent tips keep pixels in proportion to alpha (shadow dither matches).
        [branch] if (g_Sss.z > 0.5)
            clip(base.a - max(0.02, frac(52.9829189 * frac(dot(i.pos.xy, float2(0.06711056, 0.00583715))))));
        else
            clip(base.a - g_UVT2.y);   // Cutout blend: alpha clip at the threshold
    }
    float3 albedo = pow(max(base.rgb, 0.0), 2.2);   // sRGB -> linear

    // Detail albedo: gray-neutral overlay multiply at its own tiling (close-up texture).
    const uint detF = (uint)(g_Det.z + 0.5);
    [branch] if (g_Det.y > 0.0 && (detF & 1u))
    {
        float3 d = g_Detail.Sample(g_Ov0Alb_sampler, i.uv * g_Det.x).rgb;
        albedo = lerp(albedo, saturate(albedo * d * 2.0), g_Det.y);
    }

    // Overlay albedo: blend toward the slot's tinted map (or the tint alone when it has none).
#define OV_ALBEDO(N) \
    [branch] if (ovW[N] > 0.001) \
    { \
        float3 oa = (ovF[N] & 1u) ? pow(max(g_Ov##N##Alb.Sample(g_Ov0Alb_sampler, i.uv).rgb, 0.0), 2.2) : float3(1.0, 1.0, 1.0); \
        albedo = lerp(albedo, oa * pow(max(g_OvT##N.rgb, 0.0), 2.2), ovW[N] * saturate(g_OvT##N.a)); \
    }
    OV_ALBEDO(0) OV_ALBEDO(1) OV_ALBEDO(2) OV_ALBEDO(3)
    OV_ALBEDO(4) OV_ALBEDO(5) OV_ALBEDO(6) OV_ALBEDO(7)

    float metallic = saturate(g_Params.z);
    float rough    = clamp(g_Params.w, 0.04, 1.0);
    if (g_Params2.x > 0.5)                                  // metallic-roughness map wins (glTF G/B)
    {
        float3 m = g_MetalRough.Sample(g_MetalRough_sampler, i.uv).rgb;
        rough = clamp(m.g, 0.04, 1.0); metallic = saturate(m.b);
    }
    // Overlay metal/rough: the slot's MR map wins; otherwise its scalar targets (-1 = keep).
#define OV_MR(N) \
    [branch] if (ovW[N] > 0.001) \
    { \
        if (ovF[N] & 4u) { float3 m = g_Ov##N##MR.Sample(g_Ov0Alb_sampler, i.uv).rgb; rough = lerp(rough, clamp(m.g, 0.04, 1.0), ovW[N]); metallic = lerp(metallic, saturate(m.b), ovW[N]); } \
        else { if (g_OvP##N.x >= 0.0) metallic = lerp(metallic, saturate(g_OvP##N.x), ovW[N]); \
               if (g_OvP##N.y >= 0.0) rough    = lerp(rough, clamp(g_OvP##N.y, 0.04, 1.0), ovW[N]); } \
    }
    OV_MR(0) OV_MR(1) OV_MR(2) OV_MR(3) OV_MR(4) OV_MR(5) OV_MR(6) OV_MR(7)
    [branch] if (g_ParamsT.w > 0.5)   // masked metallic/roughness tween
    {
        float mtw = NukeMaskW((int)(g_ParamsT.w - 0.5), i.uv, i.wpos, g_MskStamp, g_Ov0Alb_sampler);
        metallic = lerp(metallic, saturate(g_ParamsT.x), mtw);
        rough    = lerp(rough, clamp(g_ParamsT.y, 0.04, 1.0), mtw);
    }

    float3 specF = g_Params2.w * g_Spec.Sample(g_Spec_sampler, i.uv).rgb;   // KHR specular: factor x spec map

    float3 V = normalize(g_CamPos.xyz - i.wpos);
    float3 N = normalize(i.nrm);
    // Tangent-space normal accumulates: base map first, then each overlay slot lerps its own
    // map in by its weight — an overlay can perturb even when the base has no normal map.
    // g_Params.y: 0 = no normal map; >0 = OpenGL green (+Y, flip); <0 = DirectX green (no flip).
    // RG only + reconstructed Z, so BC5 (which stores no Z) works.
    {
        float3 nTS = float3(0.0, 0.0, 1.0);
        bool anyN = false;
        if (abs(g_Params.y) > 0.5)
        {
            float2 nxy = g_Normal.Sample(g_Normal_sampler, i.uv).rg * 2.0 - 1.0;
            if (g_Params.y > 0.0) nxy.y = -nxy.y;
            nTS = float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
            anyN = true;
        }
        // Detail normal: added in tangent space at the detail tiling, scaled by strength.
        [branch] if (g_Det.y > 0.0 && (detF & 2u))
        {
            float2 dxy = g_DetailNrm.Sample(g_Ov0Alb_sampler, i.uv * g_Det.x).rg * 2.0 - 1.0;
            if (detF & 4u) dxy.y = -dxy.y;
            nTS = normalize(float3(nTS.xy + dxy * g_Det.y, nTS.z)); anyN = true;
        }
#define OV_NRM(N) \
        [branch] if (ovW[N] > 0.001 && (ovF[N] & 2u)) \
        { \
            float2 oxy = g_Ov##N##Nrm.Sample(g_Ov0Alb_sampler, i.uv).rg * 2.0 - 1.0; \
            if (ovF[N] & 16u) oxy.y = -oxy.y; \
            nTS = lerp(nTS, float3(oxy, sqrt(saturate(1.0 - dot(oxy, oxy)))), ovW[N]); anyN = true; \
        }
        OV_NRM(0) OV_NRM(1) OV_NRM(2) OV_NRM(3) OV_NRM(4) OV_NRM(5) OV_NRM(6) OV_NRM(7)
        if (anyN)
            N = PerturbNormal(N, normalize(nTS), ddx(i.wpos), ddy(i.wpos), ddx(i.uv), ddy(i.uv));
    }
    float3 swpos = i.wpos + N * g_ShadowParams.y;   // normal-offset bias: sample shadows slightly off the surface

    // Anisotropy tangent frame (cotangent trick), rotated by the flow map when present.
    const float aniso = g_Brdf1.z;
    float3 anisoT = float3(1.0, 0.0, 0.0), anisoB = float3(0.0, 1.0, 0.0);
    [branch] if (abs(aniso) > 0.001)
    {
        float3 dp1 = ddx(i.wpos), dp2 = ddy(i.wpos);
        float2 du1 = ddx(i.uv), du2 = ddy(i.uv);
        float3 dp2p = cross(dp2, N), dp1p = cross(N, dp1);
        anisoT = dp2p * du1.x + dp1p * du2.x;
        anisoT = normalize(anisoT + float3(1e-5, 0.0, 0.0));
        [branch] if (g_Brdf3.w > 0.5)
        {
            float2 f = g_Flow.Sample(g_Ov0Alb_sampler, i.uv).rg * 2.0 - 1.0;
            float3 B0 = normalize(cross(N, anisoT));
            anisoT = normalize(anisoT * f.x + B0 * f.y + float3(1e-5, 0.0, 0.0));
        }
        anisoT = normalize(anisoT - N * dot(N, anisoT));
        anisoB = cross(N, anisoT);
    }

    // IOR reshapes the dielectric base reflectance ((n-1)/(n+1))^2; ior 1.5 = the classic 0.04.
    float f0i = pow((g_Brdf2.y - 1.0) / (g_Brdf2.y + 1.0), 2.0);
    if (g_Brdf2.y < 1.01) f0i = 0.04;   // unset/zeroed CB reads as the default
    float3 F0 = lerp(f0i * specF, albedo, metallic);   // dielectric F0 scaled by KHR specular; conductor uses albedo
    // Thin-film iridescence: a spectral phase shift over the view angle re-tints F0.
    [branch] if (g_Brdf2.z > 0.0)
    {
        float ndv0 = saturate(dot(N, V));
        float3 shift = 0.5 + 0.5 * cos(6.28318 * ((g_Brdf2.w * 4.0 + 1.0) * ndv0 + float3(0.0, 0.33, 0.67)));
        F0 = lerp(F0, shift * saturate(F0 * 2.0 + 0.05), g_Brdf2.z);
    }
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
        float ndlS = dot(N, L);            // signed: subsurface wrap lights past the terminator
        float ndl  = max(ndlS, 0.0);
        // Translucency: light leaking THROUGH the surface (works for back lights too).
        [branch] if (g_Brdf2.x > 0.0 && atten > 1e-6)
        {
            float tw = pow(saturate(dot(V, -normalize(L + N * 0.4))), 3.0);
            Lo += g_Brdf2.x * g_Brdf4.rgb * albedo * tw
                * lt.colorIntensity.rgb * lt.colorIntensity.w * atten;
        }
        const float sssWrap = g_Sss.x * 0.5;
        if ((ndl <= 0.0 && ndlS <= -sssWrap) || atten <= 1e-6) continue;
        float3 H = normalize(V + L);
        float3 radiance = lt.colorIntensity.rgb * lt.colorIntensity.w * atten;
        float  shadow = 1.0;
        if (g_DrawFlags.x > 0.5)   // receiveShadows
        {
#ifdef RT_ENABLED
            bool casts = (type > 0.5 && type < 1.5) ? ((int)lt.spot.w >= 0) : ((int)lt.spot.z >= 0);
            if (casts)
            {
                float maxD = (type < 0.5) ? 1e4 : length(lt.posType.xyz - i.wpos);
                shadow = RTShadow(swpos, L, maxD);
            }
#else
            if (type > 0.5 && type < 1.5) shadow = SamplePointShadow(swpos, lt.posType.xyz, (int)lt.spot.w, lt.dirRange.w);
            else   // dir/spot 2D slot: normal-offset shadows - the receiver steps off its surface by up to
                   // 4 offsets at grazing incidence, where a shadow texel's depth slope dwarfs the depth bias (acne)
                   shadow = SampleShadow(i.wpos + N * g_ShadowParams.y * (1.0 + 3.0 * (1.0 - saturate(ndl))), (int)lt.spot.z, ndl);
#endif
        }
        radiance *= shadow;

        float  D = (abs(aniso) > 0.001) ? D_Aniso(N, H, anisoT, anisoB, rough, aniso)
                                        : DistributionGGX(N, H, rough);
        float  G = GeometrySmith(N, V, L, rough);
        float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
        float3 spec = (D * G) * F / max(4.0 * max(dot(N, V), 0.0) * ndl, 1e-4);
        // Clear coat: a second fixed-F0 glossy lobe on top; the base dims under the coat.
        [branch] if (g_Brdf1.x > 0.0)
        {
            float Fc = (0.04 + 0.96 * pow(saturate(1.0 - max(dot(H, V), 0.0)), 5.0)) * g_Brdf1.x;
            float Dc = DistributionGGX(N, H, max(g_Brdf1.y, 0.02));
            spec = spec * (1.0 - Fc) + Dc * G * Fc / max(4.0 * max(dot(N, V), 0.0) * ndl, 1e-4);
        }
        float3 kd = (1.0 - F) * (1.0 - metallic);
        // Toon: the diffuse response quantizes into a lit/shade band (MToon-style cel look);
        // the shade side tints, the response flattens so the band edge is the only gradient.
        float  ndlD = ndl;
        float3 albedoD = albedo;
        [branch] if (g_Toon.w > 0.5)
        {
            float band = smoothstep(g_Toon.x - g_Toon.y, g_Toon.x + g_Toon.y, ndl);
            albedoD = lerp(g_ToonShade.rgb * albedo, albedo, band);
            ndlD = lerp(0.55, 1.0, band);
        }
        // C5 subsurface: pre-integrated-style approximation — the diffuse response wraps past
        // the terminator and the scatter tint bleeds into the soft band (blood under skin).
        // Specular keeps the true ndl below, so no highlight appears on the dark side.
        float3 diffuse = kd * albedoD / PI;
        float  specNdl = ndlD;   // non-SSS paths keep the historical shared factor
        float3 diffRad = radiance;
        [branch] if (g_Sss.x > 0.0)
        {
            ndlD = saturate((ndlS + sssWrap) / (1.0 + sssWrap));
            // Terminator scatter: a NARROW, gentle warm band. Anything stronger reads as
            // bruised blotches on textured skin (the tint multiplies the albedo, so it only
            // shifts hue - never brightens past the plain diffuse).
            float band = saturate(1.0 - abs(ndlS) / (0.25 + sssWrap));
            band *= band * band;
            diffuse *= lerp(float3(1.0, 1.0, 1.0), g_SssTint.rgb * 1.25, band * g_Sss.x);
            specNdl = ndl;
            // The wrapped tail is light scattered THROUGH the surface — the facing shadow test
            // (RT self-shadow acne at grazing angles especially) is meaningless there and cuts
            // a hard cliff into the bright wrap zone. Fade the shadow in over the grazing band.
            float shD = lerp(1.0, shadow, smoothstep(-sssWrap, 0.35, ndlS));
            diffRad = lt.colorIntensity.rgb * lt.colorIntensity.w * atten * shD;
        }
        Lo += diffuse * diffRad * ndlD + spec * radiance * specNdl;
        // Sheen: soft retro-reflective grazing lobe (Charlie-style falloff).
        [branch] if (g_Brdf1.w > 0.0)
        {
            float ndh = max(dot(N, H), 0.0);
            float ia = 0.5 / clamp(rough, 0.07, 1.0);
            float sD = (1.0 + ia) * pow(1.0 - ndh * ndh, ia) / 6.28318;
            Lo += g_Brdf1.w * g_Brdf3.rgb * sD * radiance * ndl;
        }
    }

    float ao = (g_Params2.y > 0.5) ? g_Occlusion.Sample(g_Occlusion_sampler, i.uv).r : 1.0;
    {   // screen-space AO (ambient/IBL only); the 1x1 white fallback must not be Load-ed by pixel
        uint aoW, aoH; g_ScreenAO.GetDimensions(aoW, aoH);
        if (aoW > 1) ao *= g_ScreenAO.Load(int3((int2)i.pos.xy, 0)).r;
    }
    // Dynamic GI: probe irradiance replaces the sky irradiance wherever a volume covers the point
    // (specular keeps the sky / reflection probe). Not scaled by the ambient intensity: it is
    // real bounced light, the ambient knob stays an artistic control of the sky term.
    float3 giIrr = 0.0;
    bool   giHit = false;
    // Screen-space GI: rgb = contact bounce, a = the fraction of the hemisphere its rays hit. That
    // fraction REPLACES the far-field light (sky / probes) instead of adding to it - otherwise
    // every crease glows twice.
    float3 ssgi = 0.0; float ssgiOpen = 1.0;
    {
        uint sgW, sgH; g_ScreenGI.GetDimensions(sgW, sgH);
        if (sgW > 1) { float4 sg = g_ScreenGI.Load(int3((int2)i.pos.xy, 0)); ssgi = sg.rgb; ssgiOpen = 1.0 - saturate(sg.a); }
    }
    [branch] if (g_GICount.x > 0)
        giHit = DDGISample(g_GIIrr, g_GIVis, g_GIIrr_sampler, g_GIVol, g_GICount.x, g_GIAtlasInv.xy, g_GIAtlasInv.zw, i.wpos, N, V, giIrr);
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
        if (giHit) ambient = (kd * (giIrr * ssgiOpen + ssgi) * albedo + env * Fr * g_Ambient.w) * ao;
        else       ambient = (kd * (irr * g_Ambient.w * ssgiOpen + ssgi) * albedo + env * Fr * g_Ambient.w) * ao;
    }
    else if (giHit)
        ambient = (giIrr * ssgiOpen + ssgi) * (1.0 - metallic) * albedo * ao;   // probe light instead of the flat ambient
    else
        ambient = (g_Ambient.rgb * g_Ambient.w * ssgiOpen + ssgi) * (1.0 - metallic) * albedo * ao;   // flat ambient (no sky) + bounce
    float3 emissive = g_Emissive2.rgb * g_Emissive2.w;
    [branch] if (g_EmisT.w > 0.5)   // masked emissive tween (rgb premultiplied by intensity)
        emissive = lerp(emissive, g_EmisT.rgb, NukeMaskW((int)(g_EmisT.w - 0.5), i.uv, i.wpos, g_MskStamp, g_Ov0Alb_sampler));
    if (g_Params2.z > 0.5) emissive *= g_Emissive.Sample(g_Emissive_sampler, i.uv).rgb;
    // Luma-wipe burn edge: the band just above the dissolve threshold glows within the feather.
    if (wipeTh > 0.0 && g_UVT2.w > 0.0)
    {
        float edge = saturate(1.0 - (wipeL - wipeTh) / g_UVT2.w);
        emissive += albedo * (edge * edge * 6.0);
    }
    float3 color = ambient + Lo + emissive;

    // g_SkyParams.z == 0: emit linear HDR and let the post pass tonemap; == 1: tonemap here
    // (never during a GI probe capture: the probes integrate linear radiance).
    if (g_SkyParams.z > 0.5 && g_Misc.x < 0.5)
    {
        float W = (g_SkyParams.w > 1e-3) ? g_SkyParams.w : 1.0;   // tonemap white point
        color = color * (1.0 + color / (W * W)) / (1.0 + color);  // extended Reinhard
        color = pow(max(color, 0.0), 1.0 / 2.2);                  // linear -> sRGB
    }
    // Background refraction (Transparent blend): bend the pre-transparent scene snapshot
    // through the surface and OWN the pixel (the mix happens here, so alpha goes out 1).
    float alphaOut = base.a;
    [branch] if (g_Brdf4.w > 0.0)
    {
        uint rw, rh;
        g_SceneRefr.GetDimensions(rw, rh);
        [branch] if (rw > 1)
        {
            // Glass lens: Snell gives the deviation direction; the sample moves AGAINST it,
            // pulling content from deeper inside whatever sits behind. At an occlusion edge
            // this MAGNIFIES the object through the glass (the edge bulges outward) - the
            // opposite sign pulls from the empty side and visibly bites chunks out of it.
            float3 rd = refract(-V, N, 1.0 / max(g_Brdf2.y, 1.01));
            if (dot(rd, rd) < 1e-6) rd = reflect(-V, N);   // total internal reflection
            float3 dv = rd + V;                            // deviation vs continuing straight
            // The bend strength is PURE Snell: the IOR alone decides how far dv swings
            // (water 1.33 bends gently, glass 1.5 more, diamond 2.4 hard). The constant only
            // converts the angular deviation into screen space (assumed background distance).
            float2 off = -float2(dot(dv, normalize(ddx(i.wpos))),
                                 dot(dv, normalize(ddy(i.wpos)))) * 0.15;
            float ol = length(off);
            if (ol > 0.05) off *= 0.05 / ol;
            float2 suv = i.pos.xy / float2(rw, rh);
            // Chromatic split: the three channels refract slightly apart (dispersion).
            float3 bg;
            bg.r = g_SceneRefr.Sample(g_Ov0Alb_sampler, clamp(suv + off * 0.80, 0.002, 0.998)).r;
            bg.g = g_SceneRefr.Sample(g_Ov0Alb_sampler, clamp(suv + off,        0.002, 0.998)).g;
            bg.b = g_SceneRefr.Sample(g_Ov0Alb_sampler, clamp(suv + off * 1.20, 0.002, 0.998)).b;
            color = lerp(bg * lerp(float3(1.0, 1.0, 1.0), albedo, saturate(base.a)), color, saturate(base.a));
            alphaOut = 1.0;
        }
    }
    // Probe capture: distance in alpha; a back face (the world PSOs are double-sided, so a probe
    // inside geometry sees its walls from behind) writes 0 = "inside" for the probe classification.
    if (g_Misc.x > 0.5) alphaOut = isFront ? max(saturate(length(i.wpos - g_CamPos.xyz) / max(g_Misc.y, 1e-3)), 0.004) : 0.0;
    return float4(color, alphaOut);
}
