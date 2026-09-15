// Froxel volumetrics, pass 1: one thread per froxel. The medium (global height fog) at the
// froxel's jittered centre, lit by the frame's lights — the sun through its shadow map (or an
// inline shadow ray on ray-tracing devices = god rays), point/spot lights with their falloff
// (VFX particle lights included, they are ordinary FrameCB lights) — with the Henyey-Greenstein
// phase, plus the ambient in-scatter from the DDGI probes (sky mean / flat ambient where no
// probe covers). Output g_Scat = (in-scattered radiance per metre, extinction per metre), blended
// with last frame's grid reprojected through the previous view-projection.
#include "vol.hlsli"
#ifdef RT_ENABLED
#include "rt_common.hlsl"   // FrameCB, GICB + atlases, TLAS + instances, SkyColor
#else
#include "ddgi.hlsli"
#define MAX_LIGHTS 256
#define MAX_SHADOWS 4
struct Light { float4 posType; float4 dirRange; float4 colorIntensity; float4 spot; };
cbuffer FrameCB
{
    float4 g_CamPos; float4 g_Ambient; float4 g_LightCount; Light g_Lights[MAX_LIGHTS];
    float4x4 g_ShadowVP[MAX_SHADOWS]; float4 g_ShadowParams;
    float4 g_SkyTop; float4 g_SkyHorizon; float4 g_SkyGround; float4 g_SkyParams;
    float4 g_ProbePos; float4 g_ProbeParams; float4 g_ProbeBox;
};
cbuffer GICB { GIVolumeGPU g_GIVol[DDGI_MAX_VOLUMES]; int4 g_GICount; float4 g_GIAtlasInv; };
Texture2D g_GIIrr;  Texture2D g_GIVis;  SamplerState g_GIIrr_sampler;
Texture2DArray<float>   g_Shadow;          // comparison sampler lives on the view (as in world.ps)
SamplerComparisonState  g_Shadow_sampler;
TextureCubeArray        g_ShadowCube;      // point-light cubes (world.ps SamplePointShadow)
SamplerComparisonState  g_ShadowCube_sampler;
float3 SkyColor(float3 dir)
{
    float up = dir.y;
    float3 c = (up >= 0.0) ? lerp(g_SkyHorizon.rgb, g_SkyTop.rgb, pow(saturate(up), 0.5))
                           : lerp(g_SkyHorizon.rgb, g_SkyGround.rgb, saturate(-up));
    return c * g_SkyParams.x;
}
#endif

// Local fog volumes (World: FogVolume components, nearest 32). Six float4 each.
#define FOGVOL_MAX 32
struct FogVol
{
    float4 posShape;      // xyz = centre, w = shape (0 box, 1 sphere, 2 ellipsoid)
    float4 extDensity;    // xyz = half extents / radii (local), w = density (1/m)
    float4 rot;           // world -> local rotation quaternion (xyzw)
    float4 albedoFall;    // rgb albedo, w = edge falloff (0 hard .. 1 from the centre)
    float4 emisNoise;     // rgb emission (radiance / m), w = noise erosion amount
    float4 noiseMisc;     // x = noise scale, y = wind advection, z = local light-shaft density (1/m), w = height falloff (1/m above the shape's bottom)
    float4 fluidInfo;     // x = fluid slot + 1 (0 = analytic shape), yzw = 0
};
Texture3D<float> g_Fluid0; SamplerState g_Fluid0_sampler;   // fluid volumes' density fields (shape x noise baked, moved by the sim)
Texture3D<float> g_Fluid1; SamplerState g_Fluid1_sampler;
Texture3D<float> g_Fluid2; SamplerState g_Fluid2_sampler;
Texture3D<float> g_Fluid3; SamplerState g_Fluid3_sampler;
float FluidDensity(int slot, float3 uvw)
{
    if (slot == 1) return g_Fluid0.SampleLevel(g_Fluid0_sampler, uvw, 0);
    if (slot == 2) return g_Fluid1.SampleLevel(g_Fluid1_sampler, uvw, 0);
    if (slot == 3) return g_Fluid2.SampleLevel(g_Fluid2_sampler, uvw, 0);
    return g_Fluid3.SampleLevel(g_Fluid3_sampler, uvw, 0);
}
cbuffer FogVolCB
{
    float4 g_FogWind;     // xyz = wind direction * gusted strength (m/s), w = wind time
    int4   g_FogCount;    // x = the grid's volume count, y = with the ones culled from the grid (reflections)
    FogVol g_FogVols[FOGVOL_MAX];
};
RWTexture3D<float4> g_VolLightOut;                                  // incident light per froxel (particles sample it)
RWTexture3D<float4> g_Scat;                                         // this frame's raw grid (vol_temporal.cs blends it with the history)

static const float VPI = 3.14159265;

// 3D value noise (hash grid, trilinear) and a 3-octave fbm for the volume erosion.
float VHash(float3 p) { p = frac(p * 0.3183099 + float3(0.11, 0.17, 0.13)); p *= 17.0; return frac(p.x * p.y * p.z * (p.x + p.y + p.z)); }
float VNoise(float3 x)
{
    float3 i = floor(x), f = frac(x); f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(VHash(i), VHash(i + float3(1, 0, 0)), f.x), lerp(VHash(i + float3(0, 1, 0)), VHash(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(VHash(i + float3(0, 0, 1)), VHash(i + float3(1, 0, 1)), f.x), lerp(VHash(i + float3(0, 1, 1)), VHash(i + float3(1, 1, 1)), f.x), f.y), f.z);
}
float VFbm(float3 p) { return 0.6 * VNoise(p) + 0.3 * VNoise(p * 2.13 + 5.1) + 0.1 * VNoise(p * 4.31 + 9.7); }
float3 QRotate(float3 v, float4 q) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }

// Henyey-Greenstein phase, normalised over the sphere.
float PhaseHG(float cosT, float g)
{
    float d = 1.0 + g * g - 2.0 * g * cosT;
    return (1.0 - g * g) / (4.0 * VPI * pow(max(d, 1e-4), 1.5));
}

// Sun / spot visibility at a point (1 = lit); maxT = the distance to a spot, the horizon for the sun.
float LightVisibility(float3 P, float3 L, int slot, float maxT)
{
#ifdef RT_ENABLED
    RayDesc ray; ray.Origin = P; ray.Direction = L; ray.TMin = 0.02; ray.TMax = max(maxT - 0.05, 0.05);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0x02, ray);   // shadow-caster bit; every candidate is non-opaque (TLAS contract)
    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE)   // sprites, turned toward the ray
        {
            RTInstanceData inst = g_Instances[q.CandidateInstanceID()];
            float t, along; float2 uv;
            if (SpriteHit(inst.dynPosOffset, inst.shadowShape, q.CandidatePrimitiveIndex(), q.CandidateObjectToWorld3x4(),
                          q.WorldRayOrigin(), q.WorldRayDirection(), q.RayTMin(), q.CommittedRayT(), t, uv, along)
                && SpriteInside(inst.shadowShape, uv) && inst.shadowAlpha >= 0.35)
                q.CommitProceduralPrimitiveHit(t);
        }
        else if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            RTInstanceData inst = g_Instances[q.CandidateInstanceID()];
            float2 bc = q.CandidateTriangleBarycentrics();
            float  w0 = 1.0 - bc.x - bc.y;
            float2 uv = (q.CandidatePrimitiveIndex() & 1)
                      ? float2(0.0, 1.0) * w0 + float2(1.0, 0.0) * bc.x
                      : float2(0.0, 1.0) * w0 + float2(1.0, 1.0) * bc.x + float2(1.0, 0.0) * bc.y;
            bool inside = (inst.shadowShape == 1u) ? (length(uv - 0.5) < 0.45)
                        : (inst.shadowShape == 2u) ? (abs(uv.x - 0.5) < 0.45)
                        : true;
            if (inside && inst.shadowAlpha >= 0.35) q.CommitNonOpaqueTriangleHit();
        }
    }
    return (q.CommittedStatus() != COMMITTED_NOTHING) ? 0.0 : 1.0;
#else
    if (slot < 0) return 1.0;
    float4 lp = mul(g_ShadowVP[slot], float4(P, 1.0));
    lp.xyz /= lp.w;
    float2 uv = lp.xy * float2(0.5, -0.5) + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || lp.z > 1.0) return 1.0;
    // 3x3 PCF over a doubled kernel: the map's texel staircase along a grazing wall would otherwise
    // print through the air next to it as static stripes (the froxel jitter cannot dissolve it).
    float t = g_ShadowParams.z * 2.0, depth = lp.z - g_ShadowParams.w * 4.0, sum = 0.0;   // air: no self-shadowing, so a wide bias costs nothing and keeps a grazing floor's texel staircase out of the fog
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        sum += g_Shadow.SampleCmpLevelZero(g_Shadow_sampler, float3(uv + float2(x, y) * t, (float)slot), depth);
    return sum / 9.0;
#endif
}

// Point-light visibility: the shadow cube (world.ps SamplePointShadow) or an inline ray.
float PointVisibility(float3 P, float3 lpos, int cube, float farZ, float dist)
{
#ifdef RT_ENABLED
    return LightVisibility(P, normalize(lpos - P), -1, dist);
#else
    if (cube < 0) return 1.0;
    float3 dir = P - lpos;
    float3 ad  = abs(dir);
    float  z   = max(ad.x, max(ad.y, ad.z));
    float  n   = 0.1;
    float  ndc = (farZ / (farZ - n)) * (1.0 - n / max(z, 1e-4));
    ndc -= g_ShadowParams.w * 4.0;   // air: no self-shadowing, a wide bias is free
    return g_ShadowCube.SampleCmpLevelZero(g_ShadowCube_sampler, float4(dir, (float)cube), ndc);
#endif
}

[numthreads(8, 8, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int3 grid = (int3)g_VolGrid.xyz;
    if (any((int3)id >= grid)) return;
    // Froxel centre -> view depth -> world position (through the camera projection). Everything
    // analytic (density, phase, attenuation, the history lookup) is evaluated AT THE CENTRE: a
    // froxel is metres tall at fog distances and the height falloff changes the density across it
    // several-fold, so a jittered point sample of it, kept at 10% per frame, flickered every cell
    // (a per-cell offset: each block on its own; a grid-wide offset: the whole fog pulsing).
    // Only the sun visibility is stochastic and takes the jittered point (see Pj below).
    float3 f  = (float3(id) + 0.5) / float3(grid);
    float  z  = VolSliceZ(f.z * g_VolGrid.z);
    // Every froxel is lit where it is - NEVER clamped to the surface seen through its tile: a
    // tile straddling a silhouette serves both the object's pixels and the sky's, and the composite
    // already stops each pixel's column at that pixel's own depth. (Clamping printed the object's
    // column onto the neighbouring sky as a tile-sized, jittering halo.)
    float4 clip = float4(f.x * 2.0 - 1.0, 1.0 - f.y * 2.0, VolDeviceDepth(z), 1.0);
    float4 wp4  = mul(g_VolInvViewProj, clip);
    float3 P    = wp4.xyz / wp4.w;
    float3 camP = float3(g_VolCam.z, g_VolCam.w, g_VolJitter.w);
    float3 V    = normalize(camP - P);   // toward the eye
    // The lights are evaluated at FIXED sub-samples of the froxel (4 on Medium/High, a tetrahedron
    // of quarter offsets): a spot's cone edge or a shadow edge crossing a cell averages to a ramp
    // instead of printing the cell's staircase - and nothing flickers, the pattern never moves.
    // On the ray-traced path each sub-sample also takes the frame's low-discrepancy offset, so a
    // binary shadow ray averages further over the temporal blend.
    const int NS = (g_VolGrid.z > 40.0) ? 4 : 1;
    const float3 kSub[4] = { float3(-0.25, -0.25, -0.25), float3(0.25, 0.25, -0.25), float3(0.25, -0.25, 0.25), float3(-0.25, 0.25, 0.25) };
    float3 Ps[4];
    [unroll] for (int si = 0; si < 4; ++si)
    {
        float3 fs = float3(id) + 0.5 + ((NS > 1) ? kSub[si] : 0.0);
#ifdef RT_ENABLED
        fs += (g_VolJitter.xyz - 0.5) * ((NS > 1) ? 0.5 : 1.0);
#endif
        fs /= float3(grid);
        float4 cls = float4(fs.x * 2.0 - 1.0, 1.0 - fs.y * 2.0, VolDeviceDepth(VolSliceZ(fs.z * g_VolGrid.z)), 1.0);
        float4 wps = mul(g_VolInvViewProj, cls);
        Ps[si] = wps.xyz / wps.w;
    }

    // The froxel's depth span along its own ray: the medium is integrated over it, not sampled at
    // the centre - a slanted froxel high above a thin fog layer crosses metres of height, and a
    // centre sample printed the slices as bands ("slate") when looking down at the layer.
    float3 Pn, Pf;
    {
        float2 ndc = float2(f.x * 2.0 - 1.0, 1.0 - f.y * 2.0);
        float4 c0 = mul(g_VolInvViewProj, float4(ndc, VolDeviceDepth(VolSliceZ((float)id.z)), 1.0));
        float4 c1 = mul(g_VolInvViewProj, float4(ndc, VolDeviceDepth(VolSliceZ((float)id.z + 1.0)), 1.0));
        Pn = c0.xyz / c0.w; Pf = c1.xyz / c1.w;
    }
    // Height profile exp(-k h) averaged over the span (closed form; below the base it is 1).
    float hProfile;
    {
        float k = g_VolMedium.z;
        float h0 = max(Pn.y - g_VolMedium.y, 0.0), h1 = max(Pf.y - g_VolMedium.y, 0.0);
        float dh = h1 - h0;
        hProfile = (k * abs(dh) > 1e-3) ? (exp(-k * h0) - exp(-k * h1)) / (k * dh) : exp(-k * 0.5 * (h0 + h1));
    }
    // Medium: the global height fog (full density below the base, exponential falloff above)
    // plus every local volume covering the span: shape weight with a soft edge, the height
    // falloff and the wind-advected noise (or the simulated field), all taken at EIGHT points
    // along the span - a clump is metres across and a froxel at fog distance is metres deep, so
    // a centre sample printed the slices as bands over a clumpy layer; the points step by the
    // frame's low-discrepancy offset (a 1/8 stride, so no pulsing) and converge over the
    // temporal blend. Densities add, the scattering albedo blends by density, emission adds.
    float dens = g_VolMedium.x * hProfile;
    float3 sa  = dens * g_VolAlbedo.rgb;   // sum of density * albedo
    float3 emis = 0.0;
    float  shaftLocal = 0.0;   // the volumes' own light scattering (no extinction)
    float g    = g_VolMedium.w;
    [loop] for (int vi = 0; vi < g_FogCount.x; ++vi)
    {
        FogVol fv = g_FogVols[vi];
        float wgt = 0.0;
        const int fluidSlot = (int)fv.fluidInfo.x;
        const float jz = frac(g_VolJitter.z);
        [loop] for (int zi = 0; zi < 8; ++zi)
        {
            float3 Pz = lerp(Pn, Pf, (float(zi) + jz) / 8.0);
            float3 q = QRotate(Pz - fv.posShape.xyz, fv.rot) / fv.extDensity.xyz;   // local, unit box / sphere
            if (fluidSlot > 0)
            {   // the simulated field (shape, height, noise and motion baked in)
                if (any(abs(q) >= 1.0)) continue;
                wgt += FluidDensity(fluidSlot, q * 0.5 + 0.5);
                continue;
            }
            float  m = (fv.posShape.w < 0.5) ? max(abs(q.x), max(abs(q.y), abs(q.z))) : length(q);
            float  wz = (m < 1.0) ? (1.0 - smoothstep(1.0 - fv.albedoFall.w, 1.0, m)) : 0.0;
            wz *= exp(-fv.noiseMisc.w * (q.y + 1.0) * fv.extDensity.y);   // height falloff above the shape's bottom
            if (wz > 0.0 && fv.emisNoise.w > 0.0)
            {
                float3 np = (Pz - g_FogWind.xyz * g_FogWind.w * fv.noiseMisc.y) / fv.noiseMisc.x;
                wz *= VolErode(VFbm(np), fv.emisNoise.w);
            }
            wgt += wz;
        }
        wgt *= 0.125;
        if (wgt <= 0.0) continue;
        float dv = fv.extDensity.w * wgt;
        dens += dv; sa += dv * fv.albedoFall.rgb; emis += fv.emisNoise.rgb * wgt;
        shaftLocal += fv.noiseMisc.z * wgt;
    }

    // In-scatter from the lights (radiance units, the same values world.ps lights surfaces with).
    float3 Lsum = 0.0, Liso = 0.0;
    int cnt = (g_LightCount.y > 0.5) ? 0 : (int)g_LightCount.x;   // no lights in the world = no rays (the renderer's stand-in sun lights surfaces only)
    [loop] for (int li = 0; li < cnt; ++li)
    {
        Light lt = g_Lights[li]; float type = lt.posType.w;
        if (type > 0.5) { float dc = length(lt.posType.xyz - P); if (dc > lt.dirRange.w + 1.0) continue; }   // out of range: skip the cell
        // Every light is shadowed in the volume: the sun through its map (the god rays), a spot
        // through its map (the cone), a point light through its cube (the glow around it).
        float reach = 0.0; float3 Lc = normalize(-lt.dirRange.xyz);
        [loop] for (int s = 0; s < NS; ++s)
        {
            float3 Pv = Ps[s];
            float3 L; float atten = 1.0; float dist = 1000.0;
            if (type < 0.5) L = Lc;
            else
            {
                float3 d = lt.posType.xyz - Pv; dist = length(d); L = d / max(dist, 1e-4);
                float rng = max(lt.dirRange.w, 1e-4);
                if (dist > rng) continue;
                float win = saturate(1.0 - pow(dist / rng, 4.0));
                atten = (win * win) / (dist * dist + 1.0);
                if (type > 1.5) { float cd = dot(Lc, -L); float sc = saturate((cd - lt.spot.y) / max(lt.spot.x - lt.spot.y, 1e-4)); atten *= sc * sc; }
            }
            if (atten <= 0.0) continue;
            float vis;
            if (type < 0.5)      vis = LightVisibility(Pv, L, (int)lt.spot.z, 1000.0);
            else if (type > 1.5) vis = LightVisibility(Pv, L, (int)lt.spot.z, dist);
            else                 vis = PointVisibility(Pv, lt.posType.xyz, (int)lt.spot.w, lt.dirRange.w, dist);
            reach += atten * vis;
        }
        if (reach <= 0.0) continue;
        reach /= (float)NS;
        // phase angle: between the light's travel direction (-L) and the way to the eye (V), at the centre
        float3 Lp = (type < 0.5) ? Lc : normalize(lt.posType.xyz - P);
        float3 rad = lt.colorIntensity.rgb * lt.colorIntensity.w * reach;
        Lsum += rad * PhaseHG(-dot(V, Lp), g);
        Liso += rad;
    }
    // Ambient in-scatter: the probes where the grid covers the point (isotropic: the up-facing
    // irradiance stands for the mean radiance), else the sky mean or the flat ambient.
    float3 amb;
    if (!(g_GICount.x > 0 && DDGISample(g_GIIrr, g_GIVis, g_GIIrr_sampler, g_GIVol, g_GICount.x, g_GIAtlasInv.xy, g_GIAtlasInv.zw, P, float3(0.0, 1.0, 0.0), float3(0.0, 1.0, 0.0), amb)))
        amb = (g_SkyParams.y > 0.5)
            ? (g_SkyTop.rgb + 2.0 * g_SkyHorizon.rgb + g_SkyGround.rgb) * 0.25 * g_SkyParams.x * g_Ambient.w
            : g_Ambient.rgb * g_Ambient.w;

    float3 scat = sa * (Lsum * g_VolAlbedo.w + amb * g_VolMisc.x) + emis;
    // Light shafts: lit-air scattering from the lights only, with no extinction on the scene and
    // no ambient (the World's "Light Shafts" density, the fog's height profile). The beams through
    // window and canopy gaps show in otherwise clear air - an old house or a forest gets its rays
    // without a haze over the whole world. The term attenuates itself along the view (closed form
    // of a uniform medium), so a long lit column - the outdoors through a window - converges to
    // E * phase instead of piling up over the whole grid range.
    // (The physical phase, like the fog's: a side-view normalisation tried here multiplied the
    // whole lit column ~12x and turned the shafts into a veil over an open street.)
    scat += (g_VolScreen.w * hProfile * exp(-g_VolScreen.w * length(P - camP)) + shaftLocal) * Lsum * g_VolAlbedo.w;
    float4 cur  = float4(scat, dens);
    // Incident light at the froxel for translucents (no phase: what a diffuse smoke puff sees).
    g_VolLightOut[id] = float4(Liso * g_VolAlbedo.w + amb * g_VolMisc.x, 1.0);

    g_Scat[id] = cur;
}
