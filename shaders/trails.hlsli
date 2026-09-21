// Ground trails (W5): the renderer's camera-following carve map (NukeDiligent_Trails.cpp).
// Per texel, how much of the accumulated layer (snow/sand) movers have pressed out: 0 = untouched,
// 1 = down to the receiving surface. Fresh fall fills it back. g_Trail = (origin x, origin z, 1/size, active).
cbuffer TrailsCB { float4 g_Trail; };
Texture2D<float> g_TrailMap;   // Load-only (R8)

float TrailCarve(float3 wp)
{
    if (g_Trail.w <= 0.0) return 0.0;
    float2 uv = float2((wp.x - g_Trail.x) * g_Trail.z, (wp.z - g_Trail.y) * g_Trail.z);
    if (any(uv < 0.0) || any(uv > 1.0)) return 0.0;
    uint w, h; g_TrailMap.GetDimensions(w, h);
    float2 p = uv * float2(w, h) - 0.5;
    int2 i0 = (int2)floor(p); float2 f = frac(p);
    int2 mx = int2((int)w - 1, (int)h - 1);
    float t00 = g_TrailMap.Load(int3(clamp(i0,               int2(0, 0), mx), 0));
    float t10 = g_TrailMap.Load(int3(clamp(i0 + int2(1, 0), int2(0, 0), mx), 0));
    float t01 = g_TrailMap.Load(int3(clamp(i0 + int2(0, 1), int2(0, 0), mx), 0));
    float t11 = g_TrailMap.Load(int3(clamp(i0 + int2(1, 1), int2(0, 0), mx), 0));
    return lerp(lerp(t00, t10, f.x), lerp(t01, t11, f.x), f.y);
}
// What is left of the layer's COVER at a point: the receiving surface shows through only where
// the track reaches it (the footprint floor), while its walls keep the layer's look.
float TrailCover(float3 wp) { float c = TrailCarve(wp); c *= c; return 1.0 - c * c; }
// Carve gradient per meter: the footprint walls, for lighting without tessellation.
float2 TrailGrad(float3 wp)
{
    if (g_Trail.w <= 0.0) return float2(0.0, 0.0);
    const float e = 0.08;
    return float2(TrailCarve(wp + float3(e, 0.0, 0.0)) - TrailCarve(wp - float3(e, 0.0, 0.0)),
                  TrailCarve(wp + float3(0.0, 0.0, e)) - TrailCarve(wp - float3(0.0, 0.0, e))) / (2.0 * e);
}
