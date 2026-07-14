// Sprite quad: world-space position + uv + tint, one view*proj transform (matches debug lines).
cbuffer SpriteCB { float4x4 g_VP; };
struct VSIn { float3 pos : ATTRIB0; float2 uv : ATTRIB1; float4 col : ATTRIB2; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
void main(in VSIn i, out PSIn o)
{
    o.pos = mul(g_VP, float4(i.pos, 1.0));
    o.uv  = i.uv;
    o.col = i.col;
}
