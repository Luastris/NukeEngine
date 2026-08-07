// GPU skinning (Anim/Mesh v2 stage 3): morph targets + LBS into the skinned instance's
// vertex buffers. Dispatched once per skinned mesh per posed frame; the previous frame's
// positions are kept for TAA motion vectors, and the BLAS refits over the output.
cbuffer SkinCSParams
{
    uint g_VertCount;
    uint g_BoneCount;
    uint g_MorphCount;
    uint g_Pad0;
};
// One-member wrapper structs (identical memory layout to the bare element). NOT cosmetic:
// glslang collapses StructuredBuffers with the same element type into ONE SPIR-V block type
// named after a single resource; MoltenVK's MSL then declares `device g_X* g_X`, the struct
// is hidden by the variable and every OTHER buffer of that type fails to compile. A unique
// element struct per buffer keeps every block type unique — each declaration only hides its
// own type, which is legal.
struct SkinPalCol  { float4 v; };
struct SkinBindPos { float  v; };
struct SkinBindNrm { float  v; };
struct SkinBoneWgt { float4 v; };
struct SkinMorphD  { float  v; };
struct SkinMorphW  { float  v; };
struct SkinPosOut  { float  v; };
struct SkinNrmOut  { float  v; };
struct SkinPosPrev { float  v; };

// Palette: 4 float4 COLUMNS per bone (column-major model-space global * invBind).
StructuredBuffer<SkinPalCol>  g_Palette;
StructuredBuffer<SkinBindPos> g_BindPos;     // 3 floats / vert
StructuredBuffer<SkinBindNrm> g_BindNrm;     // 3 floats / vert
StructuredBuffer<uint2>       g_BoneIdx;     // 4 x u16 packed (x = i0|i1<<16, y = i2|i3<<16)
StructuredBuffer<SkinBoneWgt> g_BoneWgt;     // 4 weights / vert
// Morph deltas: per target, g_VertCount*3 position floats then g_VertCount*3 normal floats.
StructuredBuffer<SkinMorphD>  g_MorphDelta;
StructuredBuffer<SkinMorphW>  g_MorphWeight;
RWStructuredBuffer<SkinPosOut>  g_PosOut;
RWStructuredBuffer<SkinNrmOut>  g_NrmOut;
RWStructuredBuffer<SkinPosPrev> g_PosPrev;   // last frame's skinned positions (TAA velocity)

float3 MulPal(uint b, float3 v, float w)
{
    float4 c0 = g_Palette[b * 4 + 0].v;
    float4 c1 = g_Palette[b * 4 + 1].v;
    float4 c2 = g_Palette[b * 4 + 2].v;
    float4 c3 = g_Palette[b * 4 + 3].v;
    return (c0.xyz * v.x + c1.xyz * v.y + c2.xyz * v.z + c3.xyz * w);
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint v = id.x;
    if (v >= g_VertCount) return;

    // Previous positions FIRST (read the last frame's output before overwriting).
    g_PosPrev[v * 3].v     = g_PosOut[v * 3].v;
    g_PosPrev[v * 3 + 1].v = g_PosOut[v * 3 + 1].v;
    g_PosPrev[v * 3 + 2].v = g_PosOut[v * 3 + 2].v;

    float3 p = float3(g_BindPos[v * 3].v, g_BindPos[v * 3 + 1].v, g_BindPos[v * 3 + 2].v);
    float3 n = float3(g_BindNrm[v * 3].v, g_BindNrm[v * 3 + 1].v, g_BindNrm[v * 3 + 2].v);

    // Blend shapes before skinning.
    for (uint t = 0; t < g_MorphCount; ++t)
    {
        float w = g_MorphWeight[t].v;
        if (w == 0.0) continue;
        uint base = t * g_VertCount * 6;
        p += w * float3(g_MorphDelta[base + v * 3].v,
                        g_MorphDelta[base + v * 3 + 1].v,
                        g_MorphDelta[base + v * 3 + 2].v);
        uint nbase = base + g_VertCount * 3;
        n += w * float3(g_MorphDelta[nbase + v * 3].v,
                        g_MorphDelta[nbase + v * 3 + 1].v,
                        g_MorphDelta[nbase + v * 3 + 2].v);
    }

    uint2 bi = g_BoneIdx[v];
    uint  b0 = bi.x & 0xFFFFu, b1 = bi.x >> 16, b2 = bi.y & 0xFFFFu, b3 = bi.y >> 16;
    float4 bw = g_BoneWgt[v].v;

    float3 sp = float3(0.0, 0.0, 0.0);
    float3 sn = float3(0.0, 0.0, 0.0);
    float wsum = bw.x + bw.y + bw.z + bw.w;
    if (wsum <= 0.0)
    {
        sp = p;   // unweighted vertex (merged rigid part) stays at the morphed bind
        sn = n;
    }
    else
    {
        if (bw.x > 0.0 && b0 < g_BoneCount) { sp += bw.x * MulPal(b0, p, 1.0); sn += bw.x * MulPal(b0, n, 0.0); }
        if (bw.y > 0.0 && b1 < g_BoneCount) { sp += bw.y * MulPal(b1, p, 1.0); sn += bw.y * MulPal(b1, n, 0.0); }
        if (bw.z > 0.0 && b2 < g_BoneCount) { sp += bw.z * MulPal(b2, p, 1.0); sn += bw.z * MulPal(b2, n, 0.0); }
        if (bw.w > 0.0 && b3 < g_BoneCount) { sp += bw.w * MulPal(b3, p, 1.0); sn += bw.w * MulPal(b3, n, 0.0); }
        float len = length(sn);
        if (len > 1e-6) sn /= len;
    }

    g_PosOut[v * 3].v     = sp.x;
    g_PosOut[v * 3 + 1].v = sp.y;
    g_PosOut[v * 3 + 2].v = sp.z;
    g_NrmOut[v * 3].v     = sn.x;
    g_NrmOut[v * 3 + 1].v = sn.y;
    g_NrmOut[v * 3 + 2].v = sn.z;
}
