// Motion blur pass 1: per tile (g_Mb0.z px square), the longest velocity inside it, in pixels,
// scaled by the shutter and clamped to the max blur. Runs at tile resolution over the velocity target.
Texture2D<float2> g_Velocity;   // prepass motion (uv units: uv - prevUV), Load
cbuffer MbCB
{
    float4 g_Mb0;   // full w, full h, tile size (px), max blur (px)
    float4 g_Mb1;   // shutter, samples, 1/tiles w, 1/tiles h
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float2 main(in PSIn i) : SV_Target
{
    int2 tile = int2(i.pos.xy);
    int  ts = (int)g_Mb0.z;
    int2 o = tile * ts;
    float2 best = 0.0; float bestLen = 0.0;
    [loop] for (int y = 0; y < ts; ++y)
    [loop] for (int x = 0; x < ts; ++x)
    {
        int2 p = min(o + int2(x, y), int2(g_Mb0.xy) - 1);
        float2 v = g_Velocity.Load(int3(p, 0)) * g_Mb0.xy * g_Mb1.x;   // uv -> px, x shutter
        float l = length(v);
        if (l > bestLen) { bestLen = l; best = v; }
    }
    if (bestLen > g_Mb0.w) best *= g_Mb0.w / bestLen;
    return best;
}
