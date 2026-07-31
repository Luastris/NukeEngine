// The single wind/interaction bend implementation, shared by world.vs / gbuffer.vs / shadow.vs
// and by bend.cs (which refits the RT foliage BLAS each frame). Edit here only.
// g_WindV = (dir.xyz, strength); g_WindT = (time, pusherCount, volumeCount, _); g_WindP = (turbAmount, 1/turbScale, _, _);
// g_Push = pusher xyz + radius; g_Vol = 16 analytic volumes x 3 float4 (pos+radius | dir+strength | mode, falloff, seed).
cbuffer BendCB { float4 g_WindV; float4 g_WindT; float4 g_WindP; float4 g_Push[8]; float4 g_Vol[48]; };
float3 NukeBend(float3 wpos, float3 localPos, float4 custom, float3 pivot)
{
    float f = localPos.y * localPos.y;
    float wCoef = saturate(f * custom.z);
    float iCoef = saturate(f * custom.w);
    if (wCoef <= 0.0 && iCoef <= 0.0) return wpos;
    float phase = frac(sin(dot(pivot, float3(12.9898, 78.233, 37.719))) * 43758.5453) * 6.2831853;
    float3 off = float3(0.0, 0.0, 0.0);
    if (wCoef > 0.0)
    {
        // Global wind: traveling gust waves along the wind direction, per-blade flutter, spatial turbulence.
        float2 wdir2 = g_WindV.xz;
        float wl2 = length(wdir2);
        wdir2 = wl2 > 1e-4 ? wdir2 / wl2 : float2(1.0, 0.0);
        float along = dot(wpos.xz, wdir2);
        float wave = (0.55 + 0.45 * sin(along * 0.35 - g_WindT.x * 2.4 + phase * 0.15))
                   * (0.65 + 0.35 * sin(along * 0.11 - g_WindT.x * 0.9 + 1.7));
        float flutter = 0.8 + 0.2 * sin(g_WindT.x * (3.2 + 0.8 * frac(phase)) + phase);
        float3 wdisp = g_WindV.xyz;
        if (g_WindP.x > 0.0)
        {
            float ts = max(g_WindP.y, 0.01);
            wdisp += float3(sin(wpos.z * ts * 6.3 + g_WindT.x * 1.3),
                            0.0,
                            sin(wpos.x * ts * 5.1 - g_WindT.x * 1.1)) * g_WindP.x;
        }
        off += wdisp * (g_WindV.w * 0.025 * wave * flutter) * wCoef;
        // Analytic volumes: wind zones / force fields.
        int nv = (int)g_WindT.z;
        [loop] for (int vi = 0; vi < nv; ++vi)
        {
            float4 v0 = g_Vol[vi * 3 + 0];       // pos, radius
            float3 dv = wpos - v0.xyz;
            float dist = length(dv);
            if (dist >= v0.w) continue;
            float4 v1 = g_Vol[vi * 3 + 1];       // dir, strength
            float4 v2 = g_Vol[vi * 3 + 2];       // mode, falloff, seed
            float wgt = 1.0 - v2.y * (dist / v0.w);
            float3 disp = float3(0.0, 0.0, 0.0);
            int mode = (int)v2.x;
            if (mode == 0) disp = v1.xyz * v1.w;                              // directional
            else
            {
                float3 rad = float3(dv.x, 0.0, dv.z);
                float rl = length(rad);
                float3 rn = rl > 1e-4 ? rad / rl : float3(1.0, 0.0, 0.0);
                if (mode == 1)      disp = rn * v1.w;                          // radial (suction < 0)
                else if (mode == 2) disp = float3(-rn.z, 0.0, rn.x) * v1.w;    // vortex around Y
                else                                                           // turbulence (animated)
                    disp = float3(sin(wpos.z * 2.1 + g_WindT.x * 3.0 + v2.z),
                                  0.0,
                                  sin(wpos.x * 1.7 + g_WindT.x * 2.6 + v2.z * 1.3)) * v1.w;
            }
            // Zones animate like the global wind: directional gets traveling waves along its own
            // direction, radial/vortex get rings from the centre; both share the per-blade flutter.
            float env = flutter;
            if (mode == 0)
            {
                float2 zd2 = v1.xz;
                float zl = length(zd2);
                zd2 = zl > 1e-4 ? zd2 / zl : float2(1.0, 0.0);
                float za = dot(wpos.xz, zd2);
                env *= (0.55 + 0.45 * sin(za * 0.5 - g_WindT.x * 2.6 + phase * 0.15))
                     * (0.7 + 0.3 * sin(za * 0.17 - g_WindT.x * 1.1 + 2.3));
            }
            else if (mode == 1 || mode == 2)
                env *= (0.6 + 0.4 * sin(dist * 0.6 - g_WindT.x * 2.8 + phase * 0.1));
            off += disp * (0.025 * wgt) * wCoef * env;
        }
    }
    if (iCoef > 0.0)
    {
        int n = (int)g_WindT.y;
        [loop] for (int k = 0; k < n; ++k)
        {
            float3 d = wpos - g_Push[k].xyz;
            d.y = 0.0;
            float dist = length(d);
            float r = g_Push[k].w;
            if (dist < r && dist > 1e-4)
            {
                float t = 1.0 - dist / r;
                off += (d / dist) * (t * t * 0.5) * iCoef;
            }
        }
    }
    return wpos + off;
}
