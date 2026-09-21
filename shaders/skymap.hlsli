// The sky map: an equirectangular panorama of the whole sky the renderer refreshes once a frame
// (NukeDiligent_SkyMap.cpp) - the physical atmosphere or the procedural gradient WITH the
// volumetric clouds, no sun disc (the sun is a light: the surfaces add its glints themselves).
// Everything that needs "the sky in a direction" samples it: traced rays that escape (rt_rmiss),
// the water's reflection, world.ps image-based lighting. Its mips blur by roughness. FrameCB
// g_Misc.z = 1 while the map is live (the water carries the flag in its own CB). Declares nothing:
// the caller declares the texture and passes it with a linear (wrap) sampler.
#ifndef SKYMAP_HLSLI
#define SKYMAP_HLSLI
static const float SKYMAP_PI = 3.14159265359;
float2 SkyMapUv(float3 d)
{
    d = normalize(d);
    return float2(atan2(d.z, d.x) / (2.0 * SKYMAP_PI) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / SKYMAP_PI);
}
float3 SkyMapDir(float2 uv)
{
    float phi = (uv.x - 0.5) * 2.0 * SKYMAP_PI, theta = uv.y * SKYMAP_PI;
    float s = sin(theta);
    return float3(cos(phi) * s, cos(theta), sin(phi) * s);
}
float3 SkyMapSample(Texture2D t, SamplerState s, float3 d, float rough)
{
    return t.SampleLevel(s, SkyMapUv(d), saturate(rough) * 8.0).rgb;   // 2048 x 1024: mip 8 = 8 x 4
}
#endif
