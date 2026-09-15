// Water crossed by a reflection ray. The surface is not in the TLAS (the flat rest plane stands
// in for the geometry), but its WAVES are: the water module publishes its cascade normal maps
// and ripple sim (SetRTWaterMaps), so a ray from above that crosses the plane is shaded as the
// real surface - the reflection of the world traced from the wave normal (objects, particles,
// sky), the body colour through it by Fresnel, the sun's glints. A ray from below keeps the
// earlier approximation (attenuation + the surface seen from underneath). Included by the
// closest-hit / miss shaders only: TraceRay is not allowed in any-hit / intersection stages.
#ifndef RT_WATER_SHADE_HLSLI
#define RT_WATER_SHADE_HLSLI

// The wave normal at a surface point: the three cascades (distance-faded like water.ps) + the
// ripple sim's central differences over its window.
float3 RTWaterNormal(float3 P)
{
    float4 c = g_RTWaterCasc;   // cascade sizes, waveScale (slope scale)
    float4 n0 = g_WaterNrm0.SampleLevel(g_WaterNrm0_sampler, P.xz / c.x, 0);
    float4 n1 = g_WaterNrm1.SampleLevel(g_WaterNrm1_sampler, P.xz / c.y, 0);
    float4 n2 = g_WaterNrm2.SampleLevel(g_WaterNrm2_sampler, P.xz / c.z, 0);
    float dist = length(P - g_RTCam.xyz);
    float f1 = 0.25 + 0.75 * exp(-dist / 500.0);
    float f2 = 0.15 + 0.85 * exp(-dist / 120.0);
    float2 slope = (n0.xy + n1.xy * f1 + n2.xy * f2) * c.w;
    if (g_RTWaterRip1.y > 0.5)
    {
        float2 uv = (P.xz - g_RTWaterRip0.xy) * g_RTWaterRip0.w + 0.5;
        if (uv.x > 0.002 && uv.x < 0.998 && uv.y > 0.002 && uv.y < 0.998)
        {
            float e = g_RTWaterRip0.w * g_RTWaterRip1.z;   // one sim texel in UV
            float hx = g_WaterRipple.SampleLevel(g_WaterRipple_sampler, uv + float2(e, 0.0), 0)
                     - g_WaterRipple.SampleLevel(g_WaterRipple_sampler, uv - float2(e, 0.0), 0);
            float hz = g_WaterRipple.SampleLevel(g_WaterRipple_sampler, uv + float2(0.0, e), 0)
                     - g_WaterRipple.SampleLevel(g_WaterRipple_sampler, uv - float2(0.0, e), 0);
            slope += float2(hx, hz) * (0.5 / max(g_RTWaterRip1.z, 1e-3));
        }
    }
    return normalize(float3(-slope.x, 1.0, -slope.y));
}

// Does the segment o -> o + d * tMax cross the rest plane from above? P = the crossing.
bool RTWaterCrossAbove(float3 o, float3 d, float tMax, out float3 P)
{
    P = o;
    if (g_RTWater.y < 0.5 || o.y <= g_RTWater.x || d.y >= -1e-5) return false;
    float t = (g_RTWater.x - o.y) / d.y;
    if (t <= 0.0 || t >= tMax) return false;
    P = o + d * t;
    return true;
}

// Shade the crossing: `beyond` = the radiance that arrived from under the surface (already
// attenuated), lumT = its transmittance (0 = nothing came through), depth = the ray's recursion.
float3 RTWaterShade(float3 P, float3 d, float3 beyond, float lumT, uint depth)
{
    float3 N = RTWaterNormal(P);
    float3 V = -d;
    float  F = 0.02 + 0.98 * pow(1.0 - saturate(dot(N, V)), 5.0);
    float3 R = reflect(d, N);
    if (R.y < 0.02) R = normalize(float3(R.x, 0.02, R.z));   // the reflected leg stays above the plane
    float3 refl;
    if (depth < (uint)g_RTParams.z)
    {
        RayDesc ray; ray.Origin = P + N * 0.05 + R * 0.05; ray.Direction = R; ray.TMin = 0.02;
        ray.TMax = (g_RTParams.y > 0.5) ? g_RTParams.y : 1000.0;
        RTPayload p2; p2.color = 0.0; p2.depth = depth + 1; p2.hitT = ray.TMax;
        TraceRay(g_TLAS, RAY_FLAG_NONE, RT_REFLECT_MASK, 0, 1, 0, ray, p2);
        refl = p2.color;
    }
    else refl = EnvSample(R, 0.1);
    // The body's in-scatter (as RTWaterLook) fills what the depth swallowed.
    float3 amb = (g_SkyParams.y > 0.5)
               ? (g_SkyTop.rgb + 2.0 * g_SkyHorizon.rgb + g_SkyGround.rgb) * 0.25 * g_SkyParams.x * g_Ambient.w
               : g_Ambient.rgb * g_Ambient.w;
    float3 sunC = float3(0.0, 0.0, 0.0);
    float best = -1.0;
    [loop] for (int li = 0; li < (int)g_LightCount.x; ++li)
        if (g_Lights[li].posType.w < 0.5)
        {
            float3 cc = g_Lights[li].colorIntensity.rgb * g_Lights[li].colorIntensity.w;
            float lum = dot(cc, float3(0.299, 0.587, 0.114));
            if (lum > best) { best = lum; sunC = cc; }
        }
    float3 body = g_RTWaterCol.rgb * min(amb * 0.8 + sunC * 0.35, 1.8) * 0.8;
    // Sun / spot glints: the lights loop with F0 0.02 (black albedo: specular only), shadowed.
    float3 glint = ShadeSurface(P, N, V, float3(0.0, 0.0, 0.0), 0.0, 0.06, float3(0.0, 0.0, 0.0), 1.0, float3(0.5, 0.5, 0.5));
    return lerp(beyond + body * (1.0 - lumT), refl, F) + glint;
}

// The tail of every hit shader: `col` was shaded at hitPos, reached along d from o.
float3 RTWaterFinish(float3 o, float3 d, float3 hitPos, float3 col, uint depth)
{
    float3 wT = RTWaterTrans3(o, hitPos);
    float  lumT = dot(wT, float3(0.299, 0.587, 0.114));
    float3 P;
    if (RTWaterCrossAbove(o, d, length(hitPos - o), P)) return RTWaterShade(P, d, col * wT, lumT, depth);
    return col * wT + RTWaterLook(d) * (1.0 - lumT);
}
// The miss shader's tail: `env` is the environment the ray escaped to after tMax.
float3 RTWaterFinishMiss(float3 o, float3 d, float tMax, float3 env, uint depth)
{
    float3 P;
    if (RTWaterCrossAbove(o, d, tMax, P)) return RTWaterShade(P, d, float3(0.0, 0.0, 0.0), 0.0, depth);   // nothing below: the body
    float3 wT = RTWaterTransRay(o, d, tMax);
    return env * wT + RTWaterLook(d) * (1.0 - dot(wT, float3(0.299, 0.587, 0.114)));
}
#endif
