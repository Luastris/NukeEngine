// Software cursor VS: one screen-space quad from SV_VertexID — no vertex buffer.
// g_Rect = (x, y, w, h) in pixels (already hotspot-adjusted), g_Screen = (sw, sh, _, _).
cbuffer CursorCB { float4 g_Rect; float4 g_Screen; };

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

static const float2 kCorners[6] = {
    float2(0, 0), float2(1, 0), float2(1, 1),
    float2(0, 0), float2(1, 1), float2(0, 1)
};

void main(uint vid : SV_VertexID, out PSIn o)
{
    float2 c = kCorners[vid];
    float2 px = g_Rect.xy + c * g_Rect.zw;
    o.pos = float4(px.x / g_Screen.x * 2.0 - 1.0, 1.0 - px.y / g_Screen.y * 2.0, 0.0, 1.0);
    o.uv  = c;
}
