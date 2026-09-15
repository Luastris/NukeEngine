// Ray-facing sprites: particle quads and trail ribbons sit in the TLAS as procedural AABBs, and
// every ray sees the quad turned toward itself (a triangle quad faces the camera only, so a
// mirror beside the camera saw it edge-on). A sprite is the segment a -> b with half-widths
// wA / wB, read from the 6-vertex quad the emitter built (this frame's copy in g_DynPos):
//   shape 1 (billboard): the whole quad faces the ray, its up axis = a -> b as far as the view allows
//   shape 0 / 2 (stretched sprite / ribbon): the axis a -> b stays, the quad rotates about it
// Shared by the DXR stages (rt_common) and every RayQuery consumer (world.ps, ao.ps, vol_inject).
#ifndef RT_SPRITE_HLSLI
#define RT_SPRITE_HLSLI
ByteAddressBuffer g_DynPos;   // per-frame sprite positions (float3/vertex, 6 per quad)

struct SpriteAttr { float2 uv; float along; };   // DXR hit attributes: texture uv, 0 at a .. 1 at b

// Hit test in world space. `along` interpolates the per-vertex colour (ribbon fade/taper).
bool SpriteHit(uint dynPosOffset, uint shape, uint prim, float3x4 o2w, float3 o, float3 d, float tMin, float tMax,
               out float t, out float2 uv, out float along)
{
    t = 0.0; uv = 0.0; along = 0.0;
    if (dynPosOffset == 0xFFFFFFFFu) return false;
    uint b0 = dynPosOffset + prim * 72u;   // 6 verts * 12 bytes; quad order v0 v1 v2 v0 v2 v3
    float3 v0 = mul(o2w, float4(asfloat(g_DynPos.Load3(b0)),       1.0));
    float3 v1 = mul(o2w, float4(asfloat(g_DynPos.Load3(b0 + 12u)), 1.0));
    float3 v2 = mul(o2w, float4(asfloat(g_DynPos.Load3(b0 + 24u)), 1.0));
    float3 v3 = mul(o2w, float4(asfloat(g_DynPos.Load3(b0 + 60u)), 1.0));
    float3 a = 0.5 * (v0 + v1), b = 0.5 * (v2 + v3);   // uv (.,1) end -> uv (.,0) end
    float  wA = 0.5 * length(v1 - v0), wB = 0.5 * length(v2 - v3);
    float3 ab = b - a; float len = length(ab);
    if (len < 1e-6 || max(wA, wB) < 1e-6) return false;   // an unused slot (zeros)
    float3 axis = ab / len;
    float3 right = v1 - v0;   // the raster's u direction: the image is never mirrored
    if (shape == 1u)
    {
        float3 n  = -d;                          // face the ray
        float3 up = axis - n * dot(axis, n);
        float  ul = length(up);
        float3 side;
        if (ul < 1e-3)                           // ray along the up axis: keep the right axis instead
        { side = right - n * dot(right, n); side = normalize(side + 1e-5); up = cross(n, side); }
        else { up = up / ul; side = normalize(cross(up, n)); }
        if (dot(side, right) < 0.0) side = -side;
        float3 c = 0.5 * (a + b);
        t = dot(c - o, n) / dot(d, n);
        if (t < tMin || t > tMax) return false;
        float3 p  = o + d * t - c;
        float  lu = dot(p, side) / wA, lv = dot(p, up) / (0.5 * len);
        if (abs(lu) > 1.0 || abs(lv) > 1.0) return false;
        along = 0.5 * (lv + 1.0);
        uv = float2(0.5 * (lu + 1.0), 1.0 - along);
        return true;
    }
    float3 side = cross(axis, d); float sl = length(side);
    if (sl < 1e-4) return false;                 // the ray runs along the axis: edge-on
    side /= sl;
    if (dot(side, right) < 0.0) side = -side;
    float3 n = cross(side, axis);
    float  denom = dot(d, n);
    if (abs(denom) < 1e-6) return false;
    t = dot(a - o, n) / denom;
    if (t < tMin || t > tMax) return false;
    float3 p = o + d * t - a;
    along = dot(p, axis) / len;
    if (along < 0.0 || along > 1.0) return false;
    float w  = lerp(wA, wB, along);
    float lu = dot(p, side) / max(w, 1e-6);
    if (abs(lu) > 1.0) return false;
    uv = float2(0.5 * (lu + 1.0), 1.0 - along);
    return true;
}
// Shadow footprint, as the triangle path: 1 = disc, 2 = strip across u, else the whole quad.
bool SpriteInside(uint shape, float2 uv)
{
    return (shape == 1u) ? (length(uv - 0.5) < 0.45) : (shape == 2u) ? (abs(uv.x - 0.5) < 0.45) : true;
}
#endif
