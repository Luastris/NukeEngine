// Lens film - the per-camera wetness mask (R16F, ping-pong: reads g_Prev, writes the next).
// Two sources wet the lens: an injected soak map (a water module's waterline band: how much
// per second each pixel soaks) and rain (droplets landing at a rate). Wet pixels then drain
// down the screen by lubrication flow - a film's run-off speed grows with its thickness, so
// the sheet leaves first, a thin residue lags, breaks into beads and those slide off one by
// one; pinning holds small beads until a merge frees them; thin films evaporate faster.
Texture2D<float> g_Prev;   SamplerState g_Prev_sampler;
Texture2D<float> g_Inject; SamplerState g_Inject_sampler;
cbuffer LensCB
{
    float4 g_L0;   // dt (game clock), 1 / drain seconds, time, inject on
    float4 g_L1;   // rain drops per second, rain film amount, screen w, screen h
    float4 g_L2;   // 1/w, 1/h, composite amount, 0
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float WetHash(float x) { return frac(sin(x * 127.1 + 311.7) * 43758.5453); }

float4 main(in PSIn i) : SV_TARGET
{
    const float dt    = clamp(g_L0.x, 0.0, 0.1);
    const float drain = g_L0.y;
    const float time  = g_L0.z;
    float here = g_Prev.Sample(g_Prev_sampler, i.uv);

    // ---- sources ---------------------------------------------------------------------------
    float add = 0.0;
    if (g_L0.w > 0.5) add += g_Inject.Sample(g_Inject_sampler, i.uv) * dt;   // soak per second
    if (g_L1.x > 0.0)
    {
        // Rain: the screen in cells; every 1/30 s a cell lands a drop with probability
        // rate / 30 / cells, at a random spot inside it, with a random radius.
        // A drop can be wider than its cell, so every pixel also looks at the 8 neighbours.
        const float2 cells = float2(64.0, 36.0);
        float2 cid0 = floor(i.uv * cells);
        float  tick = floor(time * 30.0);
        float  p    = saturate(g_L1.x * (1.0 / 30.0) / (cells.x * cells.y));
        [unroll] for (int oy = -1; oy <= 1; ++oy)
        [unroll] for (int ox = -1; ox <= 1; ++ox)
        {
            float2 cid = cid0 + float2(ox, oy);
            float  key = dot(cid, float2(1.0, 91.0)) + tick * 0.371;
            if (WetHash(key) >= p) continue;
            float2 c  = (cid + float2(WetHash(key + 7.0), WetHash(key + 13.0))) / cells;
            float2 d  = (i.uv - c) * float2(g_L1.z / max(g_L1.w, 1.0), 1.0);   // aspect-correct
            float  rd = 0.009 + 0.016 * WetHash(key + 29.0);   // 1..2.5% of the screen: a real drop, not a fleck
            add += smoothstep(rd, rd * 0.35, length(d)) * 0.9;
        }
    }
    if (add > 0.002) return float4(min(here + add, 1.0), 0.0, 0.0, 1.0);   // a wetted pixel does not drain this step

    // ---- gravity-driven film -----------------------------------------------------------------
    float col  = floor(i.uv.x * 140.0);
    float colH = WetHash(col);
    float2 np = i.uv * float2(90.0, 54.0);
    float2 nf = frac(np);
    nf = nf * nf * (3.0 - 2.0 * nf);
    float2 ni = floor(np);
    float n00 = WetHash(dot(ni,                    float2(1.0, 57.0)));
    float n10 = WetHash(dot(ni + float2(1.0, 0.0), float2(1.0, 57.0)));
    float n01 = WetHash(dot(ni + float2(0.0, 1.0), float2(1.0, 57.0)));
    float n11 = WetHash(dot(ni + float2(1.0, 1.0), float2(1.0, 57.0)));
    float noise = lerp(lerp(n00, n10, nf.x), lerp(n01, n11, nf.x), nf.y);

    float thick = saturate(here);
    float vSelf = drain * (0.12 + 2.9 * thick * thick) * (0.75 + 0.5 * colH);
    float pin   = 0.10 + 0.16 * noise;
    float mobile = smoothstep(pin, pin + 0.10, thick);
    float spd = vSelf * mobile;

    float wob = (colH - 0.5) * 0.0015 * sin(time * (2.0 + colH * 3.0));
    float2 upUV = float2(i.uv.x + wob, i.uv.y - max(spd * dt, g_L2.y));
    float above = (upUV.y < 0.0) ? 0.0 : g_Prev.Sample(g_Prev_sampler, clamp(upUV, 0.001, 0.999));

    float shed = saturate(spd * dt / max(g_L2.y * 4.0, 1e-4));
    float gain = above * saturate(smoothstep(pin * 0.5, pin + 0.25, above));
    float m = max(here * (1.0 - 0.75 * shed), gain);
    float evap = drain * dt * (0.16 + 0.34 * (1.0 - thick));
    m = saturate(m - evap);
    return float4(m, 0.0, 0.0, 1.0);
}
