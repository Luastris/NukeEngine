// DDGI shared: atlas layout, octahedral mapping and the leak-free probe lookup.
// Included by world.ps (ambient term) and by the probe update shaders (multi-bounce).
//
// Atlas: every volume owns a rectangle of probe tiles; probe p of a volume sits at tile
// (p % perRow, p / perRow) inside it. Irradiance tiles are 8x8 texels (+1 border each side =
// 10x10 pitch), visibility tiles 16x16 (+1 = 18x18) storing (mean distance, mean distance^2).
// Volumes are described by GICB; g_GIIrr / g_GIVis are the two atlases (shared linear sampler).
#ifndef DDGI_HLSLI
#define DDGI_HLSLI

#define DDGI_MAX_VOLUMES 8
#define DDGI_IRR_TEXELS  8
#define DDGI_VIS_TEXELS  16
#define DDGI_IRR_PITCH   (DDGI_IRR_TEXELS + 2)
#define DDGI_VIS_PITCH   (DDGI_VIS_TEXELS + 2)

struct GIVolumeGPU
{
    float4 origin;      // xyz = probe (0,0,0) world position, w = intensity
    float4 spacing;     // xyz = probe step, w = max ray distance
    int4   counts;      // xyz = probes per axis, w = probes per atlas row
    int4   atlas;       // xy = irradiance tile origin (texels), zw = visibility tile origin (texels)
    float4 bias;        // x = normal bias, y = view bias (both in probe-spacing units), z = hysteresis, w = rays per probe
    int4   scroll;      // xyz = scrolling offset: tile of grid cell c = (c + scroll) mod counts (history survives view moves)
};

// Octahedral mapping of the unit sphere onto [-1,1]^2 (signed), and back.
float2 SignNZ(float2 v) { return float2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }   // DXC: no vector ternary
float2 OctEncodeDir(float3 d)
{
    d /= (abs(d.x) + abs(d.y) + abs(d.z));
    float2 e = d.xy;
    if (d.z < 0.0) e = (1.0 - abs(d.yx)) * SignNZ(d.xy);
    return e;
}
float3 OctDecodeDir(float2 e)
{
    float3 d = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (d.z < 0.0) d.xy = (1.0 - abs(d.yx)) * SignNZ(d.xy);
    return normalize(d);
}
// Texel centre (0..n-1) of an n x n tile -> direction; direction -> [0,n) tile coordinate.
float3 OctTexelDir(int2 t, int n) { return OctDecodeDir(((float2(t) + 0.5) / n) * 2.0 - 1.0); }
float2 OctDirTexel(float3 d, int n) { return (OctEncodeDir(d) * 0.5 + 0.5) * n; }

int3 DDGIProbeCoord(GIVolumeGPU v, int p)
{
    int nx = v.counts.x, ny = v.counts.y;
    return int3(p % nx, (p / nx) % ny, p / (nx * ny));
}
int DDGIProbeIndex(GIVolumeGPU v, int3 c) { return c.x + v.counts.x * (c.y + v.counts.y * c.z); }
// Atlas tile (storage index) of a grid cell: the grid scrolls, the tiles stay put.
int DDGIStorageIndex(GIVolumeGPU v, int3 c)
{
    int3 s = (c + v.scroll.xyz) % v.counts.xyz;
    return DDGIProbeIndex(v, s);
}
float3 DDGIProbePos(GIVolumeGPU v, int3 c) { return v.origin.xyz + float3(c) * v.spacing.xyz; }

// Atlas uv of a direction inside a probe's tile (the 1-texel border makes bilinear wrap correct).
float2 DDGIIrrUV(GIVolumeGPU v, int p, float3 dir, float2 invAtlas)
{
    int2 tile = int2(p % v.counts.w, p / v.counts.w);
    float2 t = float2(v.atlas.xy + tile * DDGI_IRR_PITCH + 1) + OctDirTexel(dir, DDGI_IRR_TEXELS);
    return t * invAtlas;
}
float2 DDGIVisUV(GIVolumeGPU v, int p, float3 dir, float2 invAtlas)
{
    int2 tile = int2(p % v.counts.w, p / v.counts.w);
    float2 t = float2(v.atlas.zw + tile * DDGI_VIS_PITCH + 1) + OctDirTexel(dir, DDGI_VIS_TEXELS);
    return t * invAtlas;
}

// Does the volume cover the point (with half a spacing of slack so the outer probes still count)?
bool DDGICovers(GIVolumeGPU v, float3 P)
{
    float3 lo = v.origin.xyz - v.spacing.xyz * 0.5;
    float3 hi = v.origin.xyz + v.spacing.xyz * float3(v.counts.xyz - 1) + v.spacing.xyz * 0.5;
    return all(P >= lo) && all(P <= hi);
}

// Irradiance at P with normal N seen from direction V (unit, surface -> eye). The classic DDGI
// lookup: the 8 surrounding probes, trilinear weights, a back-face (wrap) term, chebyshev
// visibility against the probe's stored distance, and a normal/view bias on the lookup point.
// Returns false when no volume covers P (caller keeps its sky term).
bool DDGISample(Texture2D irrTex, Texture2D visTex, SamplerState samp, GIVolumeGPU vols[DDGI_MAX_VOLUMES], int volCount,
                float2 invIrrAtlas, float2 invVisAtlas, float3 P, float3 N, float3 V, out float3 irradiance)
{
    irradiance = 0.0;
    int vi = -1;
    for (int k = 0; k < volCount; ++k) if (DDGICovers(vols[k], P)) { vi = k; break; }
    if (vi < 0) return false;
    GIVolumeGPU v = vols[vi];

    float3 bias = (N * v.bias.x + V * v.bias.y) * v.spacing.xyz;
    float3 Pb   = P + bias;
    float3 g    = (Pb - v.origin.xyz) / v.spacing.xyz;
    int3   base = clamp(int3(floor(g)), int3(0, 0, 0), v.counts.xyz - 2);
    float3 a    = saturate(g - float3(base));

    float3 sum = 0.0; float wsum = 0.0;
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        int3 off = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        int3 c   = base + off;
        int  p   = DDGIStorageIndex(v, c);   // atlas tile of this cell
        float3 probePos = DDGIProbePos(v, c);
        float3 tri = lerp(1.0 - a, a, float3(off));
        float  w   = tri.x * tri.y * tri.z;

        // back-face: probes behind the surface contribute little (smooth wrap instead of a cut)
        float3 toProbe = normalize(probePos - P);
        float  wrap = (dot(toProbe, N) + 1.0) * 0.5;
        w *= wrap * wrap + 0.2;

        // chebyshev visibility: how likely is the biased point to see this probe
        float3 d    = Pb - probePos;
        float  dist = length(d);
        float2 mom  = visTex.SampleLevel(samp, DDGIVisUV(v, p, d / max(dist, 1e-4), invVisAtlas), 0).rg;
        float  mean = mom.x, var = abs(mom.x * mom.x - mom.y);
        float  cheb = 1.0;
        if (dist > mean)
        {
            float t = dist - mean;
            cheb = var / (var + t * t);
            cheb = max(cheb * cheb * cheb, 0.0);
        }
        w *= max(cheb, 0.05);
        w  = max(w, 1e-5);

        float3 irr = irrTex.SampleLevel(samp, DDGIIrrUV(v, p, N, invIrrAtlas), 0).rgb;
        sum  += irr * w;
        wsum += w;
    }
    irradiance = (wsum > 0.0 ? sum / wsum : 0.0) * v.origin.w;
    return true;
}

#endif
