// Sky occlusion: a top-down orthographic depth of the opaque world around the camera, captured
// by the renderer before the camera passes (NukeDiligent_SkyOcc.cpp). SkyOpen() = how open a
// world point is to the sky (1 = nothing above it): sky-borne conditions (rain wet, snow, dust)
// settle only there, so roofs shelter what is under them.
// g_SkyOcc = (origin x, origin z, 1/size, active); g_SkyOcc2 = (eye Y, near, far - near, bias in depth units).
cbuffer SkyOccCB { float4 g_SkyOcc; float4 g_SkyOcc2; };
Texture2D<float> g_SkyOccMap;   // Load-only: the capture's depth (0 = eye, 1 = far)

float SkyOpen(float3 wp)
{
    if (g_SkyOcc.w <= 0.0) return 1.0;
    float2 uv = float2((wp.x - g_SkyOcc.x) * g_SkyOcc.z, 1.0 - (wp.z - g_SkyOcc.y) * g_SkyOcc.z);
    if (any(uv < 0.0) || any(uv > 1.0)) return 1.0;   // beyond the capture: open
    // This point's depth, biased toward the eye: the point itself (and anything within the bias)
    // never shades it; an occluder more than two biases above blocks it fully.
    float zs = (g_SkyOcc2.x - wp.y - g_SkyOcc2.y) / g_SkyOcc2.z - g_SkyOcc2.w;
    uint w, h; g_SkyOccMap.GetDimensions(w, h);
    float2 p = uv * float2(w, h) - 0.5;
    int2 i0 = (int2)floor(p); float2 f = frac(p);
    int2 mx = int2((int)w - 1, (int)h - 1);
    float t00 = saturate((g_SkyOccMap.Load(int3(clamp(i0,               int2(0, 0), mx), 0)) - zs) / g_SkyOcc2.w + 1.0);
    float t10 = saturate((g_SkyOccMap.Load(int3(clamp(i0 + int2(1, 0), int2(0, 0), mx), 0)) - zs) / g_SkyOcc2.w + 1.0);
    float t01 = saturate((g_SkyOccMap.Load(int3(clamp(i0 + int2(0, 1), int2(0, 0), mx), 0)) - zs) / g_SkyOcc2.w + 1.0);
    float t11 = saturate((g_SkyOccMap.Load(int3(clamp(i0 + int2(1, 1), int2(0, 0), mx), 0)) - zs) / g_SkyOcc2.w + 1.0);
    return lerp(lerp(t00, t10, f.x), lerp(t01, t11, f.x), f.y);
}

// The sky gate of one point: open to the sky, and not an underside (nothing falls up).
float SkyGate(float3 wp, float3 ng) { return SkyOpen(wp) * saturate(1.0 + ng.y * 2.0); }
