#include "rt_common.hlsl"

// Any-hit: ALPHA TEST for NON-OPAQUE instances (particle quads / cutout sprites). Only
// non-opaque BLAS geometry ever invokes this — opaque meshes commit through the fast path
// untouched. Samples the instance's albedo map at the hit UV; a transparent texel lets the
// reflection ray continue through the sprite.
[shader("anyhit")]
void main(inout RTPayload p, in BuiltInTriangleIntersectionAttributes attr)
{
    RTInstanceData inst = g_Instances[InstanceID()];
    float a = FetchDynColor(inst, PrimitiveIndex(), attr.barycentrics).a;   // per-particle fade
    if (inst.texIndex != 0xFFFFFFFFu)
    {
        float2 uv = FetchUV(inst.uvOffset, PrimitiveIndex(), attr.barycentrics);
        a *= g_MatTex[NonUniformResourceIndex(inst.texIndex)].SampleLevel(g_MatTex_sampler, uv, 0).a;
    }
    if (a < 0.35) IgnoreHit();
}
