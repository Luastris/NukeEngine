// Nuke material interface: spatial masks + masked ("twin") props — ANY shader supports the
// LiveMaterial mask system out of the box by declaring
//     cbuffer MatCB { #include "matcb_std.hlsli" };
//     #include "nuke_material.hlsli"
// Twin convention: for a maskable prop g_X declare float4 g_XT (xyz = tweened value,
// w = maskSlot+1; 0 = off) and blend with NukeMasked(). Stages without texture access
// (domain shaders) `#define NUKE_MAT_NO_TEX` before including and use NukeMaskWNoTex().

// Analytic uv->world frame OVERRIDE for stages without derivatives (domain shader): the
// caller sets these from the patch corners before evaluating masks; zero = the pixel stages
// derive the frame from ddx/ddy instead.
static float3 g_NukeMaskT = float3(0, 0, 0);
static float3 g_NukeMaskB = float3(0, 0, 0);

// Analytic mask weight (circle/ring); stamps degrade to circles here.
float NukeMaskShape(float4 A, float4 B, float4 C, float2 uv, float3 wpos)
{
    const int  mode  = (int)(A.w + 0.5);
    const bool world = (mode & 4) != 0;
    const float R    = max(B.x, 1e-4);
    float2 d2; float dist;
    if (world) { d2 = wpos.xz - A.xz; dist = length(wpos - A.xyz); }
    else
    {
        // UV masks wrap across the texture seam — the mesh surface is continuous there, so
        // the effect must carry over. The seam period in TRANSFORMED uv is the uv tiling.
        const float2 per = (abs(g_UVT.x) + abs(g_UVT.y) < 1e-6) ? float2(1.0, 1.0) : abs(g_UVT.xy);
        d2 = uv - A.xy;
        d2 -= round(d2 / per) * per;
        // UV masks size like the TEXTURE: `scale` is a fraction of the uv layout, so the
        // effect scales WITH the object (identical look on the 1 m preview sample and on a
        // scaled-up game mesh). The surface frame corrects the SHAPE only — an aspect-only
        // factor keeps circles round under uv stretch (sphere equator) without turning the
        // size into absolute meters. Frame: g_NukeMask overrides (domain shader,
        // patch-analytic) or ddx/ddy (pixel stages).
        float3 Tj = g_NukeMaskT, Bj = g_NukeMaskB;
#ifndef NUKE_MAT_NO_TEX
        if (dot(Tj, Tj) + dot(Bj, Bj) < 1e-12)
        {
            const float3 pdx = ddx(wpos), pdy = ddy(wpos);
            const float2 udx = ddx(uv),  udy = ddy(uv);
            const float  jdet = udx.x * udy.y - udx.y * udy.x;
            if (abs(jdet) > 1e-12)
            {
                Tj = ( pdx * udy.y - pdy * udx.y) / jdet;   // d wpos / du
                Bj = (-pdx * udy.x + pdy * udx.x) / jdet;   // d wpos / dv
            }
        }
#endif
        const float lt = length(Tj), lb = length(Bj);
        if (lt > 1e-9 && lb > 1e-9)
        {
            const float ar = clamp(sqrt(lt / lb), 0.25, 4.0);   // clamped: poles stay finite
            d2 = float2(d2.x * ar, d2.y / ar);
        }
        dist = length(d2);
    }
    const float soft = max(C.x, 1e-3);
    float w;
    if ((mode & 3) == 1)                       // ring(s): the edge sits at dist == scale
    {
        float rr   = dist / R;
        float ph   = B.y > 0.0 ? frac(rr * (B.y + 1.0)) : rr;
        float edge = B.y > 0.0 ? abs(ph - 0.5) * 2.0 : abs(rr - 1.0);
        w = (1.0 - smoothstep(0.0, soft, edge)) * step(dist, R * (1.0 + soft));
    }
    else                                       // filled circle
        w = 1.0 - smoothstep(R * (1.0 - soft), R, dist);
    if (B.w > 0.0) w *= saturate(1.0 - (dist / R) * B.w);   // radial fade
    return saturate(w) * saturate(C.y);
}

float NukeMaskWNoTex(int i, float2 uv, float3 wpos)
{
    if (i == 0) return NukeMaskShape(g_MskA0, g_MskB0, g_MskC0, uv, wpos);
    if (i == 1) return NukeMaskShape(g_MskA1, g_MskB1, g_MskC1, uv, wpos);
    if (i == 2) return NukeMaskShape(g_MskA2, g_MskB2, g_MskC2, uv, wpos);
    if (i == 3) return NukeMaskShape(g_MskA3, g_MskB3, g_MskC3, uv, wpos);
    if (i == 4) return NukeMaskShape(g_MskA4, g_MskB4, g_MskC4, uv, wpos);
    if (i == 5) return NukeMaskShape(g_MskA5, g_MskB5, g_MskC5, uv, wpos);
    return 0.0;
}

#ifndef NUKE_MAT_NO_TEX
Texture2D g_MskStamp;   // stamp of the material's FIRST stamp-shaped mask

// Full mask weight of slot i: stamp shapes sample the stamp inside the (rotated) radius.
float NukeMaskW(int i, float2 uv, float3 wpos, Texture2D stamp, SamplerState smp)
{
    float4 A, B, C;
    if      (i == 0) { A = g_MskA0; B = g_MskB0; C = g_MskC0; }
    else if (i == 1) { A = g_MskA1; B = g_MskB1; C = g_MskC1; }
    else if (i == 2) { A = g_MskA2; B = g_MskB2; C = g_MskC2; }
    else if (i == 3) { A = g_MskA3; B = g_MskB3; C = g_MskC3; }
    else if (i == 4) { A = g_MskA4; B = g_MskB4; C = g_MskC4; }
    else if (i == 5) { A = g_MskA5; B = g_MskB5; C = g_MskC5; }
    else return 0.0;
    const int mode = (int)(A.w + 0.5);
    if ((mode & 3) == 2 && C.z > 0.5)
    {
        const bool world = (mode & 4) != 0;
        const float R = max(B.x, 1e-4);
        float2 l = world ? wpos.xz - A.xz : uv - A.xy;
        if (!world)   // stamps wrap across the texture seam too (same period as above)
        {
            const float2 per = (abs(g_UVT.x) + abs(g_UVT.y) < 1e-6) ? float2(1.0, 1.0) : abs(g_UVT.xy);
            l -= round(l / per) * per;
            // Same aspect-only shape correction as NukeMaskShape: stamps keep their aspect
            // under uv stretch while their SIZE stays a fraction of the uv layout.
            const float3 pdx = ddx(wpos), pdy = ddy(wpos);
            const float2 udx = ddx(uv),  udy = ddy(uv);
            const float  jdet = udx.x * udy.y - udx.y * udy.x;
            if (abs(jdet) > 1e-12)
            {
                const float3 Tj = ( pdx * udy.y - pdy * udx.y) / jdet;
                const float3 Bj = (-pdx * udy.x + pdy * udx.x) / jdet;
                const float lt = length(Tj), lb = length(Bj);
                if (lt > 1e-9 && lb > 1e-9)
                {
                    const float ar = clamp(sqrt(lt / lb), 0.25, 4.0);
                    l = float2(l.x * ar, l.y / ar);
                }
            }
        }
        float sr, cr; sincos(-B.z, sr, cr);
        l = float2(l.x * cr - l.y * sr, l.x * sr + l.y * cr);
        const float2 suv = l / (2.0 * R) + 0.5;
        float w = (all(suv >= 0.0) && all(suv <= 1.0)) ? stamp.SampleLevel(smp, suv, 0).r : 0.0;
        if (B.w > 0.0) w *= saturate(1.0 - (length(l) / R) * B.w);
        return saturate(w) * saturate(C.y);
    }
    return NukeMaskShape(A, B, C, uv, wpos);
}

// Masked prop: base -> twin.xyz by the twin's mask weight (twin.w = slot+1; 0 = off).
float3 NukeMasked(float3 base, float4 twin, float2 uv, float3 wpos, Texture2D stamp, SamplerState smp)
{
    if (twin.w < 0.5) return base;
    return lerp(base, twin.xyz, NukeMaskW((int)(twin.w - 0.5), uv, wpos, stamp, smp));
}
#endif
