// Screen-space GI (contact-scale diffuse bounce on top of the DDGI probes). Per pixel a few
// cosine-distributed rays march the prepass depth; where a ray hits a front-facing surface,
// that surface's LIT colour from last frame's scene (reprojected through the velocity) is the
// bounced radiance. Rays that leave the screen or miss add nothing — the probes and the sky
// already cover the far field. Output: (E / pi, hit fraction) at this pass's resolution;
// ssgi_resolve.ps denoises, upsamples and accumulates it.
Texture2D    g_GBuffer;   SamplerState g_GBuffer_sampler;   // (octN.xy, roughness, metalness), point
Texture2D    g_Depth;     SamplerState g_Depth_sampler;     // prepass device depth, point
Texture2D    g_Velocity;  SamplerState g_Velocity_sampler;  // motion (uv units), point
Texture2D    g_History;   SamplerState g_History_sampler;   // last frame's scene colour (pre-post), linear

cbuffer SSGICB
{
    float4x4 g_View;
    float4x4 g_Proj;       // unjittered
    float4x4 g_InvProj;
    float4   g_Res;        // this pass: w, h, 1/w, 1/h
    float4   g_Params;     // radius (world), intensity, rays, steps
    float4   g_Params2;    // phase, history is LDR (tonemapped + sRGB in shader), tonemap white point, history valid
};

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
static const float PI = 3.14159265;

float3 OctDecode(float2 e)
{
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
float3 ViewPosFromUV(float2 uv, float depth)
{
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 v = mul(g_InvProj, clip);
    return v.xyz / v.w;
}
float2 ProjectToUV(float3 vp, out bool ok)
{
    float4 clip = mul(g_Proj, float4(vp, 1.0));
    ok = clip.w > 1e-4;
    float3 ndc = clip.xyz / max(clip.w, 1e-4);
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}
float2 SnapUV(float2 uv)
{
    uint w, h; g_Depth.GetDimensions(w, h);
    float2 res = float2(w, h);
    return (floor(uv * res) + 0.5) / res;
}
float Hash(float2 p, float seed)
{
    float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973) + seed * 0.3183);
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}

// The LDR path tonemaps inside world.ps (extended Reinhard + sRGB): undo it so the bounce is radiance.
float3 HistoryRadiance(float3 c)
{
    if (g_Params2.y < 0.5) return c;
    c = pow(max(c, 0.0), 2.2);
    float W = max(g_Params2.z, 1e-3);
    float3 y = min(c, 0.999);
    // y = x (1 + x / W^2) / (1 + x)  ->  x^2 / W^2 + x (1 - y) - y = 0
    float3 x = 0.5 * W * W * ((y - 1.0) + sqrt((1.0 - y) * (1.0 - y) + 4.0 * y / (W * W)));
    return max(x, 0.0);
}

float4 main(in PSIn i) : SV_Target
{
    float2 uvC   = SnapUV(i.uv);
    float  depth = g_Depth.Sample(g_Depth_sampler, uvC).r;
    if (depth >= 0.99999) return float4(0.0, 0.0, 0.0, 0.0);

    float3 P  = ViewPosFromUV(uvC, depth);
    float3 N  = normalize(mul((float3x3)g_View, OctDecode(g_GBuffer.Sample(g_GBuffer_sampler, uvC).xy)));
    float  viewZ = abs(P.z);
    float  radius = g_Params.x;
    int    rays  = (int)clamp(g_Params.z, 1.0, 16.0);
    int    steps = (int)clamp(g_Params.w, 4.0, 32.0);
    float  thick = radius * 0.35 + viewZ * 0.01;           // depth tolerance behind a hit

    float2 pix    = i.uv * g_Res.xy;
    float  noise  = Hash(pix, g_Params2.x);
    float  noise2 = Hash(pix + 17.0, g_Params2.x);
    float3 T = normalize(cross(N, abs(N.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0)));
    float3 B = cross(N, T);
    float3 P0 = P + N * (viewZ * 0.001 + 0.002);

    float3 sum = 0.0; float hits = 0.0;
    [loop]
    for (int r = 0; r < rays; ++r)
    {
        // cosine-distributed direction (golden spiral in the disc, lifted)
        float u   = (r + noise2) / rays;
        float ang = r * 2.399963 + noise * 6.2831853;
        float rd  = sqrt(u);
        float3 dir = T * (rd * cos(ang)) + B * (rd * sin(ang)) + N * sqrt(max(0.0, 1.0 - u));
        float  jit = frac(noise * 3.7 + r * 0.618034);
        bool hit = false; float2 hitUV = 0.0; float3 S = 0.0;
        [loop]
        for (int k = 0; k < steps && !hit; ++k)
        {
            float  t   = radius * (k + jit) / steps;
            float3 Q   = P0 + dir * t;
            bool ok; float2 quv = ProjectToUV(Q, ok);
            if (!ok || any(quv < 0.0) || any(quv > 1.0)) break;
            float sd = g_Depth.Sample(g_Depth_sampler, SnapUV(quv)).r;
            if (sd >= 0.99999) continue;
            S = ViewPosFromUV(SnapUV(quv), sd);
            float dz = length(Q) - length(S);              // > 0: the scene surface is in front of the sample
            if (dz > 0.0 && dz < thick) { hit = true; hitUV = quv; }
        }
        if (!hit) continue;
        // only a surface facing the ray bounces light toward us
        float3 hN = normalize(mul((float3x3)g_View, OctDecode(g_GBuffer.Sample(g_GBuffer_sampler, SnapUV(hitUV)).xy)));
        if (dot(hN, dir) >= 0.0) continue;
        float2 puv = hitUV - g_Velocity.Sample(g_Velocity_sampler, hitUV).rg;
        if (any(puv < 0.0) || any(puv > 1.0)) continue;
        sum += HistoryRadiance(g_History.SampleLevel(g_History_sampler, puv, 0).rgb);
        hits += 1.0;
    }
    // cosine sampling: mean radiance of the hits = E / pi of the bounced part (misses = 0 here)
    return float4(sum / rays * g_Params.y, hits / rays);
}
