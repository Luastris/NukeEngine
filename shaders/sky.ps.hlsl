// The sky: a procedural gradient by view-ray elevation (top/horizon/ground) or the physical
// atmosphere (the camera's sky-view LUT, the planet where a ray hits it, everything beyond seen
// through the atmosphere's transmittance), plus optional stars, moon and sun disc.
#include "atmosphere.hlsli"
cbuffer SkyCB
{
    float4x4 g_InvVP;   // inverse(view*proj): clip -> world
    float4 g_CamPos;    // xyz = camera, w = sky mode (1 = procedural, 2 = physical)
    float4 g_Top; float4 g_Horizon; float4 g_Ground;   // g_Top.w = the eclipsing moon's offset from the sun (sun radii; >= 1000 = none); g_Ground.w = tonemap white point (LDR path)
    float4 g_Params;    // x = skyIntensity, y = sunIntensity
    float4 g_SunDir;    // xyz = direction the sun light travels, w = disc angular radius (radians)
    float4 g_SunCol;    // rgb = sun colour, w = glow strength
    float4 g_MoonDir;   // xyz = direction toward the moon, w = the boundless ocean's level (g_Horizon.w = 1 when one is live)
    float4 g_MoonParams;// x = amount (0 = hidden), y = angular radius (radians), z = phase (0/1 new, .5 full), w = scene is LDR (the sky tonemaps itself)
};
Texture2D    g_StarTex;            // optional equirectangular star panorama (g_Params.w = 1 when bound)
SamplerState g_StarTex_sampler;
Texture2D    g_MoonTex;            // moon disk texture (g_MoonParams.x > 0 when shown)
SamplerState g_MoonTex_sampler;
struct PSIn { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };
static const float SKY_PI = 3.14159265359;

float4 main(in PSIn i) : SV_Target
{
    float4 wp = mul(g_InvVP, float4(i.ndc, 1.0, 1.0));
    float3 dir = normalize(wp.xyz / wp.w);   // the direction matrix has no translation (precision far from the origin): no camera subtraction
    float  up  = dir.y;

    float3 sky;
    float3 trans  = 1.0;    // the atmosphere between the camera and space (sun, moon, stars are behind it)
    float  night  = 1.0;    // stars fade as the sky brightens
    bool   ground = false;  // the ray ends on the planet
    if (g_CamPos.w > 1.5)
    {   // physical: the sky-view LUT, the planet where the ray hits it
        float4 sv = AtmoSkyView(dir);
        sky = sv.rgb;
        float3 camKm = AtmoToKm(g_CamPos.xyz);
        float  tG = AtmoRaySphere(camKm, dir, AtmoRg());
        ground = tG >= 0.0;
        if (ground)
        {
            // A boundless ocean (the camera above OR under it): the planet below the horizon IS the sea, and at
            // grazing the sea is the sky mirrored (the ocean mesh's far edge stops short of the
            // horizon by a sliver; the planet's ground showed there as a bright line).
            if (g_Horizon.w > 0.5)
                sky += AtmoSkyView(float3(dir.x, -dir.y, dir.z)).rgb * sv.a;
            else
                sky += AtmoGroundRadiance(camKm + dir * tG, sv.a);
        }
        else
        {
            trans = AtmoViewTransmittance(camKm, dir);
            night = saturate(1.0 - dot(sky, float3(0.333, 0.333, 0.333)) * 15.0);
        }
    }
    else
    {
        sky = (up >= 0.0) ? lerp(g_Horizon.rgb, g_Top.rgb, pow(saturate(up), 0.5))
                          : lerp(g_Horizon.rgb, g_Ground.rgb, saturate(-up));
        sky *= g_Params.x;
    }

    // the eclipsing moon's disc: dark, in front of the stars and the sun (the air in front of it still glows)
    const float eclCover = EclipseCover(dir, normalize(-g_SunDir.xyz), g_SunDir.w, g_Top.w);
    if (g_Params.z > 0.0 && !ground && eclCover < 1.0)   // stars (fade in at night; behind the atmosphere and the moon)
    {
        if (g_Params.w > 0.5)   // custom equirectangular star panorama
        {
            float2 uv = float2(atan2(dir.z, dir.x) / (2.0 * SKY_PI) + 0.5, acos(clamp(dir.y, -1.0, 1.0)) / SKY_PI);
            sky += g_StarTex.Sample(g_StarTex_sampler, uv).rgb * g_Params.z * trans * night * (1.0 - eclCover);
        }
        else if (dir.y > -0.05 || g_CamPos.w > 1.5)   // procedural points (the upper hemisphere; anywhere off the planet in space)
        {
            float3 sp = dir * 300.0;
            float3 id = floor(sp);
            float  hsh = frac(sin(dot(id, float3(12.9898, 78.233, 37.719))) * 43758.5453);
            if (hsh > 0.994)
            {
                float3 fp = frac(sp) - 0.5;
                float  pt = saturate(1.0 - dot(fp, fp) * 8.0);
                sky += pt * pow(hsh, 6.0) * g_Params.z * 2.0 * trans * night * (1.0 - eclCover);
            }
        }
    }

    if (g_MoonParams.x > 0.0 && !ground)   // textured moon disk
    {
        float md = dot(dir, g_MoonDir.xyz);
        float cr = cos(g_MoonParams.y);
        if (md > cr)
        {
            float3 up = abs(g_MoonDir.y) > 0.95 ? float3(0, 0, 1) : float3(0, 1, 0);
            float3 tx = normalize(cross(up, g_MoonDir.xyz));
            float3 ty = cross(g_MoonDir.xyz, tx);
            float  s  = max(sin(g_MoonParams.y), 1e-4);
            float2 d2 = float2(dot(dir, tx), dot(dir, ty)) / s;     // -1..1 across the disk
            float2 uv = saturate(d2 * 0.5 + 0.5); uv.y = 1.0 - uv.y;
            float4 m  = g_MoonTex.Sample(g_MoonTex_sampler, uv);
            float  edge = smoothstep(cr, lerp(cr, 1.0, 0.03), md);  // soft rim
            // Phase: treat the disk as a sphere lit from a phase-derived direction.
            float3 ln  = float3(d2, sqrt(saturate(1.0 - dot(d2, d2))));   // sphere normal, +Z toward viewer
            float  pa  = g_MoonParams.z * 6.2831853;                      // 0 = new, PI (.5) = full
            float  lit = saturate(dot(ln, float3(sin(pa), 0.0, -cos(pa))));
            float3 moonCol = m.rgb * (lit + 0.03) * trans;                // earthshine on the dark limb; through the atmosphere
            if (g_CamPos.w > 1.5) sky += moonCol * (m.a * g_MoonParams.x * edge);   // physical: a body behind the air, the day sky adds over it
            else                  sky = lerp(sky, moonCol, m.a * g_MoonParams.x * edge);
        }
    }

    if (g_Params.y > 0.0 && !ground)   // sun disc + glow (through the atmosphere: red at the horizon)
    {
        // The disc has the authored angular size; the glow scales with it and has its OWN strength:
        // the light's intensity only brightens the disc, so a strong sun never bloats into the sky.
        float sd   = dot(dir, normalize(-g_SunDir.xyz));   // g_SunDir is the travel direction, so negate it
        float ang  = acos(clamp(sd, -1.0, 1.0));
        float size = g_SunDir.w;
        float disk = 1.0 - smoothstep(size * 0.9, size * 1.1, ang);
        float q    = ang / size;
        float halo = exp(-q * q * 0.25) * 0.6 + exp(-q * 0.15) * 0.06;   // a tight corona + a faint wide skirt
        float3 sunTerm = g_SunCol.rgb * (g_Params.y * disk * 2.5 + g_SunCol.w * halo * (1.0 - disk)) * trans;
        // the eclipse: the moon's disc slides over the sun, hiding the disc and the glow behind
        // it; the glow outside the moon is the corona (EclipseCover: the sun shafts use the same)
        sunTerm *= 1.0 - eclCover;
        sky += sunTerm;
    }

    // The procedural gradient is authored in display space: emitted raw. The physical sky is
    // linear radiance: on the LDR path (RGBA8 scene, post is passthrough) it tonemaps itself
    // exactly like world.ps (extended Reinhard, white point, sRGB), else the post pass does.
    if (g_CamPos.w > 1.5 && g_MoonParams.w > 0.5)
    {
        float W = max(g_Ground.w, 1e-3);
        sky = sky * (1.0 + sky / (W * W)) / (1.0 + sky);
        sky = pow(max(sky, 0.0), 1.0 / 2.2);
    }
    return float4(sky, 1.0);
}
