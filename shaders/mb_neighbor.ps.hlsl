// Motion blur pass 2: the longest velocity among a tile's 3x3 neighbours, so a moving object's
// blur reaches into the tiles it streaks across.
Texture2D<float2> g_Source;   // tile max (mb_tilemax), Load
cbuffer MbCB { float4 g_Mb0; float4 g_Mb1; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float2 main(in PSIn i) : SV_Target
{
    int2 t = int2(i.pos.xy);
    int2 n = int2(round(1.0 / g_Mb1.zw));
    float2 best = 0.0; float bestLen = -1.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 p = clamp(t + int2(x, y), int2(0, 0), n - 1);
        float2 v = g_Source.Load(int3(p, 0));
        float l = length(v);
        if (l > bestLen) { bestLen = l; best = v; }
    }
    return best;
}
