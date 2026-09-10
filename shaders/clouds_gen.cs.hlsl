// Volumetric clouds, generation (once, or when the settings change): the tileable 3D noises
// and the 2D weather map. Mode in g_ClWind.w: 0 = base 128^3 (R Perlin-Worley, GBA Worley at
// 2/4/8x), 1 = detail 32^3 (Worley at 1/2/4x), 2 = weather 512^2 (R coverage field, G type field).
// All noises tile: lattice hashes wrap at the period, so the textures repeat without seams.
#include "clouds.hlsli"

RWTexture3D<float4> g_Out3D;
RWTexture2D<float4> g_Out2D;

uint3 Wrap3(int3 v, int period) { return (uint3)((v % period + period) % period); }
float Hash3(uint3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return (float)(v.x & 0x00ffffffu) / 16777216.0;
}
float3 Hash33(uint3 v)
{
    return float3(Hash3(v), Hash3(v + uint3(17u, 59u, 83u)), Hash3(v + uint3(101u, 7u, 43u)));
}
// Tileable gradient (Perlin) noise, -1..1, period cells over [0,1).
float Perlin(float3 p, int period)
{
    float3 pf = p * (float)period;
    int3   i  = (int3)floor(pf);
    float3 f  = frac(pf);
    float3 u  = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float n = 0.0;
    [loop] for (int z = 0; z <= 1; ++z) [loop] for (int y = 0; y <= 1; ++y) [loop] for (int x = 0; x <= 1; ++x)
    {   // rolled: this runs once, and the unrolled 27-cell Worley x 12 took the compiler a minute
        int3   c = int3(x, y, z);
        float3 g = normalize(Hash33(Wrap3(i + c, period)) * 2.0 - 1.0);
        float  d = dot(g, f - (float3)c);
        float3 w = lerp(1.0 - u, u, (float3)c);
        n += d * w.x * w.y * w.z;
    }
    return n;
}
float PerlinFbm(float3 p, int period, int oct)
{
    float s = 0.0, a = 0.5, norm = 0.0;
    [loop] for (int o = 0; o < oct; ++o) { s += Perlin(p, period) * a; norm += a; a *= 0.5; period *= 2; }
    return s / norm;
}
// Tileable Worley (cellular) noise: 1 at a cell point, 0 far from all - the "puffs".
float Worley(float3 p, int period)
{
    float3 pf = p * (float)period;
    int3   i  = (int3)floor(pf);
    float3 f  = frac(pf);
    float  md = 1e9;
    [loop] for (int z = -1; z <= 1; ++z) [loop] for (int y = -1; y <= 1; ++y) [loop] for (int x = -1; x <= 1; ++x)
    {
        int3   c = int3(x, y, z);
        float3 q = Hash33(Wrap3(i + c, period)) + (float3)c - f;
        md = min(md, dot(q, q));
    }
    return 1.0 - saturate(sqrt(md));
}
float WorleyFbm(float3 p, int period)
{
    float s = 0.0, a = 0.625;
    [loop] for (int o = 0; o < 3; ++o) { s += Worley(p, period) * a; a *= 0.4; period *= 2; }
    return s;
}

[numthreads(8, 8, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int mode = (int)g_ClWind.w;
    if (mode == 0)
    {
        if (any(id >= 128u)) return;
        float3 p = (float3(id) + 0.5) / 128.0;
        float  pe = saturate(Remap(PerlinFbm(p, 4, 7), -0.45, 0.45, 0.0, 1.0));   // gradient noise peaks near +-0.5: stretched to 0..1
        float  w0 = WorleyFbm(p, 4);
        float  pw = Remap(pe, w0 - 1.0, 1.0, 0.0, 1.0);   // Perlin-Worley: puffs carved into the billows
        g_Out3D[id] = float4(saturate(pw), WorleyFbm(p, 6), WorleyFbm(p, 12), WorleyFbm(p, 24));
    }
    else if (mode == 1)
    {
        if (any(id >= 32u)) return;
        float3 p = (float3(id) + 0.5) / 32.0;
        g_Out3D[id] = float4(WorleyFbm(p, 3), WorleyFbm(p, 6), WorleyFbm(p, 12), 1.0);
    }
    else
    {
        if (any(id.xy >= 512u) || id.z > 0u) return;
        float2 uv = (float2(id.xy) + 0.5) / 512.0;
        float  cover  = saturate(Remap(PerlinFbm(float3(uv, 0.37), 3, 5), -0.4, 0.4, 0.0, 1.0));
        float  type   = saturate(Remap(PerlinFbm(float3(uv, 7.11), 2, 3), -0.4, 0.4, 0.0, 1.0));
        float  precip = saturate(Remap(PerlinFbm(float3(uv, 3.31), 4, 3), -0.4, 0.4, 0.0, 1.0));
        g_Out2D[id.xy] = float4(cover, type, precip, 1.0);
    }
}
