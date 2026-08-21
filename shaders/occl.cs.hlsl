// Hi-Z occlusion test. Mode 0: one thread per tagged world AABB — project its corners, pick
// the pyramid level where the screen rect spans <= 2x2 texels, compare the box's NEAREST depth
// with the FARTHEST depth under it (standard depth: near = 0). Anything touching the near
// plane or reaching past the pyramid counts as visible. Mode 1: one thread per deferred draw
// record — emit its indirect arguments with instance count 0 when its box lost the test.
struct Aabb { float3 mn; float pad0; float3 mx; float pad1; };
struct Rec  { uint tag; uint count; uint first; uint inst; uint firstInst; uint indexed; uint pad0; uint pad1; };
StructuredBuffer<Aabb>   g_Aabbs;
StructuredBuffer<Rec>    g_Recs;
RWStructuredBuffer<uint> g_Vis;
RWByteAddressBuffer      g_Args;
Texture2D<float>         g_HiZ;
cbuffer OcclCB { float4x4 g_VP; float4 g_Dims; uint4 g_Counts; };   // Dims: w, h, mips; Counts: tags, recs, mode

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint i = tid.x;
    if (g_Counts.z == 0)
    {
        if (i >= g_Counts.x) return;
        Aabb b = g_Aabbs[i];
        float  minZ = 1.0;
        float2 lo = float2(1e9, 1e9), hi = float2(-1e9, -1e9);
        bool   nearClip = false;
        [unroll] for (int c = 0; c < 8; ++c)
        {
            float3 p = float3((c & 1) ? b.mx.x : b.mn.x, (c & 2) ? b.mx.y : b.mn.y, (c & 4) ? b.mx.z : b.mn.z);
            float4 h = mul(g_VP, float4(p, 1.0));
            if (h.w <= 1e-5) { nearClip = true; break; }
            float3 n = h.xyz / h.w;
            minZ = min(minZ, n.z);
            float2 uv = float2(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5);
            lo = min(lo, uv); hi = max(hi, uv);
        }
        uint vis = 1;
        if (!nearClip && minZ > 0.0)
        {
            lo = saturate(lo); hi = saturate(hi);
            float2 px  = (hi - lo) * g_Dims.xy;
            float  lvl = clamp(ceil(log2(max(max(px.x, px.y), 1.0))), 0.0, g_Dims.z - 1.0);
            uint   L   = (uint)lvl;
            int2 dim = max(int2((uint)g_Dims.x >> L, (uint)g_Dims.y >> L), int2(1, 1));
            float  sc  = exp2(-lvl);
            int2 t0 = clamp(int2(lo * g_Dims.xy * sc), int2(0, 0), dim - 1);
            int2 t1 = clamp(int2(hi * g_Dims.xy * sc), int2(0, 0), dim - 1);
            float d = g_HiZ.Load(int3(t0.x, t0.y, L));
            d = max(d, g_HiZ.Load(int3(t1.x, t0.y, L)));
            d = max(d, g_HiZ.Load(int3(t0.x, t1.y, L)));
            d = max(d, g_HiZ.Load(int3(t1.x, t1.y, L)));
            if (minZ > d) vis = 0;
        }
        g_Vis[i] = vis;
    }
    else
    {
        if (i >= g_Counts.y) return;
        Rec  r = g_Recs[i];
        uint v = g_Vis[r.tag];
        uint o = i * 20;
        if (r.indexed)
        {
            g_Args.Store4(o, uint4(r.count, v ? r.inst : 0u, r.first, 0u));   // indices, instances, firstIndex, baseVertex
            g_Args.Store(o + 16, r.firstInst);
        }
        else
            g_Args.Store4(o, uint4(r.count, v ? r.inst : 0u, r.first, r.firstInst));   // vertices, instances, firstVertex, firstInstance
    }
}
