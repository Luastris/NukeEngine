#include "rt_common.hlsl"

// Closest hit for sprites: never reached - the any-hit ignores every sprite hit after stacking
// its layer (rt_sprite_ahit.hlsl). Kept for the hit group; shades the quad as a layer would.
[shader("closesthit")]
void main(inout RTPayload p, in SpriteAttr attr)
{
    p.hitT = RayTCurrent();
    RTInstanceData inst = g_Instances[InstanceID()];
    float4 dc = FetchSpriteColor(inst, PrimitiveIndex(), attr.along);
    float3 c  = inst.emissiveRough.rgb * SampleEmissiveMap(inst, attr.uv) * dc.rgb * dc.a;
    if (g_SkyParams.z > 0.5) c = RTDisplayToLinear(c);
    p.color = c;
}
