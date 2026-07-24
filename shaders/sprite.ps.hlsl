// Unlit textured sprite: texture sample * per-vertex tint. Alpha blending is done by the PSO.
// SOFT PARTICLES (7.3): g_Soft = (fadeDist, near, far, enabled). When enabled, the fragment
// fades near scene geometry: both depths linearize (perspective D3D convention) and alpha
// scales by saturate((linScene - linFrag) / fadeDist). g_SceneDepth = the single-sample
// depth prepass (bound only while it exists this frame); g_Soft2.xy = 1/prepass size.
Texture2D    g_Sprite;
SamplerState g_Sprite_sampler;
Texture2D<float> g_SceneDepth;
cbuffer SpriteCB { float4x4 g_VP; float4 g_Soft; float4 g_Soft2; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
float LinD(float z) { return (g_Soft.z * g_Soft.y) / max(g_Soft.z - z * (g_Soft.z - g_Soft.y), 1e-6); }
float4 main(in PSIn i) : SV_TARGET
{
    float4 c = g_Sprite.Sample(g_Sprite_sampler, i.uv) * i.col;
    if (g_Soft.w > 0.5)
    {
        float zs = g_SceneDepth.Load(int3((int2)i.pos.xy, 0));
        float fade = saturate((LinD(zs) - LinD(i.pos.z)) / max(g_Soft.x, 1e-4));
        c *= fade;   // premultiplied-friendly: scales color AND alpha (works for both blend modes)
    }
    return c;
}
