// Volumetric clouds, the shadow map: the sun's transmittance through the layer for every
// ground point of a square around the camera (g_ClShadow: origin x, z, 1/size). Each texel
// marches from its ground point toward the sun through the layer's segment (coarse density,
// no detail: a shadow is a soft thing) and stores exp(-tau). The surface shaders multiply the
// sun's shadow by it (FrameCB g_CloudShadow), so cloud shadows sweep the ground.
#include "clouds.hlsli"

RWTexture2D<float> g_ShadowOut;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const int2 res = (int2)g_ClScreen.xy;
    if (any((int2)id.xy >= res)) return;
    float2 uv = (float2(id.xy) + 0.5) / g_ClScreen.xy;
    float  size = 1.0 / max(g_ClShadow.z, 1e-6);
    float3 p = float3(g_ClShadow.x + uv.x * size, 0.0, g_ClShadow.y + uv.y * size);   // sea level: the layer is kilometres up
    float3 L = g_ClSunDir.xyz;
    float  T = 1.0;
    float  tA, tB;
    if (L.y > 0.02 && CloudSegment(p, L, 1e7, tA, tB))
    {
        const int N = 24;
        float step = (tB - tA) / (float)N;
        float t = tA + step * 0.5;
        float tau = 0.0;
        [loop] for (int i = 0; i < N; ++i)
        {
            float hf;
            tau += CloudDensity(p + L * t, false, hf) * step;
            t += step;
        }
        T = exp(-tau * CL_SIGMA * g_ClCover.z);
    }
    g_ShadowOut[id.xy] = T;
}
