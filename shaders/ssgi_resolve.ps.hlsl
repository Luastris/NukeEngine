// SSGI resolve, two modes (g_Params.w): 0 = DENOISE at the trace resolution (5x5 depth + normal
// aware bilateral over the raw bounce), 1 = FINAL at full resolution (3x3 bilateral upsample,
// reprojection through the velocity with history rejected by depth, blend). Target 0 = (bounce
// E / pi, hit fraction) for the world PS, target 1 = history (rgb bounce, a hit fraction),
// target 2 = history depth (R16F) for the rejection
Texture2D    g_Source;    SamplerState g_Source_sampler;     // raw / denoised bounce at g_Params2.zw res
Texture2D    g_Depth;     SamplerState g_Depth_sampler;
Texture2D    g_GBuffer;   SamplerState g_GBuffer_sampler;
Texture2D    g_Velocity;  SamplerState g_Velocity_sampler;
Texture2D    g_History;   SamplerState g_History_sampler;    // last frame's history target (rgb bounce, a hit fraction)
Texture2D    g_HistoryZ;                                     // last frame's history depth, same sampler

struct PSOut { float4 color : SV_Target0; float4 hist : SV_Target1; float histZ : SV_Target2; };

cbuffer SSGICB
{
    float4x4 g_View;
    float4x4 g_Proj;
    float4x4 g_InvProj;
    float4   g_Res;        // THIS pass: w, h, 1/w, 1/h
    float4   g_Params;     // radius, intensity, rays, mode (0 denoise / 1 final)
    float4   g_Params2;    // history valid, history blend, source w, source h
};

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float3 OctDecode(float2 e)
{
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
float LinearZ(float2 uv)
{
    float d = g_Depth.Sample(g_Depth_sampler, uv).r;
    float z = 1e8;   // sky; single exit (FXC flags early returns as "potentially uninitialized")
    if (d < 0.99999)
    {
        float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
        float4 v = mul(g_InvProj, clip);
        z = abs(v.z / v.w);
    }
    return z;
}
float3 NormalAt(float2 uv) { return OctDecode(g_GBuffer.Sample(g_GBuffer_sampler, uv).xy); }
float SurfaceWeight(float zc, float3 nc, float zn, float3 nn)
{
    float wz = saturate(1.0 - abs(zn - zc) / (0.02 * zc + 0.02));
    float wn = pow(saturate(dot(nn, nc)), 8.0);
    return wz * wn;
}

float4 Denoise(float2 uv)
{
    float2 res = g_Params2.zw;
    float2 tc0 = floor(uv * res);
    float  zc  = LinearZ(uv);
    if (zc > 1e7) return 0.0;
    float3 nc  = NormalAt(uv);
    float4 sum = 0.0; float wsum = 0.0;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            float2 tc  = clamp(tc0 + float2(x, y), 0.0, res - 1.0);
            float2 tuv = (tc + 0.5) / res;
            float4 a   = g_Source.Load(int3((int2)tc, 0));
            float  ws  = exp(-(x * x + y * y) * 0.3);
            float  w   = ws * SurfaceWeight(zc, nc, LinearZ(tuv), NormalAt(tuv)) + 1e-5;
            sum += a * w; wsum += w;
        }
    }
    return sum / wsum;
}

PSOut Final(float2 uv)
{
    PSOut o;
    float depth = g_Depth.Sample(g_Depth_sampler, uv).r;
    if (depth >= 0.99999) { o.color = 0.0; o.hist = 0.0; o.histZ = 1e8; return o; }
    float  z = LinearZ(uv);
    float3 N = NormalAt(uv);
    float2 srcRes = g_Params2.zw;
    float2 rp     = uv * srcRes - 0.5;
    float2 center = floor(rp + 0.5);
    float4 sum = 0.0; float wsum = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float2 tc  = clamp(center + float2(x, y), 0.0, srcRes - 1.0);
            float2 tuv = (tc + 0.5) / srcRes;
            float4 s   = g_Source.Load(int3((int2)tc, 0));
            float2 dd  = tc - rp;
            float  w   = exp(-dot(dd, dd) * 0.7) * SurfaceWeight(z, N, LinearZ(tuv), NormalAt(tuv)) + 1e-5;
            sum += s * w; wsum += w;
        }
    }
    float4 gi = sum / wsum;   // rgb bounce, a hit fraction (the part of the far-field light these rays block)
    // Temporal: last frame's accumulated bounce where the surface is the same (depth), else fresh.
    if (g_Params2.x > 0.5)
    {
        float2 puv = uv - g_Velocity.Sample(g_Velocity_sampler, uv).rg;
        if (all(puv >= 0.0) && all(puv <= 1.0))
        {
            float4 h  = g_History.Sample(g_History_sampler, puv);
            float  hz = g_HistoryZ.Sample(g_History_sampler, puv).r;
            if (abs(hz - z) < 0.03 * z + 0.02) gi = lerp(gi, h, g_Params2.y);
        }
    }
    o.color = gi;
    o.hist  = gi;
    o.histZ = z;
    return o;
}

PSOut main(in PSIn i)
{
    if (g_Params.w < 0.5) { PSOut o; o.color = Denoise(i.uv); o.hist = 0.0; o.histZ = 0.0; return o; }
    return Final(i.uv);
}
