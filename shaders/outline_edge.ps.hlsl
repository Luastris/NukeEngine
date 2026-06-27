// Selection outline — fullscreen edge-detect. Reads the selection mask (alpha = selected). A pixel
// that is NOT selected but has a selected neighbour within `thickness` pixels is a border pixel ->
// outline colour. Constant pixel thickness => independent of distance / object size, works for any
// geometry (incl. flat planes).
Texture2D    g_Mask;
SamplerState g_Mask_sampler;
cbuffer EdgeCB { float4 g_Texel; };   // xy = 1/width, 1/height ; z = thickness in pixels
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(in PSIn i) : SV_Target
{
    if (g_Mask.Sample(g_Mask_sampler, i.uv).a > 0.5) discard;   // inside the object: no border
    float t = g_Texel.z;
    float m = 0.0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
        m = max(m, g_Mask.Sample(g_Mask_sampler, i.uv + float2(dx, dy) * g_Texel.xy * t).a);
    if (m > 0.5) return float4(1.0, 0.6, 0.1, 1.0);             // border
    discard;
    return float4(0, 0, 0, 0);
}
