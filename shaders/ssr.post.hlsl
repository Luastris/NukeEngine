// Screen-space reflections — built-in post effect (the renderer special-cases it: isSSR). A single fullscreen
// pass over the HDR chain colour. The renderer binds the G-buffer (octN.xy, roughness, metalness), the prepass
// depth, and the camera matrices (SSRCB). Reflections LAYER on top of the specular the world pass already
// produced — where a ray misses or leaves the screen the underlying reflection-probe / sky term stays.
Texture2D    g_Source;     SamplerState g_Source_sampler;   // current HDR chain colour
Texture2D    g_GBuffer;    SamplerState g_GBuffer_sampler;   // (octN.xy, roughness, metalness)
Texture2D    g_Depth;      SamplerState g_Depth_sampler;     // prepass device depth (R)

cbuffer PostParams         // inspector-tweakable (names + defaults parsed by the engine)
{
    float g_Intensity   = 1.0;    // overall reflection strength
    float g_MaxDistance = 40.0;   // view-space ray length
    float g_Thickness   = 0.6;    // view-space depth tolerance for a hit
    float g_MaxSteps    = 48.0;   // linear march steps
};
cbuffer SSRCB { float4x4 g_View; float4x4 g_Proj; float4x4 g_InvProj; float4x4 g_InvView; float4 g_SSRRes; };  // res: w,h,1/w,1/h

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// Octahedral normal decode (matches gbuffer.ps OctEncode).
float3 OctDecode(float2 e)
{
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

// uv (0,0 = top-left) + device depth -> view-space position. Mirrors the engine's mul(Matrix, vector) convention.
float3 ViewPosFromUV(float2 uv, float depth)
{
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 v = mul(g_InvProj, clip);
    return v.xyz / v.w;
}

// view-space position -> screen uv (0,0 = top-left). Returns uv; ok=false when behind the camera.
float2 ProjectToUV(float3 vp, out bool ok)
{
    float4 clip = mul(g_Proj, float4(vp, 1.0));
    ok = clip.w > 1e-4;
    float3 ndc = clip.xyz / max(clip.w, 1e-4);
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float4 main(in PSIn i) : SV_Target
{
    float3 base = g_Source.Sample(g_Source_sampler, i.uv).rgb;

    float depth = g_Depth.Sample(g_Depth_sampler, i.uv).r;
    if (depth >= 0.99999) return float4(base, 1.0);            // sky / no geometry

    float4 gb = g_GBuffer.Sample(g_GBuffer_sampler, i.uv);
    float rough = gb.z, metal = gb.w;
    float3 Nw = OctDecode(gb.xy);

    float3 vpos = ViewPosFromUV(i.uv, depth);
    float3 Nv   = normalize(mul((float3x3)g_View, Nw));
    float3 dir  = normalize(vpos);                              // eye(origin) -> pixel
    float NoV   = saturate(dot(Nv, -dir));

    // Selective: Fresnel-Schlick reflectivity, faded out on rough surfaces.
    float  F0   = lerp(0.04, 1.0, metal);
    float  refl = (F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0)) * (1.0 - rough);
    if (refl < 0.01 || g_Intensity <= 0.0) return float4(base, 1.0);

    float3 R = reflect(dir, Nv);

    int   steps = (int)clamp(g_MaxSteps, 8.0, 128.0);
    float dt    = g_MaxDistance / steps;
    // Bias the start off the surface (along the view normal) so the march never self-intersects the originating
    // surface — the prime cause of the grazing-angle "streaks". Per-pixel phase jitter breaks coherent banding.
    float2 pix      = i.uv * g_SSRRes.xy;   // interleaved gradient noise — cleaner phase dither than a sin-hash
    float  jit      = frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715))));
    float3 startPos = vpos + Nv * (0.03 + vpos.z * 0.01);
    float3 p        = startPos + R * dt * jit;
    float3 prevP    = startPos;
    float  prevDiff = -1.0;          // start: ray is IN FRONT of the scene surface
    bool   hit      = false;
    float2 hitUV    = i.uv;

    [loop]
    for (int s = 0; s < steps; ++s)
    {
        p += R * dt;
        bool ok; float2 suv = ProjectToUV(p, ok);
        if (!ok || suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;   // left the screen -> miss
        float sd = g_Depth.Sample(g_Depth_sampler, suv).r;
        if (sd >= 0.99999) { prevP = p; prevDiff = -1.0; continue; }   // sky -> stays "in front"
        float3 scenePos = ViewPosFromUV(suv, sd);
        float  diff = p.z - scenePos.z;                 // <0: ray in front of surface, >0: behind it
        // A real hit = the ray CROSSES from in-front to behind a surface, within the thickness tolerance.
        // Requiring the sign change (not just diff>0) rejects the false self-hits that caused the streaks.
        if (prevDiff < 0.0 && diff >= 0.0 && diff < g_Thickness)
        {
            float3 a = prevP, b = p;
            [unroll] for (int r = 0; r < 6; ++r)   // binary refine for a crisp hit point
            {
                float3 mid = (a + b) * 0.5;
                bool ok2; float2 muv = ProjectToUV(mid, ok2);
                float md = g_Depth.Sample(g_Depth_sampler, muv).r;
                float3 mp = ViewPosFromUV(muv, md);
                if (mid.z - mp.z >= 0.0) b = mid; else a = mid;
                hitUV = muv;
            }
            hit = true; break;
        }
        prevP = p; prevDiff = diff;
    }

    if (!hit) return float4(base, 1.0);

    // Fade: vanish toward screen borders (data runs out) AND with ray distance (far hits blend back to the probe).
    float2 e = smoothstep(0.0, 0.12, hitUV) * smoothstep(0.0, 0.12, 1.0 - hitUV);
    float  hd      = g_Depth.Sample(g_Depth_sampler, hitUV).r;
    float  distF   = 1.0 - saturate(length(ViewPosFromUV(hitUV, hd) - vpos) / max(g_MaxDistance, 1e-3));
    float  fade    = e.x * e.y * distF;

    // Glossy: blur the reflection by roughness (cheap disc taps) — softens rough reflections AND hides march noise.
    float3 reflColor = g_Source.SampleLevel(g_Source_sampler, hitUV, 0).rgb;
    float  blur = rough * 6.0;                       // radius in texels
    if (blur > 0.25)
    {
        float2 tx = g_SSRRes.zw * blur;
        const float2 off[6] = { float2(1,0), float2(-1,0), float2(0,1), float2(0,-1), float2(0.7,0.7), float2(-0.7,-0.7) };
        [unroll] for (int t = 0; t < 6; ++t) reflColor += g_Source.SampleLevel(g_Source_sampler, hitUV + off[t] * tx, 0).rgb;
        reflColor /= 7.0;
    }
    // REPLACE (bounded) rather than add: the metal already carries the probe/IBL reflection, so adding SSR on top
    // double-counts and blows out. lerp keeps the result within [base, reflColor].
    float  k = saturate(refl * g_Intensity * fade);
    return float4(lerp(base, reflColor, k), 1.0);
}
