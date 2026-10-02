// Unlit textured sprite: texture sample * per-vertex tint; alpha blending is done by the PSO.
// Soft particles: g_Soft = (fadeDist, near, far, enabled). Volumetric fog (g_Soft2 = (grid
// active, light amount, 0, 0)): the sprite is lit by the froxel grid's incident light (smoke sits
// in the god rays) and fogged by its own column (see VolFogTranslucent).
// g_SceneDepth is the single-sample depth prepass, bound while it exists this frame.
#include "vol.hlsli"
Texture2D    g_Sprite;
SamplerState g_Sprite_sampler;
Texture2D    g_Mask;   SamplerState g_Mask_sampler;   // alpha mask (setSpriteMask), white = none
Texture2D<float> g_SceneDepth;
Texture3D<float4> g_VolInteg; SamplerState g_VolInteg_sampler;   // integrated fog columns (T, L)
Texture3D<float4> g_VolLight; SamplerState g_VolLight_sampler;   // incident light per froxel
cbuffer SpriteCB { float4x4 g_VP; float4 g_Soft; float4 g_Soft2; float4 g_Sdf; float4 g_Outline; float4 g_Clip; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; float3 wpos : TEXCOORD1; };
float LinD(float z) { return (g_Soft.z * g_Soft.y) / max(g_Soft.z - z * (g_Soft.z - g_Soft.y), 1e-6); }
float4 main(in PSIn i) : SV_TARGET
{
    // Clip rect (target pixels, x0 < x1 = active): canvas masks / scroll views.
    if (g_Clip.x < g_Clip.z && (i.pos.x < g_Clip.x || i.pos.x > g_Clip.z || i.pos.y < g_Clip.y || i.pos.y > g_Clip.w)) discard;
    float4 c;
    if (g_Sdf.x > 0.5)
    {
        // SDF text (canvas widgets): red = signed distance, 0.5 = the glyph edge. Anti-aliased by
        // the screen-space derivative, an optional outline band below the edge.
        float d  = g_Sprite.Sample(g_Sprite_sampler, i.uv).r;
        float aa = max(fwidth(d), 1e-4) * 0.7 + g_Sdf.y;
        float a  = smoothstep(0.5 - aa, 0.5 + aa, d);
        float3 rgb = i.col.rgb; float alpha = a;
        if (g_Sdf.z > 0.0)
        {
            float ao = smoothstep(0.5 - g_Sdf.z - aa, 0.5 - g_Sdf.z + aa, d);
            rgb = lerp(g_Outline.rgb, i.col.rgb, a);
            alpha = max(a, ao * g_Outline.a);
        }
        c = float4(rgb, alpha * i.col.a);
    }
    else
        c = g_Sprite.Sample(g_Sprite_sampler, i.uv) * i.col;
    c.a *= g_Mask.Sample(g_Mask_sampler, i.uv).a;
    float zs = g_SceneDepth.Load(int3((int2)i.pos.xy, 0));
    if (g_Soft.w > 0.5)
    {
        float fade = saturate((LinD(zs) - LinD(i.pos.z)) / max(g_Soft.x, 1e-4));
        c *= fade;   // scales color AND alpha, so it is correct for both blend modes
    }
    if (g_Soft2.x > 0.5)
    {
        float2 uv = i.pos.xy / g_VolScreen.xy;
        float  zp = VolLinearZ(i.pos.z);
        if (g_Soft2.y > 0.0)
        {
            float3 li = g_VolLight.SampleLevel(g_VolLight_sampler, float3(uv, VolW(zp)), 0).rgb;
            c.rgb *= lerp(1.0, li, g_Soft2.y);
        }
        float zo = (zs >= 0.99999) ? g_VolRange.y : VolLinearZ(zs);
        c.rgb = VolFogTranslucent(g_VolInteg, g_VolInteg_sampler, c.rgb, uv, zp, zo);
    }
    return c;
}
