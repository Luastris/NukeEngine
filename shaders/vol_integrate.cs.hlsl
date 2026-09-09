// Froxel volumetrics, pass 2: front-to-back integration of every (x, y) column. Slice k of
// g_Integ = (in-scattered radiance reaching the eye from slices 0..k, transmittance through
// them); vol_apply.ps samples it at the pixel's depth. Each slice is read through a 3x3 tent
// over the neighbouring columns: the lights are evaluated at a few fixed sub-samples per froxel,
// so a cone or shadow edge is quantised to a few levels per column - the tent spreads each step
// over two columns (the froxel-scale softness the edges have anyway) and no staircase remains.
#include "vol.hlsli"

Texture3D<float4>   g_Scat;    // (radiance per metre, extinction per metre)
RWTexture3D<float4> g_Integ;

float4 ScatTent(int2 xy, int k, int2 grid)
{
    float4 sum = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2  c = clamp(xy + int2(x, y), int2(0, 0), grid - 1);
        float w = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
        sum += g_Scat[int3(c, k)] * w;
    }
    return sum / 16.0;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int3 grid = (int3)g_VolGrid.xyz;
    if (any((int2)id.xy >= grid.xy)) return;
    float3 L = 0.0; float T = 1.0;
    float  zPrev = g_VolRange.x;
    for (int k = 0; k < grid.z; ++k)
    {
        float zk = VolSliceZ((float)(k + 1));
        float dz = zk - zPrev; zPrev = zk;
        float4 s   = ScatTent((int2)id.xy, k, grid.xy);
        float  ext = max(s.a, 1e-5);
        float  tr  = exp(-ext * dz);
        L += T * s.rgb * (1.0 - tr) / ext;   // analytic in-scatter of a homogeneous slice
        T *= tr;
        g_Integ[int3(id.xy, k)] = float4(L, T);
    }
}
