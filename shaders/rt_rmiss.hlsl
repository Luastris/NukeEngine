#include "rt_common.hlsl"

// Miss: the reflection ray escaped the scene -> sample the environment (probe or analytic sky).
[shader("miss")]
void main(inout RTPayload p)
{
    // A ray that escaped from UNDER the water still had to cross it: the sky it returns is
    // attenuated by the submerged run (otherwise a submerged mirror showed a crisp dry sky).
    // RayTCurrent() is valid in a miss shader too (it reports the ray's TMax there) — there
    // is no RayTMax() intrinsic in HLSL, DXC rejects it.
    float3 wT = RTWaterTransRay(WorldRayOrigin(), WorldRayDirection(), RayTCurrent());
    p.color = EnvSample(WorldRayDirection(), 0.0) * wT
            + RTWaterLook(WorldRayDirection()) * (1.0 - dot(wT, float3(0.299, 0.587, 0.114)));
}
