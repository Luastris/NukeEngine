// Unlit textured sprite: texture sample * per-vertex tint. Alpha blending is done by the PSO.
Texture2D    g_Sprite;
SamplerState g_Sprite_sampler;
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
float4 main(in PSIn i) : SV_TARGET
{
    return g_Sprite.Sample(g_Sprite_sampler, i.uv) * i.col;
}
