#include "rt_common.hlsl"

// Miss: the reflection ray escaped the scene -> sample the environment (probe or analytic sky).
[shader("miss")]
void main(inout RTPayload p)
{
    p.color = EnvSample(WorldRayDirection(), 0.0);
}
