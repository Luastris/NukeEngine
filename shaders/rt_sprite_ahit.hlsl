#include "rt_common.hlsl"

// Any-hit for sprites: fade x texture alpha, the same test as the triangle any-hit.
[shader("anyhit")]
void main(inout RTPayload p, in SpriteAttr attr)
{
    RTInstanceData inst = g_Instances[InstanceID()];
    float a = FetchSpriteColor(inst, PrimitiveIndex(), attr.along).a * SampleAlphaMask(inst, attr.uv);
    if (a < 0.35) IgnoreHit();
}
