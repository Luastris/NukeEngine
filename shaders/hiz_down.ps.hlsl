// Hi-Z reduction: one mip down, MAX (farthest) of the 2x2 source footprint; an odd source edge
// folds its last column/row in so no texel is ever dropped. g_Src is a view of ONE mip.
Texture2D<float> g_Src;
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float main(in PSIn i) : SV_Target
{
    uint w, h; g_Src.GetDimensions(w, h);
    int2 p = int2(i.pos.xy) * 2;
    float d = g_Src.Load(int3(p, 0));
    d = max(d, g_Src.Load(int3(p + int2(1, 0), 0)));
    d = max(d, g_Src.Load(int3(p + int2(0, 1), 0)));
    d = max(d, g_Src.Load(int3(p + int2(1, 1), 0)));
    bool ox = (w & 1) && (p.x + 3 == (int)w);
    bool oy = (h & 1) && (p.y + 3 == (int)h);
    if (ox)
    {
        d = max(d, g_Src.Load(int3(p + int2(2, 0), 0)));
        d = max(d, g_Src.Load(int3(p + int2(2, 1), 0)));
    }
    if (oy)
    {
        d = max(d, g_Src.Load(int3(p + int2(0, 2), 0)));
        d = max(d, g_Src.Load(int3(p + int2(1, 2), 0)));
    }
    if (ox && oy) d = max(d, g_Src.Load(int3(p + int2(2, 2), 0)));
    return d;
}
