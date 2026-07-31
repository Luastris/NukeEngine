#include "rt_common.hlsl"

// Any-hit: alpha test for non-opaque instances (particle quads / cutout sprites).
// A transparent texel lets the reflection ray continue through the sprite.
[shader("anyhit")]
void main(inout RTPayload p, in BuiltInTriangleIntersectionAttributes attr)
{
    RTInstanceData inst = g_Instances[InstanceID()];
    float a = FetchDynColor(inst, PrimitiveIndex(), attr.barycentrics).a;
    if (inst.texIndex != 0xFFFFFFFFu)
    {
        float2 uv = FetchUV(inst.uvOffset, PrimitiveIndex(), attr.barycentrics);
        a *= g_MatTex[NonUniformResourceIndex(inst.texIndex)].SampleLevel(g_MatTex_sampler, uv, 0).a;
    }
    if (a < 0.35) IgnoreHit();
}
