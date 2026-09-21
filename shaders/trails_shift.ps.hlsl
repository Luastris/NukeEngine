// Ground trails - window step: carry the carve map into this frame's window (a texel shift as
// the window follows the camera) and let fresh fall fill the tracks back in.
Texture2D<float> g_Prev;
cbuffer TrailSimCB
{
    float4 g_TS0;   // shift x, shift z (texels), fill this step, previous window valid
    float4 g_TS1;   // origin x, origin z, 1/size, texels per side
    float4 g_TSImp[256];
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float main(in PSIn i) : SV_TARGET
{
    if (g_TS0.w < 0.5) return 0.0;
    const int n = (int)g_TS1.w;
    int2 src = int2(i.pos.xy) + int2((int)round(g_TS0.x), (int)round(g_TS0.y));
    if (any(src < 0) || any(src >= n)) return 0.0;
    return saturate(g_Prev.Load(int3(src, 0)) - g_TS0.z);
}
