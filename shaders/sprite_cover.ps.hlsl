// Sprite coverage for the ray-traced reflection composite: how much of the pixel a translucent
// sprite covers (texture alpha x tint alpha), accumulated as a union (ONE / INV_SRC_ALPHA) into
// an R8 mask, depth-tested against the G-buffer depth. The tracer keeps that share of the base
// colour: a puff in front of a mirror hides that much of the reflection instead of being erased.
Texture2D    g_Sprite;
SamplerState g_Sprite_sampler;
Texture2D    g_Mask;   SamplerState g_Mask_sampler;   // alpha mask (setSpriteMask), white = none
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; float3 wpos : TEXCOORD1; };
float main(in PSIn i) : SV_TARGET
{
    return saturate(g_Sprite.Sample(g_Sprite_sampler, i.uv).a * g_Mask.Sample(g_Mask_sampler, i.uv).a * i.col.a);
}
