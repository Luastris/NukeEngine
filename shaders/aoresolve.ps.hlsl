// AO resolve, two modes of one shader (g_AOParams.w): 0 = DENOISE at the raw resolution (5x5
// depth + normal aware bilateral over the raw visibility; g/b carry the raw 3x3 min/max for
// the temporal clamp), 1 = FINAL at full resolution (3x3 bilateral upsample of the denoised
// result, reprojection through the prepass velocity, history rejected by depth and clamped
// to the RAW neighbourhood box, then intensity/power). Target 0: rgb shaped visibility for
// the shading, a accumulated; target 1: the history for next frame (r accumulated, g depth).
Texture2D    g_Source;    SamplerState g_Source_sampler;     // raw / denoised visibility at g_AOParams2.zw res
Texture2D    g_Depth;     SamplerState g_Depth_sampler;      // full-res prepass depth, point
Texture2D    g_GBuffer;   SamplerState g_GBuffer_sampler;    // full-res octahedral normals, point
Texture2D    g_Velocity;  SamplerState g_Velocity_sampler;   // full-res motion (uv units), point
Texture2D    g_History;   SamplerState g_History_sampler;    // last frame's history target (r ao, g linear depth), linear

struct PSOut { float4 color : SV_Target0; float4 hist : SV_Target1; };

cbuffer AOCB
{
    float4x4 g_View;
    float4x4 g_Proj;
    float4x4 g_InvProj;
    float4x4 g_InvView;
    float4   g_AORes;      // THIS pass: w, h, 1/w, 1/h
    float4   g_AOParams;   // radius, intensity, power, mode (0 denoise / 1 final)
    float4   g_AOParams2;  // history valid, history blend, source w, source h
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

// Linear view depth of a full-res uv (huge for the sky, so it weighs nothing).
float LinearZ(float2 uv)
{
    float d = g_Depth.Sample(g_Depth_sampler, uv).r;
    if (d >= 0.99999) return 1e8;
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0);
    float4 v = mul(g_InvProj, clip);
    return abs(v.z / v.w);
}

float3 NormalAt(float2 uv) { return OctDecode(g_GBuffer.Sample(g_GBuffer_sampler, uv).xy); }

// Bilateral weight of a neighbour against the centre: same surface = close depth, same facing.
float SurfaceWeight(float zc, float3 nc, float zn, float3 nn)
{
    float wz = saturate(1.0 - abs(zn - zc) / (0.02 * zc + 0.02));
    float wn = pow(saturate(dot(nn, nc)), 8.0);
    return wz * wn;
}

// Mode 0: 5x5 bilateral at the source resolution (one texel = one output pixel).
float4 Denoise(float2 uv)
{
    float2 res = g_AOParams2.zw;
    float2 tc0 = floor(uv * res);
    float  zc  = LinearZ(uv);
    if (zc > 1e7) return float4(1.0, 1.0, 1.0, 1.0);
    float3 nc  = NormalAt(uv);
    float sum = 0.0, wsum = 0.0, mn = 1e9, mx = -1e9;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            float2 tc  = clamp(tc0 + float2(x, y), 0.0, res - 1.0);
            float2 tuv = (tc + 0.5) / res;
            float  a   = g_Source.Load(int3((int2)tc, 0)).r;
            float  ws  = exp(-(x * x + y * y) * 0.3);
            float  sw  = SurfaceWeight(zc, nc, LinearZ(tuv), NormalAt(tuv));
            float  w   = ws * sw + 1e-5;
            sum += a * w; wsum += w;
            if (abs(x) <= 1 && abs(y) <= 1 && sw > 0.1) { mn = min(mn, a); mx = max(mx, a); }   // raw 3x3 box, same surface
        }
    }
    float v = sum / wsum;
    if (mn > mx) { mn = v; mx = v; }
    return float4(v, mn, mx, v);
}

// Mode 1: upsample + temporal + shaping at full resolution.
PSOut Final(float2 uv)
{
    PSOut o;
    float depth = g_Depth.Sample(g_Depth_sampler, uv).r;
    if (depth >= 0.99999) { o.color = float4(1.0, 1.0, 1.0, 1.0); o.hist = float4(1.0, 1e8, 0.0, 0.0); return o; }
    float  z = LinearZ(uv);
    float3 N = NormalAt(uv);

    // 3x3 bilateral over the (denoised) source texels around this pixel's position in its grid.
    float2 srcRes = g_AOParams2.zw;
    float2 rp     = uv * srcRes - 0.5;
    float2 center = floor(rp + 0.5);
    float  sum = 0.0, wsum = 0.0, mn = 1e9, mx = -1e9;
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
            float  ws  = exp(-dot(dd, dd) * 0.7);
            float  sw  = SurfaceWeight(z, N, LinearZ(tuv), NormalAt(tuv));
            float  w   = ws * sw + 1e-5;
            sum += s.r * w; wsum += w;
            if (sw > 0.1) { mn = min(mn, s.g); mx = max(mx, s.b); }   // union of the raw boxes, same surface
        }
    }
    float ao = sum / wsum;
    if (mn > mx) { mn = ao; mx = ao; }

    // Temporal: reproject last frame's accumulated visibility. Rejected when the depth there
    // does not match (disocclusion / off-screen), otherwise clamped to the RAW neighbourhood
    // box — wide enough for the accumulation to converge, tight enough to drop stale shading.
    if (g_AOParams2.x > 0.5)
    {
        float2 puv = uv - g_Velocity.Sample(g_Velocity_sampler, uv).rg;
        if (all(puv >= 0.0) && all(puv <= 1.0))
        {
            float2 h = g_History.Sample(g_History_sampler, puv).rg;
            if (abs(h.y - z) < 0.03 * z + 0.02)
                ao = lerp(ao, clamp(h.x, mn, mx), g_AOParams2.y);
        }
    }

    float shaped = lerp(1.0, pow(saturate(ao), g_AOParams.z), g_AOParams.y);
    o.color = float4(shaped, shaped, shaped, ao);
    o.hist  = float4(ao, z, 0.0, 0.0);
    return o;
}

PSOut main(in PSIn i)
{
    if (g_AOParams.w < 0.5) { PSOut o; o.color = Denoise(i.uv); o.hist = 0.0; return o; }
    return Final(i.uv);
}
