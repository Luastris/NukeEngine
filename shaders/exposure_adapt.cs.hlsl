// Auto-exposure pass 2: one 16x16 group builds a 64-bin histogram of the 64x64 log-luminance grid,
// takes the mean over the middle of the distribution (the darkest and brightest 10 % of texels
// are ignored: a sun disc or a black corner must not drive the eye), clamps it to the EV range and
// eases the adapted EV toward it at the up/down speeds. The adapted EV lives in a 1x1 texture per
// camera (previous frame in, this frame out); the apply pass reads it.
Texture2D<float>   g_Lum;    // 64x64 log2 luminance
Texture2D<float>   g_Prev;   // 1x1: last frame's adapted EV (valid when g_Exp2.w = 1)
RWTexture2D<float> g_Out;    // 1x1: this frame's adapted EV
cbuffer ExpCB
{
    float4 g_Exp0;   // min EV, max EV, speed up, speed down
    float4 g_Exp1;   // compensation EV, manual EV, manual on, dt (s)
    float4 g_Exp2;   // 1/full w, 1/full h, LDR source, prev valid (1 = ease from g_Prev, 0 = snap)
};

groupshared uint  s_hist[64];
groupshared float s_sum;

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_GroupThreadID)
{
    uint flat = tid.y * 16 + tid.x;
    if (flat < 64) s_hist[flat] = 0;
    if (flat == 0) s_sum = 0.0;
    GroupMemoryBarrierWithGroupSync();
    // Each thread walks a 4x4 block of the grid: 256 threads x 16 texels = 64x64.
    const float evMin = g_Exp0.x - 2.0, evMax = g_Exp0.y + 2.0;   // the histogram spans the range with a margin
    [unroll] for (int y = 0; y < 4; ++y)
    [unroll] for (int x = 0; x < 4; ++x)
    {
        float ev = g_Lum.Load(int3(tid.x * 4 + x, tid.y * 4 + y, 0));
        uint bin = (uint)clamp((ev - evMin) / (evMax - evMin) * 63.0 + 0.5, 0.0, 63.0);
        InterlockedAdd(s_hist[bin], 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (flat == 0)
    {
        // Trimmed mean: skip the first and last 10 % of the 4096 texels, average the rest by bin centre.
        const uint total = 4096, lo = total / 10, hi = total - total / 10;
        uint seen = 0; float sum = 0.0; uint cnt = 0;
        [loop] for (uint b = 0; b < 64; ++b)
        {
            uint n = s_hist[b];
            uint from = max(seen, lo), to = min(seen + n, hi);
            if (to > from)
            {
                float ev = evMin + ((float)b + 0.5) / 64.0 * (evMax - evMin);
                sum += ev * (float)(to - from); cnt += to - from;
            }
            seen += n;
        }
        float target = (cnt > 0) ? sum / (float)cnt : 0.0;
        target = clamp(target, g_Exp0.x, g_Exp0.y);
        float prev = g_Prev.Load(int3(0, 0, 0));
        float ev;
        if (g_Exp2.w < 0.5) ev = target;   // first frame / after a reset: no easing from nothing
        else
        {
            float speed = (target > prev) ? g_Exp0.z : g_Exp0.w;
            ev = prev + (target - prev) * (1.0 - exp(-max(g_Exp1.w, 0.0) * max(speed, 0.0)));
        }
        g_Out[uint2(0, 0)] = ev;
    }
}
