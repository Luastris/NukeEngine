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
cbuffer SpriteCB { float4x4 g_VP; float4 g_Soft; float4 g_Soft2; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; float3 wpos : TEXCOORD1; };
float LinD(float z) { return (g_Soft.z * g_Soft.y) / max(g_Soft.z - z * (g_Soft.z - g_Soft.y), 1e-6); }
float4 main(in PSIn i) : SV_TARGET
{
    float4 c = g_Sprite.Sample(g_Sprite_sampler, i.uv) * i.col;
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
