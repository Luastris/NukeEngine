// DDGI atlas update from g_RayData (the trace pass, or the cube-capture conversion).
// Mode (g_GIPass.w): 0 = irradiance texels (8x8 per probe, cosine-weighted mean of the ray
// radiance, hysteresis blend), 1 = visibility texels (16x16, sharp power-weighted mean of
// distance and distance^2), 2 = irradiance borders, 3 = visibility borders (the 1-texel frame
// that makes bilinear sampling wrap around the octahedron), 4 = reset the tiles of the grid
// cells that scrolled into range (g_GIPass.y = axis, z = first cell, g_GIMisc.w = cell count).
// One thread per texel of one probe (mode 4: one thread per probe).
#include "ddgi.hlsli"

cbuffer GICB { GIVolumeGPU g_GIVol[DDGI_MAX_VOLUMES]; int4 g_GICount; float4 g_GIAtlasInv; };
cbuffer GIPassCB
{
    int4   g_GIPass;   // x = volume index, y = first probe, z = rays per probe, w = mode
    float4 g_GIRot;    // the rotation the rays were generated with
    float4 g_GIMisc;   // x = max ray distance, y = hysteresis, z = frame, w = probe count in this dispatch
};
StructuredBuffer<float4> g_RayData;
RWTexture2D<float4> g_IrrAtlas;   // RGBA16F
RWTexture2D<float2> g_VisAtlas;   // RG16F

float3 RotateQ(float3 v, float4 q) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
float3 FibDir(uint i, uint n)
{
    float phi = 2.399963 * (float)i;
    float z   = 1.0 - (2.0 * i + 1.0) / (float)n;
    float r   = sqrt(max(0.0, 1.0 - z * z));
    return float3(r * cos(phi), r * sin(phi), z);
}

// Interior texel that a border texel mirrors (octahedral wrap), n = tile size without border.
int2 BorderSource(int2 b, int n)
{
    bool left = b.x == 0, right = b.x == n + 1, top = b.y == 0, bottom = b.y == n + 1;
    if ((left || right) && (top || bottom)) return int2(left ? n : 1, top ? n : 1);   // corners: opposite corner
    if (top)    return int2(n + 1 - b.x, 1);
    if (bottom) return int2(n + 1 - b.x, n);
    if (left)   return int2(1, n + 1 - b.y);
    return int2(n, n + 1 - b.y);                                                       // right
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const int  mode  = g_GIPass.w;
    GIVolumeGPU v0 = g_GIVol[g_GIPass.x];
    if (mode == 4)
    {
        int probe = (int)tid.x;
        if (probe >= v0.counts.x * v0.counts.y * v0.counts.z) return;
        int3 c = DDGIProbeCoord(v0, probe);
        int  a = c[g_GIPass.y];
        if (a < g_GIPass.z || a >= g_GIPass.z + (int)g_GIMisc.w) return;
        int  sidx = DDGIStorageIndex(v0, c);
        int2 tile = int2(sidx % v0.counts.w, sidx / v0.counts.w);
        int2 io = v0.atlas.xy + tile * DDGI_IRR_PITCH, vo = v0.atlas.zw + tile * DDGI_VIS_PITCH;
        for (int y = 0; y < DDGI_IRR_PITCH; ++y) for (int x = 0; x < DDGI_IRR_PITCH; ++x) g_IrrAtlas[io + int2(x, y)] = 0.0;
        for (int y2 = 0; y2 < DDGI_VIS_PITCH; ++y2) for (int x2 = 0; x2 < DDGI_VIS_PITCH; ++x2) g_VisAtlas[vo + int2(x2, y2)] = 0.0;
        return;
    }
    const int  n     = (mode == 0 || mode == 2) ? DDGI_IRR_TEXELS : DDGI_VIS_TEXELS;
    const int  pitch = n + 2;
    const uint perProbe = (mode < 2) ? (uint)(n * n) : (uint)(pitch * pitch);
    const uint local = tid.x / perProbe;
    const uint texel = tid.x % perProbe;
    if (local >= (uint)g_GIMisc.w) return;
    const int probe = g_GIPass.y + (int)local;

    GIVolumeGPU v = g_GIVol[g_GIPass.x];
    const int sidx = DDGIStorageIndex(v, DDGIProbeCoord(v, probe));   // rays are per grid cell, tiles per storage slot
    int2 tile = int2(sidx % v.counts.w, sidx / v.counts.w);
    int2 tileOrigin = ((mode == 0 || mode == 2) ? v.atlas.xy : v.atlas.zw) + tile * pitch;

    if (mode >= 2)
    {   // borders
        int2 b = int2(texel % pitch, texel / pitch);
        if (b.x != 0 && b.x != n + 1 && b.y != 0 && b.y != n + 1) return;   // interior: nothing to do
        int2 src = tileOrigin + BorderSource(b, n);
        int2 dst = tileOrigin + b;
        if (mode == 2) g_IrrAtlas[dst] = g_IrrAtlas[src]; else g_VisAtlas[dst] = g_VisAtlas[src];
        return;
    }

    int2   t   = int2(texel % n, texel / n);
    float3 dir = OctTexelDir(t, n);
    const uint rays = (uint)g_GIPass.z;
    const uint base = (uint)probe * rays;
    const float maxD = g_GIMisc.x;
    const float h    = g_GIMisc.y;
    int2 dst = tileOrigin + 1 + t;

    if (mode == 0)
    {
        float3 sum = 0.0; float wsum = 0.0; uint back = 0;
        for (uint r = 0; r < rays; ++r)
        {
            float4 rd = g_RayData[base + r];
            if (rd.w < 0.0) { ++back; continue; }              // back-face hit: no light from inside walls
            float w = max(0.0, dot(dir, RotateQ(FibDir(r, rays), g_GIRot)));
            sum += rd.rgb * w; wsum += w;
        }
        float3 irr  = wsum > 1e-4 ? sum / wsum : 0.0;          // = E / pi for a uniform environment (matches the sky irradiance convention)
        // Classification: a probe whose rays mostly start inside geometry (> 25% back faces) averages
        // only the open half of its sphere - an inflated value that leaks light along every crease.
        // It keeps updating but gets no vote (a = 0.25); a = 1 = active, 0 = never written.
        const bool inside = back * 4 > rays;
        float4 prev = g_IrrAtlas[dst];
        g_IrrAtlas[dst] = float4(prev.a > 0.1 ? lerp(irr, prev.rgb, h) : irr, inside ? 0.25 : 1.0);
    }
    else
    {
        float2 sum = 0.0; float wsum = 0.0;
        for (uint r = 0; r < rays; ++r)
        {
            float4 rd = g_RayData[base + r];
            float  d  = min(abs(rd.w), maxD);
            float  w  = pow(max(0.0, dot(dir, RotateQ(FibDir(r, rays), g_GIRot))), 50.0);
            sum += float2(d, d * d) * w; wsum += w;
        }
        float2 mom  = wsum > 1e-6 ? sum / wsum : float2(maxD, maxD * maxD);
        float2 prev = g_VisAtlas[dst];
        g_VisAtlas[dst] = (prev.x > 0.0) ? lerp(mom, prev, h) : mom;
    }
}
