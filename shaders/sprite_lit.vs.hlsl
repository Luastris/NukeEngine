// Lit sprite quad: world-space position + uv + tint (same batch layout as sprite.vs),
// plus the world position passed through for point/spot attenuation in the PS.
cbuffer SpriteCB { float4x4 g_VP; };
struct VSIn { float3 pos : ATTRIB0; float2 uv : ATTRIB1; float4 col : ATTRIB2; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; float3 wpos : TEXCOORD1; };
void main(in VSIn i, out PSIn o)
{
    o.pos  = mul(g_VP, float4(i.pos, 1.0));
    o.uv   = i.uv;
    o.col  = i.col;
    o.wpos = i.pos;
}
