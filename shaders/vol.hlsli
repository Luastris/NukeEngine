// Froxel volumetrics shared: the per-camera grid constants and the exponential slice mapping.
// Included by vol_inject.cs (medium + in-scatter), vol_integrate.cs (front-to-back) and
// vol_apply.ps (composite). Slice k spans [SliceZ(k), SliceZ(k+1)] in view depth.
#ifndef VOL_HLSLI
#define VOL_HLSLI

cbuffer VolCB
{
    float4x4 g_VolView;          // camera view (the prepass camera)
    float4x4 g_VolProj;          // unjittered projection
    float4x4 g_VolInvViewProj;   // clip -> world
    float4x4 g_VolPrevViewProj;  // last frame's world -> clip (temporal reprojection)
    float4   g_VolGrid;          // froxels x, y, z, frame
    float4   g_VolRange;         // grid near, grid far, log(far / near), 1 / log(far / near)
    float4   g_VolCam;           // camera near, camera far, camera pos x, y
    float4   g_VolMedium;        // density (1/m at the base), height base, height falloff (1/m), anisotropy g
    float4   g_VolAlbedo;        // rgb albedo, w = light intensity
    float4   g_VolMisc;          // ambient intensity, history blend (0 = none), scene is LDR (1) / linear (0), white point
    float4   g_VolJitter;        // xyz froxel-space jitter [0,1), w = camera pos z
    float4   g_VolScreen;        // screen w, h, pixels per froxel, light-shaft density (1/m, no extinction)
};

// Noise erosion of a medium: clumps, not a haze. Amount 0 = solid; at 1 the fbm (0..1, mean
// ~0.5) is cut at 0.4 and steepened x3, so a clump stands three times denser than the thin
// air between clumps instead of a +-30% mottle.
float VolErode(float fbm, float amount)
{
    float clumps = saturate((fbm - 0.4) * 3.0);
    return lerp(1.0, clumps, amount);
}
float VolSliceZ(float k)   { return g_VolRange.x * exp(k / g_VolGrid.z * g_VolRange.z); }                        // k in [0, gd]
float VolZSlice(float z)   { return log(max(z, g_VolRange.x) / g_VolRange.x) * g_VolRange.w * g_VolGrid.z; }   // continuous index
// Device depth (D3D 0..1, not reversed) of a view depth under the camera projection, and back.
float VolDeviceDepth(float z) { float n = g_VolCam.x, f = g_VolCam.y; return (f / (f - n)) * (1.0 - n / max(z, 1e-4)); }
float VolLinearZ(float d)     { float n = g_VolCam.x, f = g_VolCam.y; return n * f / max(f - d * (f - n), 1e-6); }
// Grid w coordinate of a view depth (slice k's texel holds the column up to its far edge).
float VolW(float z)           { return saturate((VolZSlice(z) - 0.5) / g_VolGrid.z); }

// Translucent (sprite) fog. The post composite applies the OPAQUE column behind the pixel
// (T_o, L_o) to everything, so a sprite pre-compensates: its colour fogged by its OWN column
// (T_p, L_p) is what must come out after the post pass: c' = (c T_p + L_p - L_o) / T_o.
// Linear (HDR) path only; the LDR path keeps the plain opaque-depth fog.
float3 VolFogTranslucent(Texture3D<float4> integ, SamplerState s, float3 c, float2 uv, float zp, float zo)
{
    if (g_VolMisc.z > 0.5) return c;
    float4 vp = integ.SampleLevel(s, float3(uv, VolW(zp)), 0);
    float4 vo = integ.SampleLevel(s, float3(uv, VolW(zo)), 0);
    return max(c * vp.a + vp.rgb - vo.rgb, 0.0) / max(vo.a, 1e-3);
}

// Reflections (RT ray-gen, SSR, water): the fog of an arbitrary segment - a reflected leg from the
// mirror to what it shows. Define VOL_REFLECT before including; the includer binds this frame's
// scatter grid as g_VolFogScat (a 1x1x1 (0,0,0,1) stand-in when the grid is off). Marches the
// grid where the segment is inside it; outside, the global medium analytically - its lights are
// not known here, so only the ambient haze (passed in) remains. Returns colour * T + S.
#ifdef VOL_REFLECT
Texture3D<float4> g_VolFogScat; SamplerState g_VolFogScat_sampler;
// A stretch of the segment outside the grid: the global medium as a uniform slab (its lights
// are not known here, only the ambient haze passed in).
// Outside the grid the medium is evaluated analytically: the global height fog and light
// scattering, and with VOL_REFLECT_VOLUMES (the includer binds FogVolCB) every local volume's
// shape - a reflection of the street behind the camera keeps its fog band. With
// VOL_REFLECT_LIGHTS (FrameCB declared first) the directional lights scatter there too
// (unshadowed), so the sun's glow does not stop at the grid's edge.
#ifdef VOL_REFLECT_VOLUMES
struct VolRFogVol
{
    float4 posShape; float4 extDensity; float4 rot; float4 albedoFall; float4 emisNoise; float4 noiseMisc; float4 fluidInfo;
};
cbuffer FogVolCB { float4 g_FogWind; int4 g_FogCount; VolRFogVol g_FogVols[32]; };
float  VRHash(float3 p) { p = frac(p * 0.3183099 + float3(0.11, 0.17, 0.13)); p *= 17.0; return frac(p.x * p.y * p.z * (p.x + p.y + p.z)); }
float  VRNoise(float3 x)
{
    float3 i = floor(x), f = frac(x); f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(VRHash(i), VRHash(i + float3(1, 0, 0)), f.x), lerp(VRHash(i + float3(0, 1, 0)), VRHash(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(VRHash(i + float3(0, 0, 1)), VRHash(i + float3(1, 0, 1)), f.x), lerp(VRHash(i + float3(0, 1, 1)), VRHash(i + float3(1, 1, 1)), f.x), f.y), f.z);
}
float  VRFbm(float3 p) { return 0.6 * VRNoise(p) + 0.3 * VRNoise(p * 2.13 + 5.1) + 0.1 * VRNoise(p * 4.31 + 9.7); }
float3 VRRotate(float3 v, float4 q) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
#endif
void VolFogOutside(float3 a, float3 d, float len, float ta, float tb, float3 ambient, inout float3 S, inout float T)
{
    float L = len * max(tb - ta, 0.0);
    if (L <= 0.0) return;
    float3 V = -normalize(d);   // toward the eye of this leg (the mirror)
    float3 Lsum = 0.0;
#ifdef VOL_REFLECT_LIGHTS
    {
        int cnt = (g_LightCount.y > 0.5) ? 0 : (int)g_LightCount.x;
        [loop] for (int li = 0; li < cnt; ++li)
        {
            Light lt = g_Lights[li];
            if (lt.posType.w > 0.5) continue;   // directional only: the local lights sit inside the grid
            float3 Ld = normalize(-lt.dirRange.xyz);
            float g = g_VolMedium.w, ct = -dot(V, Ld);
            float ph = (1.0 - g * g) / (4.0 * 3.14159265 * pow(max(1.0 + g * g - 2.0 * g * ct, 1e-4), 1.5));
            Lsum += lt.colorIntensity.rgb * lt.colorIntensity.w * ph;
        }
        Lsum *= g_VolAlbedo.w;
    }
#endif
    const float3 P0 = a + d * ta, P1 = a + d * tb;
    // 1) the global medium over the whole stretch: the height profile in closed form
    {
        float k = g_VolMedium.z;
        float h0 = max(P0.y - g_VolMedium.y, 0.0), h1 = max(P1.y - g_VolMedium.y, 0.0), dh = h1 - h0;
        float prof = (k * abs(dh) > 1e-3) ? (exp(-k * h0) - exp(-k * h1)) / (k * dh) : exp(-k * 0.5 * (h0 + h1));
        float ext = g_VolMedium.x * prof;
        float sc  = g_VolScreen.w * prof * exp(-g_VolScreen.w * length(0.5 * (P0 + P1) - a));
        float3 src = ext * g_VolAlbedo.rgb * (ambient * g_VolMisc.x + Lsum) + sc * Lsum;
        float e = max(ext, 1e-5), tr = exp(-e * L);
        S += T * src * (1.0 - tr) / e;
        T *= tr;
    }
#ifdef VOL_REFLECT_VOLUMES
    // 2) every local volume: the stretch's analytic overlap with its shape, integrated with 8
    //    samples of its own - a thin band at a grazing angle is never stepped over.
    const float3 D = P1 - P0;
    [loop] for (int vi = 0; vi < g_FogCount.y; ++vi)   // y = every volume, including the ones culled from the grid
    {
        VolRFogVol fv = g_FogVols[vi];
        float3 o  = VRRotate(P0 - fv.posShape.xyz, fv.rot) / fv.extDensity.xyz;   // local unit-shape space
        float3 dl = VRRotate(D, fv.rot) / fv.extDensity.xyz;
        float s0 = 0.0, s1 = 1.0;
        if (fv.posShape.w < 0.5)
        {   // box: slab test
            [unroll] for (int ax = 0; ax < 3; ++ax)
            {
                float oo = o[ax], dd = dl[ax];
                if (abs(dd) < 1e-6) { if (abs(oo) >= 1.0) { s0 = 1.0; s1 = 0.0; } }
                else { float ta1 = (-1.0 - oo) / dd, tb1 = (1.0 - oo) / dd; s0 = max(s0, min(ta1, tb1)); s1 = min(s1, max(ta1, tb1)); }
            }
        }
        else
        {   // sphere / ellipsoid: |o + s dl| = 1
            float A = dot(dl, dl), B = 2.0 * dot(o, dl), C = dot(o, o) - 1.0;
            float disc = B * B - 4.0 * A * C;
            if (A < 1e-8 || disc < 0.0) { s0 = 1.0; s1 = 0.0; }
            else { float sq = sqrt(disc); s0 = max(s0, (-B - sq) / (2.0 * A)); s1 = min(s1, (-B + sq) / (2.0 * A)); }
        }
        if (s1 <= s0) continue;
        const int M = 8;
        const float step = L * (s1 - s0) / M;
        [loop] for (int k = 0; k < M; ++k)
        {
            float  sm = s0 + (s1 - s0) * (float(k) + 0.5) / M;
            float3 q  = o + dl * sm;
            float  m  = (fv.posShape.w < 0.5) ? max(abs(q.x), max(abs(q.y), abs(q.z))) : length(q);
            float  wgt = (m < 1.0) ? (1.0 - smoothstep(1.0 - fv.albedoFall.w, 1.0, m)) : 0.0;
            wgt *= exp(-fv.noiseMisc.w * (q.y + 1.0) * fv.extDensity.y);   // height falloff above the shape's bottom
            if (fv.emisNoise.w > 0.0 && wgt > 0.0)
            {
                float3 Pw = P0 + D * sm;
                float3 np = (Pw - g_FogWind.xyz * g_FogWind.w * fv.noiseMisc.y) / fv.noiseMisc.x;
                wgt *= VolErode(VRFbm(np), fv.emisNoise.w);
            }
            if (wgt <= 0.0) continue;
            float  dv  = fv.extDensity.w * wgt, scv = fv.noiseMisc.z * wgt;
            float3 src = dv * fv.albedoFall.rgb * (ambient * g_VolMisc.x + Lsum) + scv * Lsum + fv.emisNoise.rgb * wgt;
            float  e   = max(dv, 1e-5), tr = exp(-e * step);
            S += T * src * (1.0 - tr) / e;
            T *= tr;
        }
    }
#endif
}
float3 VolFogSegment(float3 a, float3 b, float3 color, float3 ambient)
{
    if (g_VolGrid.x < 1.5) return color;   // grid off (the stand-in constants)
    float3 d = b - a; float len = length(d);
    if (len < 1e-3) return color;
    // The segment in clip space is linear in t: clip it against the grid's frustum slab
    // (near/far, the four sides), march exactly the inside part and treat the outside parts
    // as uniform slabs. (Marching the whole segment with a fixed step and testing each sample
    // printed bands where the exit point jumped between samples.)
    float4 cA = mul(g_VolProj, float4(mul(g_VolView, float4(a, 1.0)).xyz, 1.0));
    float4 cB = mul(g_VolProj, float4(mul(g_VolView, float4(b, 1.0)).xyz, 1.0));
    float4 cD = cB - cA;
    float t0 = 0.0, t1 = 1.0;
    float p[6] = { cA.w - g_VolRange.x, g_VolRange.y - cA.w, cA.w - cA.x, cA.w + cA.x, cA.w - cA.y, cA.w + cA.y };
    float q[6] = { cD.w, -cD.w, cD.w - cD.x, cD.w + cD.x, cD.w - cD.y, cD.w + cD.y };
    [unroll] for (int k = 0; k < 6; ++k)
    {
        if (abs(q[k]) < 1e-6) { if (p[k] < 0.0) { t0 = 1.0; t1 = 0.0; } }
        else { float t = -p[k] / q[k]; if (q[k] > 0.0) t0 = max(t0, t); else t1 = min(t1, t); }
    }
    float3 S = 0.0; float T = 1.0;
    if (t0 < t1)
    {
        VolFogOutside(a, d, len, 0.0, t0, ambient, S, T);
        const int N = 16;
        const float step = len * (t1 - t0) / N;
        [loop] for (int i = 0; i < N; ++i)
        {
            float  t  = t0 + (t1 - t0) * ((float(i) + 0.5) / N);
            float4 cl = cA + cD * t;
            float2 uv = float2(cl.x / cl.w * 0.5 + 0.5, 0.5 - cl.y / cl.w * 0.5);
            float4 f  = g_VolFogScat.SampleLevel(g_VolFogScat_sampler, float3(uv, saturate(VolZSlice(cl.w) / g_VolGrid.z)), 0);
            float  ext = max(f.a, 1e-5);
            float  tr  = exp(-ext * step);
            S += T * f.rgb * (1.0 - tr) / ext;
            T *= tr;
        }
        VolFogOutside(a, d, len, t1, 1.0, ambient, S, T);
    }
    else VolFogOutside(a, d, len, 0.0, 1.0, ambient, S, T);
    return color * T + S;
}
#endif

#endif
