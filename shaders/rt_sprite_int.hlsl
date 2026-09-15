#include "rt_common.hlsl"

// Intersection for sprite instances (procedural AABBs): the quad turns toward THIS ray.
[shader("intersection")]
void main()
{
    RTInstanceData inst = g_Instances[InstanceID()];
    float t, along; float2 uv;
    if (SpriteHit(inst.dynPosOffset, inst.shadowShape, PrimitiveIndex(), ObjectToWorld3x4(),
                  WorldRayOrigin(), WorldRayDirection(), RayTMin(), RayTCurrent(), t, uv, along))
    {
        SpriteAttr a; a.uv = uv; a.along = along;
        ReportHit(t, 0, a);
    }
}
