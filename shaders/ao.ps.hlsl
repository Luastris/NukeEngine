// Screen-space ambient occlusion over the prepass depth + normals — four methods behind one
// pass, picked per world (g_AOParams2.z): 1 SSAO (normal-oriented hemisphere point samples,
// cheapest), 2 HBAO (screen directions, max horizon elevation, distance attenuation), 3 GTAO
// (slices through the view vector, two horizons, analytic cosine-weighted integral), 4 VBAO
// (GTAO slices, but a 32-sector visibility bitmask per slice with occluder thickness — thin
// objects no longer occlude like infinite walls), 5 RT-AO (DXR builds: cosine-weighted rays
// against the scene TLAS, off-screen occluders included; elsewhere it runs as GTAO). Output
// is raw visibility (1 = open) at THIS
// pass's resolution; aoresolve.ps filters, upsamples and accumulates it. Applied to the
// ambient/IBL term only (world.ps), never to direct light.
Texture2D    g_GBuffer;  SamplerState g_GBuffer_sampler;   // (octN.xy, roughness, metalness), point
Texture2D    g_Depth;    SamplerState g_Depth_sampler;     // prepass device depth (R), point
#ifdef RT_ENABLED
RaytracingAccelerationStructure g_TLAS;                    // scene TLAS (shadow-caster mask 0x02)
#endif

cbuffer AOCB
{
    float4x4 g_View;       // world -> view
    float4x4 g_Proj;       // view -> clip, unjittered (matches the prepass depth)
    float4x4 g_InvProj;
    float4x4 g_InvView;    // view -> world (RT-AO rays live in world space)
    float4   g_AORes;      // this pass: w, h, 1/w, 1/h
    float4   g_AOParams;   // radius (world units), intensity, power, slices / directions
    float4   g_AOParams2;  // steps per side, temporal phase (frame), method, unused
};

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

static const float PI = 3.14159265;
static const uint  VB_SECTORS = 32;   // VBAO: sectors over the slice's 180 degrees

// Octahedral normal decode (matches gbuffer.ps OctEncode).
float3 OctDecode(float2 e)
{
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

// uv (0,0 = top-left) + device depth -> view-space position.
float3 ViewPosFromUV(float2 uv, float depth)
{
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 v = mul(g_InvProj, clip);
    return v.xyz / v.w;
}

// view-space position -> screen uv; ok = false behind the camera.
float2 ProjectToUV(float3 vp, out bool ok)
{
    float4 clip = mul(g_Proj, float4(vp, 1.0));
    ok = clip.w > 1e-4;
    float3 ndc = clip.xyz / max(clip.w, 1e-4);
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

// uv snapped to the centre of its full-res depth texel: a position rebuilt from a texel's depth
// at a different uv leaves the surface (worst at grazing angles) and fakes horizons.
float2 SnapUV(float2 uv)
{
    uint w, h; g_Depth.GetDimensions(w, h);
    float2 res = float2(w, h);
    return (floor(uv * res) + 0.5) / res;
}

// Scene point under a uv, or false on sky / off-screen.
bool ScenePoint(float2 uv, out float3 S)
{
    S = 0.0;
    if (any(uv < 0.0) || any(uv > 1.0)) return false;
    uv = SnapUV(uv);
    float d = g_Depth.Sample(g_Depth_sampler, uv).r;
    if (d >= 0.99999) return false;
    S = ViewPosFromUV(uv, d);
    return true;
}

// Falloff over the outer third of the radius (shared by every method).
float Falloff(float dist, float radius) { return saturate((radius - dist) / (radius * 0.35)); }

// Per-pixel, per-frame white noise: uncorrelated across rows and frames, so the denoise
// averages it away and the temporal history converges instead of locking a pattern in.
float Hash(float2 p, float seed)
{
    float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973) + seed * 0.3183);
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}

// ---- 1) SSAO: cosine-distributed points in the normal hemisphere, depth-tested in screen space.
float AO_SSAO(float3 P, float3 N, float radius, int count, float noise, float noise2)
{
    // tangent frame around N
    float3 T = normalize(cross(N, abs(N.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0)));
    float3 B = cross(N, T);
    float rot = noise * 2.0 * PI;
    float occl = 0.0;
    [loop]
    for (int k = 0; k < count; ++k)
    {
        // stratified: golden-angle spiral in the disc, cosine-weighted lift, radius ramp
        float u   = (k + noise2) / count;
        float ang = k * 2.399963 + rot;
        float r   = sqrt(u);
        float3 dir = T * (r * cos(ang)) + B * (r * sin(ang)) + N * sqrt(max(0.0, 1.0 - u));
        float  len = radius * lerp(0.15, 1.0, frac(u * 3.7 + noise2));
        float3 Q   = P + dir * len;
        bool ok; float2 quv = ProjectToUV(Q, ok);
        if (!ok) continue;
        float3 S;
        if (!ScenePoint(quv, S)) continue;
        // occluded when the scene surface sits in front of the sample point (toward the eye)
        if (length(S) < length(Q) - (0.01 + 0.002 * length(P)))
            occl += Falloff(length(S - P), radius);
    }
    return 1.0 - occl / count;
}

// ---- 2) HBAO: screen directions, the horizon's elevation above the tangent plane, attenuated.
float AO_HBAO(float3 P, float3 N, float2 uv, float radius, float radPix, int dirs, int steps, float noise, float noise2)
{
    float sliceJit = noise;
    float stepJit  = noise2;
    float tMin     = 1.0 / radPix;
    float occl = 0.0;
    [loop]
    for (int s = 0; s < dirs; ++s)
    {
        float  phi  = 2.0 * PI * (s + sliceJit) / dirs;
        float2 uvStep = float2(cos(phi), -sin(phi)) * radPix * g_AORes.zw;
        float  sinH = 0.0;   // tangent plane = the surface (normal-based), so sin(t) = 0
        [loop]
        for (int k = 0; k < steps; ++k)
        {
            float t = (k + stepJit) / steps;
            t = max(t, tMin);
            float3 S;
            if (!ScenePoint(uv + uvStep * t, S)) continue;
            float3 d    = S - P;
            float  dist = length(d);
            float  elev = dot(d / max(dist, 1e-5), N);      // sin of the elevation above the plane
            sinH = max(sinH, elev * Falloff(dist, radius));
        }
        occl += sinH;
    }
    return 1.0 - occl / dirs;
}

// Slice frame shared by GTAO / VBAO: slice direction orthogonal to V, its plane normal,
// the projected normal (length + angle n from V, signed toward the slice direction).
void SliceFrame(float3 N, float3 V, float2 dir2, out float3 ortho, out float3 projN, out float projLen, out float cosN, out float n)
{
    float3 dir3 = float3(dir2, 0.0);
    ortho = normalize(dir3 - V * dot(dir3, V));
    float3 axis = normalize(cross(ortho, V));
    projN   = N - axis * dot(N, axis);
    projLen = length(projN);
    cosN    = clamp(dot(projN / max(projLen, 1e-5), V), -1.0, 1.0);
    n       = sign(dot(projN, ortho)) * acos(cosN);
}

// ---- 3) GTAO: two horizons per slice, analytic integral of the visible cosine-weighted arc.
float AO_GTAO(float3 P, float3 N, float3 V, float2 uv, float radius, float radPix, int slices, int steps, float noise, float noise2)
{
    float sliceJit = noise;
    float stepJit  = noise2;
    float tMin     = 1.0 / radPix;
    float vis = 0.0, open = 0.0;
    [loop]
    for (int s = 0; s < slices; ++s)
    {
        float  phi  = PI * (s + sliceJit) / slices;
        float2 dir2 = float2(cos(phi), sin(phi));
        float3 ortho, projN; float projLen, cosN, n;
        SliceFrame(N, V, dir2, ortho, projN, projLen, cosN, n);
        if (projLen < 1e-4) continue;
        open += projLen * (cosN + n * sin(n));   // this slice fully open (h = n +- pi/2)
        float2 uvStep = float2(dir2.x, -dir2.y) * radPix * g_AORes.zw;   // screen y points down
        float hc0 = -1.0, hc1 = -1.0;   // horizon cosines, the -dir and +dir sides
        [loop]
        for (int k = 0; k < steps; ++k)
        {
            float t = (k + stepJit) / steps;
            t = max(t, tMin);
            float2 off = uvStep * t;
            [unroll]
            for (int side = 0; side < 2; ++side)
            {
                float3 S;
                if (!ScenePoint((side == 0) ? uv - off : uv + off, S)) continue;
                float3 d    = S - P;
                float  dist = length(d);
                float  shc  = lerp(-1.0, dot(d / max(dist, 1e-5), V), Falloff(dist, radius));
                if (side == 0) hc0 = max(hc0, shc); else hc1 = max(hc1, shc);
            }
        }
        float h0 = -acos(hc0), h1 = acos(hc1);
        h0 = n + max(h0 - n, -0.5 * PI);
        h1 = n + min(h1 - n,  0.5 * PI);
        float sinN = sin(n);
        float a0 = -cos(2.0 * h0 - n) + cosN + 2.0 * h0 * sinN;
        float a1 = -cos(2.0 * h1 - n) + cosN + 2.0 * h1 * sinN;
        vis += projLen * 0.25 * (a0 + a1);
    }
    // Normalised by the open integral of the SAME slices: screen-uniform slice angles are not
    // uniform around an off-axis V, so a fixed 1/slices leaves a few % of false occlusion.
    return open > 1e-4 ? max(vis / open, 0.0) : 1.0;
}

// ---- 4) VBAO: per slice a bitmask of occluded sectors; every sample marks the sectors between
// its front point and the point one thickness behind it, so a thin occluder covers only its
// own angular extent instead of everything below its horizon.
float AO_VBAO(float3 P, float3 N, float3 V, float2 uv, float radius, float radPix, int slices, int steps, float noise, float noise2)
{
    float sliceJit = noise;
    float stepJit  = noise2;
    float tMin     = 1.0 / radPix;
    float thick    = radius * 0.5;   // assumed occluder thickness along the view direction
    float visSum = 0.0, wSum = 0.0;
    [loop]
    for (int s = 0; s < slices; ++s)
    {
        float  phi  = PI * (s + sliceJit) / slices;
        float2 dir2 = float2(cos(phi), sin(phi));
        float3 ortho, projN; float projLen, cosN, n;
        SliceFrame(N, V, dir2, ortho, projN, projLen, cosN, n);
        if (projLen < 1e-4) continue;
        float2 uvStep = float2(dir2.x, -dir2.y) * radPix * g_AORes.zw;
        uint mask = 0u;
        [loop]
        for (int k = 0; k < steps; ++k)
        {
            float t = (k + stepJit) / steps;
            t = max(t, tMin);
            float2 off = uvStep * t;
            [unroll]
            for (int side = 0; side < 2; ++side)
            {
                float3 S;
                if (!ScenePoint((side == 0) ? uv - off : uv + off, S)) continue;
                float3 df = S - P;
                float  dist = length(df);
                float  w = Falloff(dist, radius);
                if (w <= 0.0) continue;
                float3 db = df - V * thick;   // one thickness further from the eye
                // angles in the slice plane, measured from V toward the slice direction
                float af = atan2(dot(df, ortho), dot(df, V));
                float ab = atan2(dot(db, ortho), dot(db, V));
                float lo = min(af, ab), hi = max(af, ab);
                lo = clamp(lo, -0.5 * PI, 0.5 * PI); hi = clamp(hi, -0.5 * PI, 0.5 * PI);
                if (hi <= lo) continue;
                // thin occluders fade with distance by shrinking their arc
                float mid = 0.5 * (lo + hi), half = 0.5 * (hi - lo) * w;
                uint b0 = (uint)floor((mid - half + 0.5 * PI) / PI * VB_SECTORS);
                uint b1 = (uint)ceil ((mid + half + 0.5 * PI) / PI * VB_SECTORS);
                b0 = min(b0, VB_SECTORS - 1); b1 = clamp(b1, b0 + 1, VB_SECTORS);
                uint bits = (b1 - b0 >= VB_SECTORS) ? 0xFFFFFFFFu : (((1u << (b1 - b0)) - 1u) << b0);
                mask |= bits;
            }
        }
        // visible = cosine-weighted (against the projected normal) sectors left unset
        float open = 0.0, total = 0.0;
        [loop]
        for (uint b = 0; b < VB_SECTORS; ++b)
        {
            float th = (b + 0.5) / VB_SECTORS * PI - 0.5 * PI;
            float wgt = max(0.0, cos(th - n));
            total += wgt;
            if ((mask & (1u << b)) == 0u) open += wgt;
        }
        visSum += projLen * (total > 0.0 ? open / total : 1.0);
        wSum   += projLen;
    }
    return wSum > 0.0 ? saturate(visSum / wSum) : 1.0;
}

#ifdef RT_ENABLED
// ---- 5) RT-AO: cosine-weighted hemisphere rays from the world-space surface point; a hit
// inside the radius occludes, weighted by the shared falloff. Sees what the screen cannot.
float AO_RTAO(float3 Pw, float3 Nw, float radius, int rays, float noise, float noise2)
{
    float3 T = normalize(cross(Nw, abs(Nw.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0)));
    float3 B = cross(Nw, T);
    float  rot = noise * 2.0 * PI;
    float  occl = 0.0;
    [loop]
    for (int k = 0; k < rays; ++k)
    {
        float u   = (k + noise2) / rays;
        float ang = k * 2.399963 + rot;
        float r   = sqrt(u);
        float3 dir = T * (r * cos(ang)) + B * (r * sin(ang)) + Nw * sqrt(max(0.0, 1.0 - u));
        RayDesc ray; ray.Origin = Pw; ray.Direction = dir; ray.TMin = 0.02; ray.TMax = radius;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0x02, ray);
        while (q.Proceed()) q.CommitNonOpaqueTriangleHit();   // particle quads count as occluders
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            occl += Falloff(q.CommittedRayT(), radius);
    }
    return 1.0 - occl / rays;
}
#endif

float4 main(in PSIn i) : SV_Target
{
    float2 uvC   = SnapUV(i.uv);
    float  depth = g_Depth.Sample(g_Depth_sampler, uvC).r;
    if (depth >= 0.99999) return float4(1.0, 1.0, 1.0, 1.0);   // sky / no geometry

    float3 P  = ViewPosFromUV(uvC, depth);
    float3 Nw = OctDecode(g_GBuffer.Sample(g_GBuffer_sampler, uvC).xy);
    float3 N  = normalize(mul((float3x3)g_View, Nw));
    float3 V  = normalize(-P);                                  // pixel -> eye
    float  viewZ = abs(P.z);

    // World radius -> pixels at this depth (proj[1][1] = vertical focal scale); capped so a
    // near wall does not turn into a screen-wide gather.
    float radius = g_AOParams.x;
    float radPix = radius * g_Proj[1][1] * 0.5 * g_AORes.y / max(viewZ, 1e-3);
    float capPix = g_AORes.y * 0.25;
    if (radPix > capPix) { radius *= capPix / radPix; radPix = capPix; }   // shrink the world radius too: no seam at a fixed depth
    if (radPix < 1.5) return float4(1.0, 1.0, 1.0, 1.0);

    int   slices = (int)clamp(g_AOParams.w, 1.0, 16.0);
    int   steps  = (int)clamp(g_AOParams2.x, 2.0, 16.0);
    int   method = (int)g_AOParams2.z;
    float2 pix   = i.uv * g_AORes.xy;
    float phase  = g_AOParams2.y;
    float noise  = Hash(pix, phase);          // slice / direction jitter
    float noise2 = Hash(pix + 17.0, phase);   // step jitter

    P += N * (viewZ * 0.0005 + 0.001);   // a hair off the surface: the pixel's own texel is no horizon

    float vis;
    [branch]
#ifdef RT_ENABLED
    if (method == 5)
    {
        float3 Pw = mul(g_InvView, float4(P, 1.0)).xyz;
        vis = AO_RTAO(Pw, Nw, radius, slices, noise, noise2);
    }
    else
#endif
    if      (method == 1) vis = AO_SSAO(P, N, radius, slices * steps, noise, noise2);
    else if (method == 2) vis = AO_HBAO(P, N, i.uv, radius, radPix, slices, steps, noise, noise2);
    else if (method == 4) vis = AO_VBAO(P, N, V, i.uv, radius, radPix, slices, steps, noise, noise2);
    else                  vis = AO_GTAO(P, N, V, i.uv, radius, radPix, slices, steps, noise, noise2);
    vis = max(vis, 0.0);   // the resolve clamps after accumulation
    return float4(vis, vis, vis, 1.0);
}
