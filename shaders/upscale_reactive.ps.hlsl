// Reactive mask for the temporal upscalers (internal resolution, R8): how little a pixel's history
// can be trusted. Motion vectors describe the opaque surface; what is painted over it moves
// otherwise - a water surface (its reflection / refraction slide against the surface's own
// motion) and the translucent sprites (particles, no velocity at all). 0 = accumulate as usual,
// 1 = this frame only; the vendors ask for at most ~0.9.
Texture2D g_GBuffer;   // the prepass G-buffer (octN.xy, roughness, metal; the water's G-pass writes metal -1)
Texture2D g_Cover;     // the pass's sprite coverage (sprite_cover.ps; a zero texture when none)
Texture2D g_CoverWorld;   // the transparent / additive world draws (gbuffer.ps coverage mode; zero when none)
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float main(in PSIn i) : SV_TARGET
{
    const int3 p = int3((int2)i.pos.xy, 0);
    const float water  = (g_GBuffer.Load(p).w < -0.5) ? 0.8 : 0.0;
    const float sprite = saturate(g_Cover.Load(p).r) * 0.9;
    const float world  = saturate(g_CoverWorld.Load(p).r) * 0.9;
    return max(water, max(sprite, world));
}
