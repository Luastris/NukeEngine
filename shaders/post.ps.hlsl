// FINAL post stage: tonemap + encode only. Effects (exposure / grade / vignette / custom) run BEFORE this as
// the post-effect chain (see *.post.hlsl). g_Post.y = mode: 0 passthrough (HDR off), 1 SDR (Reinhard+sRGB),
// 2 HDR10 (nits -> Rec2020 -> PQ).
Texture2D    g_HDR;
SamplerState g_HDR_sampler;
cbuffer PostCB { float4 g_Post; float4 g_Grade; };   // only g_Post.y (mode) used here
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

static const float3x3 Rec709toRec2020 =
{
    0.627402, 0.329292, 0.043306,
    0.069095, 0.919544, 0.011360,
    0.016394, 0.088028, 0.895578
};
float3 LinearToPQ(float3 L)   // L = nits / 10000 -> PQ (SMPTE ST 2084)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 Lm = pow(max(L, 0.0), m1);
    return pow((c1 + c2 * Lm) / (1.0 + c3 * Lm), m2);
}

float4 main(in PSIn i) : SV_Target
{
    float3 c    = g_HDR.Sample(g_HDR_sampler, i.uv).rgb;
    float  mode = g_Post.y;

    if (mode > 1.5)   // HDR10: linear HDR -> nits -> Rec2020 -> PQ
    {
        const float paperWhite = 200.0, peak = 1000.0;
        float3 nits = min(c * paperWhite, peak);
        nits = mul(Rec709toRec2020, nits);
        return float4(LinearToPQ(nits / 10000.0), 1.0);
    }
    if (mode > 0.5)   // SDR: Reinhard + sRGB gamma
    {
        c = c / (c + 1.0);
        c = pow(max(c, 0.0), 1.0 / 2.2);
        return float4(c, 1.0);
    }
    return float4(c, 1.0);   // passthrough (HDR off; scene already tonemapped in world.ps)
}
