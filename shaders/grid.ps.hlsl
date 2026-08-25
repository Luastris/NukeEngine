// Editor infinite-grid PS: analytic anti-aliased grid on the y=0 plane, adaptive LOD +
// distance fade, colored world axes (X red, Z blue).
// g_CamStep = (cam.xyz, base step); g_GridFade = (fade distance, master alpha, depth mode, _).
// Depth mode 1: occlusion is decided HERE in world space from the prepass depth — only a
// surface ABOVE the plane hides the grid, the coplanar floor never z-fights it.
cbuffer GridCB { float4x4 g_VP; float4x4 g_InvVP; float4 g_CamStep; float4 g_GridFade; float4 g_Res; };
Texture2D g_Depth;  SamplerState g_Depth_sampler;

struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; };

// Anti-aliased line coverage for a grid of step `s` (~1.2 px lines).
float LineAA(float2 uv, float s, float2 dd)
{
    float2 g = uv / s;
    float2 dg = max(dd / s, 1e-7);
    float2 f = abs(frac(g + 0.5) - 0.5) / dg;   // distance to the nearest line, in pixels
    return 1.0 - saturate(min(f.x, f.y) - 0.6);
}

float4 main(in PSIn i) : SV_Target
{
    float overTrust = 1.0;
    if (g_GridFade.z > 0.5)
    {
        float2 suv = i.pos.xy / g_Res.xy;
        float  d   = g_Depth.SampleLevel(g_Depth_sampler, suv, 0).r;
        if (d < 1.0)
        {
            float4 clip = float4(suv.x * 2.0 - 1.0, 1.0 - suv.y * 2.0, d, 1.0);
            float4 wp   = mul(g_InvVP, clip);
            float sceneY = wp.y / wp.w;
            float distG  = length(i.wpos - g_CamStep.xyz);
            float eps    = 0.005 + 3e-7 * distG * distG;   // float-depth quantization bound at this range
            if (sceneY > eps)
            {
                if (d < i.pos.z) discard;   // surface above the plane, in front -> hides the grid
            }
            else
                // Near-plane surface under this pixel: the grid may draw over it, but that
                // right fades out where depth can no longer tell 0 from a few centimeters.
                overTrust = saturate((250.0 - distG) / 100.0);
        }
    }
    float2 uv = i.wpos.xz;
    float2 dd = fwidth(uv);
    float  ddm = max(max(dd.x, dd.y), 1e-7);
    float  base = max(g_CamStep.w, 0.001);

    // Adaptive level: keep cells >= ~8 px; the finer level dissolves as it approaches that.
    float lodf = max(0.0, log10(ddm * 8.0 / base));
    float k  = floor(lodf);
    float tb = frac(lodf);
    float s0 = base * pow(10.0, k);
    float s1 = s0 * 10.0;

    float aMinor = LineAA(uv, s0, dd) * (1.0 - tb) * 0.30;
    float aMajor = LineAA(uv, s1, dd) * 0.55;
    float alpha  = max(aMinor, aMajor);
    float3 col   = float3(0.5, 0.52, 0.55);

    // World axes as ~1.5 px colored lines.
    float ax = 1.0 - saturate(abs(uv.y) / max(dd.y, 1e-7) - 0.75);   // z = 0 -> X axis
    float az = 1.0 - saturate(abs(uv.x) / max(dd.x, 1e-7) - 0.75);   // x = 0 -> Z axis
    if (ax > alpha) { alpha = ax * 0.8; col = float3(0.75, 0.35, 0.35); }
    if (az > alpha) { alpha = az * 0.8; col = float3(0.35, 0.45, 0.80); }

    // Distance fade: the grid dissolves smoothly long before any geometric boundary.
    float dist = length(i.wpos - g_CamStep.xyz);
    float fade = 1.0 - saturate(dist / max(g_GridFade.x, 1.0));
    alpha *= g_GridFade.y * fade * fade * overTrust;
    if (alpha <= 0.002) discard;
    return float4(col, alpha);
}
