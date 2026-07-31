// Music visualizer post effect: audio-reactive ghost projections of the scene plus a sky note spectrum.
// g_NukeAudio / g_NukeNote / g_NukeChromaA..C are engine-filled system params: declare, never initialize.
Texture2D    g_Source;     SamplerState g_Source_sampler;    // current HDR chain colour
Texture2D    g_GBuffer;    SamplerState g_GBuffer_sampler;   // (octN.xy, roughness, metalness)
Texture2D    g_Depth;      SamplerState g_Depth_sampler;     // prepass device depth (R)
Texture2D    g_ObjId;      SamplerState g_ObjId_sampler;     // per-OBJECT id (generic G-buffer channel, R8)

cbuffer PostParams
{
    float g_KickAmp   = 0.030;   // ghost jump height on drums (fraction of screen height)
    float g_BassAmp   = 0.020;   // ghost split width on bass (fraction of screen width)
    float g_GhostGain = 1.35;    // overall ghost overlay strength (dense, low-transparency ghosts)
    float g_VibFreq   = 13.0;    // ghost vibration rate, Hz
    float g_SkyGain   = 0.75;    // sky note-shimmer strength
    float g_TintGain  = 1.0;     // how strongly ghosts take their resonant-note color
    float g_Threshold = 0.04;    // band level below which ghosts fade out entirely
    float g_Perspective = 0.55;  // vibration-axis lean: 0 = flat screen axes, 1 = fully radial
    float g_SpecHeight  = 0.5;   // note-spectrum extent above the horizon (ray elevation; ~half the sky)
    float g_SpecGain    = 1.1;   // note-spectrum brightness (0 = off)
    float g_SpecBlur    = 1.1;   // note-spectrum smear width, in notes (the "liquid" factor)
    float g_SpecFloor   = 0.85;  // valley lift: min column level as a share of the mean (kills dips)

    float4 g_NukeAudio;          // SYSTEM: kick, snare, bass, energy        (engine-filled)
    float4 g_NukeNote;           // SYSTEM: note, strength, beatPhase, time  (engine-filled)
    float4 g_NukeChromaA;        // SYSTEM: chroma C, C#, D, D#              (engine-filled)
    float4 g_NukeChromaB;        // SYSTEM: chroma E, F, F#, G               (engine-filled)
    float4 g_NukeChromaC;        // SYSTEM: chroma G#, A, A#, B              (engine-filled)
};
cbuffer SSRCB { float4x4 g_View; float4x4 g_Proj; float4x4 g_InvProj; float4x4 g_InvView; float4 g_SSRRes; };

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

float ChromaOf(int n)   // pitch-class energy from the packed system float4s
{
    if (n < 4)  return g_NukeChromaA[n];
    if (n < 8)  return g_NukeChromaB[n - 4];
    return g_NukeChromaC[n - 8];
}

// Pure saturated color of a pitch class: hue = n/12 around the wheel (C = red).
float3 NoteColor(int n)
{
    float h = frac((float)n / 12.0) * 6.0;
    float3 c = saturate(float3(abs(h - 3.0) - 1.0, 2.0 - abs(h - 2.0), 2.0 - abs(h - 4.0)));
    return c;
}

// One ghost tap: sample the scene at `uv`, keep it only if the surface there matches the wanted
// orientation, and tint it with that surface's resonant-note color scaled by the live chroma.
float3 GhostTap(float2 uv, bool wantHorizontal)
{
    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0) return float3(0, 0, 0);
    float d = g_Depth.Sample(g_Depth_sampler, uv).r;
    if (d >= 0.99999) return float3(0, 0, 0);                    // ghost source is sky
    float3 N = OctDecode(g_GBuffer.Sample(g_GBuffer_sampler, uv).xy);
    float ny = abs(N.y);
    float mask = wantHorizontal ? saturate((ny - 0.55) / 0.25)   // 1 = flat roof/road
                                : saturate((0.45 - ny) / 0.25);  // 1 = upright wall
    if (mask <= 0.0) return float3(0, 0, 0);

    // Resonant note = this shader's mapping of the generic per-object G-buffer id, so one object = one color.
    int    note = clamp((int)floor(g_ObjId.Sample(g_ObjId_sampler, uv).r * 11.999), 0, 11);
    float  res  = ChromaOf(note);
    float3 tint = lerp(float3(1, 1, 1), NoteColor(note) * 1.35, saturate(g_TintGain * (0.45 + 0.55 * res)));

    float3 c = g_Source.Sample(g_Source_sampler, uv).rgb;
    float  lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(float3(lum, lum, lum), c, 1.6);
    return max(c, 0.0) * tint * mask * (0.5 + 0.5 * res);
}

// Vibration axis tilted toward the screen centre: the flat screen axis blended with the pixel's
// radial direction, aspect-corrected so the lean stays circular.
float2 PerspAxis(float2 uv, float2 axis)
{
    float aspect = g_SSRRes.x / max(g_SSRRes.y, 1.0);
    float2 r = (uv - float2(0.5, 0.5)) * float2(aspect, 1.0);     // radial (outward) in circular space
    float rl = length(r);
    if (rl < 1e-3) return axis;
    r /= rl;
    // The radial direction flips across the centre, so fade the lean to zero there to keep the field continuous.
    float t = saturate(g_Perspective) * smoothstep(0.0, 0.35, rl);
    float2 d = lerp(axis, r, t);
    float dl = length(d);
    d = (dl < 1e-3) ? axis : d / dl;                               // axis and radial cancelled out
    d.x /= aspect;                                                 // back to uv space
    return d;
}

float4 main(in PSIn i) : SV_Target
{
    float3 base = g_Source.Sample(g_Source_sampler, i.uv).rgb;

    float kick   = saturate(g_NukeAudio.x);
    float bass   = saturate(g_NukeAudio.z);
    float energy = saturate(g_NukeAudio.w);
    float t      = g_NukeNote.w;

    float3 col = base;

    // ---- sky: chord shimmer + note spectrum above the horizon
    float depth = g_Depth.Sample(g_Depth_sampler, i.uv).r;
    if (depth >= 0.99999)
    {
        float3 mix3 = float3(0, 0, 0); float wsum = 0.0;
        [unroll] for (int n = 0; n < 12; ++n)
        {
            float w = ChromaOf(n);
            mix3 += NoteColor(n) * w; wsum += w;
        }
        if (wsum > 1e-4)
        {
            mix3 /= wsum;
            float lum = dot(base, float3(0.2126, 0.7152, 0.0722));
            float amt = saturate(g_SkyGain * saturate(g_NukeNote.y) * (0.3 + 0.7 * energy)) * 0.6;
            col = lerp(base, mix3 * (0.25 + lum), amt);
        }

        // Note spectrum smeared across the lower sky: note axis = screen X, band height = live chroma,
        // growing from the horizon up to g_SpecHeight of ray elevation.
        if (g_SpecGain > 0.001)
        {
            // World direction of this pixel's view ray -> elevation above the horizon.
            float4 clipd = float4(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0, 1.0, 1.0);
            float4 vv = mul(g_InvProj, clipd);
            float3 wdir = normalize(mul((float3x3)g_InvView, vv.xyz / vv.w));
            {
                // Gaussian kernel over all 12 pitch classes: smooth in x, so no banding at note boundaries.
                float pos = saturate(i.uv.x) * 12.0 - 0.5;
                float sigma = max(g_SpecBlur, 0.25);
                float vsum = 0.0, ksum = 0.0; float3 csum = float3(0, 0, 0);
                [unroll] for (int n = 0; n < 12; ++n)
                {
                    float d = (pos - (float)n) / sigma;
                    float k = exp(-d * d);
                    float chn = ChromaOf(n);
                    vsum += chn * k; ksum += k;
                    csum += NoteColor(n) * chn * k;
                }
                float  v  = vsum / max(ksum, 1e-4);               // smooth spectrum level 0..1
                float3 sc = csum / max(vsum, 1e-4);               // chroma-weighted smooth color
                // Valley lift: never drop below a share of the mean chroma level.
                float va = 0.0;
                [unroll] for (int m = 0; m < 12; ++m) va += ChromaOf(m);
                v = max(v, (va / 12.0) * saturate(g_SpecFloor));
                float hgt  = max(g_SpecHeight, 0.01) * (0.12 + 0.88 * v);
                float band = 1.0 - smoothstep(hgt * 0.25, hgt, abs(wdir.y));   // abs: mirrored about the horizon
                col += sc * (band * band) * v * g_SpecGain * (0.4 + 0.6 * energy);
            }
        }
        return float4(col, 1.0);
    }

    // ---- ghost overlays. Offsets are uniform per frame: rigid translated projections, never waves.
    float vib  = sin(6.2831853 * g_VibFreq * t);
    float vib2 = sin(6.2831853 * g_VibFreq * 0.73 * t + 1.57);

    // Horizontal surfaces jump up with the drums, plus a fainter half-height trail.
    if (kick > g_Threshold)
    {
        float2 axH = PerspAxis(i.uv, float2(0.0, 1.0));           // "down" tilted centre-ward
        float  dy  = g_KickAmp * kick * (1.0 + 0.25 * vib * kick);
        float  aH  = g_GhostGain * kick * 0.75;
        col += GhostTap(i.uv + axH * dy,        true) * aH;
        col += GhostTap(i.uv + axH * dy * 0.5,  true) * aH * 0.5;
    }

    // Vertical surfaces split into a mirrored ghost pair on the bass.
    if (bass > g_Threshold)
    {
        float2 axV = PerspAxis(i.uv, float2(1.0, 0.0));           // "right" tilted centre-ward
        float  dx  = g_BassAmp * bass * (1.0 + 0.30 * vib2 * bass);
        float  aV  = g_GhostGain * bass * 0.65;
        col += GhostTap(i.uv + axV * dx, false) * aV * (1.0 + 0.5 * vib2);
        col += GhostTap(i.uv - axV * dx, false) * aV * (1.0 - 0.5 * vib2);
    }

    return float4(col, 1.0);
}
