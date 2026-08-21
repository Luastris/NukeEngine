// Hi-Z mip 0: the camera depth as a sampleable R32F. MSAA source: the FARTHEST sample per
// pixel (conservative for occlusion). Paired with post.vs.
#ifdef HIZ_MSAA
Texture2DMS<float> g_Depth;
#else
Texture2D<float>   g_Depth;
#endif
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float main(in PSIn i) : SV_Target
{
    int2 p = int2(i.pos.xy);
#ifdef HIZ_MSAA
    uint w, h, n; g_Depth.GetDimensions(w, h, n);
    float d = 0.0;
    for (uint s = 0; s < n; ++s) d = max(d, g_Depth.Load(p, s));
    return d;
#else
    return g_Depth.Load(int3(p, 0));
#endif
}
