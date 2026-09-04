// DDGI raster fallback (no ray tracing): the World captured a probe's six cube faces with the
// normal world shading; alpha carries distance / max ray distance (world.ps, g_Misc.x). This
// pass turns the cube into the same ray set the update pass expects — one thread per ray:
// sample the cube along the frame's rotated Fibonacci direction, write (radiance, distance).
#include "ddgi.hlsli"

cbuffer GICB { GIVolumeGPU g_GIVol[DDGI_MAX_VOLUMES]; int4 g_GICount; float4 g_GIAtlasInv; };
cbuffer GIPassCB
{
    int4   g_GIPass;   // x = volume index, y = probe index, z = rays per probe, w = 0
    float4 g_GIRot;
    float4 g_GIMisc;   // x = max ray distance (= capture far plane), y = capture near plane
};
// FrameCB head (identical layout to world.ps up to the sky block): the miss radiance must match
// the ray-traced path (drawn sky when the sky is on, else the flat ambient colour).
#define MAX_LIGHTS 256
struct Light { float4 posType; float4 dirRange; float4 colorIntensity; float4 spot; };
cbuffer FrameCB { float4 g_CamPos; float4 g_Ambient; float4 g_LightCount; Light g_Lights[MAX_LIGHTS]; float4x4 g_ShadowVP[4]; float4 g_ShadowParams;
                  float4 g_SkyTop; float4 g_SkyHorizon; float4 g_SkyGround; float4 g_SkyParams; };
TextureCube g_CubeColor;  SamplerState g_CubeColor_sampler;
TextureCube<float> g_CubeBack;  SamplerState g_CubeBack_sampler;   // depth of EVERY surface (back faces included), point
RWStructuredBuffer<float4> g_RayData;

float3 RotateQ(float3 v, float4 q) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
float3 FibDir(uint i, uint n)
{
    float phi = 2.399963 * (float)i;
    float z   = 1.0 - (2.0 * i + 1.0) / (float)n;
    float r   = sqrt(max(0.0, 1.0 - z * z));
    return float3(r * cos(phi), r * sin(phi), z);
}
float3 CubeSkyColor(float3 dir)   // = world.ps SkyColor
{
    float up = dir.y;
    float3 c = (up >= 0.0) ? lerp(g_SkyHorizon.rgb, g_SkyTop.rgb, pow(saturate(up), 0.5))
                           : lerp(g_SkyHorizon.rgb, g_SkyGround.rgb, saturate(-up));
    return c * g_SkyParams.x;
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint rays = (uint)g_GIPass.z;
    if (tid.x >= rays) return;
    float3 dir = RotateQ(FibDir(tid.x, rays), g_GIRot);
    float4 c   = g_CubeColor.SampleLevel(g_CubeColor_sampler, dir, 0);
    float  d   = saturate(c.a) * g_GIMisc.x;
    float3 rad = max(c.rgb, 0.0);
    // A miss is the environment the ray-traced path sees: the analytic sky (the drawn sun disc /
    // moon / stars are not light here - the sun arrives as direct light on the hits), or the flat
    // ambient colour when the sky is off (the clear colour is not light).
    if (c.a >= 0.999) rad = (g_SkyParams.y > 0.5) ? CubeSkyColor(dir) : g_Ambient.rgb;
    // Back-face test: the depth pass drew both sides of every surface; a nearer hit there than in
    // the (front-face) colour capture means the ray started inside geometry. Stored as the
    // shortened negative distance the trace path uses, so the update pass classifies the probe.
    float bd = g_CubeBack.SampleLevel(g_CubeBack_sampler, dir, 0);
    float n = g_GIMisc.y, f = g_GIMisc.x;
    float linZ = n * f / max(f - bd * (f - n), 1e-6);
    float backDist = linZ / max(max(abs(dir.x), abs(dir.y)), abs(dir.z));
    float rayDist  = (c.a >= 0.999) ? f : d;
    bool  backface = bd < 0.99999 && backDist < rayDist - max(0.05, 0.03 * rayDist);
    g_RayData[(uint)g_GIPass.y * rays + tid.x] = float4(rad, backface ? -max(backDist * 0.2, 0.01) : max(d, 0.01));
}
