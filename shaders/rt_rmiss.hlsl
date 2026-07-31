#include "rt_common.hlsl"

// Miss: the reflection ray escaped the scene -> sample the environment (probe or analytic sky).
[shader("miss")]
void main(inout RTPayload p)
{
    // A ray escaping from under water is attenuated by its submerged run.
    // RayTCurrent() reports TMax in a miss shader; HLSL has no RayTMax() intrinsic.
    float3 wT = RTWaterTransRay(WorldRayOrigin(), WorldRayDirection(), RayTCurrent());
    p.color = EnvSample(WorldRayDirection(), 0.0) * wT
            + RTWaterLook(WorldRayDirection()) * (1.0 - dot(wT, float3(0.299, 0.587, 0.114)));
}
