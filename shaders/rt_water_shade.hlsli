// Water crossed by a reflection ray. The surface is not in the TLAS (the flat rest plane stands
// in for the geometry), but its WAVES are: the water module publishes its cascade normal maps
// and ripple sim (SetRTWaterMaps), so a ray from above that crosses the plane is shaded as the
// real surface - the reflection of the world traced from the wave normal (objects, particles,
// sky), the body colour through it by Fresnel, the sun's glints. A ray from BELOW (a submerged
// mirror, a leg that dived) is the underwater post's view: the surface from underneath (the
// world above through the refracted leg, the underwater world in the reflected leg past the
// critical angle), the column's fog by Opacity Depth, the photon caustics on what it hits.
// Included by the closest-hit / miss shaders only: TraceRay is not allowed in any-hit /
// intersection stages.
#ifndef RT_WATER_SHADE_HLSLI
#define RT_WATER_SHADE_HLSLI

// The wave normal at a surface point: the three cascades (distance-faded like water.ps) + the
// ripple sim's central differences over its window. `d` = the ray that reached P: the cascades
// are read at the mip the pixel's footprint on the surface asks for (the raster path gets this
// from its derivatives) - at mip 0 the fine cascades alias at range, the reflected rays scatter
// under the plane and the horizon smears into streaks.
float3 RTWaterNormal(float3 P, float3 d)
{
    float4 c = g_RTWaterCasc;   // cascade sizes, waveScale (slope scale)
    float dist = length(P - g_RTCam.xyz);
    const float foot = dist * g_RTWaterCol.w / max(abs(d.y), 0.05);   // metres per pixel on the surface, the grazing stretch included
    const float l0 = log2(max(foot / (c.x / 256.0), 1.0));
    const float l1 = log2(max(foot / (c.y / 256.0), 1.0));
    const float l2 = log2(max(foot / (c.z / 256.0), 1.0));
    float4 n0 = g_WaterNrm0.SampleLevel(g_WaterNrm0_sampler, P.xz / c.x, l0);
    float4 n1 = g_WaterNrm1.SampleLevel(g_WaterNrm1_sampler, P.xz / c.y, l1);
    float4 n2 = g_WaterNrm2.SampleLevel(g_WaterNrm2_sampler, P.xz / c.z, l2);
    float f1 = 0.25 + 0.75 * exp(-dist / 500.0);
    float f2 = 0.15 + 0.85 * exp(-dist / 120.0);
    float2 slope = (n0.xy + n1.xy * f1 + n2.xy * f2) * c.w;
    {   // capillary ripples, as nukewater_slope.hlsl WaterCapillary (the same maps, the same scroll)
        float fade = exp(-dist / 35.0);
        if (fade > 0.02)
        {
            const float sA = c.z * 0.11, sB = c.z * 0.053, time = g_RTCam.w;
            float2 pA = (P.xz + float2(0.37, -0.21) * time) / sA;
            float2 rB = float2(P.x * 0.866 - P.z * 0.5, P.x * 0.5 + P.z * 0.866);
            float2 pB = (rB + float2(-0.29, 0.33) * time) / sB;
            slope += (g_WaterNrm2.SampleLevel(g_WaterNrm2_sampler, pA, 0).xy * 0.55 + g_WaterNrm2.SampleLevel(g_WaterNrm2_sampler, pB, 0).xy * 0.35) * fade * c.w;
        }
    }
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
    if (g_RTVortexN.x > 0.5)
    {
        float fe = max(foot, 0.1);
        slope += float2(RTFunnelH(P.xz + float2(fe, 0.0)) - RTFunnelH(P.xz - float2(fe, 0.0)),
                        RTFunnelH(P.xz + float2(0.0, fe)) - RTFunnelH(P.xz - float2(0.0, fe))) * (0.5 / fe);
    }
    return normalize(float3(-slope.x, 1.0, -slope.y));
}

// Does the segment o -> o + d * tMax cross the rest plane from above? P = the crossing. A ray
// entering a whirlpool's hole goes on down to the funnel's wall: a march from the plane to the
// deepest point, then a bisection, so the mirror sees the funnel as the hole it is.
bool RTWaterCrossAbove(float3 o, float3 d, float tMax, out float3 P)
{
    P = o;
    if (g_RTWater.y < 0.5 || o.y <= g_RTWater.x || d.y >= -1e-5) return false;
    float t = (g_RTWater.x - o.y) / d.y;
    if (t <= 0.0 || t >= tMax) return false;
    if (g_RTVortexN.x > 0.5 && RTFunnelH((o + d * t).xz) < -1e-3)
    {
        const float tEnd = (g_RTWater.x - g_RTVortexN.y - o.y) / d.y;
        const float stepT = max(tEnd - t, 0.0) / 12.0;
        float lo = t, hi = tEnd;
        [loop] for (int i = 1; i <= 12; ++i)
        {
            float tt = t + stepT * (float)i;
            float3 Q = o + d * tt;
            if (Q.y <= g_RTWater.x + RTFunnelH(Q.xz)) { hi = tt; break; }
            lo = tt;
        }
        [loop] for (int i = 0; i < 5; ++i)
        {
            float mid = (lo + hi) * 0.5;
            float3 Q = o + d * mid;
            if (Q.y <= g_RTWater.x + RTFunnelH(Q.xz)) hi = mid; else lo = mid;
        }
        t = hi;
        if (t >= tMax) return false;   // the floor stands in the hole, dry
    }
    P = o + d * t;
    return true;
}

// Shade the crossing: `beyond` = the radiance that arrived from under the surface (already
// attenuated), lumT = its transmittance (0 = nothing came through), depth = the ray's recursion.
float3 RTWaterShade(float3 P, float3 d, float3 beyond, float lumT, uint depth)
{
    float3 N = RTWaterNormal(P, d);
    float3 V = -d;
    float  F = 0.02 + 0.98 * pow(1.0 - saturate(dot(N, V)), 5.0);
    float3 R = reflect(d, N);
    if (R.y < 0.02) R = normalize(float3(R.x, 0.02, R.z));   // the reflected leg stays above the plane
    float3 refl;
    if (depth < (uint)g_RTParams.z)
    {
        RayDesc ray; ray.Origin = P + N * 0.05 + R * 0.05; ray.Direction = R; ray.TMin = 0.02;
        ray.TMax = (g_RTParams.y > 0.5) ? g_RTParams.y : 1000.0;
        RTPayload p2 = RTNewPayload(depth + 1u, ray.TMax, 0.1, RT_PAY_SURFACE);   // into the air off the surface
        TraceRay(g_TLAS, RAY_FLAG_NONE, RT_REFLECT_MASK, 0, 1, 0, ray, p2);
        RTCompose(p2);
        refl = p2.color;
    }
    else refl = EnvSample(R, 0.1);
    // The body's in-scatter (as RTWaterLook) fills what the depth swallowed.
    float3 amb = (g_SkyParams.y > 0.5)
               ? (g_SkyTop.rgb + 2.0 * g_SkyHorizon.rgb + g_SkyGround.rgb) * 0.25 * g_SkyParams.x * g_Ambient.w
               : g_Ambient.rgb * g_Ambient.w;
    float3 sunC = float3(0.0, 0.0, 0.0), sunL = float3(0.0, 1.0, 0.0);
    float best = -1.0;
    [loop] for (int li = 0; li < (int)g_LightCount.x; ++li)
        if (g_Lights[li].posType.w < 0.5)
        {
            float3 cc = g_Lights[li].colorIntensity.rgb * g_Lights[li].colorIntensity.w;
            float lum = dot(cc, float3(0.299, 0.587, 0.114));
            if (lum > best) { best = lum; sunC = cc; sunL = normalize(-g_Lights[li].dirRange.xyz); }
        }
    // The body's light is shadowed like the raster surface's (water.ps sunSh): under a hull or
    // a bridge the water is dark in the mirror too.
    float sunSh = (best > 0.0) ? RTShadow(P + N * 0.05 + sunL * 0.05, sunL, 1.0e4) : 1.0;
    float3 body = g_RTWaterCol.rgb * min(amb * 0.8 + sunC * 0.35 * sunSh, 1.8) * 0.8;
    // Sun / spot glints: the lights loop with F0 0.02 (black albedo: specular only), shadowed.
    float3 glint = ShadeSurface(P, N, V, float3(0.0, 0.0, 0.0), 0.0, 0.06, float3(0.0, 0.0, 0.0), 1.0, float3(0.5, 0.5, 0.5));
    float3 shaded = lerp(beyond + body * (1.0 - lumT), refl, F) + glint;
    // Foam, the raster law (water.ps): the cascades' whitecaps + the wake field reveal the lace.
    {
        float4 c = g_RTWaterCasc;
        float dist = length(P - g_RTCam.xyz);
        float foot = dist * g_RTWaterCol.w / max(abs(d.y), 0.05);
        float f1 = 0.25 + 0.75 * exp(-dist / 500.0);
        float f2 = 0.15 + 0.85 * exp(-dist / 120.0);
        float cov = saturate(g_WaterNrm0.SampleLevel(g_WaterNrm0_sampler, P.xz / c.x, log2(max(foot / (c.x / 256.0), 1.0))).w
                           + g_WaterNrm1.SampleLevel(g_WaterNrm1_sampler, P.xz / c.y, log2(max(foot / (c.y / 256.0), 1.0))).w * f1
                           + g_WaterNrm2.SampleLevel(g_WaterNrm2_sampler, P.xz / c.z, log2(max(foot / (c.z / 256.0), 1.0))).w * 0.35 * f2);
        if (g_RTFoam.w > 0.0)
        {
            float2 uv = (P.xz - g_RTWaterRip0.xy) * g_RTWaterRip0.w + 0.5;
            float2 eb = min(uv, 1.0 - uv);
            float fw = saturate(min(eb.x, eb.y) * 12.0);
            if (fw > 0.0) cov = saturate(cov + g_WaterSimFoam.SampleLevel(g_WaterSimFoam_sampler, uv, 0.0) * fw * 0.9);
        }
        // A simulated whirlpool: its broken water, and the lace drawn through the carried coordinates.
        float2 laceP = P.xz;
        if (g_RTWhirl.x > 0.5)
        {
            [loop] for (int i = 0; i < (int)g_RTVortexN.x; ++i)
            {
                float2 uv = (P.xz - g_RTWhirlGrid[i].xy) * g_RTWhirlGrid[i].z + 0.5;
                if (uv.x <= 0.0 || uv.y <= 0.0 || uv.x >= 1.0 || uv.y >= 1.0) continue;
                float4 st = g_WaterWhirl.SampleLevel(g_WaterWhirl_sampler, float3(uv, (float)i), 0.0);
                cov = saturate(cov + st.w);
                float4 o = g_WaterWhirlUV.SampleLevel(g_WaterWhirl_sampler, float3(uv, (float)i), 0.0);
                laceP += lerp(o.xy, o.zw, abs(g_RTWhirl.y * 2.0 - 1.0)) * saturate(length(st.yz) / 0.4);
            }
        }
        if (cov > 0.003)
        {
            const float t = g_RTVortexN.z;
            float lodA = log2(max(foot / (5.3 / 1024.0), 1.0)), lodB = log2(max(foot / (14.1 / 1024.0), 1.0));
            float2 rB = float2(laceP.x * 0.809 - laceP.y * 0.588, laceP.x * 0.588 + laceP.y * 0.809);
            float3 a = g_WaterLace.SampleLevel(g_WaterLace_sampler, laceP / 5.3 + float2(0.007, 0.004) * t, lodA).xyz;
            float3 b = g_WaterLace.SampleLevel(g_WaterLace_sampler, rB / 14.1 + float2(-0.004, 0.006) * t, lodB).xyz;
            b.yz = float2(b.y * 0.809 + b.z * 0.588, b.z * 0.809 - b.y * 0.588);
            float3 lace = a * 0.72 + b * 0.28;
            float thr = 0.92 - cov * abs(g_RTFoam.w) * 1.12;
            float soft = 0.05 + 0.3 * saturate(dist * 0.01);
            float bodyF = saturate((lace.x - thr) * 2.5);
            float fA = smoothstep(thr - soft, thr + soft, lace.x);
            float3 Nf = normalize(N + float3(-lace.y, 0.0, -lace.z) * 18.0);
            float3 foamLit = g_RTFoam.rgb * lerp(0.7, 1.0, bodyF) * (amb * 0.95 + sunC * sunSh * saturate(dot(Nf, sunL)) * 0.32);
            shaded = lerp(shaded, foamLit, fA * (0.45 + 0.5 * bodyF));
        }
    }
    return shaded;
}

// ---- from below -----------------------------------------------------------------------------

// The eye's ambient for the water column (the same probe the underwater post reads).
float3 RTUnderAmb(float3 P)
{
    float3 irr;
    if (g_GICount.x > 0 && DDGISample(g_GIIrr, g_GIVis, g_GIIrr_sampler, g_GIVol, g_GICount.x, g_GIAtlasInv.xy, g_GIAtlasInv.zw, P, float3(0.0, 1.0, 0.0), float3(0.0, 1.0, 0.0), irr)) return irr;
    return (g_SkyParams.y > 0.5)
         ? (g_SkyTop.rgb + 2.0 * g_SkyHorizon.rgb + g_SkyGround.rgb) * 0.25 * g_SkyParams.x * g_Ambient.w
         : g_Ambient.rgb * g_Ambient.w;
}
// `seen` reached the eye along `run` metres of water. The SAME law and numbers as waterunder.ps:
// the Scatter Color tints the haze, Deep Color acts on the seen surface;
// the Scatter Color alpha scales the scattering; the column is lit by the eye's ambient AND the
// sun that reaches its midpoint through the water above it (shadowed by a shadow ray), so a
// mirror shows the water the eye sees.
float3 RTUnderFog(float3 seen, float run, float3 mid)
{
    if (g_RTWaterCau1.y < 0.5) return seen;   // Underwater Fog off: clear water
    const float fade = g_RTWaterAbs.w;        // 1 / Opacity Depth
    const float tint = max(g_RTWaterCau1.z, 0.0);
    float sink = 1.0 - exp(-run * 1.5 * tint * fade);
    float3 T = exp(-g_RTWaterAbs.rgb * (run * 3.0 * fade)) * (1.0 - sink);
    float3 scatter = (tint > 1e-3) ? g_RTWaterCol.rgb / tint : float3(0.0, 0.0, 0.0);   // g_RTWaterCol carries scatter x tint
    float3 light = RTUnderAmb(g_RTCam.xyz) * 2.4;
    {   // the brightest directional, through the water above the column's midpoint
        float3 sunC = float3(0.0, 0.0, 0.0), sunL = float3(0.0, 1.0, 0.0);
        float best = -1.0;
        [loop] for (int li = 0; li < (int)g_LightCount.x; ++li)
            if (g_Lights[li].posType.w < 0.5)
            {
                float3 cc = g_Lights[li].colorIntensity.rgb * g_Lights[li].colorIntensity.w;
                float lum = dot(cc, float3(0.299, 0.587, 0.114));
                if (lum > best) { best = lum; sunC = cc; sunL = normalize(-g_Lights[li].dirRange.xyz); }
            }
        if (best > 0.0 && sunL.y > 0.05)
        {
            float  h = max(g_RTWater.x - mid.y, 0.0);
            float3 Tsun = exp(-g_RTWaterAbs.rgb * (h / sunL.y) * 3.0 * fade);
            light += sunC * Tsun * RTShadow(mid + sunL * 0.05, sunL, 1.0e4) * 0.35;
        }
    }
    return seen * T + scatter * sink * light;
}

// Does the segment o -> o + d * tMax cross the rest plane from BELOW? P = the crossing. An
// origin inside the wave band (above the rest level) heading up is AT the surface: P = o.
// A whirlpool's wall is met first: the water ends there, not at the level.
bool RTWaterCrossBelow(float3 o, float3 d, float tMax, out float3 P)
{
    P = o;
    if (!RTUnderEye(o) || d.y <= 1e-5) return false;
    float t = max(g_RTWater.x - o.y, 0.0) / d.y;
    if (g_RTVortexN.x > 0.5 && t > 0.0)
    {
        const float stepT = t / 12.0;
        float lo = 0.0, hi = t;
        bool met = false;
        [loop] for (int i = 1; i <= 12 && !met; ++i)
        {
            float tt = stepT * (float)i;
            float3 Q = o + d * tt;
            if (Q.y >= g_RTWater.x + RTFunnelH(Q.xz)) { hi = tt; met = true; } else lo = tt;
        }
        if (met)
        {
            [loop] for (int i = 0; i < 5; ++i)
            {
                float mid = (lo + hi) * 0.5;
                float3 Q = o + d * mid;
                if (Q.y >= g_RTWater.x + RTFunnelH(Q.xz)) hi = mid; else lo = mid;
            }
            t = hi;
        }
    }
    if (t >= tMax) return false;
    P = o + d * t;
    return true;
}

// Caustics on submerged geometry, the underwater post's law (nukewater_caustic.hlsl): the tile is
// indexed by where the sun enters the water above the hit, the mip comes from the ray's footprint
// alone, a fold glows and does not clip.
float3 RTWaterCaustic(float3 hitPos, float3 col, float run)
{
    if (g_RTWaterCau.w <= 0.001 || hitPos.y >= g_RTWater.x) return col;
    float2 flatDev = float2(0.0, 0.0);
    [loop] for (int li = 0; li < (int)g_LightCount.x; ++li)
    {
        if (g_Lights[li].posType.w >= 0.5) continue;
        float3 sd = normalize(g_Lights[li].dirRange.xyz);
        if (sd.y >= -0.05) continue;
        const float eta = 1.0 / 1.333;
        float k = 1.0 - eta * eta * (1.0 - sd.y * sd.y);
        flatDev = sd.xz * eta / max(sqrt(max(k, 0.0)), 0.2);
        break;   // first directional above the horizon, as WaterGpu::RunWaterCaustic picks it
    }
    float depBelow = g_RTWater.x - hitPos.y;
    float2 uv = (hitPos.xz - flatDev * depBelow - g_RTWaterCau.xy) * g_RTWaterCau.z + 0.5;
    float2 eb = min(uv, 1.0 - uv);
    float cw = saturate(min(eb.x, eb.y) * 10.0) * saturate(depBelow / 0.08) * exp(-run / 30.0);
    if (cw <= 0.0) return col;
    uint side, sideH; g_WaterCaustic.GetDimensions(side, sideH);
    float texel = 1.0 / (g_RTWaterCau.z * (float)side);
    float lod = max(log2(max(run * g_RTWaterCol.w, 1e-5) / texel), 0.0);
    float f = pow(max(g_WaterCaustic.SampleLevel(g_WaterCaustic_sampler, uv, lod), 0.0), g_RTWaterCau1.x);
    if (f > 1.0) f = 1.0 + (f - 1.0) / (1.0 + (f - 1.0) / 3.0);   // kCausticPeak
    return col * max(1.0 + (f - 1.0) * g_RTWaterCau.w * cw, 0.0);
}

// The surface seen from underneath at P (d = the ray, heading up): the world above through the
// refracted leg (Snell, water -> air), the underwater world in the reflected leg, mixed by the
// exact Fresnel of the interface (total internal reflection past the critical angle).
float3 RTWaterShadeBelow(float3 P, float3 d, uint depth)
{
    float3 N = RTWaterNormal(P, d);               // up
    const float eta = 1.333;                      // n_water / n_air
    float  ci  = saturate(dot(d, N));             // cos of incidence, from below
    float  st2 = eta * eta * (1.0 - ci * ci);     // sin^2 of the transmitted angle
    float  F   = 1.0;                             // past the critical angle: a perfect mirror
    float3 above = float3(0.0, 0.0, 0.0);
    const float maxD = (g_RTParams.y > 0.5) ? g_RTParams.y : 1000.0;
    if (st2 < 1.0)
    {
        float ct = sqrt(1.0 - st2);
        float rs = (eta * ci - ct) / (eta * ci + ct);
        float rp = (eta * ct - ci) / (eta * ct + ci);
        F = 0.5 * (rs * rs + rp * rp);
        float3 Tdir = refract(d, -N, eta);
        if (depth < (uint)g_RTParams.z)
        {
            RayDesc ray; ray.Origin = P + N * 0.05; ray.Direction = Tdir; ray.TMin = 0.02; ray.TMax = maxD;
            RTPayload p2 = RTNewPayload(depth + 1u, maxD, 0.0, RT_PAY_SURFACE);   // the transmitted leg is in the air
            TraceRay(g_TLAS, RAY_FLAG_NONE, RT_REFLECT_MASK, 0, 1, 0, ray, p2);
            RTCompose(p2);
            above = p2.color;
        }
        else above = EnvMiss(Tdir);
    }
    float3 below = float3(0.0, 0.0, 0.0);
    if (F > 0.002)
    {
        float3 R = reflect(d, N);                 // back down into the water
        if (depth < (uint)g_RTParams.z)
        {
            RayDesc ray; ray.Origin = P - N * 0.05; ray.Direction = R; ray.TMin = 0.02; ray.TMax = maxD;
            RTPayload p2 = RTNewPayload(depth + 1u, maxD, 0.0, 0u);   // back down: under water
            TraceRay(g_TLAS, RAY_FLAG_NONE, RT_REFLECT_MASK, 0, 1, 0, ray, p2);   // its own tail fogs the run
            RTCompose(p2);
            below = p2.color;
        }
        else below = RTUnderFog(float3(0.0, 0.0, 0.0), 1.0e4, P);   // the column at infinity
    }
    return lerp(above, below, F);
}

// A submerged eye's leg: what it saw at the end of `run` metres of water, then the column.
// Runs the fog has swallowed (T below 0.03 %) skip the shading (with the fog on: clear water
// shows the far world - the TIR mirror of the bottom - and must trace it).
float3 RTUnderLeg(float3 o, float3 d, float run, bool crossed, float3 P, float3 hitPos, float3 col, uint depth)
{
    float3 mid = o + d * min(run, 60.0) * 0.5;
    if (g_RTWaterCau1.y > 0.5 && run * 1.5 * g_RTWaterAbs.w > 8.0) return RTUnderFog(float3(0.0, 0.0, 0.0), run, mid);
    float3 seen = crossed ? RTWaterShadeBelow(P, d, depth) : RTWaterCaustic(hitPos, col, run);
    return RTUnderFog(seen, run, mid);
}

// The tail of every hit shader: `col` was shaded at hitPos, reached along d from o.
float3 RTWaterFinish(float3 o, float3 d, float3 hitPos, float3 col, uint depth, uint flags)
{
    if (flags & RT_PAY_SURFACE) return col;   // left the surface into the air (its foot is inside the wave band, not under water)
    if (RTUnderEye(o))
    {
        float len = length(hitPos - o);
        float3 P;
        bool crossed = RTWaterCrossBelow(o, d, len, P);
        return RTUnderLeg(o, d, crossed ? length(P - o) : len, crossed, P, hitPos, col, depth);
    }
    float3 P;
    if (RTWaterCrossAbove(o, d, length(hitPos - o), P))
    {
        // The water starts at P: above a funnel's wall the ray ran through air.
        float3 wTP = RTWaterTrans3(P, hitPos);
        return RTWaterShade(P, d, col * wTP, dot(wTP, float3(0.299, 0.587, 0.114)), depth);
    }
    if (RTInHole(hitPos)) return col;   // the floor a whirlpool has laid dry
    float3 wT = RTWaterTrans3(o, hitPos);
    float  lumT = dot(wT, float3(0.299, 0.587, 0.114));
    return col * wT + RTWaterLook(d) * (1.0 - lumT);
}
// The miss shader's tail: `env` is the environment the ray escaped to after tMax.
float3 RTWaterFinishMiss(float3 o, float3 d, float tMax, float3 env, uint depth, uint flags)
{
    if (flags & RT_PAY_SURFACE) return env;   // left the surface into the air: the sky, nothing crossed
    if (RTUnderEye(o))
    {
        // Heading up it always meets the surface (the plane is boundless); heading down it
        // found nothing: the column alone.
        float3 PB;
        if (RTWaterCrossBelow(o, d, 1.0e9, PB))   // inside the band: at the surface already
            return RTUnderLeg(o, d, length(PB - o), true, PB, o, env, depth);
        return RTUnderLeg(o, d, tMax, false, o, o + d * tMax, env, depth);
    }
    float3 P;
    // A ray from above heading down that found nothing before tMax would cross the water beyond
    // it: the surface is still there (the ocean reaches the horizon), so shade it at the far
    // crossing rather than show the planet under the horizon.
    if (RTWaterCrossAbove(o, d, 1.0e9, P)) return RTWaterShade(P, d, float3(0.0, 0.0, 0.0), 0.0, depth);   // nothing below: the body
    float3 wT = RTWaterTransRay(o, d, tMax);
    return env * wT + RTWaterLook(d) * (1.0 - dot(wT, float3(0.299, 0.587, 0.114)));
}
#endif
