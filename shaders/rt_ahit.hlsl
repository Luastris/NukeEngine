#include "rt_common.hlsl"

// Any-hit: alpha test for non-opaque instances (particle quads / cutout sprites).
// A transparent texel lets the reflection ray continue through the sprite.
[shader("anyhit")]
void main(inout RTPayload p, in BuiltInTriangleIntersectionAttributes attr)
{
    RTInstanceData inst = g_Instances[InstanceID()];
    float a = FetchDynColor(inst, PrimitiveIndex(), attr.barycentrics).a
            * SampleAlphaMask(inst, FetchUV(inst.uvOffset, PrimitiveIndex(), attr.barycentrics));
    if (a < 0.35) IgnoreHit();
}
