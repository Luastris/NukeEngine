#include "rt_common.hlsl"

// Default closest hit: standard metallic-roughness PBR (the "world" shader + any material without its own
// "<name>.surf.hlsl"). Reproduces the engine's standard lighting at the hit, then recurses (TraceRay) for
// reflective surfaces. Custom shaders get their OWN auto-generated closest-hit instead (see GenChitSource).
[shader("closesthit")]
void main(inout RTPayload p, in BuiltInTriangleIntersectionAttributes attr)
{
    uint instId = InstanceID();
    RTInstanceData inst = g_Instances[instId];
    uint   prim = PrimitiveIndex();
    float2 bc   = attr.barycentrics;
    float3 wdir = WorldRayDirection();

    float3 hitN = FetchWorldNormal(inst.nrmOffset, prim, bc, ObjectToWorld3x4());
    if (dot(hitN, wdir) > 0.0) hitN = -hitN;
    float3 hitPos = WorldRayOrigin() + wdir * RayTCurrent();
    float2 uv     = FetchUV(inst.uvOffset, prim, bc);
    float3 albedo = pow(max(SampleAlbedo(inst, uv), 0.0), 2.2);   // sRGB -> linear, exactly like world.ps
    float3 V      = -wdir;                                        // view = back toward where the reflection ray came from
    float  metal  = inst.albedoMetal.w, rough = inst.emissiveRough.w;

    float3 col = ShadeSurface(hitPos, hitN, V, albedo, metal, rough, inst.emissiveRough.rgb);   // honest PBR diffuse

    // Specular reflection: trace the ACTUAL scene (recursion) for sharp reflections; blurred env for rough.
    float3 R = reflect(wdir, hitN);
    float3 env = ReflEnv(R, rough), traced = env;
    if (p.depth < (uint)g_RTParams.z)
    {
        RayDesc ray; ray.Origin = hitPos + hitN * 0.08 + R * 0.05; ray.Direction = R; ray.TMin = 0.02;
        ray.TMax = (g_RTParams.y > 0.5) ? g_RTParams.y : 1000.0;
        RTPayload p2; p2.color = 0.0; p2.depth = p.depth + 1;
        TraceRay(g_TLAS, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, p2);
        traced = p2.color;
    }
    col += SpecFr(hitN, V, rough, albedo, metal) * lerp(traced, env, rough);   // sharp(traced) -> blurred(env) by roughness
    p.color = col;
}
