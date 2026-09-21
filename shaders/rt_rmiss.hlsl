#include "rt_common.hlsl"
#include "rt_water_shade.hlsli"

// Miss: the reflection ray escaped the scene -> sample the environment (probe or analytic sky).
[shader("miss")]
void main(inout RTPayload p)
{
    // Water on the way: a ray from above shades the surface it crossed, one escaping from
    // under water is attenuated by its submerged run (rt_water_shade.hlsli).
    // RayTCurrent() reports TMax in a miss shader; HLSL has no RayTMax() intrinsic.
    p.hitT = RayTCurrent();
    p.color = RTWaterFinishMiss(WorldRayOrigin(), WorldRayDirection(), RayTCurrent(),
                                EnvMiss(WorldRayDirection()), p.depth);
}
